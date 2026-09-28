/* ============================================================================
 * plugin_fixture_write.c  --  writes the file that test_plugin_read decodes
 *
 * Linked against libH5Zcomp (needs H5Pset_comp to build cd_values). Writes a
 * small-config dataset per available codec plus a reference copy of the data.
 * ==========================================================================*/
#include "test_common.h"

int main(int argc, char **argv) {
    const char *fname = argc > 1 ? argv[1] : "plugin_fixture.h5";
    hsize_t dims[3] = { 16, 24, 32 };
    const size_t n = 16 * 24 * 32;
    double *w = (double *)malloc(n * sizeof(double));
    h5zc_fill_synthetic(w, 3, dims);

    h5zc_init();
    hid_t fid = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hid_t sid = H5Screate_simple(3, dims, NULL);

    /* reference, no filter */
    hid_t ref = H5Dcreate2(fid, "reference", H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    H5Dwrite(ref, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, w);
    H5Dclose(ref);

    struct { const char *ds, *id, *json; } c[] = {
        { "noop",  "noop",  NULL },
        { "bzip2", "bzip2", NULL },
        { "sz3",   "sz3",   "{\"sz3:error_bound_mode_str\":\"abs\",\"sz3:abs_error_bound\":1e-3}" },
    };
    int rc = 0;
    for (size_t k = 0; k < sizeof(c) / sizeof(c[0]); k++) {
        if (!h5zc_have_codec(c[k].id)) { printf("skip %s (not in LibPressio)\n", c[k].id); continue; }
        hid_t dcpl = h5zc_make_dcpl_n(c[k].id, c[k].json, 3, dims, 4);
        hid_t did = H5Dcreate2(fid, c[k].ds, H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, dcpl, H5P_DEFAULT);
        if (did < 0 || H5Dwrite(did, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, w) < 0) {
            fprintf(stderr, "fixture: write %s failed\n", c[k].ds); rc = 1;
        } else printf("wrote %s\n", c[k].ds);
        if (did >= 0) H5Dclose(did);
        H5Pclose(dcpl);
    }
    /* mid-size (32 KiB) inline config: > 256 cd_values, must decode autonomously */
    {
        size_t big = 32u * 1024u;
        char *json = (char *)malloc(big + 1);
        const char *head = "{\"_pad\":\"";
        memcpy(json, head, strlen(head));
        memset(json + strlen(head), 'y', big - strlen(head) - 2);
        memcpy(json + big - 2, "\"}", 3);
        hid_t dcpl = h5zc_make_dcpl_n("noop", json, 3, dims, 4);
        hid_t did = H5Dcreate2(fid, "mid_noop", H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, dcpl, H5P_DEFAULT);
        if (did < 0 || H5Dwrite(did, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, w) < 0) {
            fprintf(stderr, "fixture: write mid_noop failed\n"); rc = 1;
        } else printf("wrote mid_noop (32 KiB inline config)\n");
        if (did >= 0) H5Dclose(did);
        H5Pclose(dcpl);
        free(json);
    }
    /* large (blob) config: decodable only after H5Pget_comp_json in the reader */
    {
        size_t big = 300u * 1024u;
        char *json = (char *)malloc(big + 1);
        const char *head = "{\"_pad\":\"";
        memcpy(json, head, strlen(head));
        memset(json + strlen(head), 'x', big - strlen(head) - 2);
        memcpy(json + big - 2, "\"}", 3);
        hid_t dcpl = h5zc_make_dcpl_n("noop", json, 3, dims, 4);
        hid_t did = H5Dcreate2(fid, "blob_noop", H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, dcpl, H5P_DEFAULT);
        if (did < 0 || H5Dwrite(did, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, w) < 0) {
            fprintf(stderr, "fixture: write blob_noop failed\n"); rc = 1;
        } else printf("wrote blob_noop (large config)\n");
        if (did >= 0) H5Dclose(did);
        H5Pclose(dcpl);
        free(json);
    }
    H5Sclose(sid); H5Fclose(fid); free(w);
    return rc;
}
