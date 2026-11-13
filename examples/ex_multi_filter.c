/* ============================================================================
 * ex_multi_filter.c  --  H5Zcomp together with other filters in one pipeline
 *
 *     ./ex_multi_filter [codec] [options-json-without-braces]
 *     ./ex_multi_filter sz3 '"pressio:abs":1e-3'
 *
 * A dataset-level filter replaces the per-chunk pipeline for the requests it
 * handles, so it runs the dataset's OTHER filters itself, once per
 * compression unit, through H5Zapply_pipeline(): the filters before it on the
 * unit's data, the filters after it on the unit's compressed stream (and the
 * reverse on read). The whole pipeline therefore runs per unit, not per
 * storage chunk.
 *
 *   A  H5Zcomp ("h5z:group":"all") -> deflate -> fletcher32
 *        one unit striped over 16 storage chunks; deflate and the checksum
 *        run once, on the unit's compressed stream
 *   B  H5Zcomp ("h5z:group":"all") alone, for comparison
 *   C  H5Zcomp -> deflate -> fletcher32, one unit per chunk, written through
 *        the dataset-level path (H5Pset_comp_direct)
 *   D  the same as C, written through HDF5's ordinary per-chunk pipeline
 *
 * C and D must store identical bytes in every chunk: when a unit is one chunk,
 * "per unit" IS "per chunk", so such datasets stay readable by any HDF5.
 *
 * Filters BEFORE H5Zcomp (shuffle) run on the unit's data the same way. Only
 * size-preserving ones can precede it, and with a lossy codec they rarely
 * make sense: shuffle reorders the bytes of each float, so the codec would no
 * longer see floats.
 *
 * Two combinations are refused, with a message:
 *   - a native (device) buffer when a filter precedes H5Zcomp: those filters
 *     run on the host;
 *   - "h5z:progressive" when a filter follows H5Zcomp: it rewrites the codec's
 *     stream, so a prefix of what is stored is no longer a decodable prefix.
 * ==========================================================================*/
#include "hdf5.h"
#include "H5Zpressio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NZ       64
#define NY       96
#define NX       96
#define CZ       4 /* storage chunk: a 4-plane slab -> 16 storage chunks */
#define FILENAME "ex_multi_filter.h5"

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

/* Create a dataset whose pipeline is [shuffle] -> H5Zcomp -> [deflate -> fletcher32]. */
static hid_t
create(hid_t file, const char *name, const char *codec, const char *json, int shuffle, int after)
{
    const hsize_t dims[3] = {NZ, NY, NX}, chunk[3] = {CZ, NY, NX};
    hid_t         space = H5Screate_simple(3, dims, NULL), dcpl = H5Pcreate(H5P_DATASET_CREATE), dset;

    H5Pset_chunk(dcpl, 3, chunk);
    if (shuffle)
        H5Pset_shuffle(dcpl); /* before H5Zcomp */
    if (H5Pset_comp(dcpl, codec, json) < 0) {
        fprintf(stderr, "%s: H5Pset_comp failed\n", name);
        return H5I_INVALID_HID;
    }
    if (after) {
        H5Pset_deflate(dcpl, 6); /* after H5Zcomp: on its stream */
        H5Pset_fletcher32(dcpl);
    }
    dset = H5Dcreate2(file, name, H5T_NATIVE_FLOAT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
    H5Pclose(dcpl);
    H5Sclose(space);
    return dset;
}

/* Write through DXPL, reopen, read back with a plain H5Dread, print a row. */
static int
run(hid_t file, const char *name, const char *label, const char *codec, const char *json, int after,
    hid_t dxpl, const float *w, float *rd)
{
    const size_t        raw = sizeof(float) * NZ * NY * NX;
    hid_t               dset;
    H5Z_comp_io_stats_t st;
    hsize_t             stored, nchunks = 0;
    double              err;

    if ((dset = create(file, name, codec, json, 0, after)) < 0) {
        fprintf(stderr, "%s: could not create the dataset\n", name);
        return -1;
    }
    if (H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, w) < 0) {
        fprintf(stderr, "%s: H5Dwrite failed\n", name);
        return -1;
    }
    H5Zcomp_get_io_stats(&st);
    H5Dclose(dset);

    dset = H5Dopen2(file, name, H5P_DEFAULT);
    memset(rd, 0, raw);
    if (H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rd) < 0) {
        fprintf(stderr, "%s: H5Dread failed\n", name);
        return -1;
    }
    stored = H5Dget_storage_size(dset);
    H5Dget_num_chunks(dset, H5S_ALL, &nchunks);
    err = max_err(w, rd, (size_t)NZ * NY * NX);
    printf("  %-44s %-14s %6llu %10llu %8.2fx   %.2e\n", label, st.handled ? "dataset-level" : "per-chunk",
           (unsigned long long)nchunks, (unsigned long long)stored, (double)raw / (double)stored, err);
    H5Dclose(dset);
    return err < 1.0 ? 0 : -1;
}

