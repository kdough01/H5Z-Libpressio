/* ============================================================================
 * ex_decoupled_chunks.c  --  compression unit != storage chunk ("h5z:group")
 *
 *     ./ex_decoupled_chunks [codec] [options-json-without-braces]
 *     ./ex_decoupled_chunks sz3 '"pressio:abs":1e-3'
 *
 * The same field is stored four ways:
 *
 *   A  small storage chunks, one stream per chunk     the classic filter: the
 *                                                     ratio pays for the small unit
 *   B  the SAME small storage chunks, "h5z:group":"all"
 *                                                     one compression unit for the
 *                                                     whole dataset, striped over
 *                                                     the chunks
 *   C  the same chunks, "h5z:group":"4,1,1"           a unit of 4 chunks: the
 *                                                     ratio / read-granularity knob
 *   D  one chunk = the whole dataset                  the only lever a filter had
 *                                                     before; capped at 4 GiB
 *
 * B, C and D differ from A only in the options JSON. H5Dcreate / H5Dwrite /
 * H5Dread are untouched, the file is an ordinary chunked dataset (same chunk
 * index, same number of chunks as A), and h5dump reads it with the plugin on
 * HDF5_PLUGIN_PATH.
 *
 * What makes B and C possible is one addition to the blob branch: the filter
 * class may define dset_write/dset_read, and HDF5 offers each H5Dwrite/H5Dread
 * to them before it splits the request into chunks. The filter compresses a
 * unit and stores slices of the stream with H5Dwrite_chunk.
 * ==========================================================================*/
#include "hdf5.h"
#include "H5Zpressio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NZ 64
#define NY 96
#define NX 96
#define CZ 4 /* storage chunk: a 4-plane slab -> 16 storage chunks */
#define FILENAME "ex_decoupled_chunks.h5"

static float *
make_field(void)
{
    float *f = (float *)malloc(sizeof(float) * NZ * NY * NX);
    for (int k = 0; k < NZ; k++)
        for (int j = 0; j < NY; j++)
            for (int i = 0; i < NX; i++)
                f[((size_t)k * NY + j) * NX + i] =
                    (float)(sin(0.07 * i) * cos(0.05 * j) * sin(0.09 * k + 0.3) + 0.002 * (i + j + k));
    return f;
}

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

/* Create + write + reopen + read one variant; print a table row. */
static int
run(hid_t file, const char *name, const char *label, const hsize_t *chunk, const char *codec,
    const char *json, const float *w, float *rd)
{
    const hsize_t dims[3] = {NZ, NY, NX};
    const size_t  raw     = sizeof(float) * NZ * NY * NX;
    hid_t         space   = H5Screate_simple(3, dims, NULL);
    hid_t         dcpl    = H5Pcreate(H5P_DATASET_CREATE);
    hid_t         dset;
    hsize_t       nchunks = 0, stored;
    H5Z_comp_io_stats_t st;

    H5Pset_chunk(dcpl, 3, chunk);
    if (H5Pset_comp(dcpl, codec, json) < 0 ||
        (dset = H5Dcreate2(file, name, H5T_NATIVE_FLOAT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0) {
        fprintf(stderr, "%s: could not create the dataset\n", name);
        return -1;
    }
    /* A perfectly ordinary write. */
    if (H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, w) < 0) {
        fprintf(stderr, "%s: H5Dwrite failed\n", name);
        return -1;
    }
    H5Zcomp_get_io_stats(&st);
    H5Dclose(dset);

    /* Reopen: nothing but the file tells the reader how it was grouped. */
    dset = H5Dopen2(file, name, H5P_DEFAULT);
    memset(rd, 0, raw);
    if (H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rd) < 0) {
        fprintf(stderr, "%s: H5Dread failed\n", name);
        return -1;
    }
    stored = H5Dget_storage_size(dset);
    H5Dget_num_chunks(dset, H5S_ALL, &nchunks);
    printf("  %-34s %8llu %10llu %8llu %9.2fx   %.2e\n", label, (unsigned long long)nchunks,
           (unsigned long long)(st.handled ? st.units : nchunks), (unsigned long long)stored,
           (double)raw / (double)stored, max_err(w, rd, (size_t)NZ * NY * NX));

    H5Dclose(dset);
    H5Pclose(dcpl);
    H5Sclose(space);
    return 0;
}

