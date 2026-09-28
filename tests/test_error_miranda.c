/* ============================================================================
 * test_error_miranda.c  --  port of the VOL repo's error-handling test
 *
 * Same five checks as the VOL, same expectations:
 *   1. unavailable compressor ('zstd' unless your LibPressio has it)  -> H5Dcreate fails
 *   2. malformed options JSON                                         -> H5Dcreate fails
 *   3. unknown option key                                             -> INFO (LibPressio ignores it)
 *   4. noop passthrough                                               -> write succeeds
 *   5. sz3 abs 1e-3 round-trip                                        -> within bound
 * plus filter-only checks for the large-config (blob + DXPL) path:
 *   6. a blob-config dataset writes/reads with no DXPL in the same process
 *      (the fresh-process case is covered by test_plugin_read)
 *   7. a large config with bad JSON is rejected at H5Dcreate
 *
 * Uses Miranda density.d64 when reachable (as the VOL test does); otherwise a
 * synthetic 64^3 field so the suite still runs on a laptop / in CI.
 * ==========================================================================*/
#include "test_common.h"

static char *big_json(const char *head_opts, size_t bytes) {
    /* {<head_opts>,"_pad":"xxxx..."} of ~bytes length */
    const char *tail = "\"}";
    char head[256];
    snprintf(head, sizeof(head), "{%s%s\"_pad\":\"", head_opts, head_opts[0] ? "," : "");
    size_t h = strlen(head), t = strlen(tail), pad = bytes > h + t ? bytes - h - t : 1;
    char *s = (char *)malloc(h + pad + t + 1);
    memcpy(s, head, h); memset(s + h, 'x', pad); memcpy(s + h + pad, tail, t);
    s[h + pad + t] = '\0';
    return s;
}

static void expect_create_fails(hid_t file_id, hid_t space_id, int rank, const hsize_t *dims,
                                const char *label, const char *dsname,
                                const char *id, const char *json) {
    hid_t dcpl = h5zc_make_dcpl(id, json, rank, dims);
    h5zc_quiet(1);
    hid_t dset = (dcpl >= 0)
        ? H5Dcreate2(file_id, dsname, H5T_NATIVE_DOUBLE, space_id, H5P_DEFAULT, dcpl, H5P_DEFAULT)
        : H5I_INVALID_HID;
    h5zc_quiet(0);
    if (dset < 0) H5ZC_PASS("%s: H5Dcreate2 correctly failed", label);
    else { H5ZC_FAIL("%s: H5Dcreate2 should have failed but succeeded", label); H5Dclose(dset); }
    if (dcpl >= 0) H5Pclose(dcpl);
}

