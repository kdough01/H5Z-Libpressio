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
#include <libpressio_ext/cpp/pressio.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <exception>
#include <mutex>
#include <unordered_map>
#include <utility>

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
 * Built codec context (+ one-entry per-thread cache)
 * ------------------------------------------------------------------------ */
struct comp_ctx {
    std::string                id;
    enum pressio_dtype         dtype;
    size_t                     ndims;
    size_t                     dims[H5S_MAX_RANK];
    struct pressio            *library;
    struct pressio_compressor *compressor;
};

static void comp_ctx_free(comp_ctx *x) {
    if (!x) return;
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

static void *comp_compress(comp_ctx *x, const void *data, size_t nbytes, size_t *out_csize) {
    struct pressio_data *input = NULL, *output = NULL;
    void *ret = NULL;
    size_t force_bytes = 0;   /* zfp CUDA workaround, see below */
    size_t rdims[H5S_MAX_RANK], byte_dims[1];
    enum pressio_dtype in_dtype; size_t in_ndims; size_t *in_dims;

    if (comp_is_byte_stream(x->id.c_str())) {
        in_dtype = pressio_byte_dtype; in_ndims = 1; byte_dims[0] = nbytes; in_dims = byte_dims;
    } else {
        if (x->ndims == 0) { fprintf(stderr, "[H5Z_comp] no shape for '%s'\n", x->id.c_str()); return NULL; }
        if (comp_logical_nbytes(x) != nbytes) {
            fprintf(stderr, "[H5Z_comp] shape/size mismatch for '%s': dims imply %zu B, chunk is %zu B\n",
                    x->id.c_str(), comp_logical_nbytes(x), nbytes);
            return NULL;
        }
        in_dtype = x->dtype; in_ndims = x->ndims;
        for (size_t i = 0; i < x->ndims; i++) rdims[i] = x->dims[x->ndims - 1 - i];
        in_dims = rdims;
    }

    input  = pressio_data_new_nonowning_domain(in_dtype, (void *)data, in_ndims, in_dims, "malloc");
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
        memcpy(ret, src, sz);
        *out_csize = sz;
    }
done:
    if (input)  pressio_data_free(input);
    if (output) pressio_data_free(output);
    return ret;
}

static void *comp_decompress(comp_ctx *x, const void *cbuf, size_t csize, size_t *out_nbytes) {
    size_t out_bytes = comp_logical_nbytes(x);
    void *ret = H5allocate_memory(out_bytes, 0);
    if (!ret) return NULL;

    if (x->id == "noop") {
        if (csize != out_bytes) {   /* e.g. a >= 4 GiB chunk truncated by HDF5 */
            fprintf(stderr, "[H5Z_comp] noop chunk is %zu B on disk but should be %zu B\n", csize, out_bytes);
            H5free_memory(ret); return NULL;
        }
        memcpy(ret, cbuf, out_bytes); *out_nbytes = out_bytes; return ret;
    }

    struct pressio_data *input = NULL, *output = NULL;
    size_t rdims[H5S_MAX_RANK], byte_dims[1], comp_dims[1];
    enum pressio_dtype out_dtype; size_t out_ndims; size_t *out_dims; int ok = 0;

    if (comp_is_byte_stream(x->id.c_str())) {
        out_dtype = pressio_byte_dtype; out_ndims = 1; byte_dims[0] = out_bytes; out_dims = byte_dims;
    } else {
        out_dtype = x->dtype; out_ndims = x->ndims;
        for (size_t i = 0; i < x->ndims; i++) rdims[i] = x->dims[x->ndims - 1 - i];
        out_dims = rdims;
    }

    comp_dims[0] = csize;
    input  = pressio_data_new_nonowning_domain(pressio_byte_dtype, (void *)cbuf, 1, comp_dims, "malloc");
    output = pressio_data_new_nonowning_domain(out_dtype, ret, out_ndims, out_dims, "malloc");
    if (!input || !output) goto done;

    if (pressio_compressor_decompress(x->compressor, input, output) != 0) {
        fprintf(stderr, "[H5Z_comp] decompress '%s' failed: %s\n",
                x->id.c_str(), pressio_compressor_error_msg(x->compressor));
        goto done;
    }
    comp_make_host_resident(output);
    {
        size_t sz = 0; void *src = pressio_data_ptr(output, &sz);
        if (!src) goto done;
        if (src != ret) memcpy(ret, src, sz < out_bytes ? sz : out_bytes);
    }
    ok = 1;
done:
    if (input)  pressio_data_free(input);
    if (output) pressio_data_free(output);
    if (!ok) { H5free_memory(ret); return NULL; }
    *out_nbytes = out_bytes;
    return ret;
}

/* ---------------------------------------------------------------------------
 * H5Z_class3_t callbacks
 * ------------------------------------------------------------------------ */
static size_t comp_filter_impl(unsigned flags, size_t cd_nelmts, const unsigned cd_values[],
                               hid_t dxpl_id, size_t nbytes, size_t *buf_size, void **buf) {
    try {
        comp_ctx *x = comp_ctx_get(cd_values, cd_nelmts, dxpl_id);
        if (!x) return 0;
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
    (void)scaled; (void)ndims; (void)state;
    return comp_filter_impl(flags, cd_nelmts, cd_values, dxpl_id, nbytes, buf_size, buf);
}
#else
static size_t H5Z_filter2_comp(unsigned flags, size_t cd_nelmts, const unsigned cd_values[],
                               hid_t dxpl_id, const hsize_t *scaled, size_t ndims,
                               size_t nbytes, size_t *buf_size, void **buf) {
    (void)scaled; (void)ndims;
    return comp_filter_impl(flags, cd_nelmts, cd_values, dxpl_id, nbytes, buf_size, buf);
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
        comp_ctx_free(x);
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
        /* set_config/get_config, init/term, write/read/close_blob: NULL =>
         * library defaults (the blob goes to the global heap). */
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

    /* Inline iff the FINAL cd_values (after set_local adds dtype + up to
     * H5S_MAX_RANK chunk dims) fit under the object-header message cap. */
    if (comp_cd_len(H5S_MAX_RANK, il, jl) <= H5Z_COMP_INLINE_MAX_UINTS) {
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

/* HDF5 plugin entry points */
H5PL_type_t H5PLget_plugin_type(void) { return H5PL_TYPE_FILTER; }
const void *H5PLget_plugin_info(void) { return comp_class(); }

} /* extern "C" */
