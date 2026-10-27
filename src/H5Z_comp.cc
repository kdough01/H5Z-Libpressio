/* ============================================================================
 * H5Z_comp.cc  --  a LibPressio-backed HDF5 filter (RFC-HDFG-2026 "blobComp")
 *
 * v2: H5Z_class3_t. Supports arbitrarily large codec configs.
 *
 * Config transport (two paths, chosen automatically by size):
 *
 *   SMALL config (fits cd_values, <= ~60 KiB -- the object-header message cap):
 *       Carried inline in cd_values. Fully autonomous both directions -- a
 *       plain H5Dread just works, because cd_values reach the decode callback.
 *
 *   LARGE config (> ~60 KiB -- ROIBIN masks, SZ4 JIT source, ...):
 *       The JSON is stored in the filter's BLOB (H5Pappend_filter_blob), so it
 *       persists in the file and is self-describing. But the decode callback
 *       (filter2) is never handed the blob by the library, so the config is
 *       delivered to filter2 through a DXPL property instead:
 *         - write:  app calls H5Pset_comp_dxpl(dxpl, json) before H5Dwrite
 *         - read:   app calls H5Pget_comp_json(dcpl, &json) to pull the stored
 *                   config out of the file, then H5Pset_comp_dxpl(dxpl, json)
 *                   before H5Dread
 *       filter2 gets dxpl_id (class3), reads the property, and builds the codec
 *       from it. This is the app-cooperation route; it needs no change to the
 *       HDF5 library.
 *
 * GPU codecs work via LibPressio's domain manager (device/host transfers happen
 * inside the codec call; the output is pulled back to host with the same
 * make_host_resident the VOL uses). No raw CUDA in this file.
 *
 * v3: dataset-level path (H5ZCOMP_HAVE_DSET_IO, detected by CMake). On an HDF5
 * whose H5Z_class3_t has dset_write/dset_read, the filter can take a whole
 * H5Dwrite/H5Dread and then supports what the per-chunk callback cannot:
 *   - native (GPU) buffers        H5Pset_comp_native_buffer(dxpl, domain)
 *   - compression unit != chunk   "h5z:group" in the options JSON
 *   - progressive / ROI reads     "h5z:progressive" + H5Pset_comp_progressive
 * See the "Dataset-level transfers" section below. The per-chunk path is
 * unchanged, and is all there is on an HDF5 without those callbacks.
 *
 * Changes since the in-tree version (hdf5@blobComp c9f4a0d):
 *   - set_local records the CHUNK dims (H5Pget_chunk), not the dataset extent.
 *     The old code only worked when the dataset was a single chunk.
 *   - H5Pset_comp's large-config path no longer puts the filter in the pipeline
 *     twice (H5Pappend_filter_blob already appends it; cd_values are now set
 *     on that same entry with H5Pmodify_filter).
 *   - H5Pget_comp_json finds the filter's pipeline index instead of assuming 0.
 *   - can_apply validates the compressor id and options JSON, so a bad config
 *     fails at H5Dcreate (as in the VOL) instead of at the first H5Dwrite.
 *   - Invalid options JSON is an error in the filter too (was silently ignored).
 *   - The H5Z_class3_t is filled field-by-field, and filter2's signature is
 *     chosen at configure time, so this builds against both the blob@e77c5db
 *     layout and the later layout that added init/term and a `state` argument
 *     (H5ZCOMP_HAVE_FILTER_STATE, detected by CMake).
 * ==========================================================================*/

extern "C" {
#include "hdf5.h"
#include "H5PLextern.h"
#include <libpressio/libpressio.h>
#include <libpressio_ext/json/pressio_options_json.h>  /* pressio_options_new_json */
}

#include "H5Zpressio.h"

#include <libpressio_ext/cpp/data.h>
#include <libpressio_ext/cpp/domain.h>
#include <libpressio_ext/cpp/domain_manager.h>
#include <libpressio_ext/cpp/options.h>
#include <libpressio_ext/cpp/pressio.h>

#ifdef H5ZCOMP_HAVE_SPERR
#include <SPERR_C_API.h>   /* sperr_trunc_3d: progressive (prefix) decode */
#endif
#ifdef H5ZCOMP_HAVE_LIBPRESSIO_SPERR
extern "C" void register_libpressio_sperr(void);   /* liblibpressio_sperr.so */
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <exception>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

/* ---------------------------------------------------------------------------
 * Identity, cd_values format, DXPL property
 * ------------------------------------------------------------------------ */
#define H5Z_COMP_CD_MAGIC      0x434F4D50u    /* 'COMP'                          */
#define H5Z_COMP_CD_VERSION    2u
#define H5Z_COMP_FLAG_EXTERNAL 0x1u           /* JSON is in the blob, via DXPL   */
#define H5Z_COMP_FLAG_DEFERRED 0x2u           /* id+JSON wait in the DCPL prop;  */
                                              /* set_local inlines them         */
#define H5Z_COMP_DXPL_PROP     "H5Zcomp_json" /* DXPL prop carrying the JSON     */
#define H5Z_COMP_DCPL_PROP     "H5Zcomp_cfg"  /* DCPL prop: "id\0json\0"         */

/* H5Pget_filter2 / H5Pget_filter_by_id2 refuse any *cd_nelmts above 256 ("probable
 * uninitialized *cd_nelmts argument"), even though H5Pset_filter now accepts up
 * to H5Z_MAX_CD_NELMTS. So set_local/can_apply cannot read back a large inline
 * config through the public API. H5Pset_comp therefore also stashes id+JSON in
 * a DCPL property, which the library copies into the dataset's DCPL, and
 * set_local reads that instead. Only configs whose pre-set_local cd_values fit
 * in 256 are stored fully inline up front (so they work even without the prop). */
#define H5Z_COMP_GET_MAX       256u

/* Inline ceiling. H5Z_MAX_CD_NELMTS allows 65535 cd_values (~256 KiB), but the
 * pipeline is ONE object-header message and those are capped at
 * H5O_MESG_MAX_SIZE = 64 KiB ("object header message is too large" at
 * H5Dcreate -- measured: ~65.4 KB of JSON is the most a noop entry can carry).
 * Stay well under it; anything larger goes to the blob. */
#define H5Z_COMP_INLINE_MAX_UINTS (60u * 1024u / 4u)

/*
 * cd_values layout (v2):
 *   [0] magic
 *   [1] version = 2
 *   [2] flags                         (bit0 = EXTERNAL: JSON in blob; bit1 = DEFERRED)
 *   [3] dtype   (enum pressio_dtype)
 *   [4] ndims   (chunk rank)
 *   [5 .. 4+ndims] chunk dims (natural order)
 *   [B]   id_len     (B = 5 + ndims)
 *   [B+1] json_len   (EXTERNAL: length of the "@fnv1a64:<hex>" content tag)
 *   [B+2 ...] id bytes then json bytes, 4 bytes/uint LE
 */

/* ---------------------------------------------------------------------------
 * byte <-> uint packing
 * ------------------------------------------------------------------------ */
static size_t comp_uints_for_bytes(size_t nbytes) { return (nbytes + 3u) / 4u; }

static void comp_pack_bytes(const unsigned char *bytes, size_t nbytes, unsigned *out) {
    size_t nu = comp_uints_for_bytes(nbytes);
    for (size_t i = 0; i < nu; i++) {
        unsigned v = 0;
        for (size_t b = 0; b < 4; b++) {
            size_t idx = i * 4 + b;
            if (idx < nbytes) v |= (unsigned)bytes[idx] << (8u * b);
        }
        out[i] = v;
    }
}
static void comp_unpack_bytes(const unsigned *in, size_t nbytes, unsigned char *out) {
    for (size_t i = 0; i < nbytes; i++)
        out[i] = (unsigned char)((in[i / 4] >> (8u * (i % 4))) & 0xffu);
}

/* ---------------------------------------------------------------------------
 * Parsed configuration
 * ------------------------------------------------------------------------ */
struct comp_cd {
    unsigned flags;
    unsigned dtype;
    size_t   ndims;
    size_t   dims[H5S_MAX_RANK];
    char    *id;
    char    *json;      /* inline JSON; the content tag when EXTERNAL */
};

static void comp_cd_free(comp_cd *c) {
    if (!c) return;
    free(c->id);   c->id = NULL;
    free(c->json); c->json = NULL;
}

static int comp_parse_cd(const unsigned *cd, size_t n, comp_cd *c) {
    memset(c, 0, sizeof(*c));
    if (n < 7) return -1;
    if (cd[0] != H5Z_COMP_CD_MAGIC)   return -1;
    if (cd[1] != H5Z_COMP_CD_VERSION) return -1;
    c->flags = cd[2];
    c->dtype = cd[3];
    c->ndims = cd[4];
    if (c->ndims > H5S_MAX_RANK) return -1;
    size_t base = 5 + c->ndims;
    if (n < base + 2) return -1;
    for (size_t i = 0; i < c->ndims; i++) c->dims[i] = cd[5 + i];

    size_t id_len = cd[base + 0], json_len = cd[base + 1], payload = id_len + json_len;
    if (n < base + 2 + comp_uints_for_bytes(payload)) return -1;

    unsigned char *blob = (unsigned char *)malloc(payload ? payload : 1);
    if (!blob) return -1;
    comp_unpack_bytes(cd + base + 2, payload, blob);
    c->id = (char *)malloc(id_len + 1);
    c->json = (char *)malloc(json_len + 1);
    if (!c->id || !c->json) { free(blob); comp_cd_free(c); return -1; }
    memcpy(c->id, blob, id_len);              c->id[id_len]     = '\0';
    memcpy(c->json, blob + id_len, json_len); c->json[json_len] = '\0';
    free(blob);
    return 0;
}

static int comp_build_cd(unsigned flags, unsigned dtype, size_t ndims, const size_t *dims,
                         const char *id, const char *json,
                         unsigned *cd, size_t cap, size_t *n_out) {
    size_t id_len = id ? strlen(id) : 0, json_len = json ? strlen(json) : 0;
    size_t payload = id_len + json_len, base = 5 + ndims;
    size_t n = base + 2 + comp_uints_for_bytes(payload);
    if (ndims > H5S_MAX_RANK) return -1;
    if (n > cap || n > H5Z_MAX_CD_NELMTS) return -1;
    cd[0] = H5Z_COMP_CD_MAGIC; cd[1] = H5Z_COMP_CD_VERSION; cd[2] = flags;
    cd[3] = dtype; cd[4] = (unsigned)ndims;
    for (size_t i = 0; i < ndims; i++) cd[5 + i] = (unsigned)dims[i];
    cd[base + 0] = (unsigned)id_len; cd[base + 1] = (unsigned)json_len;
    unsigned char *blob = (unsigned char *)malloc(payload ? payload : 1);
    if (!blob) return -1;
    if (id_len)   memcpy(blob, id, id_len);
    if (json_len) memcpy(blob + id_len, json, json_len);
    comp_pack_bytes(blob, payload, cd + base + 2);
    free(blob);
    *n_out = n;
    return 0;
}

/* ---------------------------------------------------------------------------
 * HDF5 datatype -> pressio_dtype; byte-stream test (from the VOL adaptor)
 * ------------------------------------------------------------------------ */
static enum pressio_dtype comp_hdf5_to_pressio_dtype(hid_t type_id) {
    size_t size = H5Tget_size(type_id);
    H5T_class_t cls = H5Tget_class(type_id);
    if (cls == H5T_FLOAT)   { if (size == 4) return pressio_float_dtype; if (size == 8) return pressio_double_dtype; }
    if (cls == H5T_INTEGER) { if (size == 4) return pressio_int32_dtype; if (size == 8) return pressio_int64_dtype; }
    return pressio_byte_dtype;
}
static int comp_is_byte_stream(const char *id) {
    return strncmp(id, "nvcomp", 6) == 0 || strcmp(id, "zstd") == 0;
}

/* ---------------------------------------------------------------------------
 * Bring a codec output back to host (device->host D2H for GPU codecs; no-op
 * for CPU). Verbatim from compress.cc so it resolves against LibPressio.
 * ------------------------------------------------------------------------ */
static int comp_domain_is_host_accessible(const char *dom) {
    if (!dom) return 0;
    return strcmp(dom, "malloc") == 0 || strcmp(dom, "cudamallochost") == 0 ||
           strcmp(dom, "cudahostalloc") == 0 || strcmp(dom, "cudamallocmanaged") == 0;
}
static void comp_make_host_resident(struct pressio_data *data) {
    if (!data) return;
    if (!comp_domain_is_host_accessible(pressio_data_domain_id(data))) {
        pressio_data *d = data;
        *d = domain_manager().make_readable(
            libpressio::domain_plugins().build("malloc"), std::move(*d));
    }
}

