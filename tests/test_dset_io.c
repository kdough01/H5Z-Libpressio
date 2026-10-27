/* ============================================================================
 * test_dset_io.c  --  the dataset-level path (H5Z_class3_t dset_write/dset_read)
 *
 * Filter-only, no VOL test file to mirror: these are the three things the VOL
 * adaptor could do and a per-chunk filter could not.
 *
 *   [direct]       host buffers through the dataset-level path produce files
 *                  the per-chunk path reads, and the reverse (same on-disk
 *                  format for ungrouped datasets), incl. padded edge chunks
 *   [native]       a buffer declared with H5Pset_comp_native_buffer is used in
 *                  place (nothing staged), and layouts it cannot serve are
 *                  refused instead of dereferenced
 *   [group]        "h5z:group": compression unit != storage chunk -- same
 *                  chunk count on disk, better ratio, any hyperslab readable,
 *                  ragged edges, partial writes refused, fill value for
 *                  unwritten units, config in the blob needs no app help
 *   [progressive]  "h5z:progressive" + H5Pset_comp_progressive[_roi]: fewer
 *                  storage chunks read, error falls as the percentage rises
 *                  (needs the sperr codec; skipped otherwise)
 *
 * Skips (exit 77) when the filter was built against an HDF5 without the
 * dataset-level callbacks.
 * ==========================================================================*/
#include "test_common.h"

#define NZ 24
#define NY 20
#define NX 18
#define N  ((size_t)NZ * NY * NX)

static float *field(void) {
    float *f = (float *)malloc(sizeof(float) * N);
    for (int k = 0; k < NZ; k++)
        for (int j = 0; j < NY; j++)
            for (int i = 0; i < NX; i++)
                f[((size_t)k * NY + j) * NX + i] =
                    (float)(sin(0.21 * i) * cos(0.17 * j) * sin(0.13 * k + 0.3) + 0.01 * (i + j + k));
    return f;
}

static double maxae(const float *a, const float *b, size_t n) {
    double m = 0;
    for (size_t i = 0; i < n; i++) { double e = fabs((double)a[i] - (double)b[i]); if (!(e <= m)) m = e; }
    return m;
}

