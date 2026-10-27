/* ============================================================================
 * ex_progressive.c  --  progressive (reduced-fidelity) reads with SPERR
 *
 *     ./ex_progressive [tolerance]          # default 1e-4 (sperr pwe mode)
 *
 * SPERR's stream is embedded: a prefix of it decodes to a coarser version of
 * the same data. A per-chunk filter cannot use that -- HDF5 reads the whole
 * stored chunk before the filter is called, and the chunk cache would hand
 * the next reader whatever fidelity happened to be decoded first.
 *
 * Decoupling the compression unit from the storage chunk is what makes it
 * work. The dataset is written with
 *
 *   "h5z:group":"all"        one SPERR stream for the whole volume ...
 *   "h5z:progressive":true   ... written as a single SPERR chunk, so that any
 *                            prefix of it is decodable
 *
 * on 32 small storage chunks. The filter stripes the stream over those chunks
 * in order, so "the first p% of the stream" is "the first p% of the storage
 * chunks", and
 *
 *   H5Pset_comp_progressive(dxpl, 10);
 *   H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, buf);
 *
 * reads 4 of the 32 chunks from the file and decodes a full-size volume from
 * them. Nothing is cached, so the next H5Dread can ask for a different
 * percentage.
 *
 * Part 2 uses units of 8 chunks ("h5z:group":"8,1,1" -> 4 units) to read a
 * region of interest at full fidelity and the rest at 10%, in one H5Dread --
 * something a single SPERR truncation percentage cannot express.
 * ==========================================================================*/
#include "hdf5.h"
#include "H5Zpressio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NZ 128
#define NY 128
#define NX 128
#define CZ 4 /* storage chunk = a 4-plane slab -> 32 storage chunks */
#define N  ((size_t)NZ * NY * NX)
#define FILENAME "ex_progressive.h5"

static float *
make_field(void)
{
    float *f = (float *)malloc(sizeof(float) * N);
    for (int k = 0; k < NZ; k++)
        for (int j = 0; j < NY; j++)
            for (int i = 0; i < NX; i++) {
                double x = i / (double)NX, y = j / (double)NY, z = k / (double)NZ;
                f[((size_t)k * NY + j) * NX + i] =
                    (float)(sin(9 * x) * cos(7 * y) * sin(5 * z + 0.3) + 0.25 * sin(40 * x * y) * cos(31 * z) +
                            0.05 * sin(97 * x + 61 * y + 43 * z));
            }
    return f;
}

/* RMSE and max error over the planes [k0, k1). */
static void
errors(const float *a, const float *b, int k0, int k1, double *rmse, double *maxe)
{
    double s = 0, m = 0;
    for (size_t i = (size_t)k0 * NY * NX; i < (size_t)k1 * NY * NX; i++) {
        double e = fabs((double)a[i] - (double)b[i]);
        s += e * e;
        if (!(e <= m))
            m = e;
    }
    *rmse = sqrt(s / (double)((size_t)(k1 - k0) * NY * NX));
    *maxe = m;
}