/* ---------------------------------------------------------------------------
 * Pipeline lookup helpers
 * ------------------------------------------------------------------------ */
/* Index of our filter in a DCPL's pipeline, or -1. */
static int comp_pipeline_index(hid_t dcpl_id) {
    int nf = H5Pget_nfilters(dcpl_id);
    for (int i = 0; i < nf; i++) {
        unsigned flags = 0; size_t nel = 0; unsigned fc = 0;
        H5Z_filter_t id = H5Pget_filter2(dcpl_id, (unsigned)i, &flags, &nel, NULL, 0, NULL, &fc);
        if (id == (H5Z_filter_t)H5Z_FILTER_COMP) return i;
    }
    return -1;
}

/* The blob JSON attached to our filter on a DCPL; malloc'd, or NULL. */
static char *comp_read_blob_json(hid_t dcpl_id) {
    int idx = comp_pipeline_index(dcpl_id);
    if (idx < 0) return NULL;
    size_t sz = 0;
    if (H5Pget_filter_blob(dcpl_id, (unsigned)idx, 0, NULL, &sz) < 0 || sz == 0) return NULL;
    char *buf = (char *)malloc(sz + 1);
    if (!buf) return NULL;
    size_t cap = sz;
    if (H5Pget_filter_blob(dcpl_id, (unsigned)idx, 0, buf, &cap) < 0) { free(buf); return NULL; }
    buf[sz] = '\0';
    return buf;
}

/* Number of cd_values comp_build_cd would produce. */
static size_t comp_cd_len(size_t ndims, size_t id_len, size_t json_len) {
    return 5 + ndims + 2 + comp_uints_for_bytes(id_len + json_len);
}

/* DCPL property carrying "id\0json\0" (see H5Z_COMP_GET_MAX). */
static herr_t comp_set_dcpl_cfg(hid_t dcpl_id, const char *id, const char *json) {
    size_t il = strlen(id), jl = strlen(json), sz = il + 1 + jl + 1;
    char *buf = (char *)malloc(sz);
    if (!buf) return -1;
    memcpy(buf, id, il + 1);
    memcpy(buf + il + 1, json, jl + 1);
    if (H5Pexist(dcpl_id, H5Z_COMP_DCPL_PROP) > 0) H5Premove(dcpl_id, H5Z_COMP_DCPL_PROP);
    herr_t r = H5Pinsert2(dcpl_id, H5Z_COMP_DCPL_PROP, sz, buf, NULL, NULL, NULL, NULL, NULL, NULL);
    free(buf);
    return r;
}
static int comp_get_dcpl_cfg(hid_t dcpl_id, char **id, char **json) {
    *id = *json = NULL;
    if (H5Pexist(dcpl_id, H5Z_COMP_DCPL_PROP) <= 0) return -1;
    size_t sz = 0;
    if (H5Pget_size(dcpl_id, H5Z_COMP_DCPL_PROP, &sz) < 0 || sz < 2) return -1;
    char *buf = (char *)malloc(sz + 1);
    if (!buf) return -1;
    if (H5Pget(dcpl_id, H5Z_COMP_DCPL_PROP, buf) < 0) { free(buf); return -1; }
    buf[sz] = '\0';
    size_t il = strnlen(buf, sz);
    *id = strdup(buf);
    *json = strdup(il + 1 < sz ? buf + il + 1 : "");
    free(buf);
    return (*id && *json) ? 0 : -1;
}

/* Resolve this DCPL's filter config for can_apply / set_local.
 * c->id / c->json are filled (json = inline JSON, or the tag when EXTERNAL). */
static int comp_read_config(hid_t dcpl_id, comp_cd *c, unsigned *flt_flags) {
    memset(c, 0, sizeof(*c));
    size_t nel = 0;
    if (H5Pget_filter_by_id2(dcpl_id, H5Z_FILTER_COMP, flt_flags, &nel, NULL, 0, NULL, NULL) < 0)
        return -1;

    unsigned cd_small[H5Z_COMP_GET_MAX];
    int have_cd = 0;
    if (nel >= 7 && nel <= H5Z_COMP_GET_MAX) {
        size_t n2 = nel;
        if (H5Pget_filter_by_id2(dcpl_id, H5Z_FILTER_COMP, flt_flags, &n2, cd_small, 0, NULL, NULL) >= 0 &&
            comp_parse_cd(cd_small, n2, c) == 0)
            have_cd = 1;
    }

    char *pid = NULL, *pjson = NULL;
    if (comp_get_dcpl_cfg(dcpl_id, &pid, &pjson) == 0) {
        /* The property is authoritative for this DCPL (it is what H5Pset_comp
         * was given); keep the EXTERNAL bit from cd_values if we could read it. */
        unsigned flags = have_cd ? (c->flags & H5Z_COMP_FLAG_EXTERNAL) : 0u;
        comp_cd_free(c);
        memset(c, 0, sizeof(*c));
        c->flags = flags;
        c->id    = pid;
        c->json  = pjson;   /* EXTERNAL never sets the prop, so this is inline JSON */
        return c->json ? 0 : -1;
    }
    if (have_cd && !(c->flags & H5Z_COMP_FLAG_DEFERRED)) return 0;

    comp_cd_free(c);
    fprintf(stderr, "[H5Z_comp] cannot recover the filter config from this DCPL: its cd_values "
            "(%zu) exceed what H5Pget_filter_by_id2 will return (%u) and the %s property "
            "is absent. Re-attach the filter with H5Pset_comp().\n",
            nel, H5Z_COMP_GET_MAX, H5Z_COMP_DCPL_PROP);
    return -1;
}

/* ---------------------------------------------------------------------------
 * "h5z:" keys -- dataset-level settings carried in the options JSON
 *
 * The options JSON is already the filter's persisted, self-describing config
 * (inline in cd_values, or in the blob when large). Settings that belong to
 * the FILTER rather than to the codec ride in it under an "h5z:" prefix -- the
 * same convention as the VOL adaptor's "vol:" keys -- and are removed before
 * the rest is handed to LibPressio:
 *
 *   "h5z:group"        storage chunks per COMPRESSION UNIT, one entry per
 *                      dataset dimension: "4,1,1" or [4,1,1]. A 0 entry means
 *                      every chunk along that dimension; the single value 0
 *                      (or "all") makes the whole dataset one unit. Decouples
 *                      the compression unit from the HDF5 storage chunk.
 *   "h5z:progressive"  true: each unit's stream is written so that a PREFIX of
 *                      it decodes at reduced fidelity (sperr: one SPERR chunk
 *                      per unit), which is what H5Pset_comp_progressive needs.
 *
 * Both need the dataset-level callbacks (H5Z_class3_t dset_write/dset_read).
 * ------------------------------------------------------------------------ */
struct comp_h5z_cfg {
    bool                 grouped     = false;
    bool                 group_all   = false;   /* whole dataset is one unit       */
    std::vector<hsize_t> group;                 /* chunks per unit per dim; 0=all  */
    bool                 progressive = false;
};

/* Codecs whose stream prefix can be decoded (after comp_truncate_stream). */
static bool comp_progressive_codec(const std::string &id) {
#ifdef H5ZCOMP_HAVE_SPERR
    return id == "sperr";
#else
    (void)id; return false;
#endif
}

/* Read the h5z: keys out of parsed options and erase them. 0 / -1 (message printed). */
static int comp_parse_h5z(struct pressio_options *opts, comp_h5z_cfg *cfg) {
    *cfg = comp_h5z_cfg();
    try {
        std::vector<std::string> keys;
        for (auto const &kv : *opts)
            if (kv.first.compare(0, 4, "h5z:") == 0) keys.push_back(kv.first);
        for (auto const &k : keys) {
            pressio_option const &o = opts->get(k);
            if (k == "h5z:group") {
                std::vector<double> v; bool ok = true;
                if (o.holds_alternative<std::string>()) {
                    const std::string sv = o.get_value<std::string>();
                    if (sv == "all") v.push_back(0);
                    else for (const char *q = sv.c_str(); *q;) {
                        char *e = NULL; double d = strtod(q, &e);
                        if (e == q) { ok = false; break; }
                        v.push_back(d);
                        for (q = e; *q == ',' || *q == 'x' || *q == ' '; q++) {}
                    }
                } else if (o.holds_alternative<double>()) v.push_back(o.get_value<double>());
                else if (o.holds_alternative<pressio_data>()) v = o.get_value<pressio_data>().to_vector<double>();
                else ok = false;
                for (double d : v) if (!(d >= 0) || d != std::floor(d) || d > 4294967295.0) ok = false;
                if (!ok || v.empty() || v.size() > (size_t)H5S_MAX_RANK) {
                    fprintf(stderr, "[H5Z_comp] \"h5z:group\" must be \"all\", 0, or one non-negative "
                            "integer per dataset dimension (e.g. \"4,1,1\")\n");
                    return -1;
                }
                cfg->grouped = true;
                if (v.size() == 1 && v[0] == 0) cfg->group_all = true;
                else for (double d : v) cfg->group.push_back((hsize_t)d);
            } else if (k == "h5z:progressive") {
                if (o.holds_alternative<bool>()) cfg->progressive = o.get_value<bool>();
                else if (o.holds_alternative<double>()) cfg->progressive = o.get_value<double>() != 0;
                else if (o.holds_alternative<std::string>()) {
                    const std::string sv = o.get_value<std::string>();
                    cfg->progressive = (sv == "true" || sv == "1" || sv == "on");
                } else { fprintf(stderr, "[H5Z_comp] \"h5z:progressive\" must be true or false\n"); return -1; }
            } else {
                fprintf(stderr, "[H5Z_comp] unknown filter option \"%s\" (known: h5z:group, "
                        "h5z:progressive)\n", k.c_str());
                return -1;
            }
        }
        for (auto const &k : keys) opts->erase(k);
    } catch (const std::exception &e) {
        fprintf(stderr, "[H5Z_comp] bad h5z: option: %s\n", e.what());
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Built codec context (+ one-entry per-thread cache)
 * ------------------------------------------------------------------------ */
struct comp_ctx {
    std::string                id;
    enum pressio_dtype         dtype;
    size_t                     ndims;
    size_t                     dims[H5S_MAX_RANK];
    struct pressio            *library;
    struct pressio_compressor *compressor;
    /* Decompression uses its own clone: LibPressio's sz3 plugin loads the
     * configuration stored in the stream into the compressor on decompress,
     * and later compress calls on that object then produce a different (in
     * the case measured, ~9% larger) stream. Keeping the two directions on
     * separate objects makes every compress independent of earlier reads. */
    struct pressio_compressor *decompressor;
    comp_h5z_cfg               h5z;
};

static struct pressio_compressor *comp_decompressor(struct comp_ctx *x) {
    if (!x->decompressor) x->decompressor = pressio_compressor_clone(x->compressor);
    return x->decompressor ? x->decompressor : x->compressor;
}

static void comp_ctx_free(comp_ctx *x) {
    if (!x) return;
    if (x->decompressor) pressio_compressor_release(x->decompressor);
    if (x->compressor) pressio_compressor_release(x->compressor);
    if (x->library)    pressio_release(x->library);
    delete x;
}
static size_t comp_logical_nbytes(const comp_ctx *x) {
    size_t n = (size_t)pressio_dtype_size(x->dtype);
    for (size_t i = 0; i < x->ndims; i++) n *= x->dims[i];
    return n;
}
static comp_ctx *comp_ctx_build(const char *id, const char *json,
                                enum pressio_dtype dtype, size_t ndims, const size_t *dims) {
    comp_ctx *x = new comp_ctx();
    x->id = id ? id : "";
    x->dtype = dtype; x->ndims = ndims;
    for (size_t i = 0; i < ndims; i++) x->dims[i] = dims[i];
    x->library = pressio_instance();
    if (!x->library) { comp_ctx_free(x); return NULL; }
    x->compressor = pressio_get_compressor(x->library, x->id.c_str());
    if (!x->compressor) {
        fprintf(stderr, "[H5Z_comp] unknown compressor id '%s': %s\n",
                x->id.c_str(), pressio_error_msg(x->library));
        comp_ctx_free(x); return NULL;
    }
    if (json && json[0]) {
        struct pressio_options *opts = pressio_options_new_json(x->library, json);
        if (!opts) {
            fprintf(stderr, "[H5Z_comp] invalid options JSON for '%s': %s\n",
                    x->id.c_str(), pressio_error_msg(x->library));
            comp_ctx_free(x); return NULL;
        }
        if (comp_parse_h5z(opts, &x->h5z) < 0) {   /* also strips the h5z: keys */
            pressio_options_free(opts);
            comp_ctx_free(x); return NULL;
        }
        int rc = pressio_compressor_set_options(x->compressor, opts);
        pressio_options_free(opts);
        if (rc != 0) {
            fprintf(stderr, "[H5Z_comp] set_options('%s') failed: %s\n",
                    x->id.c_str(), pressio_compressor_error_msg(x->compressor));
            comp_ctx_free(x); return NULL;
        }
    }
    return x;
}

/* Read the JSON the app stashed on the DXPL (large-config path). malloc'd. */
static char *comp_read_dxpl_json(hid_t dxpl_id) {
    if (dxpl_id < 0) return NULL;
    if (H5Pexist(dxpl_id, H5Z_COMP_DXPL_PROP) <= 0) return NULL;
    size_t sz = 0;
    if (H5Pget_size(dxpl_id, H5Z_COMP_DXPL_PROP, &sz) < 0 || sz == 0) return NULL;
    char *buf = (char *)malloc(sz);
    if (!buf) return NULL;
    if (H5Pget(dxpl_id, H5Z_COMP_DXPL_PROP, buf) < 0) { free(buf); return NULL; }
    buf[sz - 1] = '\0';
    return buf;
}

/* ---------------------------------------------------------------------------
 * Large-config registry.
 *
 * Why: HDF5 runs the filter when a chunk leaves the chunk cache, and for writes
 * that is usually at H5Dclose/H5Fclose, whose API context carries H5P_DEFAULT
 * -- NOT the DXPL given to H5Dwrite. So a config delivered only on the DXPL is
 * gone by the time the forward filter runs (verified on blob@e77c5db).
 *
 * Fix: an EXTERNAL dataset's cd_values carry a content tag ("@fnv1a64:<hex>")
 * of its blob JSON in the inline-JSON slot, and every place that sees the JSON
 * (H5Pset_comp, can_apply/set_local via the blob, H5Pset_comp_dxpl,
 * H5Pget_comp_json) registers tag -> JSON here. filter2 resolves the tag from
 * the registry first and only falls back to the DXPL; a DXPL JSON whose tag
 * does not match the file's is rejected.
 * ------------------------------------------------------------------------ */
#define H5Z_COMP_TAG_PREFIX "@fnv1a64:"

static std::string comp_json_tag(const char *json) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)json; *p; ++p) { h ^= *p; h *= 1099511628211ull; }
    char buf[40];
    snprintf(buf, sizeof(buf), H5Z_COMP_TAG_PREFIX "%016llx", (unsigned long long)h);
    return buf;
}
static std::mutex                                   g_reg_mu;
static std::unordered_map<std::string, std::string> g_reg;

