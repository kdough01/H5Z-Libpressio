/* ============================================================================
 * ex_large_config.c  --  arbitrarily large codec config via blob + DXPL.
 *
 * Demonstrates the path added in v2 for configs too big for cd_values (ROIBIN
 * mask, SZ4 JIT source, ...). We use codec "noop" with a >60 KiB config so the
 * example runs anywhere and stays a bit-exact round-trip -- the point is the
 * config TRANSPORT, not the codec. A real case swaps noop for the real codec
 * and the padding for the real mask/source.
 *
 * Flow:
 *   write:  H5Pset_comp(dcpl, id, BIG)      -> routes BIG into the filter blob
 *           H5Pset_comp_dxpl(dxpl, BIG)      -> hand BIG to filter2 for this write
 *           H5Dwrite(..., dxpl, ...)
 *   read:   H5Pget_comp_json(dcpl, &cfg)     -> pull BIG back out of the file
 *           H5Pset_comp_dxpl(rdxpl, cfg)     -> hand it to filter2 for this read
 *           H5Dread(..., rdxpl, ...)
 * ==========================================================================*/
#include "hdf5.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern herr_t H5Z_register_comp(void);
extern herr_t H5Pset_comp(hid_t dcpl, const char *id, const char *json);
extern herr_t H5Pset_comp_dxpl(hid_t dxpl, const char *json);
extern herr_t H5Pget_comp_json(hid_t dcpl, char **json);

#define NX 128
#define NY 128
#define FILENAME "ex_large_config.h5"
#define DSET     "data"

/* ~512 KiB of syntactically-valid JSON: a real option + a big padding key. */
static char *make_big_json(size_t bytes) {
    const char *head = "{\"pressio:abs\":1e-3,\"_pad\":\"";
    const char *tail = "\"}";
    size_t h = strlen(head), t = strlen(tail), pad = bytes > h + t ? bytes - h - t : 1;
    char *s = (char *)malloc(h + pad + t + 1);
    memcpy(s, head, h); memset(s + h, 'x', pad); memcpy(s + h + pad, tail, t);
    s[h + pad + t] = '\0';
    return s;
}

int main(void) {
    char *BIG = make_big_json(512u * 1024u);
    printf("config size = %zu bytes (%.0f KiB, well past the ~60 KiB inline ceiling)\n",
           strlen(BIG), strlen(BIG) / 1024.0);

    static int wdata[NX][NY], rdata[NX][NY];
    for (int i = 0; i < NX; i++) for (int j = 0; j < NY; j++) wdata[i][j] = i * NY + j;

    if (H5Z_register_comp() < 0) return 1;

    hid_t   file  = H5Fcreate(FILENAME, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hsize_t dims[2] = {NX, NY}, chunk[2] = {32, 32};
    hid_t   space = H5Screate_simple(2, dims, NULL);
    hid_t   dcpl  = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_chunk(dcpl, 2, chunk);

    /* Too big for cd_values -> H5Pset_comp stores it in the filter blob. */
    if (H5Pset_comp(dcpl, "noop", BIG) < 0) { fprintf(stderr, "H5Pset_comp failed\n"); return 1; }

    /* Confirm it really went to the blob (an inline/small config would not). */
    size_t blobsz = 0;
    if (H5Pget_filter_blob(dcpl, 0, 0, NULL, &blobsz) >= 0 && blobsz > 0)
        printf("config stored in filter blob: %zu bytes\n", blobsz);

    hid_t dset = H5Dcreate2(file, DSET, H5T_NATIVE_INT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);

    /* Hand the config to filter2 for the write. */
    hid_t wdxpl = H5Pcreate(H5P_DATASET_XFER);
    if (H5Pset_comp_dxpl(wdxpl, BIG) < 0) { fprintf(stderr, "set_comp_dxpl(write) failed\n"); return 1; }
    if (H5Dwrite(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, wdxpl, wdata) < 0) {
        fprintf(stderr, "H5Dwrite failed (did filter2 get the DXPL config?)\n"); return 1;
    }
    printf("write OK\n");

    H5Pclose(wdxpl); H5Dclose(dset); H5Pclose(dcpl); H5Sclose(space); H5Fclose(file);

    /* ---- reopen and read ---- */
    file = H5Fopen(FILENAME, H5F_ACC_RDONLY, H5P_DEFAULT);
    dset = H5Dopen2(file, DSET, H5P_DEFAULT);

    /* Pull the persisted config out of the file, then feed it to filter2. */
    hid_t dcpl_out = H5Dget_create_plist(dset);
    char *cfg = NULL;
    if (H5Pget_comp_json(dcpl_out, &cfg) < 0 || !cfg) { fprintf(stderr, "no stored config\n"); return 1; }
    printf("recovered config from file: %zu bytes\n", strlen(cfg));

    hid_t rdxpl = H5Pcreate(H5P_DATASET_XFER);
    H5Pset_comp_dxpl(rdxpl, cfg);
    if (H5Dread(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, rdxpl, rdata) < 0) {
        fprintf(stderr, "H5Dread failed\n"); return 1;
    }

    int ok = 1;
    for (int i = 0; i < NX && ok; i++) for (int j = 0; j < NY; j++)
        if (rdata[i][j] != wdata[i][j]) { ok = 0; break; }
    printf(ok ? "ROUND-TRIP OK (large config via blob+DXPL)\n" : "ROUND-TRIP FAILED\n");

    free(cfg); free(BIG);
    H5Pclose(rdxpl); H5Pclose(dcpl_out); H5Dclose(dset); H5Fclose(file);
    return ok ? 0 : 1;
}
