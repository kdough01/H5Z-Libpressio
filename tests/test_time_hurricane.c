/* ============================================================================
 * test_time_hurricane.c  --  port of the VOL repo's GPU Hurricane test
 *
 * Hurricane-ISABEL CLOUDf01 (100x500x500 float32) through the GPU codecs the
 * VOL test used (nvcomp, cusz, cuszp) plus the uncompressed reference, timed
 * per write/read. In the filter every one of these pays the per-chunk
 * host->device and device->host copies (see examples/NOTES.md section 2), so
 * compare these times against the VOL's devres/rtrip arms, not hostsim.
 *
 * GPU codecs your LibPressio lacks are skipped. Exit 77 when the data is not
 * reachable.
 *
 *   test_time_hurricane [codec]     e.g. test_time_hurricane cuszp
 *
 * With a codec argument only that case (plus the reference) runs. ctest runs
 * each GPU codec in its OWN process: a CUDA fault such as "illegal memory
 * access" is sticky and makes every later CUDA call in the process fail, so
 * one bad codec would otherwise show up as failures of all the others.
 * ==========================================================================*/
#include "test_common.h"

#define NX    100
#define NY    500
#define NZ    500
#define NELEM ((size_t)NX * NY * NZ)

typedef struct {
    const char *dset_name;
    const char *compressor;
    const char *opts_json;
    double      bound;   /* 0 => lossless */
} test_case_t;

static const test_case_t TEST_CASES[] = {
    { "Pf48_nvcomp",    "nvcomp", NULL,                                                   0.0  },
    { "Pf48_cusz",      "cusz",   "{\"pressio:abs\": 1e-3}",                              1e-3 },
    { "Pf48_cuszp",     "cuszp",  "{\"pressio:abs\": 1e-3, \"cuszp:mode_str\": \"outlier\"}", 1e-3 },
    { "Pf48_reference", NULL,     NULL,                                                   0.0  },
};
#define N_TEST_CASES (int)(sizeof(TEST_CASES) / sizeof(TEST_CASES[0]))

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : NULL;
    struct timespec t0, t1;
    printf("GPU Hurricane test (H5Zcomp filter) starting\n");

    char path[512];
    snprintf(path, sizeof(path), "%s/CLOUDf01.bin", HURRICANE_PATH);
    if (access(path, R_OK) != 0) {
        printf("%s not reachable -- skipping\n", path);
        return H5ZC_SKIP;
    }
    size_t nelem = 0;
    float *field = (float *)h5zc_read_raw(path, sizeof(float), &nelem);
    if (!field || nelem != NELEM) { fprintf(stderr, "bad Hurricane field (%zu elems)\n", nelem); return 1; }
    printf("Loaded %s (%zu floats)\n", path, nelem);
    {
        size_t nonfin = 0;
        for (size_t i = 0; i < nelem; i++) if (!isfinite(field[i])) nonfin++;
        if (nonfin) printf("NOTE: %zu non-finite values (NaN/Inf) in this nonclean field; "
                           "stats below skip them\n", nonfin);
    }

    h5zc_init();
    hid_t file_id = H5Fcreate("hurricane_gpu_h5zcomp.h5", H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hsize_t dims[3] = { NX, NY, NZ };
    hid_t space_id = H5Screate_simple(3, dims, NULL);
    int written[N_TEST_CASES] = { 0 };

    printf("\n--- Writes ---\n");
    for (int i = 0; i < N_TEST_CASES; i++) {
        const test_case_t *tc = &TEST_CASES[i];
        const char *cid = tc->compressor ? tc->compressor : DEFAULT_COMPRESSOR;
        if (only && tc->compressor && strcmp(only, tc->compressor) != 0) continue;
        if (!h5zc_have_codec(cid)) { H5ZC_SKIPMSG("%s: '%s' not in LibPressio", tc->dset_name, cid); continue; }

        hid_t dcpl = h5zc_make_dcpl(tc->compressor, tc->opts_json, 3, dims);
        hid_t dset = H5Dcreate2(file_id, tc->dset_name, H5T_NATIVE_FLOAT, space_id,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT);
        if (dset < 0) { H5ZC_FAIL("H5Dcreate2 %s", tc->dset_name); H5Pclose(dcpl); continue; }
        clock_gettime(CLOCK_MONOTONIC, &t0);
        herr_t ret = H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, field);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        hsize_t stored = H5Dget_storage_size(dset);
        printf("  Write %-20s  compressor=%-10s  ret=%2d  time=%.3f ms  ratio=%.2fx\n",
               tc->dset_name, cid, (int)ret, elapsed_ms(t0, t1),
               stored ? (double)(NELEM * sizeof(float)) / (double)stored : 0.0);
        if (ret < 0) H5ZC_FAIL("H5Dwrite %s", tc->dset_name); else written[i] = 1;
        H5Dclose(dset); H5Pclose(dcpl);
    }

    printf("\n--- Reads ---\n");
    float *rbuf = (float *)malloc(NELEM * sizeof(float));
    for (int i = 0; i < N_TEST_CASES; i++) {
        const test_case_t *tc = &TEST_CASES[i];
        if (!written[i]) continue;
        hid_t dset = H5Dopen2(file_id, tc->dset_name, H5P_DEFAULT);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        herr_t ret = H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rbuf);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        H5Dclose(dset);
        printf("  Read  %-20s  ret=%2d  time=%.3f ms\n", tc->dset_name, (int)ret, elapsed_ms(t0, t1));
        if (ret < 0) { H5ZC_FAIL("H5Dread %s", tc->dset_name); continue; }
        printf("  First 10 values [%s]:\n", tc->dset_name);
        for (int j = 0; j < 10; j++)
            printf("    [%d] orig=%.6e  decomp=%.6e  diff=%.2e\n", j, field[j], rbuf[j], field[j] - rbuf[j]);
        Stats st = compute_stats_f(field, rbuf, NELEM);
        if (tc->bound == 0.0 && st.maxae != 0.0) H5ZC_FAIL("%s lossless but maxae=%.3e", tc->dset_name, st.maxae);
        else if (tc->bound > 0.0 && st.maxae > 2.0 * tc->bound)
            H5ZC_FAIL("%s maxae=%.3e > 2x %.1e", tc->dset_name, st.maxae, tc->bound);
        else H5ZC_PASS("%s maxae=%.3e RMSE=%.3e (finite points; %zu non-finite skipped)",
                       tc->dset_name, st.maxae, st.rmse, st.nonfinite);
        fflush(stdout);
    }
    free(rbuf);
    H5Sclose(space_id);
    H5Fclose(file_id);
    free(field);
    remove("hurricane_gpu_h5zcomp.h5");
    return h5zc_summary("test_time_hurricane");
}