static std::string comp_register_json(const char *json) {
    std::string tag = comp_json_tag(json);
    std::lock_guard<std::mutex> lk(g_reg_mu);
    g_reg[tag] = json;
    return tag;
}
static bool comp_lookup_json(const std::string &tag, std::string *out) {
    std::lock_guard<std::mutex> lk(g_reg_mu);
    auto it = g_reg.find(tag);
    if (it == g_reg.end()) return false;
    *out = it->second;
    return true;
}

static thread_local comp_ctx   *g_ctx = NULL;
static thread_local std::string  g_key;

static comp_ctx *comp_ctx_get(const unsigned *cd, size_t n, hid_t dxpl_id) {
    comp_cd c;
    if (comp_parse_cd(cd, n, &c) < 0) { fprintf(stderr, "[H5Z_comp] malformed cd_values\n"); return NULL; }
    if (c.flags & H5Z_COMP_FLAG_DEFERRED) {
        fprintf(stderr, "[H5Z_comp] cd_values still DEFERRED: set_local did not run\n");
        comp_cd_free(&c); return NULL;
    }

    /* cd_values identify the codec completely (EXTERNAL ones via the tag), so
     * they are the cache key -- no per-chunk compare of a multi-MB JSON. */
    std::string key((const char *)cd, n * sizeof(unsigned));
    if (g_ctx && g_key == key) { comp_cd_free(&c); return g_ctx; }

    std::string ext_json;
    const char *json = c.json;
    if (c.flags & H5Z_COMP_FLAG_EXTERNAL) {
        std::string tag = c.json;            /* "@fnv1a64:..." */
        if (!comp_lookup_json(tag, &ext_json)) {
            char *dj = comp_read_dxpl_json(dxpl_id);
            if (!dj) {
                fprintf(stderr, "[H5Z_comp] '%s' uses a large (blob) config this process has not "
                        "seen -- before H5Dread call H5Pget_comp_json(H5Dget_create_plist(dset), &json) "
                        "(or H5Pset_comp_dxpl(dxpl, json)).\n", c.id);
                comp_cd_free(&c); return NULL;
            }
            if (comp_json_tag(dj) != tag) {
                fprintf(stderr, "[H5Z_comp] '%s': the JSON on the DXPL does not match the config "
                        "stored with this dataset (tag %s)\n", c.id, tag.c_str());
                free(dj); comp_cd_free(&c); return NULL;
            }
            comp_register_json(dj);
            ext_json = dj;
            free(dj);
        }
        json = ext_json.c_str();
    }

    comp_ctx *x = comp_ctx_build(c.id, json, (enum pressio_dtype)c.dtype, c.ndims, c.dims);
    comp_cd_free(&c);
    if (!x) return NULL;
    if (g_ctx) comp_ctx_free(g_ctx);
    g_ctx = x; g_key = key;
    return x;
}

/* ---------------------------------------------------------------------------
 * native-path compress / decompress
 * ------------------------------------------------------------------------ */
/* ---------------------------------------------------------------------------
 * zfp CUDA size workaround.
 *
 * With zfp:execution=cuda and fixed rate, zfp_compress() (as called by
 * LibPressio's zfp plugin) reports 1/8 of the real stream size -- verified on
 * Swing with the pressio CLI alone: rate 8 on Miranda reports 4,718,592 B /
 * bit_rate 1, while the stream is 37,748,736 B. The buffer does hold the whole
 * stream, so in-memory round trips look fine, but a filter stores only the
 * reported bytes and the dataset becomes undecodable.
 *
 * For fixed rate the true size is known exactly: ceil(nblocks*maxbits/64)*8
 * with nblocks = prod(ceil(dim/4)). If the codec is zfp on CUDA in fixed-rate
 * mode, the reported size is short, and the buffer's capacity covers the true
 * size, we use the true size. Disable with H5ZCOMP_NO_ZFP_CUDA_FIX=1.
 * ------------------------------------------------------------------------ */
static size_t comp_zfp_cuda_true_size(comp_ctx *x, size_t in_ndims, const size_t *in_dims) {
    if (x->id != "zfp") return 0;
    const char *e = getenv("H5ZCOMP_NO_ZFP_CUDA_FIX");
    if (e && *e && *e != '0') return 0;
    struct pressio_options *o = pressio_compressor_get_options(x->compressor);
    if (!o) return 0;
    int32_t exec = -1; uint32_t minbits = 0, maxbits = 0;
    pressio_options_get_integer(o, "zfp:execution", &exec);
    pressio_options_get_uinteger(o, "zfp:minbits", &minbits);
    pressio_options_get_uinteger(o, "zfp:maxbits", &maxbits);
    pressio_options_free(o);
    if (exec != 2 /* zfp_exec_cuda */ || maxbits == 0 || minbits != maxbits) return 0;
    unsigned long long nblocks = 1;
    for (size_t i = 0; i < in_ndims; i++) nblocks *= (unsigned long long)((in_dims[i] + 3) / 4);
    unsigned long long bits = nblocks * (unsigned long long)maxbits;
    return (size_t)(((bits + 63) / 64) * 8);
}

/* Compress one unit. `data` is `nbytes` of x->dtype in LibPressio domain
 * `domain`, with natural-order (slowest first) shape dims[ndims]. Returns the
 * codec output, made host-resident, and its valid length in *out_size (which
 * can exceed size_in_bytes() -- see the zfp CUDA note above). NULL on failure.
 * The caller frees the result with pressio_data_free(). */
static struct pressio_data *comp_compress_core(comp_ctx *x, const void *data, size_t nbytes,
                                               const char *domain, size_t ndims, const size_t *dims,
                                               size_t *out_size) {
    struct pressio_data *input = NULL, *output = NULL, *ret = NULL;
    size_t force_bytes = 0;   /* zfp CUDA workaround, see below */
    size_t rdims[H5S_MAX_RANK], byte_dims[1];
    enum pressio_dtype in_dtype; size_t in_ndims; size_t *in_dims;

    if (comp_is_byte_stream(x->id.c_str())) {
        in_dtype = pressio_byte_dtype; in_ndims = 1; byte_dims[0] = nbytes; in_dims = byte_dims;
    } else {
        if (ndims == 0) { fprintf(stderr, "[H5Z_comp] no shape for '%s'\n", x->id.c_str()); return NULL; }
        size_t logical = (size_t)pressio_dtype_size(x->dtype);
        for (size_t i = 0; i < ndims; i++) logical *= dims[i];
        if (logical != nbytes) {
            fprintf(stderr, "[H5Z_comp] shape/size mismatch for '%s': dims imply %zu B, chunk is %zu B\n",
                    x->id.c_str(), logical, nbytes);
            return NULL;
        }
        in_dtype = x->dtype; in_ndims = ndims;
        for (size_t i = 0; i < ndims; i++) rdims[i] = dims[ndims - 1 - i];
        in_dims = rdims;
    }

    input  = pressio_data_new_nonowning_domain(in_dtype, (void *)data, in_ndims, in_dims, domain);
    /* EMPTY output: let the codec allocate it in whatever domain it wants,
     * exactly as the pressio CLI does. A pre-sized host buffer looked like the
     * VOL's host arm but is the prime suspect for broken zfp CUDA rows (streams
     * 1/8 the expected size, undecodable): with a non-empty output libpressio's
     * zfp plugin migrates it to cudamalloc and REUSES it as the device
     * bitstream instead of taking its own buffer. The cusz crash that prompted
     * the pre-sizing was NaN/Inf input, not the buffer (it reproduces in the
     * pressio CLI, which passes an empty output). */
    output = pressio_data_new_empty(pressio_byte_dtype, 0, NULL);
    if (!input || !output) goto done;

    if (pressio_compressor_compress(x->compressor, input, output) != 0) {
        fprintf(stderr, "[H5Z_comp] compress '%s' failed: %s\n",
                x->id.c_str(), pressio_compressor_error_msg(x->compressor));
        goto done;
    }
    {
        size_t want = comp_zfp_cuda_true_size(x, in_ndims, in_dims);
        size_t have = output->size_in_bytes();
        if (want > have) {
            /* Where the full stream lives depends on the plugin's path:
             *  - it reused a pre-sized output: capacity_in_bytes() covers it;
             *  - it malloc'd its own bitstream (our empty-output case): the
             *    buffer is zfp_stream_maximum_size() >= want bytes, but
             *    pressio_data::move() records capacity == the short size.
             * Reading `want` bytes is safe in both; reshape() only works in the
             * first, so the second is handled by force_bytes at the copy. */
            static bool warned = false;
            if (!warned) {
                fprintf(stderr, "[H5Z_comp] zfp CUDA reported %zu B for a fixed-rate stream of "
                        "%zu B; storing the full stream (zfp CUDA size bug workaround, "
                        "H5ZCOMP_NO_ZFP_CUDA_FIX=1 disables)\n", have, want);
                warned = true;
            }
            if (output->capacity_in_bytes() >= want) output->reshape({want});
            else if (comp_domain_is_host_accessible(pressio_data_domain_id(output))) force_bytes = want;
            else {
                fprintf(stderr, "[H5Z_comp] zfp CUDA stream is short (%zu of %zu B) and not "
                        "host-resident -- refusing to store a truncated stream\n", have, want);
                goto done;
            }
        }
    }
    comp_make_host_resident(output);
    {
        size_t sz = 0; void *src = pressio_data_ptr(output, &sz);
        if (force_bytes > sz) sz = force_bytes;
        if (!src || sz == 0) goto done;
        *out_size = sz;
        ret = output; output = NULL;
    }
done:
    if (input)  pressio_data_free(input);
    if (output) pressio_data_free(output);
    return ret;
}

/* Per-chunk path: one HDF5 chunk (host memory, shape x->dims) -> H5allocate'd stream. */
static void *comp_compress(comp_ctx *x, const void *data, size_t nbytes, size_t *out_csize) {
    void *ret = NULL;
    size_t sz = 0;
    struct pressio_data *out = comp_compress_core(x, data, nbytes, "malloc", x->ndims, x->dims, &sz);
    if (!out) return NULL;
    /* HDF5 records a filtered chunk's stored size in 32 bits. A larger
     * result is written without complaint and silently truncated on read
     * (seen: einspline37 noop, one 12.6 GiB chunk -> maxae 21 on read-back),
     * so refuse it here and let the write fail loudly. */
    if (sz >= ((size_t)1 << 32)) {
        fprintf(stderr, "[H5Z_comp] '%s' produced %zu B for one chunk; HDF5 cannot store a "
                "filtered chunk >= 4 GiB -- use smaller chunks\n", x->id.c_str(), sz);
        goto done;
    }
    ret = H5allocate_memory(sz, 0);
    if (!ret) goto done;
    memcpy(ret, pressio_data_ptr(out, NULL), sz);
    *out_csize = sz;
done:
    pressio_data_free(out);
    return ret;
}

