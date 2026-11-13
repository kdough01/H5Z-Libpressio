/* ============================================================================
 * ex_multi_dataset.c  --  one H5Dwrite_multi / H5Dread_multi over datasets
 *                         that do and do not use the dataset-level path
 *
 *     ./ex_multi_dataset [codec] [options-json-without-braces]
 *     ./ex_multi_dataset sz3 '"pressio:abs":1e-3'
 *
 * Three datasets in one request:
 *
 *   grouped    H5Zcomp, "h5z:group":"all"   -> handled by H5Zcomp's dset_write
 *   per_chunk  H5Zcomp, one unit per chunk  -> declined: HDF5's per-chunk path
 *   raw        no filter                    -> HDF5's normal path
 *
 * HDF5 offers each dataset of the request to its own dataset-level filter.
 * The datasets a filter handles are done; the rest go through the normal path
 * together, in one call. Nothing is written twice and the application makes
 * one call, exactly as it would without H5Zcomp.
 * ==========================================================================*/
#include "hdf5.h"
#include "H5Zpressio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NZ       32
#define NY       64
#define NX       64
#define CZ       4
#define NDSET    3
#define FILENAME "ex_multi_dataset.h5"

static double
max_err(const float *a, const float *b, size_t n)
{
    double m = 0;
    for (size_t i = 0; i < n; i++) {
        double e = fabs((double)a[i] - (double)b[i]);
        if (!(e <= m))
            m = e;
    }
    return m;
}

int
main(int argc, char **argv)
{
    const char   *codec = (argc > 1) ? argv[1] : "sz3";
    const char   *opts  = (argc > 2) ? argv[2] : "\"pressio:abs\":1e-3";
    const char   *names[NDSET] = {"grouped", "per_chunk", "raw"};
    const hsize_t dims[3] = {NZ, NY, NX}, chunk[3] = {CZ, NY, NX};
    const size_t  n = (size_t)NZ * NY * NX;
    char          plain[512], all[512];
    hid_t         file, space, ds[NDSET], mt[NDSET], ms[NDSET], fs[NDSET];
    float        *w[NDSET], *r[NDSET];
    const void   *wb[NDSET];
    void         *rb[NDSET];
    int           rc = 0;

    if (H5Z_register_comp() < 0) {
        fprintf(stderr, "register failed\n");
        return 1;
    }
    if (!H5Zcomp_has_dset_io()) {
        printf("SKIP: this build has no dataset-level filter callbacks\n");
        return 77;
    }
    snprintf(plain, sizeof(plain), "{%s}", opts);
    snprintf(all, sizeof(all), "{%s,\"h5z:group\":\"all\"}", opts);

    file  = H5Fcreate(FILENAME, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    space = H5Screate_simple(3, dims, NULL);
    for (int d = 0; d < NDSET; d++) {
        hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);

        H5Pset_chunk(dcpl, 3, chunk);
        if (d < 2 && H5Pset_comp(dcpl, codec, d == 0 ? all : plain) < 0)
            return 1;
        if ((ds[d] = H5Dcreate2(file, names[d], H5T_NATIVE_FLOAT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0)
            return 1;
        H5Pclose(dcpl);

        w[d] = (float *)malloc(sizeof(float) * n);
        r[d] = (float *)calloc(n, sizeof(float));
        for (size_t i = 0; i < n; i++)
            w[d][i] = (float)(sin(0.03 * (double)i + d) + 0.1 * d);
        mt[d] = H5T_NATIVE_FLOAT;
        ms[d] = H5S_ALL;
        fs[d] = H5S_ALL;
        wb[d] = w[d];
        rb[d] = r[d];
    }

    /* One call for all three datasets, as an application would make it. */
    if (H5Dwrite_multi(NDSET, ds, mt, ms, fs, H5P_DEFAULT, wb) < 0) {
        fprintf(stderr, "H5Dwrite_multi failed\n");
        return 1;
    }
    /* Reopen, so the reads decode what was stored, not cached chunks. */
    for (int d = 0; d < NDSET; d++) {
        H5Dclose(ds[d]);
        ds[d] = H5Dopen2(file, names[d], H5P_DEFAULT);
    }
    if (H5Dread_multi(NDSET, ds, mt, ms, fs, H5P_DEFAULT, rb) < 0) {
        fprintf(stderr, "H5Dread_multi failed\n");
        return 1;
    }

    printf("codec '%s' %s, %dx%dx%d float per dataset (%zu B raw), one H5Dwrite_multi + one H5Dread_multi\n\n",
           codec, plain, NZ, NY, NX, sizeof(float) * n);
    printf("  %-10s %-30s %10s %8s   %s\n", "dataset", "pipeline / path", "stored B", "ratio", "max err");
    for (int d = 0; d < NDSET; d++) {
        hsize_t     stored = H5Dget_storage_size(ds[d]);
        double      e      = max_err(w[d], r[d], n);
        const char *how    = d == 0 ? "H5Zcomp group=all: dataset-level"
                             : d == 1 ? "H5Zcomp per chunk: normal path"
                                      : "no filter: normal path";

        printf("  %-10s %-30s %10llu %7.2fx   %.2e\n", names[d], how, (unsigned long long)stored,
               (double)(sizeof(float) * n) / (double)stored, e);
        rc |= !(e < 1.0);
        if (d == 2)
            rc |= (e != 0.0); /* unfiltered data comes back exactly */
    }

    for (int d = 0; d < NDSET; d++) {
        H5Dclose(ds[d]);
        free(w[d]);
        free(r[d]);
    }
    H5Sclose(space);
    H5Fclose(file);
    printf("\n%s\n", rc ? "FAILED" : "OK");
    return rc ? 1 : 0;
}
