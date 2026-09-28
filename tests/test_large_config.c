/* ============================================================================
 * test_large_config.c  --  filter-only: configs larger than cd_values
 *
 * No VOL equivalent (the VOL keeps its options in its own container). Checks
 * the blob + DXPL path end to end:
 *   - small config stays inline (no blob), large config goes to the blob
 *   - a 32 KiB config (past the 256-value getter limit, below the blob
 *     threshold) round-trips with a plain H5Dread
 *   - the filter is in the pipeline exactly once on the large path
 *   - the blob survives H5Dcreate -> file -> H5Dopen byte-for-byte
 *   - write+read with H5Pset_comp_dxpl round-trips (lossless noop, multi-chunk)
 *   - the same with a real codec (sz3 if available) whose options are padded
 *     past the ceiling, so the bound is actually enforced from the blob config
 * ==========================================================================*/
#include "test_common.h"

#define NX 64
#define NY 48

static char *padded_json(const char *opts, size_t bytes) {
    char head[512];
    snprintf(head, sizeof(head), "{%s%s\"_pad\":\"", opts, opts[0] ? "," : "");
    const char *tail = "\"}";
    size_t h = strlen(head), t = strlen(tail), pad = bytes > h + t ? bytes - h - t : 1;
    char *s = (char *)malloc(h + pad + t + 1);
    memcpy(s, head, h); memset(s + h, 'x', pad); memcpy(s + h + pad, tail, t);
    s[h + pad + t] = '\0';
    return s;
}

static size_t blob_size(hid_t dcpl) {
    size_t sz = 0;
    int nf = H5Pget_nfilters(dcpl);
    for (int i = 0; i < nf; i++) {
        unsigned fl; size_t n = 0; unsigned fc;
        if (H5Pget_filter2(dcpl, (unsigned)i, &fl, &n, NULL, 0, NULL, &fc) == H5Z_FILTER_COMP) {
            H5Pget_filter_blob(dcpl, (unsigned)i, 0, NULL, &sz);
            return sz;
        }
    }
    return 0;
}

static void test_routing(void) {
    hsize_t dims[2] = { NX, NY };
    char *small = padded_json("\"pressio:abs\":1e-3", 1024);
    char *big   = padded_json("\"pressio:abs\":1e-3", 512u * 1024u);

    hid_t d1 = h5zc_make_dcpl_n("noop", small, 2, dims, 1);
    hid_t d2 = h5zc_make_dcpl_n("noop", big, 2, dims, 1);
    if (d1 < 0 || d2 < 0) { H5ZC_FAIL("[routing] H5Pset_comp failed"); goto out; }

    if (blob_size(d1) == 0) H5ZC_PASS("[routing] 1 KiB config stays inline (no blob)");
    else                    H5ZC_FAIL("[routing] 1 KiB config unexpectedly went to the blob");

    if (blob_size(d2) == strlen(big) + 1) H5ZC_PASS("[routing] 512 KiB config stored in blob (%zu B)", strlen(big) + 1);
    else                                  H5ZC_FAIL("[routing] 512 KiB config: blob size %zu != %zu", blob_size(d2), strlen(big) + 1);

    if (H5Pget_nfilters(d2) == 1) H5ZC_PASS("[routing] large path adds the filter exactly once");
    else                          H5ZC_FAIL("[routing] large path pipeline has %d filters (want 1)", H5Pget_nfilters(d2));
out:
    if (d1 >= 0) H5Pclose(d1);
    if (d2 >= 0) H5Pclose(d2);
    free(small); free(big);
}