static hid_t make_dset(hid_t file, const char *name, const hsize_t *chunk, const char *codec, const char *json) {
    const hsize_t dims[3] = { NZ, NY, NX };
    hid_t space = H5Screate_simple(3, dims, NULL);
    hid_t dcpl  = H5Pcreate(H5P_DATASET_CREATE);
    hid_t dset  = H5I_INVALID_HID;
    H5Pset_chunk(dcpl, 3, chunk);
    if (H5Pset_comp(dcpl, codec, json) >= 0)
        dset = H5Dcreate2(file, name, H5T_NATIVE_FLOAT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
    H5Pclose(dcpl); H5Sclose(space);
    return dset;
}

static H5Z_comp_io_stats_t stats(void) { H5Z_comp_io_stats_t s; H5Zcomp_get_io_stats(&s); return s; }

/* ---- [direct] ------------------------------------------------------------ */
static void test_direct(hid_t file, const float *w, const char *codec, const char *json, double bound) {
    const hsize_t chunk[3] = { 7, 20, 18 };   /* 24 = 3*7 + 3: the last chunk is padded */
    float *rd = (float *)malloc(sizeof(float) * N);
    char name[64];
    hid_t dxpl = H5Pcreate(H5P_DATASET_XFER);
    H5Pset_comp_direct(dxpl, 1);

    /* dataset-level write, per-chunk read */
    snprintf(name, sizeof(name), "direct_w_%s", codec);
    hid_t d = make_dset(file, name, chunk, codec, json);
    if (d < 0) { H5ZC_FAIL("[direct/%s] create", codec); goto out; }
    if (H5Dwrite(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, w) < 0 || !stats().handled) {
        H5ZC_FAIL("[direct/%s] dataset-level write (handled=%d)", codec, stats().handled); H5Dclose(d); goto out;
    }
    {
        H5Z_comp_io_stats_t s = stats();
        /* 3 full slabs are compressed where they lie; only the padded edge chunk is staged */
        if (s.units == 4 && s.bytes_staged == sizeof(float) * 7 * 20 * 18)
            H5ZC_PASS("[direct/%s] write: 4 units, only the padded edge chunk staged (%llu B)", codec, s.bytes_staged);
        else H5ZC_FAIL("[direct/%s] write stats: units=%llu staged=%llu", codec, s.units, s.bytes_staged);
    }
    H5Dclose(d);
    d = H5Dopen2(file, name, H5P_DEFAULT);
    memset(rd, 0, sizeof(float) * N);
    if (H5Dread(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rd) < 0 || stats().handled)
        H5ZC_FAIL("[direct/%s] per-chunk read of a dataset-level write", codec);
    else if (maxae(w, rd, N) <= bound) H5ZC_PASS("[direct/%s] dataset-level write -> per-chunk read (maxae %.2e)", codec, maxae(w, rd, N));
    else H5ZC_FAIL("[direct/%s] dataset-level write -> per-chunk read: maxae %.3e > %.3e", codec, maxae(w, rd, N), bound);
    H5Dclose(d);

    /* per-chunk write, dataset-level read */
    snprintf(name, sizeof(name), "direct_r_%s", codec);
    d = make_dset(file, name, chunk, codec, json);
    if (d < 0 || H5Dwrite(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, w) < 0 || stats().handled) {
        H5ZC_FAIL("[direct/%s] per-chunk write", codec); if (d >= 0) H5Dclose(d); goto out;
    }
    H5Dclose(d);
    d = H5Dopen2(file, name, H5P_DEFAULT);
    memset(rd, 0, sizeof(float) * N);
    if (H5Dread(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, rd) < 0 || !stats().handled)
        H5ZC_FAIL("[direct/%s] dataset-level read of a per-chunk write", codec);
    else if (maxae(w, rd, N) <= bound) H5ZC_PASS("[direct/%s] per-chunk write -> dataset-level read (maxae %.2e)", codec, maxae(w, rd, N));
    else H5ZC_FAIL("[direct/%s] per-chunk write -> dataset-level read: maxae %.3e > %.3e", codec, maxae(w, rd, N), bound);

    /* a part of a chunk: the dataset-level path declines, HDF5 does it */
    {
        const hsize_t start[3] = { 3, 2, 1 }, count[3] = { 2, 5, 4 };
        float patch[2 * 5 * 4], back[2 * 5 * 4];
        hid_t fsp = H5Dget_space(d), msp = H5Screate_simple(3, count, NULL);
        for (int i = 0; i < 40; i++) patch[i] = 100.0f + (float)i;
        H5Sselect_hyperslab(fsp, H5S_SELECT_SET, start, NULL, count, NULL);
        if (H5Dwrite(d, H5T_NATIVE_FLOAT, msp, fsp, dxpl, patch) < 0 || stats().handled)
            H5ZC_FAIL("[direct/%s] partial-chunk write should fall back to the per-chunk path", codec);
        else if (H5Dread(d, H5T_NATIVE_FLOAT, msp, fsp, dxpl, back) < 0 || maxae(patch, back, 40) > bound)
            H5ZC_FAIL("[direct/%s] partial-chunk write/read-back", codec);
        else H5ZC_PASS("[direct/%s] partial-chunk write declined -> per-chunk path; read back through either", codec);
        H5Sclose(msp); H5Sclose(fsp);
    }
    H5Dclose(d);
out:
    H5Pclose(dxpl); free(rd);
}

/* ---- [native] ------------------------------------------------------------ */
static void test_native(hid_t file, const float *w) {
    const hsize_t slab[3] = { 6, 20, 18 }, brick[3] = { 6, 10, 9 };
    float *rd = (float *)malloc(sizeof(float) * N);
    hid_t dxpl = H5Pcreate(H5P_DATASET_XFER);
    H5Pset_comp_native_buffer(dxpl, "malloc");   /* the host domain stands in for a device */

    hid_t d = make_dset(file, "native_slab", slab, "noop", "{}");
    if (d < 0) { H5ZC_FAIL("[native] create"); goto out; }
    if (H5Dwrite(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, w) < 0) H5ZC_FAIL("[native] write");
    else {
        H5Z_comp_io_stats_t s = stats();
        if (s.handled && s.native && s.units == 4 && s.bytes_staged == 0 && s.bytes_raw == sizeof(float) * N)
            H5ZC_PASS("[native] write: 4 units compressed in place, 0 B staged");
        else H5ZC_FAIL("[native] write stats: handled=%d native=%d units=%llu staged=%llu", s.handled, s.native, s.units, s.bytes_staged);
    }
    memset(rd, 0, sizeof(float) * N);
    if (H5Dread(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, rd) < 0) H5ZC_FAIL("[native] read");
    else {
        H5Z_comp_io_stats_t s = stats();
        if (s.handled && s.native && s.bytes_staged == 0 && memcmp(w, rd, sizeof(float) * N) == 0)
            H5ZC_PASS("[native] read: decoded in place, bit-exact (noop)");
        else H5ZC_FAIL("[native] read: handled=%d native=%d staged=%llu equal=%d", s.handled, s.native, s.bytes_staged,
                       memcmp(w, rd, sizeof(float) * N) == 0);
    }
    /* whole units as a hyperslab: units 1 and 2 (planes 6..17) into a 12-plane buffer */
    {
        const hsize_t start[3] = { 6, 0, 0 }, count[3] = { 12, 20, 18 };
        hid_t fsp = H5Dget_space(d), msp = H5Screate_simple(3, count, NULL);
        H5Sselect_hyperslab(fsp, H5S_SELECT_SET, start, NULL, count, NULL);
        memset(rd, 0, sizeof(float) * N);
        if (H5Dread(d, H5T_NATIVE_FLOAT, msp, fsp, dxpl, rd) < 0) H5ZC_FAIL("[native] unit-aligned hyperslab read");
        else if (stats().units == 2 && stats().bytes_staged == 0 &&
                 memcmp(rd, w + (size_t)6 * NY * NX, sizeof(float) * 12 * NY * NX) == 0)
            H5ZC_PASS("[native] unit-aligned hyperslab: 2 units, in place");
        else H5ZC_FAIL("[native] unit-aligned hyperslab: units=%llu staged=%llu", stats().units, stats().bytes_staged);
        H5Sclose(msp); H5Sclose(fsp);
    }
    /* a double buffer for a float dataset: HDF5 could not convert on a device */
    h5zc_quiet(1);
    if (H5Dwrite(d, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, dxpl, w) < 0) H5ZC_PASS("[native] datatype mismatch refused");
    else H5ZC_FAIL("[native] datatype mismatch was accepted");
    h5zc_quiet(0);
    H5Dclose(d);

    /* units that are NOT contiguous in the buffer must be refused, not gathered */
    d = make_dset(file, "native_brick", brick, "noop", "{}");
    h5zc_quiet(1);
    if (d >= 0 && H5Dwrite(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, w) < 0) H5ZC_PASS("[native] non-contiguous units refused");
    else H5ZC_FAIL("[native] non-contiguous units were accepted");
    h5zc_quiet(0);
    if (d >= 0) H5Dclose(d);
out:
    H5Pclose(dxpl); free(rd);
}

/* ---- [group] ------------------------------------------------------------- */
static void test_group_one(hid_t file, const float *w, const char *tag, const hsize_t *chunk, const char *codec,
                           const char *json, double bound, unsigned long long want_units) {
    float *rd = (float *)malloc(sizeof(float) * N);
    char name[64];
    snprintf(name, sizeof(name), "group_%s", tag);
    hid_t d = make_dset(file, name, chunk, codec, json);
    if (d < 0) { H5ZC_FAIL("[group/%s] create", tag); free(rd); return; }
    if (H5Dwrite(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, w) < 0 || !stats().handled ||
        stats().units != want_units) {
        H5ZC_FAIL("[group/%s] write (handled=%d units=%llu, want %llu)", tag, stats().handled, stats().units, want_units);
        H5Dclose(d); free(rd); return;
    }
    hsize_t nchunks = 0, expect = 1;
    const hsize_t dims[3] = { NZ, NY, NX };
    for (int i = 0; i < 3; i++) expect *= (dims[i] + chunk[i] - 1) / chunk[i];
    H5Dget_num_chunks(d, H5S_ALL, &nchunks);
    H5Dclose(d);

    d = H5Dopen2(file, name, H5P_DEFAULT);   /* a plain reader: default DXPL, nothing registered by hand */
    memset(rd, 0, sizeof(float) * N);
    if (H5Dread(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rd) < 0) H5ZC_FAIL("[group/%s] read", tag);
    else if (maxae(w, rd, N) <= bound && nchunks == expect)
        H5ZC_PASS("[group/%s] %llu units over %llu storage chunks, maxae %.2e, stored %llu B", tag, want_units,
                  (unsigned long long)nchunks, maxae(w, rd, N), (unsigned long long)H5Dget_storage_size(d));
    else H5ZC_FAIL("[group/%s] maxae %.3e (bound %.3e), chunks %llu (want %llu)", tag, maxae(w, rd, N), bound,
                   (unsigned long long)nchunks, (unsigned long long)expect);

    /* an arbitrary hyperslab into a differently shaped, offset memory selection */
    {
        const hsize_t start[3] = { 5, 3, 2 }, count[3] = { 11, 9, 13 };
        const hsize_t mdims[3] = { 13, 12, 15 }, mstart[3] = { 1, 2, 1 };
        float *part = (float *)calloc(13 * 12 * 15, sizeof(float));
        hid_t fsp = H5Dget_space(d), msp = H5Screate_simple(3, mdims, NULL);
        double m = 0; int untouched = 1;
        H5Sselect_hyperslab(fsp, H5S_SELECT_SET, start, NULL, count, NULL);
        H5Sselect_hyperslab(msp, H5S_SELECT_SET, mstart, NULL, count, NULL);
        if (H5Dread(d, H5T_NATIVE_FLOAT, msp, fsp, H5P_DEFAULT, part) < 0) H5ZC_FAIL("[group/%s] hyperslab read", tag);
        else {
            for (int k = 0; k < 13; k++) for (int j = 0; j < 12; j++) for (int i = 0; i < 15; i++) {
                float v = part[(k * 12 + j) * 15 + i];
                int in = k >= 1 && k < 12 && j >= 2 && j < 11 && i >= 1 && i < 14;
                if (!in) { if (v != 0.0f) untouched = 0; continue; }
                double e = fabs((double)v - (double)w[((size_t)(k - 1 + 5) * NY + (j - 2 + 3)) * NX + (i - 1 + 2)]);
                if (!(e <= m)) m = e;
            }
            if (m <= bound && untouched) H5ZC_PASS("[group/%s] hyperslab -> offset memory selection (maxae %.2e, %llu units decoded)", tag, m, stats().units);
            else H5ZC_FAIL("[group/%s] hyperslab: maxae %.3e, memory outside the selection %s", tag, m, untouched ? "untouched" : "CLOBBERED");
        }
        H5Sclose(msp); H5Sclose(fsp); free(part);
    }
    H5Dclose(d); free(rd);
}

static char *padded_json(const char *opts, size_t bytes) {
    char head[512];
    snprintf(head, sizeof(head), "{%s,\"_pad\":\"", opts);
    size_t h = strlen(head), pad = bytes > h + 2 ? bytes - h - 2 : 1;
    char *s = (char *)malloc(h + pad + 3);
    memcpy(s, head, h); memset(s + h, 'x', pad); memcpy(s + h + pad, "\"}", 3);
    return s;
}

static void test_group(const char *fname, hid_t file, const float *w) {
    const hsize_t slab[3] = { 4, 20, 18 }, brick[3] = { 5, 7, 6 };   /* brick: ragged in every dimension */
    const int have_sz3 = h5zc_have_codec("sz3");
    const char *sz3 = "{\"sz3:error_bound_mode_str\":\"abs\",\"sz3:abs_error_bound\":1e-3";
    char json[256];

    test_group_one(file, w, "all_noop", slab, "noop", "{\"h5z:group\":\"all\"}", 0.0, 1);
    test_group_one(file, w, "2x1x1_noop", slab, "noop", "{\"h5z:group\":\"2,1,1\"}", 0.0, 3);
    test_group_one(file, w, "array_noop", slab, "noop", "{\"h5z:group\":[4,0,0]}", 0.0, 2);   /* 6 slabs: 4 + 2 */
    test_group_one(file, w, "ragged_noop", brick, "noop", "{\"h5z:group\":\"2,2,2\"}", 0.0, 3 * 2 * 2);

    if (have_sz3) {
        snprintf(json, sizeof(json), "%s,\"h5z:group\":0}", sz3);
        test_group_one(file, w, "all_sz3", slab, "sz3", json, 2e-3, 1);
        snprintf(json, sizeof(json), "%s,\"h5z:group\":\"2,2,2\"}", sz3);
        test_group_one(file, w, "ragged_sz3", brick, "sz3", json, 2e-3, 12);

        /* the point of it all: same storage chunks, better ratio */
        snprintf(json, sizeof(json), "%s}", sz3);
        hid_t d = make_dset(file, "group_ref_sz3", slab, "sz3", json);
        if (d >= 0 && H5Dwrite(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, w) >= 0) {
            H5Dclose(d);
            d = H5Dopen2(file, "group_ref_sz3", H5P_DEFAULT);
            hid_t g = H5Dopen2(file, "group_all_sz3", H5P_DEFAULT);
            hsize_t per_chunk = H5Dget_storage_size(d), grouped = H5Dget_storage_size(g);
            if (grouped < per_chunk) H5ZC_PASS("[group] sz3 on 6 storage chunks: %llu B per chunk -> %llu B as one unit",
                                               (unsigned long long)per_chunk, (unsigned long long)grouped);
            else H5ZC_FAIL("[group] grouping did not help: %llu B vs %llu B", (unsigned long long)grouped, (unsigned long long)per_chunk);
            H5Dclose(g);
        } else H5ZC_FAIL("[group] reference dataset");
        if (d >= 0) H5Dclose(d);
    } else H5ZC_SKIPMSG("[group] sz3 not in this LibPressio build");

    /* a write that covers only part of a unit must be refused */
    {
        hid_t d = H5Dopen2(file, "group_2x1x1_noop", H5P_DEFAULT);
        const hsize_t start[3] = { 0, 0, 0 }, part[3] = { 4, 20, 18 }, unit[3] = { 8, 20, 18 };
        hid_t fsp = H5Dget_space(d), msp = H5Screate_simple(3, part, NULL), usp = H5Screate_simple(3, unit, NULL);
        float *zeros = (float *)calloc(8 * 20 * 18, sizeof(float)), *rd = (float *)malloc(sizeof(float) * N);
        H5Sselect_hyperslab(fsp, H5S_SELECT_SET, start, NULL, part, NULL);
        h5zc_quiet(1);
        if (H5Dwrite(d, H5T_NATIVE_FLOAT, msp, fsp, H5P_DEFAULT, zeros) < 0) H5ZC_PASS("[group] partial-unit write refused");
        else H5ZC_FAIL("[group] partial-unit write was accepted");
        h5zc_quiet(0);
        /* ... while a whole unit can be rewritten on its own */
        H5Sselect_hyperslab(fsp, H5S_SELECT_SET, start, NULL, unit, NULL);
        if (H5Dwrite(d, H5T_NATIVE_FLOAT, usp, fsp, H5P_DEFAULT, zeros) < 0 || stats().units != 1)
            H5ZC_FAIL("[group] whole-unit rewrite");
        else if (H5Dread(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rd) < 0)
            H5ZC_FAIL("[group] read after whole-unit rewrite");
        else {
            int ok = 1;
            for (size_t i = 0; i < N; i++) if (rd[i] != (i < (size_t)8 * NY * NX ? 0.0f : w[i])) { ok = 0; break; }
            if (ok) H5ZC_PASS("[group] one whole unit rewritten, the others intact");
            else H5ZC_FAIL("[group] data wrong after whole-unit rewrite");
        }
        H5Sclose(usp); H5Sclose(msp); H5Sclose(fsp); H5Dclose(d); free(zeros); free(rd);
    }

    /* never-written units read as the fill value */
    {
        const hsize_t dims[3] = { NZ, NY, NX };
        hid_t space = H5Screate_simple(3, dims, NULL), dcpl = H5Pcreate(H5P_DATASET_CREATE);
        float fv = -7.5f, *rd = (float *)malloc(sizeof(float) * N);
        H5Pset_chunk(dcpl, 3, slab);
        H5Pset_fill_value(dcpl, H5T_NATIVE_FLOAT, &fv);
        H5Pset_comp(dcpl, "noop", "{\"h5z:group\":\"all\"}");
        hid_t d = H5Dcreate2(file, "group_unwritten", H5T_NATIVE_FLOAT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
        int ok = d >= 0 && H5Dread(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rd) >= 0;
        for (size_t i = 0; ok && i < N; i++) if (rd[i] != fv) ok = 0;
        if (ok) H5ZC_PASS("[group] unwritten dataset reads as the fill value");
        else H5ZC_FAIL("[group] unwritten dataset");
        if (d >= 0) H5Dclose(d);
        H5Pclose(dcpl); H5Sclose(space); free(rd);
    }

    /* bad specs fail at H5Dcreate */
    {
        h5zc_quiet(1);
        hid_t a = make_dset(file, "group_bad_rank", slab, "noop", "{\"h5z:group\":\"2,1\"}");
        hid_t b = make_dset(file, "group_bad_key", slab, "noop", "{\"h5z:grupo\":\"all\"}");
        h5zc_quiet(0);
        if (a < 0 && b < 0) H5ZC_PASS("[group] wrong rank and unknown h5z: key rejected at H5Dcreate");
        else H5ZC_FAIL("[group] bad group spec accepted (rank:%s key:%s)", a < 0 ? "rejected" : "ACCEPTED", b < 0 ? "rejected" : "ACCEPTED");
        if (a >= 0) H5Dclose(a);
        if (b >= 0) H5Dclose(b);
    }

    /* a config too big for 256 cd_values goes to the blob; a reader that has
     * never seen it gets it from init(), with no H5Pget_comp_json step */
    {
        char *big = padded_json("\"h5z:group\":\"all\"", 4096);
        hid_t d = make_dset(file, "group_blob", slab, "noop", big);
        float *rd = (float *)malloc(sizeof(float) * N);
        if (d < 0 || H5Dwrite(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, w) < 0) H5ZC_FAIL("[group] blob-config write");
        else {
            hid_t dcpl = H5Dget_create_plist(d);
            size_t bsz = 0;
            H5Pget_filter_blob(dcpl, 0, 0, NULL, &bsz);
            H5Pclose(dcpl); H5Dclose(d); d = -1;
            H5Fflush(file, H5F_SCOPE_GLOBAL);
            /* a second, independent open of the file */
            hid_t f2 = H5Fopen(fname, H5F_ACC_RDONLY, H5P_DEFAULT);
            hid_t d2 = H5Dopen2(f2, "group_blob", H5P_DEFAULT);
            memset(rd, 0, sizeof(float) * N);
            if (bsz == strlen(big) + 1 && d2 >= 0 && H5Dread(d2, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rd) >= 0 &&
                memcmp(w, rd, sizeof(float) * N) == 0)
                H5ZC_PASS("[group] 4 KiB config in the blob (%zu B), read back with a plain H5Dread", bsz);
            else H5ZC_FAIL("[group] blob-config read (blob %zu B, want %zu)", bsz, strlen(big) + 1);
            if (d2 >= 0) H5Dclose(d2);
            H5Fclose(f2);
        }
        if (d >= 0) H5Dclose(d);
        free(big); free(rd);
    }
}

/* ---- [progressive] ------------------------------------------------------- */
#define PZ 64
#define PY 64
#define PX 64
#define PN ((size_t)PZ * PY * PX)

static double rmse_planes(const float *a, const float *b, int k0, int k1) {
    double s = 0;
    for (size_t i = (size_t)k0 * PY * PX; i < (size_t)k1 * PY * PX; i++) { double e = (double)a[i] - (double)b[i]; s += e * e; }
    return sqrt(s / (double)((size_t)(k1 - k0) * PY * PX));
}

static void test_progressive(hid_t file) {
    if (!h5zc_have_codec("sperr")) { H5ZC_SKIPMSG("[progressive] sperr not in this LibPressio build"); return; }
    const hsize_t dims[3] = { PZ, PY, PX }, chunk[3] = { 4, PY, PX };   /* 16 storage chunks */
    float *w = (float *)malloc(sizeof(float) * PN), *rd = (float *)malloc(sizeof(float) * PN);
    for (int k = 0; k < PZ; k++) for (int j = 0; j < PY; j++) for (int i = 0; i < PX; i++) {
        double x = i / (double)PX, y = j / (double)PY, z = k / (double)PZ;
        w[((size_t)k * PY + j) * PX + i] = (float)(sin(9 * x) * cos(7 * y) * sin(5 * z + 0.3) + 0.25 * sin(40 * x * y) * cos(31 * z));
    }
    hid_t space = H5Screate_simple(3, dims, NULL), dxpl = H5Pcreate(H5P_DATASET_XFER);

    for (int variant = 0; variant < 2; variant++) {
        const char *name = variant ? "prog_4units" : "prog_1unit";
        const char *json = variant
            ? "{\"sperr:mode_str\":\"pwe\",\"sperr:tolerance\":1e-4,\"h5z:group\":\"4,1,1\",\"h5z:progressive\":true}"
            : "{\"sperr:mode_str\":\"pwe\",\"sperr:tolerance\":1e-4,\"h5z:group\":\"all\",\"h5z:progressive\":true}";
        hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
        H5Pset_chunk(dcpl, 3, chunk);
        h5zc_quiet(1);
        hid_t d = H5Pset_comp(dcpl, "sperr", json) < 0 ? -1 : H5Dcreate2(file, name, H5T_NATIVE_FLOAT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
        h5zc_quiet(0);
        H5Pclose(dcpl);
        if (d < 0) { H5ZC_SKIPMSG("[progressive] filter built without SPERR truncation support"); break; }
        if (H5Dwrite(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, w) < 0) { H5ZC_FAIL("[progressive/%s] write", name); H5Dclose(d); continue; }
        H5Dclose(d);
        d = H5Dopen2(file, name, H5P_DEFAULT);

        if (!variant) {
            const unsigned pcts[] = { 10, 25, 50, 100 };
            double prev = 1e300; unsigned long long prev_io = 0; int ok = 1;
            for (int p = 0; p < 4; p++) {
                H5Pset_comp_progressive(dxpl, pcts[p]);
                if (H5Dread(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, rd) < 0) { ok = 0; break; }
                H5Z_comp_io_stats_t s = stats();
                double r = rmse_planes(w, rd, 0, PZ);
                H5ZC_INFO("[progressive] %3u%%: %2llu/16 chunks, %7llu of %llu B, rmse %.3e", pcts[p], s.chunks, s.bytes_io, s.bytes_stored, r);
                if (!isfinite(r) || r > prev * 1.0001 || s.bytes_io <= prev_io) ok = 0;
                if (pcts[p] < 100 && s.chunks >= 16) ok = 0;
                if (pcts[p] == 100 && (s.chunks != 16 || maxae(w, rd, PN) > 2e-4)) ok = 0;
                prev = r; prev_io = s.bytes_io;
            }
            if (ok) H5ZC_PASS("[progressive] more bytes read -> lower error; 100%% meets the bound");
            else H5ZC_FAIL("[progressive] error/bytes not monotone, or 100%% misses the bound");

            /* the next read is not served from a cached low-fidelity decode */
            if (H5Dread(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rd) >= 0 && maxae(w, rd, PN) <= 2e-4)
                H5ZC_PASS("[progressive] a plain read after a 10%% read is full fidelity");
            else H5ZC_FAIL("[progressive] plain read after progressive reads");
        } else {
            const hsize_t start[3] = { 20, 0, 0 }, count[3] = { 8, PY, PX };   /* inside unit 1 (planes 16..31) */
            H5Pset_comp_progressive_roi(dxpl, 3, start, count, 100, 25);
            if (H5Dread(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, rd) < 0) H5ZC_FAIL("[progressive] ROI read");
            else {
                H5Z_comp_io_stats_t s = stats();
                double in = rmse_planes(w, rd, 16, 32), out = rmse_planes(w, rd, 32, 64);
                /* 4 chunks for the ROI unit + 1 of 4 for each of the other three */
                if (s.chunks == 7 && maxae(w + (size_t)16 * PY * PX, rd + (size_t)16 * PY * PX, (size_t)16 * PY * PX) <= 2e-4 && out > in)
                    H5ZC_PASS("[progressive] ROI: 7 of 16 chunks; rmse %.2e in the ROI unit, %.2e outside", in, out);
                else H5ZC_FAIL("[progressive] ROI: chunks=%llu rmse in=%.3e out=%.3e", s.chunks, in, out);
            }
        }
        H5Dclose(d);
    }

    /* a dataset NOT written as progressive must refuse a progressive read */
    {
        hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
        H5Pset_chunk(dcpl, 3, chunk);
        H5Pset_comp(dcpl, "sperr", "{\"sperr:mode_str\":\"pwe\",\"sperr:tolerance\":1e-4}");
        hid_t d = H5Dcreate2(file, "prog_plain", H5T_NATIVE_FLOAT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
        H5Pclose(dcpl);
        if (d >= 0 && H5Dwrite(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, w) >= 0) {
            H5Pset_comp_progressive(dxpl, 50);
            h5zc_quiet(1);
            herr_t st = H5Dread(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, dxpl, rd);
            h5zc_quiet(0);
            if (st < 0) H5ZC_PASS("[progressive] refused on a dataset written without \"h5z:progressive\"");
            else H5ZC_FAIL("[progressive] accepted on a non-progressive dataset");
        } else H5ZC_FAIL("[progressive] plain sperr dataset");
        if (d >= 0) H5Dclose(d);
    }
    H5Pclose(dxpl); H5Sclose(space); free(w); free(rd);
}

int main(void) {
    const char *fname = "test_dset_io.h5";
    h5zc_init();
    if (!H5Zcomp_has_dset_io()) {
        printf("SKIP: filter built against an HDF5 without dataset-level filter callbacks\n");
        return H5ZC_SKIP;
    }
    float *w = field();
    hid_t file = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file < 0) { fprintf(stderr, "FATAL: cannot create %s\n", fname); return 1; }

    test_direct(file, w, "noop", "{}", 0.0);
    if (h5zc_have_codec("sz3")) test_direct(file, w, "sz3", "{\"sz3:error_bound_mode_str\":\"abs\",\"sz3:abs_error_bound\":1e-3}", 2e-3);
    else H5ZC_SKIPMSG("[direct] sz3 not in this LibPressio build");
    test_native(file, w);
    test_group(fname, file, w);
    test_progressive(file);

    H5Fclose(file);
    free(w);
    return h5zc_summary("test_dset_io");
}