static void test_error_handling(hid_t file_id, const double *field, size_t nelem,
                                int ndims, const hsize_t *dims) {
    printf("\n=== Error Handling Tests ===\n");
    hid_t space_id = H5Screate_simple(ndims, dims, NULL);

    printf("\n-- Test 1: Unavailable compressor --\n");
    {
        const char *missing = h5zc_have_codec("zstd") ? "definitely_not_a_codec" : "zstd";
        H5ZC_INFO("using '%s' as the unavailable compressor", missing);
        expect_create_fails(file_id, space_id, ndims, dims, "unavailable compressor",
                            "err_bad_compressor", missing, "{\"zstd:clevel\":3}");
    }

    printf("\n-- Test 2: Malformed JSON options --\n");
    if (h5zc_have_codec("sz3"))
        expect_create_fails(file_id, space_id, ndims, dims, "bad JSON", "err_bad_json",
                            "sz3", "{this is not valid json!!!}");
    else
        expect_create_fails(file_id, space_id, ndims, dims, "bad JSON", "err_bad_json",
                            "noop", "{this is not valid json!!!}");

    printf("\n-- Test 3: Invalid JSON option key for compressor --\n");
    if (h5zc_have_codec("sz3")) {
        hid_t dcpl = h5zc_make_dcpl("sz3", "{\"sz3:this_key_does_not_exist\":999}", ndims, dims);
        h5zc_quiet(1);
        hid_t dset = H5Dcreate2(file_id, "err_bad_option", H5T_NATIVE_DOUBLE, space_id,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT);
        h5zc_quiet(0);
        if (dset < 0) H5ZC_PASS("H5Dcreate2 failed for invalid option key");
        else { H5ZC_INFO("H5Dcreate2 succeeded -- libpressio silently ignored unknown key"); H5Dclose(dset); }
        H5Pclose(dcpl);
    } else H5ZC_SKIPMSG("sz3 not in LibPressio");

    printf("\n-- Test 4: noop passthrough (should always succeed) --\n");
    {
        hid_t dcpl = h5zc_make_dcpl(NULL, NULL, ndims, dims);
        hid_t dset = H5Dcreate2(file_id, "err_noop_passthrough", H5T_NATIVE_DOUBLE, space_id,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT);
        if (dset >= 0) {
            herr_t w = H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, field);
            if (w >= 0) H5ZC_PASS("noop write succeeded as expected");
            else        H5ZC_FAIL("noop write should have succeeded");
            H5Dclose(dset);
        } else H5ZC_FAIL("noop H5Dcreate2 should have succeeded");
        H5Pclose(dcpl);
    }

    printf("\n-- Test 5: Successful sz3 round-trip (regression check) --\n");
    if (h5zc_have_codec("sz3")) {
        hid_t dcpl = h5zc_make_dcpl("sz3",
            "{\"sz3:error_bound_mode_str\":\"abs\",\"sz3:abs_error_bound\":1e-3}", ndims, dims);
        hid_t dset = H5Dcreate2(file_id, "err_sz3_roundtrip", H5T_NATIVE_DOUBLE, space_id,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT);
        H5Pclose(dcpl);
        if (dset < 0) H5ZC_FAIL("sz3 H5Dcreate2 failed unexpectedly");
        else {
            herr_t w = H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, field);
            H5Dclose(dset);
            if (w < 0) H5ZC_FAIL("sz3 write failed unexpectedly");
            else {
                double *rbuf = (double *)calloc(nelem, sizeof(double));
                dset = H5Dopen2(file_id, "err_sz3_roundtrip", H5P_DEFAULT);
                herr_t r = H5Dread(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, rbuf);
                H5Dclose(dset);
                if (r < 0) H5ZC_FAIL("sz3 read failed unexpectedly");
                else {
                    Stats st = compute_stats(field, rbuf, nelem);
                    /* The VOL gated on RMSE; the bound is pointwise, so gate on maxae. */
                    if (st.maxae <= 1e-3)
                        H5ZC_PASS("sz3 round-trip maxae=%.4e RMSE=%.4e within 1e-3", st.maxae, st.rmse);
                    else
                        H5ZC_FAIL("sz3 round-trip maxae=%.4e exceeds 1e-3", st.maxae);
                }
                free(rbuf);
            }
        }
    } else H5ZC_SKIPMSG("sz3 not in LibPressio");

    printf("\n-- Test 6: blob-config dataset, plain H5Dwrite/H5Dread (no DXPL) --\n");
    {
        /* The chunk cache defers the forward filter to H5Dclose/H5Fclose, whose
         * context DXPL is H5P_DEFAULT, so the filter must find the config via
         * its registry (tag in cd_values) -- not the DXPL. This used to fail. */
        char *BIG = big_json("", 300u * 1024u);   /* > cd_values ceiling -> blob path */
        hid_t dcpl = h5zc_make_dcpl("noop", BIG, ndims, dims);
        hid_t dset = H5Dcreate2(file_id, "err_blob_no_dxpl", H5T_NATIVE_DOUBLE, space_id,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT);
        herr_t w = (dset >= 0) ? H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, field) : -1;
        if (dset >= 0) H5Dclose(dset);
        H5Pclose(dcpl);
        if (w < 0) H5ZC_FAIL("blob-config write without DXPL failed");
        else {
            H5Fflush(file_id, H5F_SCOPE_LOCAL);
            double *rbuf = (double *)calloc(nelem, sizeof(double));
            dset = H5Dopen2(file_id, "err_blob_no_dxpl", H5P_DEFAULT);
            herr_t r = H5Dread(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, rbuf);
            H5Dclose(dset);
            if (r < 0) H5ZC_FAIL("in-process read of blob-config dataset failed");
            else if (memcmp(rbuf, field, nelem * sizeof(double)) != 0) H5ZC_FAIL("blob-config noop data differs");
            else H5ZC_PASS("blob-config write+read without DXPL (registry path)");
            free(rbuf);
        }
        free(BIG);
    }

    printf("\n-- Test 7: blob-config with malformed JSON --\n");
    {
        char *BIG = big_json("", 300u * 1024u);
        BIG[0] = '[';                               /* break it */
        expect_create_fails(file_id, space_id, ndims, dims, "bad blob JSON",
                            "err_blob_bad_json", "noop", BIG);
        free(BIG);
    }

    H5Sclose(space_id);
    printf("\n=== Error Handling Tests Complete ===\n");
}

int main(void) {
    printf("H5Zcomp error handling test starting\n");
    h5zc_init();

    hid_t file_id = H5Fcreate("h5zcomp_errors.h5", H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file_id < 0) { fprintf(stderr, "H5Fcreate failed\n"); return 1; }

    char path[512];
    snprintf(path, sizeof(path), "%s/density.d64", MIRANDA_PATH);
    size_t nelem = 0;
    double *data = NULL;
    hsize_t dims[3];

    if (access(path, R_OK) == 0 && (data = read_raw_double(path, &nelem)) && nelem == MIR_NELEM) {
        printf("Field: Miranda %s\n", path);
        dims[0] = MIR_NX; dims[1] = MIR_NY; dims[2] = MIR_NZ;
    } else {
        free(data);
        dims[0] = dims[1] = dims[2] = 64;
        nelem = 64 * 64 * 64;
        data = (double *)malloc(nelem * sizeof(double));
        h5zc_fill_synthetic(data, 3, dims);
        printf("Field: synthetic 64^3 (Miranda not reachable at %s)\n", MIRANDA_PATH);
    }

    test_error_handling(file_id, data, nelem, 3, dims);

    free(data);
    H5Fclose(file_id);
    remove("h5zcomp_errors.h5");
    return h5zc_summary("test_error_miranda");
}