/* Decompress one unit of natural-order shape dims[ndims] into `dst`, which is
 * `out_bytes` of memory in LibPressio domain `dst_domain`. Returns 0 on success.
 *
 * dst is handed to the codec as a NON-OWNING output in its own domain. A codec
 * that works in that domain (a GPU codec given device memory, a CPU codec
 * given host memory) decodes straight into it: LibPressio's make_writeable()
 * returns an accessible output as is. A codec that works elsewhere answers in
 * a buffer of its own, and the result is then moved to dst -- memcpy for host
 * memory, the domain manager's send for anything else. */
static int comp_decompress_core(comp_ctx *x, const void *cbuf, size_t csize, size_t ndims,
                                const size_t *dims, void *dst, size_t out_bytes, const char *dst_domain) {
    const bool host_dst = comp_domain_is_host_accessible(dst_domain);
    if (x->id == "noop") {
        if (csize != out_bytes) {   /* e.g. a >= 4 GiB chunk truncated by HDF5 */
            fprintf(stderr, "[H5Z_comp] noop chunk is %zu B on disk but should be %zu B\n", csize, out_bytes);
            return -1;
        }
        if (host_dst) { memcpy(dst, cbuf, out_bytes); return 0; }
    }

    struct pressio_data *input = NULL, *output = NULL;
    size_t rdims[H5S_MAX_RANK], byte_dims[1], comp_dims[1];
    enum pressio_dtype out_dtype; size_t out_ndims; size_t *out_dims; int ok = 0;

    if (comp_is_byte_stream(x->id.c_str())) {
        out_dtype = pressio_byte_dtype; out_ndims = 1; byte_dims[0] = out_bytes; out_dims = byte_dims;
    } else {
        out_dtype = x->dtype; out_ndims = ndims;
        for (size_t i = 0; i < ndims; i++) rdims[i] = dims[ndims - 1 - i];
        out_dims = rdims;
    }

    comp_dims[0] = csize;
    input  = pressio_data_new_nonowning_domain(pressio_byte_dtype, (void *)cbuf, 1, comp_dims, "malloc");
    output = pressio_data_new_nonowning_domain(out_dtype, dst, out_ndims, out_dims, dst_domain);
    if (!input || !output) goto done;

    if (pressio_compressor_decompress(comp_decompressor(x), input, output) != 0) {
        fprintf(stderr, "[H5Z_comp] decompress '%s' failed: %s\n",
                x->id.c_str(), pressio_compressor_error_msg(comp_decompressor(x)));
        goto done;
    }
    if (host_dst) {
        comp_make_host_resident(output);
        size_t sz = 0; void *src = pressio_data_ptr(output, &sz);
        if (!src) goto done;
        if (src != dst) memcpy(dst, src, sz < out_bytes ? sz : out_bytes);
    } else if (pressio_data_ptr(output, NULL) != dst) {
        /* The codec answered in its own buffer (e.g. a CPU codec asked to fill
         * device memory). Send it to the application's buffer. */
        if (output->size_in_bytes() < out_bytes) {
            fprintf(stderr, "[H5Z_comp] decompress '%s' produced %zu B, expected %zu B\n",
                    x->id.c_str(), output->size_in_bytes(), out_bytes);
            goto done;
        }
        try {
            pressio_data target = pressio_data::nonowning(
                out_dtype, dst, std::vector<size_t>(out_dims, out_dims + out_ndims), dst_domain);
            domain_manager().send(target, *output);
        } catch (const std::exception &e) {
            fprintf(stderr, "[H5Z_comp] '%s' decoded into a '%s' buffer of its own and LibPressio cannot "
                    "move that into the '%s' buffer given to H5Dread: %s\n", x->id.c_str(),
                    pressio_data_domain_id(output), dst_domain, e.what());
            goto done;
        }
    }
    ok = 1;
done:
    if (input)  pressio_data_free(input);
    if (output) pressio_data_free(output);
    return ok ? 0 : -1;
}

/* Per-chunk path: one stored chunk -> H5allocate'd host buffer of shape x->dims. */
static void *comp_decompress(comp_ctx *x, const void *cbuf, size_t csize, size_t *out_nbytes) {
    size_t out_bytes = comp_logical_nbytes(x);
    void *ret = H5allocate_memory(out_bytes, 0);
    if (!ret) return NULL;
    if (comp_decompress_core(x, cbuf, csize, x->ndims, x->dims, ret, out_bytes, "malloc") != 0) {
        H5free_memory(ret); return NULL;
    }
    *out_nbytes = out_bytes;
    return ret;
}

/* ---------------------------------------------------------------------------
 * H5Z_class3_t callbacks
 * ------------------------------------------------------------------------ */
/* What a grouped dataset's chunk holds when HDF5 allocated it with a fill
 * template (H5D_ALLOC_TIME_EARLY/LATE run the fill chunk through the pipeline
 * once, with no chunk position) and it was never written: a unit that reads
 * as the fill value. */
#define H5Z_COMP_FILL_MAGIC 0x465A3548u   /* "H5ZF" */

static size_t comp_filter_impl(unsigned flags, size_t cd_nelmts, const unsigned cd_values[],
                               hid_t dxpl_id, const hsize_t *scaled, size_t nbytes, size_t *buf_size,
                               void **buf) {
    try {
        comp_ctx *x = comp_ctx_get(cd_values, cd_nelmts, dxpl_id);
        if (!x) return 0;
        if (x->h5z.grouped && !(flags & H5Z_FLAG_REVERSE) && scaled == NULL) {
            void *out = H5allocate_memory(8, 0);
            if (!out) return 0;
            const uint32_t v[2] = { H5Z_COMP_FILL_MAGIC, 1u };
            memcpy(out, v, 8);
            H5free_memory(*buf); *buf = out; *buf_size = 8;
            return 8;
        }
        if (x->h5z.grouped) {
            /* A grouped dataset's chunks hold SEGMENTS of a unit's stream, not
             * one stream each; only the dataset-level callbacks can (de)code
             * them. Fail loudly instead of returning garbage. */
            fprintf(stderr, "[H5Z_comp] this dataset uses \"h5z:group\" (compression units larger "
                    "than a chunk); it can only be %s through an HDF5 with dataset-level filter "
                    "callbacks (H5Z_class3_t dset_write/dset_read), one dataset per call, and "
                    "with no other filter in the pipeline\n",
                    (flags & H5Z_FLAG_REVERSE) ? "read" : "written");
            return 0;
        }
        if (flags & H5Z_FLAG_REVERSE) {
            size_t out_nbytes = 0;
            void *out = comp_decompress(x, *buf, nbytes, &out_nbytes);
            if (!out) return 0;
            H5free_memory(*buf); *buf = out; *buf_size = out_nbytes; return out_nbytes;
        } else {
            size_t csize = 0;
            void *out = comp_compress(x, *buf, nbytes, &csize);
            if (!out || csize == 0) { if (out) H5free_memory(out); return 0; }
            H5free_memory(*buf); *buf = out; *buf_size = csize; return csize;
        }
    } catch (const std::exception &e) { fprintf(stderr, "[H5Z_comp] exception: %s\n", e.what()); return 0; }
      catch (...) { fprintf(stderr, "[H5Z_comp] non-standard exception\n"); return 0; }
}

#ifdef H5ZCOMP_HAVE_FILTER_STATE
/* Later blob layout: filter2 also receives per-dataset state (unused here). */
static size_t H5Z_filter2_comp(unsigned flags, size_t cd_nelmts, const unsigned cd_values[],
                               hid_t dxpl_id, const hsize_t *scaled, size_t ndims, void *state,
                               size_t nbytes, size_t *buf_size, void **buf) {
    (void)ndims; (void)state;
    return comp_filter_impl(flags, cd_nelmts, cd_values, dxpl_id, scaled, nbytes, buf_size, buf);
}
#else
static size_t H5Z_filter2_comp(unsigned flags, size_t cd_nelmts, const unsigned cd_values[],
                               hid_t dxpl_id, const hsize_t *scaled, size_t ndims,
                               size_t nbytes, size_t *buf_size, void **buf) {
    (void)ndims;
    return comp_filter_impl(flags, cd_nelmts, cd_values, dxpl_id, scaled, nbytes, buf_size, buf);
}
#endif

/* can_apply: runs at H5Dcreate. Build the codec once so an unknown compressor
 * or malformed options JSON rejects the dataset up front, matching the VOL.
 * Returns 0 ("cannot apply") so a mandatory filter makes H5Dcreate fail. */
static htri_t H5Z_comp_can_apply(hid_t dcpl_id, hid_t type_id, hid_t space_id) {
    (void)type_id; (void)space_id;
    htri_t ret = 0;
    char *blob_json = NULL;
    comp_cd c; memset(&c, 0, sizeof(c));
    try {
        unsigned flt_flags = 0;
        if (comp_read_config(dcpl_id, &c, &flt_flags) < 0) goto done;

        const char *json = c.json;
        if (c.flags & H5Z_COMP_FLAG_EXTERNAL) {
            blob_json = comp_read_blob_json(dcpl_id);
            if (!blob_json) { fprintf(stderr, "[H5Z_comp] EXTERNAL config but no blob on the DCPL\n"); goto done; }
            if (comp_register_json(blob_json) != c.json) {
                fprintf(stderr, "[H5Z_comp] blob JSON does not match the tag in cd_values\n"); goto done;
            }
            json = blob_json;
        }
        comp_ctx *x = comp_ctx_build(c.id, json, pressio_byte_dtype, 0, NULL);
        if (!x) goto done;   /* message already printed */
        const comp_h5z_cfg h5z = x->h5z;
        const std::string  cid = x->id;
        comp_ctx_free(x);
        if (h5z.grouped || h5z.progressive) {
#ifndef H5ZCOMP_HAVE_DSET_IO
            fprintf(stderr, "[H5Z_comp] \"h5z:group\" / \"h5z:progressive\" need an HDF5 with "
                    "dataset-level filter callbacks (H5Z_class3_t dset_write/dset_read); this "
                    "build of the filter was configured against one without them\n");
            goto done;
#else
            hsize_t hc[H5S_MAX_RANK];
            int rank = H5Pget_chunk(dcpl_id, H5S_MAX_RANK, hc);
            if (h5z.grouped && !h5z.group_all && (int)h5z.group.size() != rank) {
                fprintf(stderr, "[H5Z_comp] \"h5z:group\" has %zu entries but the dataset has %d "
                        "dimensions\n", h5z.group.size(), rank);
                goto done;
            }
            if (h5z.progressive && !comp_progressive_codec(cid)) {
                fprintf(stderr, "[H5Z_comp] \"h5z:progressive\" needs a codec whose stream can be "
                        "truncated (sperr, with the filter built against SPERR); '%s' is not\n",
                        cid.c_str());
                goto done;
            }
            if (H5Pget_nfilters(dcpl_id) != 1) {
                fprintf(stderr, "[H5Z_comp] \"h5z:group\" / \"h5z:progressive\" require H5Zcomp to be "
                        "the only filter in the pipeline\n");
                goto done;
            }
#endif
        }
        ret = 1;
    } catch (...) { ret = -1; }
done:
    comp_cd_free(&c); free(blob_json);
    return ret;
}

static herr_t H5Z_comp_set_local(hid_t dcpl_id, hid_t type_id, hid_t space_id) {
    (void)space_id;
    herr_t ret = -1;
    unsigned *cd2 = NULL;
    comp_cd c; memset(&c, 0, sizeof(c));
    try {
        size_t cap = H5Z_MAX_CD_NELMTS;
        unsigned flt_flags = 0;
        if (comp_read_config(dcpl_id, &c, &flt_flags) < 0) goto done;

        enum pressio_dtype dt = comp_hdf5_to_pressio_dtype(type_id);

        /* The filter sees one CHUNK at a time (edge chunks are padded to full
         * size by HDF5), so the codec shape is the chunk shape. */
        hsize_t hchunk[H5S_MAX_RANK];
        int rank = H5Pget_chunk(dcpl_id, H5S_MAX_RANK, hchunk);
        if (rank <= 0 || rank > H5S_MAX_RANK) {
            fprintf(stderr, "[H5Z_comp] set_local: dataset is not chunked\n");
            goto done;
        }
        size_t dims[H5S_MAX_RANK];
        for (int i = 0; i < rank; i++) {
            if (hchunk[i] > 0xFFFFFFFFull) { fprintf(stderr, "[H5Z_comp] chunk dim too large\n"); goto done; }
            dims[i] = (size_t)hchunk[i];
        }

        size_t n2 = 0;
        cd2 = (unsigned *)malloc(cap * sizeof(unsigned));
        if (!cd2) goto done;
        /* final cd_values: flags (DEFERRED cleared) + dtype + chunk dims + id +
         * inline JSON (the content tag when EXTERNAL). The decode side needs only these. */
        unsigned flags = c.flags & ~H5Z_COMP_FLAG_DEFERRED;
        if (comp_build_cd(flags, (unsigned)dt, (size_t)rank, dims, c.id, c.json, cd2, cap, &n2) < 0) {
            fprintf(stderr, "[H5Z_comp] set_local: config does not fit in cd_values\n");
            goto done;
        }
        if (H5Pmodify_filter(dcpl_id, H5Z_FILTER_COMP, flt_flags, n2, cd2) < 0) goto done;
        ret = 0;
    } catch (...) { ret = -1; }
done:
    comp_cd_free(&c); free(cd2);
    return ret;
}

