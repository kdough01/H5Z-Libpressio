/* ============================================================================
 * test_plugin_read.c  --  "can a normal HDF5 reader open it?"
 *
 * Deliberately NOT linked against libH5Zcomp or LibPressio: the only way this
 * program can decode the datasets is HDF5 finding the plugin on
 * HDF5_PLUGIN_PATH (ctest sets it to the build's lib/ dir). This is the
 * interoperability property the filter has and the VOL does not -- the
 * filter-side analogue of the VOL repo's tier2_h5repack_roundtrip job.
 * ==========================================================================*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hdf5.h"

#define FILTER_ID 33010

int main(int argc, char **argv) {
    const char *fname = argc > 1 ? argv[1] : "plugin_fixture.h5";
    const char *pp = getenv("HDF5_PLUGIN_PATH");
    printf("HDF5_PLUGIN_PATH=%s\n", pp ? pp : "(unset)");

    hid_t fid = H5Fopen(fname, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (fid < 0) { fprintf(stderr, "cannot open %s (run plugin_fixture_write first)\n", fname); return 1; }

    const size_t n = 16 * 24 * 32;
    double *ref = (double *)malloc(n * sizeof(double));
    double *buf = (double *)malloc(n * sizeof(double));
    hid_t d = H5Dopen2(fid, "reference", H5P_DEFAULT);
    H5Dread(d, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, ref);
    H5Dclose(d);

    struct { const char *ds; double tol; } c[] = { { "noop", 0.0 }, { "bzip2", 0.0 }, { "sz3", 1e-3 },
                                           { "mid_noop", 0.0 } /* 32 KiB inline config */ };
    int fails = 0, ran = 0;
    for (size_t k = 0; k < sizeof(c) / sizeof(c[0]); k++) {
        if (H5Lexists(fid, c[k].ds, H5P_DEFAULT) <= 0) { printf("  SKIP: %s not in fixture\n", c[k].ds); continue; }
        ran++;
        d = H5Dopen2(fid, c[k].ds, H5P_DEFAULT);
        memset(buf, 0, n * sizeof(double));
        if (d < 0 || H5Dread(d, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) < 0) {
            printf("  FAIL: %s could not be decoded via the plugin path\n", c[k].ds);
            fails++;
        } else {
            double mx = 0.0;
            for (size_t i = 0; i < n; i++) { double e = fabs(ref[i] - buf[i]); if (e > mx) mx = e; }
            if (mx <= c[k].tol) printf("  PASS: %s decoded by dynamically loaded plugin (maxae=%.3e)\n", c[k].ds, mx);
            else { printf("  FAIL: %s maxae=%.3e > %.1e\n", c[k].ds, mx, c[k].tol); fails++; }
        }
        if (d >= 0) H5Dclose(d);
    }
    /* Large-config dataset: a reader that never calls H5Pget_comp_json cannot
     * know the blob config -- this must fail CLEANLY (error, not crash). This
     * is the documented transparency limit of the DXPL/registry route. */
    if (H5Lexists(fid, "blob_noop", H5P_DEFAULT) > 0) {
        d = H5Dopen2(fid, "blob_noop", H5P_DEFAULT);
        H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
        herr_t r = (d >= 0) ? H5Dread(d, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) : -1;
        H5Eset_auto2(H5E_DEFAULT, (H5E_auto2_t)H5Eprint2, stderr);
        if (d >= 0) H5Dclose(d);
        if (r < 0) printf("  PASS: blob_noop (large config) refused cleanly without H5Pget_comp_json\n");
        else { printf("  FAIL: blob_noop decoded without its config?\n"); fails++; }
    }
    if (H5Zfilter_avail(FILTER_ID) > 0) printf("  PASS: filter %d is now registered (loaded on demand)\n", FILTER_ID);
    else { printf("  FAIL: filter %d still not available\n", FILTER_ID); fails++; }

    H5Fclose(fid); free(ref); free(buf);
    printf("----- test_plugin_read: %s (%d run, %d failed) -----\n", fails ? "FAILED" : "PASSED", ran, fails);
    return fails ? 1 : 0;
}
