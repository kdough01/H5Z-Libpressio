/* ============================================================================
 * test_cpu_write.c  --  port of the VOL repo's test_cpu_write.c
 *
 * Same seven datasets (bzip2 l5/l9, sz3 abs 1e-3/1e-6, zfp accuracy/rate,
 * default noop) on the same tiny 5x4 float field, written into ONE file and
 * read back. The VOL version only printed values; this one also asserts:
 * lossless => bit-exact, error-bounded => within bound, rate mode => finite.
 * ==========================================================================*/
#include "test_common.h"

#define NROW 5
#define NCOL 4
#define NEL  (NROW * NCOL)

typedef struct {
    const char *name;
    const char *id;       /* NULL => default (noop) */
    const char *json;
    float       scale;    /* buf[i] = i * scale     */
    double      tol;      /* <0 => rate mode (no bound, just finite) */
} case_t;

static const case_t CASES[] = {
    { "bzip2_l5",     "bzip2", "{\"bzip2:block_size\": 5}", 0.5f, 0.0 },
    { "bzip2_l9",     "bzip2", "{\"bzip2:block_size\": 9}", 1.5f, 0.0 },
    { "sz3_abs1e3",   "sz3",   "{\"sz3:error_bound_mode_str\": \"abs\", \"sz3:abs_error_bound\": 1e-3}", 0.5f, 1e-3 },
    { "sz3_abs1e6",   "sz3",   "{\"sz3:error_bound_mode_str\": \"abs\", \"sz3:abs_error_bound\": 1e-6}", 0.5f, 1e-6 },
    { "zfp_acc1e3",   "zfp",   "{\"zfp:accuracy\": 1e-3}", 0.5f, 1e-3 },
    { "zfp_rate8",    "zfp",   "{\"zfp:rate\": 8.0, \"zfp:type\": 3, \"zfp:dims\": 2, \"zfp:wra\": 0}", 0.5f, -1.0 },
    { "default_data", NULL,    NULL,                        2.0f, 0.0 },
};
#define N_CASES (int)(sizeof(CASES) / sizeof(CASES[0]))

int main(void) {
    printf("----- H5Zcomp small-write test -----\n");
    h5zc_init();

    hsize_t dims[2] = { NROW, NCOL };
    int     written[N_CASES] = { 0 };
    float   buf[NEL];

    hid_t fid = H5Fcreate("test_cpu_write.h5", H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hid_t sid = H5Screate_simple(2, dims, NULL);
    if (fid < 0 || sid < 0) { fprintf(stderr, "file/space create failed\n"); return 1; }

    /* ---- writes ---- */
    for (int k = 0; k < N_CASES; k++) {
        const case_t *c = &CASES[k];
        if (c->id && !h5zc_have_codec(c->id)) { H5ZC_SKIPMSG("%s: '%s' not in LibPressio", c->name, c->id); continue; }

        hid_t dcpl = h5zc_make_dcpl(c->id, c->json, 2, dims);
        hid_t dset = H5Dcreate2(fid, c->name, H5T_NATIVE_FLOAT, sid, H5P_DEFAULT, dcpl, H5P_DEFAULT);
        if (dset < 0) { H5ZC_FAIL("%s: H5Dcreate2", c->name); H5Pclose(dcpl); continue; }
        for (int i = 0; i < NEL; i++) buf[i] = (float)i * c->scale;
        if (H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) < 0)
            H5ZC_FAIL("%s: H5Dwrite", c->name);
        else
            written[k] = 1;
        H5Dclose(dset); H5Pclose(dcpl);
    }
    H5Sclose(sid);
    H5Fclose(fid);

    /* ---- read back all (fresh open, so decode uses only what is in the file) ---- */
    fid = H5Fopen("test_cpu_write.h5", H5F_ACC_RDONLY, H5P_DEFAULT);
    for (int k = 0; k < N_CASES; k++) {
        const case_t *c = &CASES[k];
        if (!written[k]) continue;
        float rbuf[NEL], ref[NEL];
        memset(rbuf, 0, sizeof(rbuf));
        for (int i = 0; i < NEL; i++) ref[i] = (float)i * c->scale;

        hid_t dset = H5Dopen2(fid, c->name, H5P_DEFAULT);
        if (dset < 0 || H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rbuf) < 0) {
            H5ZC_FAIL("%s: read", c->name);
            if (dset >= 0) H5Dclose(dset);
            continue;
        }
        H5Dclose(dset);

        printf("%s: ", c->name);
        for (int i = 0; i < NEL; i++) printf("%.4f ", rbuf[i]);
        printf("\n");

        Stats st = compute_stats_f(ref, rbuf, NEL);
        if (c->tol < 0.0) {
            if (isfinite(st.maxae)) H5ZC_PASS("%s: rate mode, maxae=%.3e (not gated)", c->name, st.maxae);
            else                    H5ZC_FAIL("%s: non-finite output", c->name);
        } else if (st.maxae <= c->tol) {
            H5ZC_PASS("%s: maxae=%.3e <= %.1e", c->name, st.maxae, c->tol);
        } else {
            H5ZC_FAIL("%s: maxae=%.3e > %.1e", c->name, st.maxae, c->tol);
        }
    }
    H5Fclose(fid);
    remove("test_cpu_write.h5");
    return h5zc_summary("test_cpu_write");
}
