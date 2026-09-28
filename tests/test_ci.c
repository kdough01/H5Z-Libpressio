/* ============================================================================
 * test_ci.c  --  fast, data-free CI suite (port of the VOL repo's test_ci.c)
 *
 * VOL original: noop + bzip2 round-trips on a 32^3 double field, plus "an
 * unknown compressor is rejected". Here the same checks go through the filter,
 * and because the filter's unit of work is a CHUNK, every codec is also run
 * with several chunk layouts -- including a ragged one whose edge chunks HDF5
 * pads, which is the case the old set_local got wrong.
 * ==========================================================================*/
#include "test_common.h"

#define NX 32
#define NY 32
#define NZ 32

typedef struct {
    const char *id;
    const char *json;
    double      tol;      /* max abs error allowed; 0 => bit-exact */
} codec_case_t;

static const codec_case_t CODECS[] = {
    { "noop",  NULL,                                                    0.0  },
    { "bzip2", NULL,                                                    0.0  },
    { "sz3",   "{\"sz3:error_bound_mode_str\":\"abs\",\"sz3:abs_error_bound\":1e-3}", 1e-3 },
    { "zfp",   "{\"zfp:accuracy\":1e-3}",                               1e-3 },
};
#define N_CODECS (int)(sizeof(CODECS) / sizeof(CODECS[0]))

typedef struct { const char *name; hsize_t dims[3]; int chunk_n; } layout_t;

static const layout_t LAYOUTS[] = {
    { "1chunk",  { NX, NY, NZ }, 1 },   /* whole dataset = one chunk (VOL-like) */
    { "8chunks", { NX, NY, NZ }, 8 },   /* even split                          */
    { "ragged",  { 30, NY, NZ }, 4 },   /* 30 rows / 4 -> chunk 8, last chunk padded */
};
#define N_LAYOUTS (int)(sizeof(LAYOUTS) / sizeof(LAYOUTS[0]))

static void test_roundtrip(const codec_case_t *cc, const layout_t *lo) {
    size_t n = (size_t)lo->dims[0] * lo->dims[1] * lo->dims[2];
    double *w = (double *)malloc(n * sizeof(double));
    double *r = (double *)calloc(n, sizeof(double));
    char fname[256], ds[64];
    hsize_t stored = 0;

    snprintf(fname, sizeof(fname), "test_ci_%s_%s.h5", cc->id, lo->name);
    snprintf(ds, sizeof(ds), "data");
    h5zc_fill_synthetic(w, 3, lo->dims);

    if (h5zc_roundtrip(fname, ds, cc->id, cc->json, H5T_NATIVE_DOUBLE, 3, lo->dims,
                       lo->chunk_n, w, r, H5P_DEFAULT, H5P_DEFAULT, &stored) != 0) {
        H5ZC_FAIL("[%s/%s] round-trip failed", cc->id, lo->name);
    } else {
        Stats st = compute_stats(w, r, n);
        double ratio = stored ? (double)(n * sizeof(double)) / (double)stored : 0.0;
        if (st.maxae > cc->tol)
            H5ZC_FAIL("[%s/%s] max abs error %.3e > tol %.3e", cc->id, lo->name, st.maxae, cc->tol);
        else
            H5ZC_PASS("[%s/%s] max abs error %.3e (tol %.1e), ratio %.2fx",
                      cc->id, lo->name, st.maxae, cc->tol, ratio);
    }
    remove(fname);
    free(w); free(r);
}

/* VOL: H5Dcreate2 must reject an unknown compressor. The filter now does the
 * same thing through can_apply. */
static void test_bad_compressor(void) {
    hsize_t dims[3] = { NX, NY, NZ };
    hid_t fid = H5Fcreate("test_ci_bad.h5", H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hid_t sid = H5Screate_simple(3, dims, NULL);
    hid_t dcpl = h5zc_make_dcpl_n("does_not_exist", NULL, 3, dims, 1);

    h5zc_quiet(1);
    hid_t did = (dcpl >= 0)
        ? H5Dcreate2(fid, "data", H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, dcpl, H5P_DEFAULT)
        : H5I_INVALID_HID;
    h5zc_quiet(0);

    if (dcpl < 0)
        H5ZC_PASS("[bad_compressor] H5Pset_comp rejected unknown compressor");
    else if (did < 0)
        H5ZC_PASS("[bad_compressor] H5Dcreate2 rejected unknown compressor");
    else {
        H5ZC_FAIL("[bad_compressor] H5Dcreate2 accepted an unknown compressor");
        H5Dclose(did);
    }
    if (dcpl >= 0) H5Pclose(dcpl);
    H5Sclose(sid); H5Fclose(fid);
    remove("test_ci_bad.h5");
}

/* Filter-specific: the DCPL must carry the filter exactly once, and the chunk
 * dims set_local records must be the CHUNK, not the dataset extent. */
static void test_pipeline_shape(void) {
    hsize_t dims[3] = { NX, NY, NZ };
    hid_t fid = H5Fcreate("test_ci_pipe.h5", H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hid_t sid = H5Screate_simple(3, dims, NULL);
    hid_t dcpl = h5zc_make_dcpl_n("noop", NULL, 3, dims, 8);
    hid_t did = H5Dcreate2(fid, "data", H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, dcpl, H5P_DEFAULT);
    hid_t pl = H5Dget_create_plist(did);

    int nf = H5Pget_nfilters(pl);
    if (nf != 1) H5ZC_FAIL("[pipeline] expected 1 filter in pipeline, found %d", nf);
    else         H5ZC_PASS("[pipeline] filter appears exactly once");

    unsigned cd[64]; size_t ncd = 64; unsigned fl = 0;
    if (H5Pget_filter_by_id2(pl, H5Z_FILTER_COMP, &fl, &ncd, cd, 0, NULL, NULL) < 0) {
        H5ZC_FAIL("[pipeline] H5Pget_filter_by_id2 failed");
    } else {
        /* cd[4] = rank, cd[5..] = chunk dims (see H5Z_comp.cc) */
        hsize_t chunk[3];
        H5Pget_chunk(pl, 3, chunk);
        if (ncd >= 8 && cd[4] == 3 && cd[5] == chunk[0] && cd[6] == chunk[1] && cd[7] == chunk[2])
            H5ZC_PASS("[pipeline] set_local recorded chunk dims %ux%ux%u", cd[5], cd[6], cd[7]);
        else
            H5ZC_FAIL("[pipeline] set_local dims [%u: %u %u %u] != chunk %llux%llux%llu",
                      cd[4], cd[5], cd[6], cd[7], (unsigned long long)chunk[0],
                      (unsigned long long)chunk[1], (unsigned long long)chunk[2]);
    }
    H5Pclose(pl); H5Dclose(did); H5Pclose(dcpl); H5Sclose(sid); H5Fclose(fid);
    remove("test_ci_pipe.h5");
}

int main(void) {
    printf("----- H5Zcomp Filter Tests (CI) -----\n");
    h5zc_init();

    for (int c = 0; c < N_CODECS; c++) {
        if (!h5zc_have_codec(CODECS[c].id)) {
            H5ZC_SKIPMSG("[%s] not in this LibPressio build", CODECS[c].id);
            continue;
        }
        for (int l = 0; l < N_LAYOUTS; l++) test_roundtrip(&CODECS[c], &LAYOUTS[l]);
    }
    test_bad_compressor();
    test_pipeline_shape();

    return h5zc_summary("test_ci");
}