#ifdef H5ZCOMP_HAVE_DSET_IO
/* ===========================================================================
 * Dataset-level transfers (H5Z_class3_t init/term + dset_write/dset_read)
 *
 * The per-chunk callback above is handed one HOST chunk at a time, usually at
 * H5Dclose, and must return that chunk's bytes. Three things cannot be
 * expressed through it:
 *
 *   native buffers   the application's buffer lives where HDF5 cannot read it
 *                    (GPU memory). Here the buffer reaches the codec untouched,
 *                    in the LibPressio domain named on the DXPL.
 *   decoupling       one compression unit spans several storage chunks
 *                    ("h5z:group"). The unit's stream is striped, in order,
 *                    across its member chunks with H5Dwrite_chunk.
 *   progressive      a read asks for the first pct% of each unit's stream. With
 *                    striping that is simply "read the first k member chunks".
 *
 * HDF5 offers each H5Dwrite/H5Dread to these callbacks before it touches the
 * buffer. They engage only when asked to -- a grouped dataset, or a DXPL
 * carrying H5Pset_comp_native_buffer / _progressive / _direct -- and otherwise
 * return 0, so every existing code path and benchmark is unchanged.
 *
 * File layout. Ungrouped: exactly what the per-chunk path writes (one stream
 * per chunk), so either path reads the other's files. Grouped: every member
 * chunk of a unit holds [comp_seg header][its slice of the unit's stream].
 * No HDF5 format change either way.
 * ========================================================================= */

#define H5Z_COMP_DXPL_NATIVE "H5Zcomp_native"       /* char[H5Z_COMP_DOMAIN_MAX]  */
#define H5Z_COMP_DXPL_PROG   "H5Zcomp_progressive"  /* comp_prog_prop             */
#define H5Z_COMP_DXPL_DIRECT "H5Zcomp_direct"       /* int                        */
#define H5Z_COMP_DOMAIN_MAX  64

struct comp_prog_prop {
    unsigned pct_in, pct_out;     /* inside / outside the region of interest */
    int      has_roi, rank;
    hsize_t  start[H5S_MAX_RANK], count[H5S_MAX_RANK];
};

/* ---- per-dataset state, built by init ----------------------------------- */
struct comp_state {
    bool               valid = false;   /* config recovered; callbacks may engage */
    std::string        id, json;        /* codec id + full options JSON           */
    enum pressio_dtype dtype = pressio_byte_dtype;
    size_t             esize = 0;       /* bytes per dataset element              */
    int                rank  = 0;
    hsize_t            chunk[H5S_MAX_RANK];
    comp_h5z_cfg       h5z;
    comp_ctx          *ctx = NULL;      /* codec, built on first use              */
    ~comp_state() { comp_ctx_free(ctx); }
};

/* The codec is built lazily so that merely opening a dataset (h5ls, h5dump -H)
 * never fails or prints because a codec is missing. */
static comp_ctx *comp_state_ctx(comp_state *st) {
    if (!st->ctx) st->ctx = comp_ctx_build(st->id.c_str(), st->json.c_str(), st->dtype, 0, NULL);
    return st->ctx;
}

static herr_t H5Z_comp_init(hid_t file_id, hid_t dcpl_id, hid_t type_id, hid_t space_id,
                            unsigned idx, void **state_out) {
    (void)file_id;
    comp_state *st = NULL;
    try {
        st = new comp_state();
        comp_cd c; memset(&c, 0, sizeof(c));
        unsigned cd[H5Z_COMP_GET_MAX]; size_t nel = H5Z_COMP_GET_MAX; unsigned flags = 0;
        bool have = false;

        /* cd_values are readable back only up to 256 values (see H5Z_COMP_GET_MAX) */
        if (H5Pget_filter2(dcpl_id, idx, &flags, &nel, cd, 0, NULL, NULL) == (H5Z_filter_t)H5Z_FILTER_COMP &&
            nel <= H5Z_COMP_GET_MAX && comp_parse_cd(cd, nel, &c) == 0 && !(c.flags & H5Z_COMP_FLAG_DEFERRED)) {
            st->id = c.id;
            if (c.flags & H5Z_COMP_FLAG_EXTERNAL) {
                /* Large config: this is where the blob finally reaches the
                 * filter without any help from the application. */
                size_t sz = 0;
                if (H5Pget_filter_blob(dcpl_id, idx, 0, NULL, &sz) >= 0 && sz > 0) {
                    std::string blob(sz, '\0'); size_t cap = sz;
                    if (H5Pget_filter_blob(dcpl_id, idx, 0, &blob[0], &cap) >= 0) {
                        blob.resize(strnlen(blob.c_str(), sz));
                        if (comp_json_tag(blob.c_str()) == c.json) { st->json = blob; have = true; }
                    }
                }
            } else { st->json = c.json; have = true; }
        }
        comp_cd_free(&c);
        if (!have) {   /* mid-size inline config: only the creating DCPL still carries it */
            char *pid = NULL, *pjson = NULL;
            if (comp_get_dcpl_cfg(dcpl_id, &pid, &pjson) == 0) { st->id = pid; st->json = pjson; have = true; }
            free(pid); free(pjson);
        }
        if (have) {
            st->dtype = comp_hdf5_to_pressio_dtype(type_id);
            st->esize = H5Tget_size(type_id);
            /* space_id is the chunk shape. (The DCPL's own layout is not
             * filled in yet when init runs at H5Dopen.) */
            st->rank  = H5Sget_simple_extent_ndims(space_id);
            if (st->rank > 0 && st->rank <= H5S_MAX_RANK) H5Sget_simple_extent_dims(space_id, st->chunk, NULL);
            else st->rank = 0;
            st->valid = st->rank > 0 && st->esize > 0;
            if (st->valid && st->json.find("h5z:") != std::string::npos) {
                struct pressio *lib = pressio_instance();
                struct pressio_options *o = lib ? pressio_options_new_json(lib, st->json.c_str()) : NULL;
                if (!o || comp_parse_h5z(o, &st->h5z) < 0) st->valid = false;
                if (o) pressio_options_free(o);
                if (lib) pressio_release(lib);
            }
        }
    } catch (...) { if (st) st->valid = false; }
    if (getenv("H5ZCOMP_DEBUG"))
        fprintf(stderr, "[H5Z_comp] init: idx=%u valid=%d id='%s' rank=%d esize=%zu grouped=%d progressive=%d json=%zu B\n",
                idx, st ? (int)st->valid : -1, st ? st->id.c_str() : "", st ? st->rank : -1,
                st ? st->esize : 0, st ? (int)st->h5z.grouped : -1, st ? (int)st->h5z.progressive : -1,
                st ? st->json.size() : 0);
    /* Never fail: a dataset whose config init cannot recover still works
     * through the per-chunk path, which receives cd_values directly. */
    *state_out = st;
    return 0;
}

static herr_t H5Z_comp_term(void *state) {
    delete (comp_state *)state;
    return 0;
}

/* ---- small helpers ------------------------------------------------------- */
struct comp_hid {   /* closes an hid_t on scope exit */
    hid_t id; herr_t (*closer)(hid_t);
    comp_hid(hid_t i, herr_t (*c)(hid_t)) : id(i), closer(c) {}
    ~comp_hid() { if (id >= 0 && closer) closer(id); }
    comp_hid(const comp_hid &) = delete; comp_hid &operator=(const comp_hid &) = delete;
};

static bool comp_dxpl_get(hid_t dxpl, const char *name, void *out, size_t size) {
    size_t sz = 0;
    if (dxpl < 0 || H5Pexist(dxpl, name) <= 0) return false;
    if (H5Pget_size(dxpl, name, &sz) < 0 || sz != size) return false;
    return H5Pget(dxpl, name, out) >= 0;
}
static herr_t comp_dxpl_set(hid_t dxpl, const char *name, const void *val, size_t size) {
    if (H5Pexist(dxpl, name) > 0) H5Premove(dxpl, name);
    return H5Pinsert2(dxpl, name, size, (void *)val, NULL, NULL, NULL, NULL, NULL, NULL);
}

static thread_local H5Z_comp_io_stats_t g_io;   /* last dataset-level transfer */

typedef std::vector<std::pair<hsize_t, hsize_t> > comp_runs;   /* (offset, length), in elements */

/* A selection as (offset,length) runs over its dataspace, in selection order,
 * with adjacent runs merged. */
static int comp_sel_runs(hid_t space, comp_runs &runs) {
    runs.clear();
    comp_hid it(H5Ssel_iter_create(space, 1, 0), H5Ssel_iter_close);
    if (it.id < 0) return -1;
    const size_t MAXSEQ = 1024;
    std::vector<hsize_t> off(MAXSEQ); std::vector<size_t> len(MAXSEQ);
    for (;;) {
        size_t nseq = 0, nel = 0;
        if (H5Ssel_iter_get_seq_list(it.id, MAXSEQ, (size_t)-1, &nseq, &nel, off.data(), len.data()) < 0) return -1;
        if (nseq == 0) break;
        for (size_t i = 0; i < nseq; i++) {
            if (!runs.empty() && runs.back().first + runs.back().second == off[i]) runs.back().second += len[i];
            else runs.emplace_back(off[i], (hsize_t)len[i]);
        }
    }
    return 0;
}

/* One compression unit touched by a request. */
struct comp_unit {
    hsize_t   start[H5S_MAX_RANK], count[H5S_MAX_RANK];   /* its box, clipped to the extent */
    hsize_t   cdims[H5S_MAX_RANK];                        /* shape the codec sees           */
    hsize_t   nbox = 0, nsel = 0, ncodec = 0;             /* elements: box / selected / codec */
    comp_runs fruns, mruns;                               /* selected elements: file / memory */
};

/* One request. */
struct comp_req {
    comp_state *st = NULL;
    hid_t       dset = -1;
    bool        write = false, must = false;   /* must: cannot fall back to the per-chunk path */
    bool        grouped = false, native = false, has_prog = false;
    char        domain[H5Z_COMP_DOMAIN_MAX];
    comp_prog_prop prog;
    int         rank = 0;
    hsize_t     ext[H5S_MAX_RANK], unit[H5S_MAX_RANK];
    hid_t       fspace = -1, mspace = -1;
    bool        own_f = false;
    ~comp_req() { if (own_f && fspace >= 0) H5Sclose(fspace); }
};