int
main(int argc, char **argv)
{
    const char   *codec = (argc > 1) ? argv[1] : "sz3";
    const char   *opts  = (argc > 2) ? argv[2] : "\"pressio:abs\":1e-3";
    const hsize_t small[3] = {CZ, NY, NX}, whole[3] = {NZ, NY, NX};
    char          plain[512], all[512], four[512];
    float        *w  = make_field();
    float        *rd = (float *)malloc(sizeof(float) * NZ * NY * NX);
    int           rc = 0;

    if (H5Z_register_comp() < 0) {
        fprintf(stderr, "register failed\n");
        return 1;
    }
    if (!H5Zcomp_has_dset_io()) {
        printf("SKIP: this build has no dataset-level filter callbacks (\"h5z:group\" unavailable)\n");
        return 77;
    }

    /* The grouping is part of the filter's own config -- no new HDF5 property. */
    snprintf(plain, sizeof(plain), "{%s}", opts);
    snprintf(all, sizeof(all), "{%s,\"h5z:group\":\"all\"}", opts);
    snprintf(four, sizeof(four), "{%s,\"h5z:group\":\"4,1,1\"}", opts);

    hid_t file = H5Fcreate(FILENAME, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);

    printf("codec '%s' %s on a %dx%dx%d float field (%zu B raw)\n\n", codec, plain, NZ, NY, NX,
           sizeof(float) * NZ * NY * NX);
    printf("  %-34s %8s %10s %8s %10s   %s\n", "layout", "storage", "compress.", "stored", "ratio", "max err");
    printf("  %-34s %8s %10s %8s\n", "", "chunks", "units", "bytes");
    rc |= run(file, "A_per_chunk", "A  4-plane chunks, per chunk", small, codec, plain, w, rd);
    rc |= run(file, "B_group_all", "B  4-plane chunks, group=all", small, codec, all, w, rd);
    rc |= run(file, "C_group_4", "C  4-plane chunks, group=4,1,1", small, codec, four, w, rd);
    rc |= run(file, "D_one_chunk", "D  one chunk (whole dataset)", whole, codec, plain, w, rd);
    if (rc)
        return 1;

    /* A partial read of a grouped dataset: any hyperslab works. The filter
     * decodes only the units the selection touches (1 of 4 here). */
    {
        const hsize_t       start[3] = {20, 10, 30}, count[3] = {8, 40, 50};
        float              *part     = (float *)malloc(sizeof(float) * 8 * 40 * 50);
        hid_t               dset     = H5Dopen2(file, "C_group_4", H5P_DEFAULT);
        hid_t               fsp      = H5Dget_space(dset);
        hid_t               msp      = H5Screate_simple(3, count, NULL);
        H5Z_comp_io_stats_t st;
        double              m = 0;

        H5Sselect_hyperslab(fsp, H5S_SELECT_SET, start, NULL, count, NULL);
        if (H5Dread(dset, H5T_NATIVE_FLOAT, msp, fsp, H5P_DEFAULT, part) < 0) {
            fprintf(stderr, "hyperslab read failed\n");
            return 1;
        }
        H5Zcomp_get_io_stats(&st);
        for (int k = 0; k < 8; k++)
            for (int j = 0; j < 40; j++)
                for (int i = 0; i < 50; i++) {
                    double e = fabs((double)part[(k * 40 + j) * 50 + i] -
                                    (double)w[((size_t)(k + 20) * NY + (j + 10)) * NX + (i + 30)]);
                    if (!(e <= m))
                        m = e;
                }
        printf("\nhyperslab [20:28, 10:50, 30:80] of C: decoded %llu of 4 units, read %llu of 16 storage "
               "chunks, max err %.2e\n",
               st.units, st.chunks, m);
        rc |= !(m < 1.0);
        H5Sclose(msp);
        H5Sclose(fsp);
        H5Dclose(dset);
        free(part);
    }

    H5Fclose(file);
    free(w);
    free(rd);
    printf("\n%s\n", rc ? "FAILED" : "OK  (try: HDF5_PLUGIN_PATH=<build>/lib h5dump -H -p " FILENAME ")");
    return rc ? 1 : 0;
}
