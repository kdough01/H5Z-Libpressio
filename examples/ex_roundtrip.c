/* ============================================================================
 * ex_roundtrip.c  --  H5Zcomp filter, end-to-end, codec-agnostic.
 *
 * Same code path for a CPU codec and a GPU codec -- that is the point:
 *
 *     ./ex_roundtrip sz3    '{"pressio:abs":1e-3}'     # CPU
 *     ./ex_roundtrip cuszp  '{"pressio:abs":1e-3}'     # GPU (needs CUDA libpressio)
 *
 * Build/registration: either link this against the plugin object, or
 * export HDF5_PLUGIN_PATH=<build dir> and delete the H5Z_register_comp() call
 * (HDF5 will load the plugin on demand). H5Pset_comp/H5Z_register_comp are
 * declared extern below; they live in H5Z_comp.cc.
 *
 * WHERE THE GPU COPIES ARE (answers "no device->host copies?" = you can't):
 *
 *   H5Dwrite  ->  HDF5 gives the filter a HOST chunk buffer
 *                 ->  libpressio H2D copy      (host -> device)   [unavoidable]
 *                 ->  GPU compress
 *                 ->  libpressio D2H copy      (device -> host)   [unavoidable]
 *                 ->  HDF5 stores the host bytes to the file
 *
 * The filter MUST return host memory, so both copies happen every chunk no
 * matter the codec or config. That is filter limitation #1 (host residency);
 * the VOL is the only way to keep the data device-resident and skip them.
 * ==========================================================================*/
#include "hdf5.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern herr_t H5Z_register_comp(void);
extern herr_t H5Pset_comp(hid_t dcpl, const char *compressor_id, const char *options_json);

#define NX 256
#define NY 256
#define CX 64
#define CY 64
#define FILENAME "ex_roundtrip.h5"
#define DSET     "field"

int
main(int argc, char **argv)
{
    const char *id   = (argc > 1) ? argv[1] : "sz3";
    const char *json = (argc > 2) ? argv[2] : "{\"pressio:abs\":1e-3}";

    static float wdata[NX][NY], rdata[NX][NY];
    for (int i = 0; i < NX; i++)
        for (int j = 0; j < NY; j++)
            wdata[i][j] = (float)(sin(i * 0.05) * cos(j * 0.05) + 0.01 * i);

    if (H5Z_register_comp() < 0) { fprintf(stderr, "register failed\n"); return 1; }

    hid_t   file  = H5Fcreate(FILENAME, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hsize_t dims[2]  = {NX, NY};
    hsize_t chunk[2] = {CX, CY};
    hid_t   space = H5Screate_simple(2, dims, NULL);
    hid_t   dcpl  = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_chunk(dcpl, 2, chunk);

    /* Attach our filter with the libpressio codec id + options JSON.
     * set_local fills in the datatype and chunk dims at H5Dcreate time. */
    if (H5Pset_comp(dcpl, id, json) < 0) { fprintf(stderr, "H5Pset_comp failed\n"); return 1; }

    hid_t dset = H5Dcreate2(file, DSET, H5T_NATIVE_FLOAT, space,
                            H5P_DEFAULT, dcpl, H5P_DEFAULT);
    if (dset < 0) { fprintf(stderr, "H5Dcreate failed (codec '%s' registered/available?)\n", id); return 1; }

    if (H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, wdata) < 0)
        { fprintf(stderr, "H5Dwrite failed\n"); return 1; }

    hsize_t stored = H5Dget_storage_size(dset);
    printf("codec '%s'  raw=%zu B  stored=%llu B  ratio=%.2fx\n",
           id, (size_t)(NX * NY * sizeof(float)),
           (unsigned long long)stored,
           stored ? (double)(NX * NY * sizeof(float)) / (double)stored : 0.0);

    H5Dclose(dset); H5Pclose(dcpl); H5Sclose(space); H5Fclose(file);

    /* Reopen fresh and read back -- purely autonomous: no app-side config
     * handling, because the codec config rode in cd_values. */
    file = H5Fopen(FILENAME, H5F_ACC_RDONLY, H5P_DEFAULT);
    dset = H5Dopen2(file, DSET, H5P_DEFAULT);
    if (H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rdata) < 0)
        { fprintf(stderr, "H5Dread failed\n"); return 1; }

    double maxerr = 0.0;
    for (int i = 0; i < NX; i++)
        for (int j = 0; j < NY; j++) {
            double e = fabs((double)rdata[i][j] - (double)wdata[i][j]);
            if (e > maxerr) maxerr = e;
        }
    printf("max abs error after round-trip = %.3e\n", maxerr);
    printf(isfinite(maxerr) ? "ROUND-TRIP OK\n" : "ROUND-TRIP FAILED (non-finite)\n");

    H5Dclose(dset); H5Fclose(file);
    return isfinite(maxerr) ? 0 : 1;
}