/* Decide whether to take the request. 1 engage, 0 decline, -1 error. */
static htri_t comp_req_setup(comp_req &r, void *state, hid_t dset_id, hid_t mem_type_id,
                             hid_t mem_space_id, hid_t file_space_id, hid_t dxpl_id, bool write) {
    comp_state *st = (comp_state *)state;
    if (!st || !st->valid) return 0;
    r.st = st; r.dset = dset_id; r.write = write; r.rank = st->rank;
    r.grouped = st->h5z.grouped;

    memset(r.domain, 0, sizeof(r.domain));
    r.native = comp_dxpl_get(dxpl_id, H5Z_COMP_DXPL_NATIVE, r.domain, sizeof(r.domain)) && r.domain[0];
    r.domain[sizeof(r.domain) - 1] = '\0';
    r.has_prog = !write && comp_dxpl_get(dxpl_id, H5Z_COMP_DXPL_PROG, &r.prog, sizeof(r.prog));
    int direct = 0;
    if (!comp_dxpl_get(dxpl_id, H5Z_COMP_DXPL_DIRECT, &direct, sizeof(direct))) {
        const char *e = getenv("H5ZCOMP_DIRECT");
        direct = (e && *e && *e != '0');
    }
    /* A progressive dataset is always WRITTEN here when possible, so that
     * every unit's stream gets the truncatable form. */
    if (!r.grouped && !r.native && !r.has_prog && !direct && !(write && st->h5z.progressive)) return 0;
    /* A grouped dataset has no per-chunk representation, and HDF5 cannot
     * dereference a native buffer: for those, declining is not an option. */
    r.must = r.grouped || r.native;
#define COMP_BAIL(...) do { if (!r.must && !r.has_prog) return 0; \
                            fprintf(stderr, "[H5Z_comp] " __VA_ARGS__); return -1; } while (0)

    if (r.has_prog && !st->h5z.progressive) {
        fprintf(stderr, "[H5Z_comp] progressive read requested, but this dataset was not written "
                "with \"h5z:progressive\":true -- its streams cannot be truncated safely\n");
        return -1;
    }
    {
        comp_hid ft(H5Dget_type(dset_id), H5Tclose);
        if (ft.id < 0 || H5Tequal(mem_type_id, ft.id) <= 0)
            COMP_BAIL("the memory datatype must equal the dataset's datatype for %s\n",
                      r.native ? "a native (device) buffer" : "this dataset");
    }
    {
        comp_hid ds(H5Dget_space(dset_id), H5Sclose);
        if (ds.id < 0 || H5Sget_simple_extent_ndims(ds.id) != r.rank ||
            H5Sget_simple_extent_dims(ds.id, r.ext, NULL) < 0)
            COMP_BAIL("unsupported dataspace\n");
        if (file_space_id == H5S_ALL) { r.fspace = ds.id; ds.id = -1; r.own_f = true; }
        else {
            hsize_t fe[H5S_MAX_RANK];
            if (H5Sget_simple_extent_ndims(file_space_id) != r.rank ||
                H5Sget_simple_extent_dims(file_space_id, fe, NULL) < 0 ||
                memcmp(fe, r.ext, sizeof(hsize_t) * (size_t)r.rank) != 0)
                COMP_BAIL("the file dataspace's extent does not match the dataset\n");
            r.fspace = file_space_id;
        }
    }
    r.mspace = (mem_space_id == H5S_ALL) ? r.fspace : mem_space_id;
    {
        H5S_sel_type ft = H5Sget_select_type(r.fspace), mt = H5Sget_select_type(r.mspace);
        const bool fok = (ft == H5S_SEL_ALL || ft == H5S_SEL_HYPERSLABS || ft == H5S_SEL_NONE);
        const bool mok = (mt == H5S_SEL_ALL || mt == H5S_SEL_HYPERSLABS || mt == H5S_SEL_NONE);
        if (!fok || !mok) COMP_BAIL("only 'all' and hyperslab selections are supported here\n");
        if (H5Sget_select_npoints(r.fspace) != H5Sget_select_npoints(r.mspace))
            COMP_BAIL("file and memory selections differ in size\n");
    }
    for (int d = 0; d < r.rank; d++) {
        if (!r.grouped) { r.unit[d] = st->chunk[d]; continue; }
        const hsize_t g = st->h5z.group_all ? 0 : st->h5z.group[(size_t)d];
        const hsize_t nchunks = (r.ext[d] + st->chunk[d] - 1) / st->chunk[d];
        r.unit[d] = (g == 0 ? (nchunks ? nchunks : 1) : g) * st->chunk[d];
    }
#undef COMP_BAIL
    return 1;
}

/* The units a request touches, each with the file/memory runs of its
 * selected elements. */
static int comp_plan(const comp_req &r, std::vector<comp_unit> &units) {
    units.clear();
    if (H5Sget_select_npoints(r.fspace) == 0) return 0;
    hsize_t lo[H5S_MAX_RANK], hi[H5S_MAX_RANK], idx[H5S_MAX_RANK], ulo[H5S_MAX_RANK], uhi[H5S_MAX_RANK];
    if (H5Sget_select_bounds(r.fspace, lo, hi) < 0) return -1;
    for (int d = 0; d < r.rank; d++) { ulo[d] = lo[d] / r.unit[d]; uhi[d] = hi[d] / r.unit[d]; idx[d] = ulo[d]; }

    for (;;) {
        comp_unit u;
        u.nbox = 1; u.ncodec = 1;
        for (int d = 0; d < r.rank; d++) {
            u.start[d] = idx[d] * r.unit[d];
            u.count[d] = std::min(r.unit[d], r.ext[d] - u.start[d]);
            /* Ungrouped units must decode through the per-chunk path too, which
             * always sees a full (padded) chunk; grouped units are clipped. */
            u.cdims[d] = r.grouped ? u.count[d] : r.unit[d];
            u.nbox *= u.count[d]; u.ncodec *= u.cdims[d];
        }
        {
            comp_hid is(H5Scopy(r.fspace), H5Sclose);
            if (is.id < 0 || H5Sselect_hyperslab(is.id, H5S_SELECT_AND, u.start, NULL, u.count, NULL) < 0) return -1;
            hssize_t n = H5Sget_select_npoints(is.id);
            if (n < 0) return -1;
            if (n > 0) {
                u.nsel = (hsize_t)n;
                comp_hid mp(H5Sselect_project_intersection(r.fspace, r.mspace, is.id), H5Sclose);
                if (mp.id < 0 || comp_sel_runs(is.id, u.fruns) < 0 || comp_sel_runs(mp.id, u.mruns) < 0) return -1;
                units.push_back(std::move(u));
            }
        }
        int d = r.rank - 1;
        for (; d >= 0; d--) { if (idx[d] < uhi[d]) { idx[d]++; break; } idx[d] = ulo[d]; }
        if (d < 0) break;
    }
    return 0;
}

/* True when the unit's selected elements are the whole unit, in order, as one
 * contiguous run of the application's buffer -- so the codec can work on that
 * memory directly, wherever it lives. */
static bool comp_unit_in_place(const comp_unit &u) {
    return u.nsel == u.nbox && u.ncodec == u.nbox && u.mruns.size() == 1 && u.mruns[0].second == u.nbox;
}

/* Walk a unit's selected elements: f(offset in the codec's buffer, offset in
 * the application's buffer, count), all in elements. */
template <class F>
static int comp_walk(const comp_req &r, const comp_unit &u, F f) {
    size_t mi = 0; hsize_t mused = 0;
    const int last = r.rank - 1;
    for (size_t fi = 0; fi < u.fruns.size(); fi++) {
        hsize_t off = u.fruns[fi].first, left = u.fruns[fi].second;
        while (left) {
            hsize_t c[H5S_MAX_RANK], t = off, uoff = 0;
            for (int d = last; d >= 0; d--) { c[d] = t % r.ext[d]; t /= r.ext[d]; }
            for (int d = 0; d <= last; d++) {
                if (c[d] < u.start[d] || c[d] >= u.start[d] + u.count[d]) return -1;
                uoff = uoff * u.cdims[d] + (c[d] - u.start[d]);
            }
            const hsize_t n = std::min(left, u.start[last] + u.count[last] - c[last]);
            for (hsize_t done = 0; done < n;) {
                if (mi >= u.mruns.size()) return -1;
                const hsize_t k = std::min(u.mruns[mi].second - mused, n - done);
                f(uoff + done, u.mruns[mi].first + mused, k);
                done += k; mused += k;
                if (mused == u.mruns[mi].second) { mi++; mused = 0; }
            }
            off += n; left -= n;
        }
    }
    return 0;
}

/* ---- grouped storage: a unit's stream striped across its member chunks ---- */
#define H5Z_COMP_SEG_MAGIC 0x475A3548u   /* "H5ZG" */