/* 1 if every chunk of the two datasets holds the same bytes and filter mask. */
static int
same_chunks(hid_t file, const char *a, const char *b)
{
    hid_t   da = H5Dopen2(file, a, H5P_DEFAULT), db = H5Dopen2(file, b, H5P_DEFAULT);
    hsize_t n = 0;
    int     same = 1;

    H5Dget_num_chunks(da, H5S_ALL, &n);
    for (hsize_t c = 0; c < n && same; c++) {
        hsize_t  off[3], sa = 0, sb = 0;
        unsigned ma = 0, mb = 0;
        haddr_t  aa, ab;

        H5Dget_chunk_info(da, H5S_ALL, c, off, &ma, &aa, &sa);
        if (H5Dget_chunk_info_by_coord(db, off, &mb, &ab, &sb) < 0 || sa != sb || ma != mb) {
            same = 0;
            break;
        }
        {
            unsigned char *ba = (unsigned char *)malloc((size_t)sa), *bb = (unsigned char *)malloc((size_t)sb);
            uint32_t       fa = 0, fb = 0;
            size_t         la = (size_t)sa, lb = (size_t)sb;

            if (H5Dread_chunk2(da, H5P_DEFAULT, off, &fa, ba, &la) < 0 ||
                H5Dread_chunk2(db, H5P_DEFAULT, off, &fb, bb, &lb) < 0 || la != lb || memcmp(ba, bb, la) != 0)
                same = 0;
            free(ba);
            free(bb);
        }
    }
    H5Dclose(da);
    H5Dclose(db);
    return same;
}

int
main(int argc, char **argv)
{
    const char *codec = (argc > 1) ? argv[1] : "sz3";
    const char *opts  = (argc > 2) ? argv[2] : "\"pressio:abs\":1e-3";
    char        plain[512], all[512];
    float      *w  = make_field();
    float      *rd = (float *)malloc(sizeof(float) * NZ * NY * NX);
    hid_t       direct, file;
    int         rc = 0, identical;

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
    direct = H5Pcreate(H5P_DATASET_XFER);
    H5Pset_comp_direct(direct, 1);

    file = H5Fcreate(FILENAME, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    printf("codec '%s' %s on a %dx%dx%d float field (%zu B raw), 16 storage chunks of 4 planes\n\n", codec,
           plain, NZ, NY, NX, sizeof(float) * NZ * NY * NX);
    printf("  %-44s %-14s %6s %10s %9s   %s\n", "pipeline", "written by", "chunks", "stored B", "ratio",
           "max err");
    rc |= run(file, "A", "A  H5Zcomp(group=all) -> deflate -> fletcher32", codec, all, 1, H5P_DEFAULT, w, rd);
    rc |= run(file, "B", "B  H5Zcomp(group=all)", codec, all, 0, H5P_DEFAULT, w, rd);
    rc |= run(file, "C", "C  H5Zcomp -> deflate -> fletcher32", codec, plain, 1, direct, w, rd);
    rc |= run(file, "D", "D  H5Zcomp -> deflate -> fletcher32", codec, plain, 1, H5P_DEFAULT, w, rd);
    if (rc)
        return 1;

    identical = same_chunks(file, "C", "D");
    printf("\nC and D store %s bytes in every chunk (one unit per chunk: per unit == per chunk)\n",
           identical ? "identical" : "DIFFERENT");
    rc |= !identical;

    /* A native buffer cannot be combined with a filter before H5Zcomp. */
    {
        hid_t  dset = create(file, "E", codec, all, 1, 0), nat = H5Pcreate(H5P_DATASET_XFER);
        herr_t st;

        H5Pset_comp_native_buffer(nat, "malloc");
        H5E_BEGIN_TRY
        {
            st = H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, nat, w);
        }
        H5E_END_TRY
        printf("native buffer + shuffle before H5Zcomp: %s\n", st < 0 ? "refused (expected)" : "ACCEPTED");
        rc |= !(st < 0);
        H5Pclose(nat);
        H5Dclose(dset);
    }

    H5Pclose(direct);
    H5Fclose(file);
    free(w);
    free(rd);
    printf("\n%s\n", rc ? "FAILED" : "OK");
    return rc ? 1 : 0;
}