static void test_blob_roundtrip(const char *codec, const char *opts, double tol) {
    hsize_t dims[2] = { NX, NY };
    const size_t n = (size_t)NX * NY;
    char *big = padded_json(opts, 400u * 1024u);
    double *w = (double *)malloc(n * sizeof(double));
    double *r = (double *)calloc(n, sizeof(double));
    h5zc_fill_synthetic(w, 2, dims);
    const char *fname = "test_large_config.h5";
    int ok = 0;

    hid_t fid = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hid_t sid = H5Screate_simple(2, dims, NULL);
    hid_t dcpl = h5zc_make_dcpl_n(codec, big, 2, dims, 4);
    hid_t did = H5Dcreate2(fid, "data", H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, dcpl, H5P_DEFAULT);
    hid_t wx = H5Pcreate(H5P_DATASET_XFER);
    H5Pset_comp_dxpl(wx, big);
    if (did < 0 || H5Dwrite(did, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, wx, w) < 0) {
        H5ZC_FAIL("[blob/%s] create/write failed", codec);
        if (did >= 0) H5Dclose(did);
        H5Pclose(wx); H5Pclose(dcpl); H5Sclose(sid); H5Fclose(fid);
        goto out;
    }
    H5Pclose(wx); H5Dclose(did); H5Pclose(dcpl); H5Sclose(sid); H5Fclose(fid);

    fid = H5Fopen(fname, H5F_ACC_RDONLY, H5P_DEFAULT);
    did = H5Dopen2(fid, "data", H5P_DEFAULT);
    {
        hid_t pl = H5Dget_create_plist(did);
        char *cfg = NULL;
        if (H5Pget_comp_json(pl, &cfg) < 0 || !cfg)
            H5ZC_FAIL("[blob/%s] H5Pget_comp_json found no stored config", codec);
        else if (strcmp(cfg, big) != 0)
            H5ZC_FAIL("[blob/%s] recovered config differs from what was written", codec);
        else {
            H5ZC_PASS("[blob/%s] blob recovered from file byte-for-byte (%zu B)", codec, strlen(cfg));
            hid_t rx = H5Pcreate(H5P_DATASET_XFER);
            H5Pset_comp_dxpl(rx, cfg);
            if (H5Dread(did, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, rx, r) >= 0) ok = 1;
            H5Pclose(rx);
        }
        free(cfg);
        H5Pclose(pl);
    }
    H5Dclose(did); H5Fclose(fid);

    if (!ok) H5ZC_FAIL("[blob/%s] read with DXPL config failed", codec);
    else {
        Stats st = compute_stats(w, r, n);
        if (st.maxae <= tol) H5ZC_PASS("[blob/%s] round-trip maxae=%.3e (tol %.1e)", codec, st.maxae, tol);
        else                 H5ZC_FAIL("[blob/%s] round-trip maxae=%.3e > tol %.1e", codec, st.maxae, tol);
    }
out:
    remove(fname);
    free(big); free(w); free(r);
}

/* 1 KiB .. ~60 KiB: too big for H5Pget_filter_by_id2 (256 cd_values) but still
 * inline. Goes through the DEFERRED stub + DCPL property, ends up fully in
 * cd_values, and must decode with a plain H5Dread -- no blob, no DXPL. */
static void test_mid_inline(const char *codec, const char *opts, double tol) {
    hsize_t dims[2] = { NX, NY };
    const size_t n = (size_t)NX * NY;
    char *mid = padded_json(opts, 32u * 1024u);
    double *w = (double *)malloc(n * sizeof(double));
    double *r = (double *)calloc(n, sizeof(double));
    h5zc_fill_synthetic(w, 2, dims);
    hsize_t stored = 0;

    hid_t probe = h5zc_make_dcpl_n(codec, mid, 2, dims, 4);
    size_t bs = probe >= 0 ? blob_size(probe) : 1;
    if (probe >= 0) H5Pclose(probe);
    if (bs != 0) { H5ZC_FAIL("[mid/%s] 32 KiB config went to the blob", codec); goto out; }

    if (h5zc_roundtrip("test_mid_config.h5", "data", codec, mid, H5T_NATIVE_DOUBLE, 2, dims, 4,
                       w, r, H5P_DEFAULT, H5P_DEFAULT, &stored) != 0) {
        H5ZC_FAIL("[mid/%s] 32 KiB inline config round-trip failed", codec);
    } else {
        Stats st = compute_stats(w, r, n);
        if (st.maxae <= tol) H5ZC_PASS("[mid/%s] 32 KiB inline config, plain H5Dread, maxae=%.3e", codec, st.maxae);
        else                 H5ZC_FAIL("[mid/%s] maxae=%.3e > tol %.1e", codec, st.maxae, tol);
    }
out:
    remove("test_mid_config.h5");
    free(mid); free(w); free(r);
}

int main(void) {
    printf("----- H5Zcomp large-config (blob + DXPL) tests -----\n");
    h5zc_init();
    test_routing();
    test_mid_inline("noop", "", 0.0);
    if (h5zc_have_codec("sz3"))
        test_mid_inline("sz3", "\"sz3:error_bound_mode_str\":\"abs\",\"sz3:abs_error_bound\":1e-4", 1e-4);
    test_blob_roundtrip("noop", "", 0.0);
    if (h5zc_have_codec("sz3"))
        test_blob_roundtrip("sz3", "\"sz3:error_bound_mode_str\":\"abs\",\"sz3:abs_error_bound\":1e-4", 1e-4);
    else
        H5ZC_SKIPMSG("[blob/sz3] sz3 not in LibPressio");
    return h5zc_summary("test_large_config");
}