struct comp_seg {
    uint32_t seg = 0, nsegs = 0;
    uint64_t stream_len = 0, seg_len = 0, raw_len = 0;
};
static size_t comp_seg_hdr_size(int rank) { return 40 + 16 * (size_t)rank; }
static void comp_put(std::vector<unsigned char> &b, uint64_t v, int n) {
    for (int i = 0; i < n; i++) b.push_back((unsigned char)(v >> (8 * i)));
}
static uint64_t comp_get(const unsigned char *p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void comp_seg_encode(std::vector<unsigned char> &b, const comp_seg &h, int rank, const comp_unit &u) {
    b.clear();
    comp_put(b, H5Z_COMP_SEG_MAGIC, 4); comp_put(b, 1, 2); comp_put(b, (uint64_t)rank, 2);
    comp_put(b, h.seg, 4); comp_put(b, h.nsegs, 4);
    comp_put(b, h.stream_len, 8); comp_put(b, h.seg_len, 8); comp_put(b, h.raw_len, 8);
    for (int d = 0; d < rank; d++) comp_put(b, u.start[d], 8);
    for (int d = 0; d < rank; d++) comp_put(b, u.count[d], 8);
}
/* Decode + check against the unit the reader expects. */
static int comp_seg_decode(const unsigned char *p, size_t n, int rank, const comp_unit &u, uint32_t want_seg,
                           comp_seg *h) {
    if (n < comp_seg_hdr_size(rank) || comp_get(p, 4) != H5Z_COMP_SEG_MAGIC || comp_get(p + 4, 2) != 1 ||
        comp_get(p + 6, 2) != (uint64_t)rank) {
        fprintf(stderr, "[H5Z_comp] storage chunk does not hold a compression-unit segment\n");
        return -1;
    }
    h->seg = (uint32_t)comp_get(p + 8, 4); h->nsegs = (uint32_t)comp_get(p + 12, 4);
    h->stream_len = comp_get(p + 16, 8); h->seg_len = comp_get(p + 24, 8); h->raw_len = comp_get(p + 32, 8);
    for (int d = 0; d < rank; d++)
        if (comp_get(p + 40 + 8 * d, 8) != u.start[d] || comp_get(p + 40 + 8 * (rank + d), 8) != u.count[d]) {
            fprintf(stderr, "[H5Z_comp] compression unit on disk has a different shape than the "
                    "dataset's current extent implies (was the dataset resized after writing?)\n");
            return -1;
        }
    if (h->seg != want_seg || h->nsegs == 0 || h->seg_len == 0) {
        fprintf(stderr, "[H5Z_comp] inconsistent compression-unit segment header\n");
        return -1;
    }
    return 0;
}

/* Element offsets (chunk origins) of a unit's member chunks, in raster order. */
static void comp_unit_members(const comp_req &r, const comp_unit &u, std::vector<std::vector<hsize_t> > &m) {
    m.clear();
    hsize_t n[H5S_MAX_RANK], i[H5S_MAX_RANK];
    for (int d = 0; d < r.rank; d++) { n[d] = (u.count[d] + r.st->chunk[d] - 1) / r.st->chunk[d]; i[d] = 0; }
    for (;;) {
        std::vector<hsize_t> off((size_t)r.rank);
        for (int d = 0; d < r.rank; d++) off[(size_t)d] = u.start[d] + i[d] * r.st->chunk[d];
        m.push_back(off);
        int d = r.rank - 1;
        for (; d >= 0; d--) { if (++i[d] < n[d]) break; i[d] = 0; }
        if (d < 0) break;
    }
}

/* Raw stored bytes of one chunk. 1 read, 0 never written, -1 error. */
static int comp_read_chunk(hid_t dset, const hsize_t *offset, std::vector<unsigned char> &buf) {
    /* (Also flushes a dirty cached copy of the chunk, so the size is current.
     * H5Dget_chunk_storage_size would fail for a chunk that does not exist.) */
    hsize_t stored = 0; haddr_t addr = HADDR_UNDEF; unsigned fmask = 0;
    if (H5Dget_chunk_info_by_coord(dset, offset, &fmask, &addr, &stored) < 0) return -1;
    if (addr == HADDR_UNDEF || stored == 0) { buf.clear(); return 0; }
    buf.resize((size_t)stored);
    uint32_t mask = 0; size_t sz = buf.size();
    if (H5Dread_chunk2(dset, H5P_DEFAULT, offset, &mask, buf.data(), &sz) < 0 || sz != buf.size()) return -1;
    g_io.chunks++; g_io.bytes_io += sz;
    return 1;
}

/* "sperr:chunks" = the unit itself, so the stream is one SPERR chunk and any
 * prefix of it is a valid lower-fidelity stream. Per call: edge units differ. */
static int comp_pin_sperr_chunks(comp_ctx *x, int rank, const hsize_t *cdims) {
    std::vector<uint64_t> v;
    for (int d = rank - 1; d >= 0; d--) if (cdims[d] > 1) v.push_back((uint64_t)cdims[d]);
    if (v.size() != 3) {
        fprintf(stderr, "[H5Z_comp] \"h5z:progressive\" with sperr needs 3-D compression units "
                "(this one has %zu non-unit dimensions)\n", v.size());
        return -1;
    }
    size_t three = 3; int rc = -1;
    struct pressio_options *o = pressio_options_new();
    struct pressio_data *d = pressio_data_new_copy(pressio_uint64_dtype, v.data(), 1, &three);
    if (o && d) {
        pressio_options_set_data(o, "sperr:chunks", d);
        rc = pressio_compressor_set_options(x->compressor, o);
    }
    if (d) pressio_data_free(d);
    if (o) pressio_options_free(o);
    return rc == 0 ? 0 : -1;
}

/* Turn the first `have` bytes of a `total`-byte stream into a decodable
 * stream of at most `have` bytes. Codec specific. */
static int comp_truncate_stream(const std::string &id, const unsigned char *s, size_t have, size_t total,
                                unsigned max_pct, std::vector<unsigned char> &out) {
#ifdef H5ZCOMP_HAVE_SPERR
    if (id == "sperr") {
        /* SPERR re-headers the stream for an integer percentage; take the
         * largest one whose bytes we actually hold. */
        unsigned pct = (unsigned)(((unsigned long long)have * 100ull) / (total ? total : 1));
        if (pct > max_pct) pct = max_pct;
        if (pct > 99) pct = 99;
        for (; pct >= 1; pct--) {
            void *dst = NULL; size_t dlen = 0;
            if (C_API::sperr_trunc_3d(s, have, pct, &dst, &dlen) == 0 && dst) {
                out.assign((unsigned char *)dst, (unsigned char *)dst + dlen);
                free(dst);
                return 0;
            }
            if (dst) free(dst);
        }
        fprintf(stderr, "[H5Z_comp] sperr could not truncate the stream (%zu of %zu B)\n", have, total);
        return -1;
    }
#else
    (void)s; (void)have; (void)total; (void)max_pct; (void)out;
#endif
    fprintf(stderr, "[H5Z_comp] no progressive decode for codec '%s' in this build\n", id.c_str());
    return -1;
}

/* Percentage of a unit's stream this read wants. */
static unsigned comp_unit_pct(const comp_req &r, const comp_unit &u) {
    if (!r.has_prog) return 100;
    if (!r.prog.has_roi) return r.prog.pct_in;
    for (int d = 0; d < r.rank && d < r.prog.rank; d++)
        if (u.start[d] + u.count[d] <= r.prog.start[d] || r.prog.start[d] + r.prog.count[d] <= u.start[d])
            return r.prog.pct_out;
    return r.prog.pct_in;
}

/* ---- dset_write ---------------------------------------------------------- */
static htri_t H5Z_comp_dset_write(void *state, hid_t dset_id, hid_t mem_type_id, hid_t mem_space_id,
                                  hid_t file_space_id, hid_t dxpl_id, const void *buf) {
    try {
        memset(&g_io, 0, sizeof(g_io));
        comp_req r;
        htri_t go = comp_req_setup(r, state, dset_id, mem_type_id, mem_space_id, file_space_id, dxpl_id, true);
        if (go <= 0) return go;
        comp_state *st = r.st;
        const size_t es = st->esize;

        std::vector<comp_unit> units;
        if (comp_plan(r, units) < 0) { fprintf(stderr, "[H5Z_comp] could not map the selection onto compression units\n"); return -1; }

        /* Check everything before writing anything. */
        for (size_t i = 0; i < units.size(); i++) {
            const comp_unit &u = units[i];
            if (u.nsel != u.nbox) {
                /* Part of a unit: that needs read-modify-write. The per-chunk
                 * path does it for ordinary datasets. */
                if (!r.must) return 0;
                fprintf(stderr, "[H5Z_comp] a write to %s must cover whole compression units "
                        "(unit at element offset %llu.. is only partly selected)\n",
                        r.grouped ? "a grouped dataset" : "a dataset from a native buffer",
                        (unsigned long long)u.start[0]);
                return -1;
            }
            if (r.native && !comp_unit_in_place(u)) {
                fprintf(stderr, "[H5Z_comp] native buffer: every compression unit must be one contiguous "
                        "block of the buffer. Use units that span all but the first dimension (and "
                        "chunk sizes that divide the extent), or a host buffer.\n");
                return -1;
            }
        }
        comp_ctx *x = comp_state_ctx(st);
        if (!x) return -1;

        std::vector<unsigned char> scratch, seg, hdr;
        std::vector<std::vector<hsize_t> > members;
        for (size_t i = 0; i < units.size(); i++) {
            const comp_unit &u = units[i];
            const size_t nbytes = (size_t)u.ncodec * es;
            const void  *src;
            const char  *domain = "malloc";

            if (comp_unit_in_place(u)) {
                src = (const char *)buf + (size_t)u.mruns[0].first * es;
                if (r.native) domain = r.domain;
            } else {
                scratch.assign(nbytes, 0);
                if (comp_walk(r, u, [&](hsize_t uo, hsize_t mo, hsize_t n) {
                        memcpy(scratch.data() + (size_t)uo * es, (const char *)buf + (size_t)mo * es, (size_t)n * es);
                    }) < 0) return -1;
                src = scratch.data();
                g_io.bytes_staged += nbytes;
            }
            if (st->h5z.progressive && x->id == "sperr" && comp_pin_sperr_chunks(x, r.rank, u.cdims) < 0) return -1;

            size_t dims[H5S_MAX_RANK];
            for (int d = 0; d < r.rank; d++) dims[d] = (size_t)u.cdims[d];
            size_t len = 0;
            struct pressio_data *out = comp_compress_core(x, src, nbytes, domain, (size_t)r.rank, dims, &len);
            if (!out) return -1;
            const unsigned char *stream = (const unsigned char *)pressio_data_ptr(out, NULL);
            herr_t st_w = 0;

            if (!r.grouped) {
                if (len >= ((size_t)1 << 32)) {
                    fprintf(stderr, "[H5Z_comp] '%s' produced %zu B for one chunk; HDF5 cannot store a filtered "
                            "chunk >= 4 GiB -- use \"h5z:group\" with smaller chunks\n", x->id.c_str(), len);
                    st_w = -1;
                } else st_w = H5Dwrite_chunk(dset_id, H5P_DEFAULT, 0, u.start, len, stream);
                g_io.chunks++; g_io.bytes_io += len; g_io.bytes_stored += len;
            } else {
                /* Stripe the stream over the unit's member chunks. No member
                 * exceeds the 4 GiB chunk limit however large the unit is. */
                comp_unit_members(r, u, members);
                comp_seg h;
                h.nsegs = (uint32_t)members.size();
                h.stream_len = len; h.raw_len = nbytes;
                h.seg_len = (len + h.nsegs - 1) / h.nsegs;
                if (h.seg_len == 0) h.seg_len = 1;
                for (uint32_t k = 0; k < h.nsegs && st_w >= 0; k++) {
                    const uint64_t b = std::min<uint64_t>(len, (uint64_t)k * h.seg_len);
                    const uint64_t e = std::min<uint64_t>(len, b + h.seg_len);
                    h.seg = k;
                    comp_seg_encode(hdr, h, r.rank, u);
                    seg.assign(hdr.begin(), hdr.end());
                    seg.insert(seg.end(), stream + b, stream + e);
                    if (seg.size() >= ((size_t)1 << 32)) {
                        fprintf(stderr, "[H5Z_comp] a segment of %zu B exceeds the 4 GiB chunk limit\n", seg.size());
                        st_w = -1; break;
                    }
                    st_w = H5Dwrite_chunk(dset_id, H5P_DEFAULT, 0, members[k].data(), seg.size(), seg.data());
                    g_io.chunks++; g_io.bytes_io += seg.size(); g_io.bytes_stored += seg.size();
                }
            }
            pressio_data_free(out);
            if (st_w < 0) { fprintf(stderr, "[H5Z_comp] writing a compression unit failed\n"); return -1; }
            g_io.units++; g_io.bytes_raw += (uint64_t)u.nbox * es;
        }
        g_io.handled = 1; g_io.native = r.native ? 1 : 0;
        return 1;
    } catch (const std::exception &e) { fprintf(stderr, "[H5Z_comp] exception: %s\n", e.what()); return -1; }
      catch (...) { fprintf(stderr, "[H5Z_comp] non-standard exception\n"); return -1; }
}

/* ---- dset_read ----------------------------------------------------------- */
static htri_t H5Z_comp_dset_read(void *state, hid_t dset_id, hid_t mem_type_id, hid_t mem_space_id,
                                 hid_t file_space_id, hid_t dxpl_id, void *buf) {
    try {
        memset(&g_io, 0, sizeof(g_io));
        comp_req r;
        htri_t go = comp_req_setup(r, state, dset_id, mem_type_id, mem_space_id, file_space_id, dxpl_id, false);
        if (go <= 0) return go;
        comp_state *st = r.st;
        const size_t es = st->esize;

        std::vector<comp_unit> units;
        if (comp_plan(r, units) < 0) { fprintf(stderr, "[H5Z_comp] could not map the selection onto compression units\n"); return -1; }

        if (r.native)
            for (size_t i = 0; i < units.size(); i++)
                if (!comp_unit_in_place(units[i])) {
                    fprintf(stderr, "[H5Z_comp] native buffer: every compression unit must be read whole "
                            "into one contiguous block of the buffer. Use units that span all but the "
                            "first dimension (and chunk sizes that divide the extent), or a host buffer.\n");
                    return -1;
                }
        comp_ctx *x = comp_state_ctx(st);
        if (!x) return -1;

        std::vector<unsigned char> stream, part, trunc, scratch, fill;
        std::vector<std::vector<hsize_t> > members;
        for (size_t i = 0; i < units.size(); i++) {
            const comp_unit &u = units[i];
            const size_t   nbytes = (size_t)u.ncodec * es;
            const unsigned pct    = comp_unit_pct(r, u);
            size_t have = 0, total = 0;
            bool   missing = false;

            /* 1. fetch the unit's stream, or as much of it as this read wants */
            if (!r.grouped) {
                int rc = comp_read_chunk(dset_id, u.start, stream);
                if (rc < 0) return -1;
                missing = (rc == 0);
                have = total = stream.size();
                g_io.bytes_stored += total;
            } else {
                comp_unit_members(r, u, members);
                int rc = comp_read_chunk(dset_id, members[0].data(), part);
                if (rc < 0) return -1;
                if (rc == 1 && part.size() == 8 && comp_get(part.data(), 4) == H5Z_COMP_FILL_MAGIC) rc = 0;
                if (rc == 0) missing = true;
                else {
                    const size_t hs = comp_seg_hdr_size(r.rank);
                    comp_seg h;
                    if (comp_seg_decode(part.data(), part.size(), r.rank, u, 0, &h) < 0) return -1;
                    if (h.nsegs != members.size() || h.raw_len != nbytes) {
                        fprintf(stderr, "[H5Z_comp] compression unit on disk does not match the dataset's "
                                "\"h5z:group\" layout\n");
                        return -1;
                    }
                    total = (size_t)h.stream_len;
                    /* Whole storage chunks only: the granularity of a
                     * progressive read is one segment of the stream. */
                    uint64_t want = (pct >= 100) ? h.stream_len : (h.stream_len * pct + 99) / 100;
                    uint64_t nseg = (want + h.seg_len - 1) / h.seg_len;
                    if (nseg < 1) nseg = 1;
                    if (nseg > h.nsegs) nseg = h.nsegs;
                    have = (size_t)std::min<uint64_t>(h.stream_len, nseg * h.seg_len);
                    stream.assign(part.begin() + (long)hs, part.end());
                    for (uint32_t k = 1; k < nseg && stream.size() < have; k++) {
                        comp_seg hk;
                        if (comp_read_chunk(dset_id, members[k].data(), part) <= 0 ||
                            comp_seg_decode(part.data(), part.size(), r.rank, u, k, &hk) < 0) {
                            fprintf(stderr, "[H5Z_comp] segment %u of a compression unit is missing or bad\n", k);
                            return -1;
                        }
                        stream.insert(stream.end(), part.begin() + (long)hs, part.end());
                    }
                    if (stream.size() < have) { fprintf(stderr, "[H5Z_comp] compression unit stream is short\n"); return -1; }
                    g_io.bytes_stored += total + (uint64_t)h.nsegs * hs;
                }
            }

            /* 2. where the decoded unit goes */
            const bool  in_place = comp_unit_in_place(u);
            void       *dst;
            const char *domain = "malloc";
            if (in_place) {
                dst = (char *)buf + (size_t)u.mruns[0].first * es;
                if (r.native) domain = r.domain;
            } else {
                scratch.resize(nbytes);
                dst = scratch.data();
                g_io.bytes_staged += nbytes;
            }

            /* 3. decode (or fill a never-written unit) */
            if (missing) {
                if (r.native) { fprintf(stderr, "[H5Z_comp] native buffer: part of the selection was never written\n"); return -1; }
                if (fill.empty()) {
                    fill.assign(es, 0);
                    comp_hid dcpl(H5Dget_create_plist(dset_id), H5Pclose);
                    comp_hid ft(H5Dget_type(dset_id), H5Tclose);
                    H5D_fill_value_t fst = H5D_FILL_VALUE_UNDEFINED;
                    if (dcpl.id >= 0 && ft.id >= 0 && H5Pfill_value_defined(dcpl.id, &fst) >= 0 &&
                        fst != H5D_FILL_VALUE_UNDEFINED)
                        H5Pget_fill_value(dcpl.id, ft.id, fill.data());
                }
                for (size_t e = 0; e < (size_t)u.ncodec; e++) memcpy((char *)dst + e * es, fill.data(), es);
            } else {
                const unsigned char *sp = stream.data();
                size_t               sl = have;
                if (have < total || pct < 100) {
                    if (comp_truncate_stream(x->id, stream.data(), have, total, pct, trunc) < 0) return -1;
                    sp = trunc.data(); sl = trunc.size();
                }
                size_t dims[H5S_MAX_RANK];
                for (int d = 0; d < r.rank; d++) dims[d] = (size_t)u.cdims[d];
                if (comp_decompress_core(x, sp, sl, (size_t)r.rank, dims, dst, nbytes, domain) != 0) return -1;
            }

            /* 4. deliver the selected part */
            if (!in_place &&
                comp_walk(r, u, [&](hsize_t uo, hsize_t mo, hsize_t n) {
                    memcpy((char *)buf + (size_t)mo * es, scratch.data() + (size_t)uo * es, (size_t)n * es);
                }) < 0) return -1;
            g_io.units++; g_io.bytes_raw += (uint64_t)u.nsel * es;
        }
        g_io.handled = 1; g_io.native = r.native ? 1 : 0;
        return 1;
    } catch (const std::exception &e) { fprintf(stderr, "[H5Z_comp] exception: %s\n", e.what()); return -1; }
      catch (...) { fprintf(stderr, "[H5Z_comp] non-standard exception\n"); return -1; }
}
#endif /* H5ZCOMP_HAVE_DSET_IO */

/* ---------------------------------------------------------------------------
 * Registration / public API
 * ------------------------------------------------------------------------ */
/* Filled by name rather than positionally: the H5Z_class3_t layout changed on
 * the blob branch after e77c5db (description moved, init/term inserted). */
static H5Z_class3_t g_comp_class;
static bool         g_comp_class_ready = false;

static const H5Z_class3_t *comp_class(void) {
    if (!g_comp_class_ready) {
        memset(&g_comp_class, 0, sizeof(g_comp_class));
        g_comp_class.version         = H5Z_CLASS3_T_VERS;
        g_comp_class.id              = (H5Z_filter_t)H5Z_FILTER_COMP;
        g_comp_class.encoder_present = 1;
        g_comp_class.decoder_present = 1;
        g_comp_class.name            = "comp";   /* canonical name [A-Za-z0-9_.-] */
        g_comp_class.description     = "LibPressio-backed CPU/GPU compression filter";
        g_comp_class.can_apply       = H5Z_comp_can_apply;
        g_comp_class.set_local       = H5Z_comp_set_local;
        g_comp_class.filter          = H5Z_filter2_comp;
        /* set_config/get_config, write/read/close_blob: NULL => library
         * defaults (the blob goes to the global heap). */
#ifdef H5ZCOMP_HAVE_DSET_IO
        g_comp_class.init            = H5Z_comp_init;
        g_comp_class.term            = H5Z_comp_term;
        g_comp_class.dset_write      = H5Z_comp_dset_write;
        g_comp_class.dset_read       = H5Z_comp_dset_read;
#endif
#ifdef H5ZCOMP_HAVE_LIBPRESSIO_SPERR
        register_libpressio_sperr();   /* keeps the "sperr" plugin linked in and registered */
#endif
        g_comp_class_ready = true;
    }
    return &g_comp_class;
}

extern "C" {

/* Re-registering the same id replaces the class, so this is idempotent. */
herr_t H5Z_register_comp(void) { return H5Zregister(comp_class()); }

/* Attach the filter to a DCPL. Small configs ride in cd_values; a config too
 * big for cd_values is stored in the filter's blob and flagged EXTERNAL --
 * the caller must then hand it to the filter at runtime via H5Pset_comp_dxpl. */
herr_t H5Pset_comp(hid_t dcpl_id, const char *compressor_id, const char *options_json) {
    if (!compressor_id || !compressor_id[0]) return -1;
    const char *json = options_json ? options_json : "";
    const size_t il = strlen(compressor_id), jl = strlen(json);
    size_t cap = H5Z_MAX_CD_NELMTS;
    unsigned *cd = (unsigned *)malloc(cap * sizeof(unsigned));
    if (!cd) return -1;
    size_t n = 0;
    herr_t ret = -1;

    /* A config with "h5z:" keys is consumed by the dataset-level callbacks,
     * which learn it in init() -- and init() can read back cd_values only up
     * to H5Z_COMP_GET_MAX, or else the blob. So such a config goes inline
     * only when small, and to the blob otherwise (never the MID form). */
    const bool needs_init = strstr(json, "h5z:") != NULL;

    /* Inline iff the FINAL cd_values (after set_local adds dtype + up to
     * H5S_MAX_RANK chunk dims) fit under the object-header message cap. */
    if (comp_cd_len(H5S_MAX_RANK, il, jl) <= (needs_init ? H5Z_COMP_GET_MAX : H5Z_COMP_INLINE_MAX_UINTS)) {
        if (comp_cd_len(0, il, jl) <= H5Z_COMP_GET_MAX) {
            /* SMALL: whole config in cd_values now (readable back by set_local) */
            if (comp_build_cd(0u, 0u, 0, NULL, compressor_id, json, cd, cap, &n) < 0) goto done;
        } else {
            /* MID: > 256 cd_values cannot be read back by set_local, so put a
             * stub here and let set_local inline the JSON from the DCPL prop. */
            if (comp_build_cd(H5Z_COMP_FLAG_DEFERRED, 0u, 0, NULL, compressor_id, "", cd, cap, &n) < 0) goto done;
        }
        if (H5Pset_filter(dcpl_id, (H5Z_filter_t)H5Z_FILTER_COMP, H5Z_FLAG_MANDATORY, n, cd) < 0) goto done;
        ret = comp_set_dcpl_cfg(dcpl_id, compressor_id, json);
    } else {
        /* LARGE: H5Pappend_filter_blob appends the pipeline entry itself and
         * attaches the JSON as its blob; then put the id + EXTERNAL flag in
         * that SAME entry's cd_values (H5Pmodify_filter keeps the blob). */
        std::string tag = comp_register_json(json);
        if (comp_build_cd(H5Z_COMP_FLAG_EXTERNAL, 0u, 0, NULL, compressor_id, tag.c_str(), cd, cap, &n) < 0) goto done;
        if (H5Pexist(dcpl_id, H5Z_COMP_DCPL_PROP) > 0) H5Premove(dcpl_id, H5Z_COMP_DCPL_PROP);
        if (H5Pappend_filter_blob(dcpl_id, (H5Z_filter_t)H5Z_FILTER_COMP, H5Z_FLAG_MANDATORY,
                                  json, jl + 1) < 0) goto done;
        ret = H5Pmodify_filter(dcpl_id, (H5Z_filter_t)H5Z_FILTER_COMP, H5Z_FLAG_MANDATORY, n, cd);
    }
done:
    free(cd);
    return ret;
}

/* Hand a large config to the filter for one H5Dwrite/H5Dread. Call on the DXPL
 * you pass to that operation; the JSON is copied, so it need not outlive this. */
herr_t H5Pset_comp_dxpl(hid_t dxpl_id, const char *options_json) {
    if (!options_json) return -1;
    comp_register_json(options_json);
    size_t sz = strlen(options_json) + 1;
    if (H5Pexist(dxpl_id, H5Z_COMP_DXPL_PROP) > 0)
        H5Premove(dxpl_id, H5Z_COMP_DXPL_PROP);
    return H5Pinsert2(dxpl_id, H5Z_COMP_DXPL_PROP, sz, (void *)options_json,
                      NULL, NULL, NULL, NULL, NULL, NULL);
}

/* Pull the persisted large config out of a reopened dataset's DCPL
 * (H5Dget_create_plist). Caller frees *options_json. */
herr_t H5Pget_comp_json(hid_t dcpl_id, char **options_json) {
    if (!options_json) return -1;
    *options_json = comp_read_blob_json(dcpl_id);
    if (*options_json) comp_register_json(*options_json);   /* lets a plain H5Dread decode it */
    return *options_json ? 0 : -1;
}

/* ---- per-transfer settings for the dataset-level callbacks ---------------- */

/* The buffer given to H5Dwrite/H5Dread on this DXPL is memory of LibPressio
 * domain `domain` ("cudamalloc", ...). HDF5 never dereferences it; the codec
 * works on it in place. NULL or "" clears the setting. */
herr_t H5Pset_comp_native_buffer(hid_t dxpl_id, const char *domain) {
#ifdef H5ZCOMP_HAVE_DSET_IO
    char d[H5Z_COMP_DOMAIN_MAX];
    memset(d, 0, sizeof(d));
    if (!domain || !domain[0]) {
        if (H5Pexist(dxpl_id, H5Z_COMP_DXPL_NATIVE) > 0) return H5Premove(dxpl_id, H5Z_COMP_DXPL_NATIVE);
        return 0;
    }
    if (strlen(domain) >= sizeof(d)) return -1;
    strcpy(d, domain);
    return comp_dxpl_set(dxpl_id, H5Z_COMP_DXPL_NATIVE, d, sizeof(d));
#else
    (void)dxpl_id; (void)domain;
    fprintf(stderr, "[H5Z_comp] native buffers need an HDF5 with dataset-level filter callbacks\n");
    return -1;
#endif
}

/* Read only the first `pct`% (1..100) of every compression unit's stream. */
herr_t H5Pset_comp_progressive(hid_t dxpl_id, unsigned pct) {
    return H5Pset_comp_progressive_roi(dxpl_id, 0, NULL, NULL, pct, pct);
}

/* As above, but units intersecting the box [start, start+count) are read at
 * `pct_in`% and all others at `pct_out`%. rank 0 = no box (pct_in everywhere). */
herr_t H5Pset_comp_progressive_roi(hid_t dxpl_id, int rank, const hsize_t *start, const hsize_t *count,
                                   unsigned pct_in, unsigned pct_out) {
#ifdef H5ZCOMP_HAVE_DSET_IO
    comp_prog_prop p;
    memset(&p, 0, sizeof(p));
    if (pct_in < 1 || pct_in > 100 || pct_out < 1 || pct_out > 100) return -1;
    if (rank < 0 || rank > H5S_MAX_RANK || (rank > 0 && (!start || !count))) return -1;
    p.pct_in = pct_in; p.pct_out = pct_out; p.has_roi = rank > 0; p.rank = rank;
    for (int d = 0; d < rank; d++) { p.start[d] = start[d]; p.count[d] = count[d]; }
    return comp_dxpl_set(dxpl_id, H5Z_COMP_DXPL_PROG, &p, sizeof(p));
#else
    (void)dxpl_id; (void)rank; (void)start; (void)count; (void)pct_in; (void)pct_out;
    fprintf(stderr, "[H5Z_comp] progressive reads need an HDF5 with dataset-level filter callbacks\n");
    return -1;
#endif
}

/* Route an ordinary host-buffer transfer through the dataset-level path too
 * (compress straight from / decode straight into the application's buffer,
 * inside the H5Dwrite/H5Dread call). Off by default so existing results do
 * not change; H5ZCOMP_DIRECT=1 in the environment turns it on globally. */
herr_t H5Pset_comp_direct(hid_t dxpl_id, int enable) {
#ifdef H5ZCOMP_HAVE_DSET_IO
    int v = enable ? 1 : 0;
    return comp_dxpl_set(dxpl_id, H5Z_COMP_DXPL_DIRECT, &v, sizeof(v));
#else
    (void)dxpl_id; (void)enable;
    return -1;
#endif
}

/* What the last H5Dwrite/H5Dread on this thread did, if it went through the
 * dataset-level path (stats->handled == 1). */
herr_t H5Zcomp_get_io_stats(H5Z_comp_io_stats_t *stats) {
    if (!stats) return -1;
#ifdef H5ZCOMP_HAVE_DSET_IO
    *stats = g_io;
#else
    memset(stats, 0, sizeof(*stats));
#endif
    return 0;
}

/* 1 if this build has the dataset-level path (native buffers, "h5z:group",
 * progressive reads), 0 if only the per-chunk filter. */
int H5Zcomp_has_dset_io(void) {
#ifdef H5ZCOMP_HAVE_DSET_IO
    return 1;
#else
    return 0;
#endif
}

/* HDF5 plugin entry points */
H5PL_type_t H5PLget_plugin_type(void) { return H5PL_TYPE_FILTER; }
const void *H5PLget_plugin_info(void) { return comp_class(); }

} /* extern "C" */