static hid_t
create(hid_t file, const char *name, const char *json)
{
    const hsize_t dims[3] = {NZ, NY, NX}, chunk[3] = {CZ, NY, NX};
    hid_t         space = H5Screate_simple(3, dims, NULL);
    hid_t         dcpl  = H5Pcreate(H5P_DATASET_CREATE);
    hid_t         dset  = H5I_INVALID_HID;

    H5Pset_chunk(dcpl, 3, chunk);
    if (H5Pset_comp(dcpl, "sperr", json) >= 0)
        dset = H5Dcreate2(file, name, H5T_NATIVE_FLOAT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
    H5Pclose(dcpl);
    H5Sclose(space);
    return dset;
}

int
main(int argc, char **argv)
{
    const double tol = (argc > 1) ? atof(argv[1]) : 1e-4;
    const int    pcts[] = {5, 10, 25, 50, 75, 100};
    float       *w = make_field(), *rd = (float *)malloc(sizeof(float) * N);
    char         json[512];
    double       prev = 1e300;
    int          ok   = 1;

    if (H5Z_register_comp() < 0) {
        fprintf(stderr, "register failed\n");
        return 1;
    }
    if (!H5Zcomp_has_dset_io()) {
        printf("SKIP: this build has no dataset-level filter callbacks (progressive reads unavailable)\n");
        return 77;
    }

    hid_t file = H5Fcreate(FILENAME, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hid_t dxpl = H5Pcreate(H5P_DATASET_XFER);

    /* ---------------- Part 1: one unit, uniform percentage ---------------- */
    snprintf(json, sizeof(json),
             "{\"sperr:mode_str\":\"pwe\",\"sperr:tolerance\":%g,"
             "\"h5z:group\":\"all\",\"h5z:progressive\":true}",
             tol);
    hid_t dset = create(file, "volume", json);
    if (dset < 0) {
        printf("SKIP: could not create a sperr dataset (is the LibPressio sperr plugin loaded, and "
               "was the filter built with SPERR?)\n");
        return 77;
    }
    if (H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, w) < 0) {
        fprintf(stderr, "H5Dwrite failed\n");
        return 1;
    }
    H5Dclose(dset);

    dset = H5Dopen2(file, "volume", H5P_DEFAULT);
    printf("sperr pwe=%g, %dx%dx%d float (%zu B raw), stored %llu B in 32 storage chunks, 1 compression unit\n\n",
           tol, NZ, NY, NX, sizeof(float) * N, (unsigned long long)H5Dget_storage_size(dset));
    printf("  %5s %10s %12s %10s %12s %12s\n", "pct", "chunks", "bytes read", "of stored", "rmse", "max err");
    for (size_t p = 0; p < sizeof(pcts) / sizeof(pcts[0]); p++) {
        H5Z_comp_io_stats_t st;
        double              rmse, maxe;

        H5Pset_comp_progressive(dxpl, (unsigned)pcts[p]);
        memset(rd, 0, sizeof(float) * N);
        if (H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, rd) < 0) {
            fprintf(stderr, "progressive H5Dread (%d%%) failed\n", pcts[p]);
            return 1;
        }
        H5Zcomp_get_io_stats(&st);
        errors(w, rd, 0, NZ, &rmse, &maxe);
        printf("  %4d%% %7llu/32 %12llu %9.1f%% %12.4e %12.4e\n", pcts[p], st.chunks, st.bytes_io,
               100.0 * (double)st.bytes_io / (double)st.bytes_stored, rmse, maxe);
        /* More of the stream must never make the result worse. */
        if (!isfinite(rmse) || rmse > prev * 1.0001)
            ok = 0;
        prev = rmse;
        if (pcts[p] == 100 && maxe > 2 * tol)
            ok = 0;
    }
    H5Dclose(dset);

    /* ---------------- Part 2: four units, region of interest -------------- */
    snprintf(json, sizeof(json),
             "{\"sperr:mode_str\":\"pwe\",\"sperr:tolerance\":%g,"
             "\"h5z:group\":\"8,1,1\",\"h5z:progressive\":true}",
             tol);
    dset = create(file, "volume_4units", json);
    if (dset < 0 || H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, w) < 0) {
        fprintf(stderr, "writing the 4-unit dataset failed\n");
        return 1;
    }
    {
        /* Planes 40..59 are the region of interest: they lie in unit 1 (planes 32..63). */
        const hsize_t       start[3] = {40, 0, 0}, count[3] = {20, NY, NX};
        H5Z_comp_io_stats_t st;
        double              r_in, m_in, r_out, m_out;

        H5Pset_comp_progressive_roi(dxpl, 3, start, count, 100 /* inside */, 10 /* outside */);
        memset(rd, 0, sizeof(float) * N);
        if (H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, rd) < 0) {
            fprintf(stderr, "ROI H5Dread failed\n");
            return 1;
        }
        H5Zcomp_get_io_stats(&st);
        errors(w, rd, 32, 64, &r_in, &m_in);
        errors(w, rd, 64, 128, &r_out, &m_out);
        printf("\nROI read (planes 40..59 at 100%%, everything else at 10%%), 4 units x 8 storage chunks:\n"
               "  read %llu of 32 storage chunks, %llu of %llu stored bytes (%.1f%%)\n"
               "  unit holding the ROI : rmse %.4e  max err %.4e   (bound %g)\n"
               "  units outside        : rmse %.4e  max err %.4e\n",
               st.chunks, st.bytes_io, st.bytes_stored, 100.0 * (double)st.bytes_io / (double)st.bytes_stored,
               r_in, m_in, tol, r_out, m_out);
        if (!(m_in <= 2 * tol) || !(r_out > r_in) || st.chunks >= 32)
            ok = 0;
    }
    H5Dclose(dset);

    H5Pclose(dxpl);
    H5Fclose(file);
    free(w);
    free(rd);
    printf("\n%s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
