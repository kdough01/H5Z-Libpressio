/* ============================================================================
 * test_common.h  --  shared helpers for the H5Zcomp test suite
 *
 * The filter-side counterpart of the VOL repo's tests/miranda.h. The VOL tests
 * select a codec with DCPL properties ("pressio:compressor", "vol:options_json")
 * on a CONTIGUOUS dataset; a filter needs a CHUNKED dataset instead, so
 * h5zc_make_dcpl() builds the chunk layout and calls H5Pset_comp().
 *
 * Chunking: by default the whole dataset is ONE chunk, which is the closest
 * match to what the VOL compresses. Set H5ZCOMP_CHUNK_N=<n> to split it into n
 * chunks (slowest dimension first -- same rule as bench_comp_timing).
 *
 * Exit code 77 = "skipped" (ctest SKIP_RETURN_CODE), used when an SDRBench
 * dataset is not reachable from this machine.
 * ==========================================================================*/
#ifndef H5ZCOMP_TEST_COMMON_H
#define H5ZCOMP_TEST_COMMON_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5Zpressio.h"
#include <libpressio/libpressio.h>

#ifndef BENCH_DATA_ROOT
#define BENCH_DATA_ROOT "/lcrc/project/ECP-EZ/public/compression"
#endif

#define H5ZC_SKIP 77

#define MIRANDA_PATH   BENCH_DATA_ROOT "/Miranda/SDRBENCH-Miranda-256x384x384"
#define HURRICANE_PATH BENCH_DATA_ROOT "/Hurricane-ISABEL/nonclean-data"

#define MIR_NX    256
#define MIR_NY    384
#define MIR_NZ    384
#define MIR_NELEM ((size_t)MIR_NX * MIR_NY * MIR_NZ)

#define DEFAULT_COMPRESSOR "noop"

/* ---- pass/fail bookkeeping ------------------------------------------------ */
static int h5zc_failures = 0;
static int h5zc_skips    = 0;

#define H5ZC_PASS(...) do { printf("  PASS: "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define H5ZC_FAIL(...) do { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); h5zc_failures++; } while (0)
#define H5ZC_SKIPMSG(...) do { printf("  SKIP: "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); h5zc_skips++; } while (0)
#define H5ZC_INFO(...) do { printf("  INFO: "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static int h5zc_summary(const char *suite) {
    printf("\n----- %s: %s (%d failure%s, %d skipped) -----\n", suite,
           h5zc_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
           h5zc_failures, h5zc_failures == 1 ? "" : "s", h5zc_skips);
    return h5zc_failures ? 1 : 0;
}

/* ---- setup ---------------------------------------------------------------- */
static void h5zc_init(void) {
    if (H5Z_register_comp() < 0) {
        fprintf(stderr, "FATAL: H5Z_register_comp failed\n");
        exit(1);
    }
    if (H5Zfilter_avail(H5Z_FILTER_COMP) <= 0) {
        fprintf(stderr, "FATAL: filter %d not available after registration\n", H5Z_FILTER_COMP);
        exit(1);
    }
}

/* 1 if this LibPressio build provides compressor `id`. Lets the suite skip
 * (not fail) codecs your spack env was built without. */
static int h5zc_have_codec(const char *id) {
    struct pressio *lib = pressio_instance();
    if (!lib) return 0;
    struct pressio_compressor *c = pressio_get_compressor(lib, id);
    int ok = (c != NULL);
    if (c) pressio_compressor_release(c);
    pressio_release(lib);
    return ok;
}

static int h5zc_env_int(const char *name, int dflt) {
    const char *s = getenv(name);
    return (s && *s) ? atoi(s) : dflt;
}

static double elapsed_ms(struct timespec t0, struct timespec t1) {
    return (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
}

/* Slowest-dimension-first split into ~chunk_n chunks. */
static void h5zc_chunk_dims(int rank, const hsize_t *dims, int chunk_n, hsize_t *chunk) {
    for (int i = 0; i < rank; i++) chunk[i] = dims[i];
    if (chunk_n <= 1) return;
    hsize_t remaining = (hsize_t)chunk_n;
    for (int i = 0; i < rank && remaining > 1; i++) {
        hsize_t split = dims[i] < remaining ? dims[i] : remaining;
        if (split < 1) split = 1;
        chunk[i] = (dims[i] + split - 1) / split;
        if (chunk[i] < 1) chunk[i] = 1;
        remaining = (remaining + split - 1) / split;
    }
}

/* Chunked DCPL carrying the H5Zcomp filter. compressor NULL => "noop".
 * chunk_n <= 0 => H5ZCOMP_CHUNK_N env, default 1 (one chunk = whole dataset). */
static hid_t h5zc_make_dcpl_n(const char *compressor, const char *json_opts,
                              int rank, const hsize_t *dims, int chunk_n) {
    hsize_t chunk[H5S_MAX_RANK];
    if (chunk_n <= 0) chunk_n = h5zc_env_int("H5ZCOMP_CHUNK_N", 1);
    h5zc_chunk_dims(rank, dims, chunk_n, chunk);

    hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
    if (dcpl < 0) return H5I_INVALID_HID;
    if (H5Pset_chunk(dcpl, rank, chunk) < 0 ||
        H5Pset_comp(dcpl, compressor ? compressor : DEFAULT_COMPRESSOR, json_opts) < 0) {
        H5Pclose(dcpl);
        return H5I_INVALID_HID;
    }
    return dcpl;
}
static hid_t h5zc_make_dcpl(const char *compressor, const char *json_opts,
                            int rank, const hsize_t *dims) {
    return h5zc_make_dcpl_n(compressor, json_opts, rank, dims, 0);
}

/* Silence / restore the HDF5 error stack around calls that are EXPECTED to fail. */
static void h5zc_quiet(int on) {
    if (on) H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
    else    H5Eset_auto2(H5E_DEFAULT, (H5E_auto2_t)H5Eprint2, stderr);
}

/* ---- data --------------------------------------------------------------- */
static void *h5zc_read_raw(const char *path, size_t elem_size, size_t *out_nelem) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    if (fsize <= 0 || fsize % (long)elem_size != 0) {
        fprintf(stderr, "Bad file size %ld for %s\n", fsize, path);
        fclose(f); return NULL;
    }
    size_t nelem = (size_t)fsize / elem_size;
    void *data = malloc((size_t)fsize);
    if (!data) { fprintf(stderr, "OOM for %s\n", path); fclose(f); return NULL; }
    size_t got = fread(data, elem_size, nelem, f);
    fclose(f);
    if (got != nelem) { fprintf(stderr, "Short read %s\n", path); free(data); return NULL; }
    *out_nelem = nelem;
    return data;
}
static double *read_raw_double(const char *path, size_t *out_nelem) {
    return (double *)h5zc_read_raw(path, sizeof(double), out_nelem);
}

typedef struct { double min, max, mean, rmse, maxae; } Stats;

static Stats compute_stats(const double *orig, const double *decomp, size_t n) {
    Stats s = { orig[0], orig[0], 0.0, 0.0, 0.0 };
    double sum = 0, sse = 0;
    for (size_t i = 0; i < n; i++) {
        if (orig[i] < s.min) s.min = orig[i];
        if (orig[i] > s.max) s.max = orig[i];
        sum += orig[i];
        double diff = orig[i] - decomp[i];
        sse += diff * diff;
        if (fabs(diff) > s.maxae) s.maxae = fabs(diff);
    }
    s.mean = sum / (double)n;
    s.rmse = sqrt(sse / (double)n);
    return s;
}

static Stats compute_stats_f(const float *orig, const float *decomp, size_t n) {
    Stats s = { orig[0], orig[0], 0.0, 0.0, 0.0 };
    double sum = 0, sse = 0;
    for (size_t i = 0; i < n; i++) {
        double o = orig[i], d = decomp[i];
        if (o < s.min) s.min = o;
        if (o > s.max) s.max = o;
        sum += o;
        sse += (o - d) * (o - d);
        if (fabs(o - d) > s.maxae) s.maxae = fabs(o - d);
    }
    s.mean = sum / (double)n;
    s.rmse = sqrt(sse / (double)n);
    return s;
}

/* Smooth synthetic field used when no SDRBench data is reachable. */
static void h5zc_fill_synthetic(double *buf, int rank, const hsize_t *dims) {
    size_t n = 1;
    for (int i = 0; i < rank; i++) n *= (size_t)dims[i];
    for (size_t i = 0; i < n; i++)
        buf[i] = sin((double)i * 0.01) + 0.5 * cos((double)i * 0.0007);
}

/* Write `wbuf` into a new dataset with the given codec, reopen the file, read it
 * back into `rbuf`. Returns 0 on success; *stored gets the on-disk bytes.
 * wdxpl / rdxpl may be H5P_DEFAULT. chunk_n as in h5zc_make_dcpl_n. */
static int h5zc_roundtrip(const char *fname, const char *dsname,
                          const char *compressor, const char *json,
                          hid_t mem_type, int rank, const hsize_t *dims, int chunk_n,
                          const void *wbuf, void *rbuf,
                          hid_t wdxpl, hid_t rdxpl, hsize_t *stored) {
    int rc = -1;
    hid_t fid = H5I_INVALID_HID, sid = H5I_INVALID_HID, did = H5I_INVALID_HID, dcpl = H5I_INVALID_HID;

    fid = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (fid < 0) { fprintf(stderr, "[%s] H5Fcreate failed\n", dsname); goto done; }
    sid = H5Screate_simple(rank, dims, NULL);
    dcpl = h5zc_make_dcpl_n(compressor, json, rank, dims, chunk_n);
    if (sid < 0 || dcpl < 0) { fprintf(stderr, "[%s] dcpl/space failed\n", dsname); goto done; }
    did = H5Dcreate2(fid, dsname, mem_type, sid, H5P_DEFAULT, dcpl, H5P_DEFAULT);
    if (did < 0) { fprintf(stderr, "[%s] H5Dcreate2 failed\n", dsname); goto done; }
    if (H5Dwrite(did, mem_type, H5S_ALL, H5S_ALL, wdxpl, wbuf) < 0) {
        fprintf(stderr, "[%s] H5Dwrite failed\n", dsname); goto done;
    }
    if (stored) *stored = H5Dget_storage_size(did);
    H5Dclose(did); did = H5I_INVALID_HID;
    H5Fclose(fid); fid = H5I_INVALID_HID;

    fid = H5Fopen(fname, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (fid < 0) { fprintf(stderr, "[%s] H5Fopen failed\n", dsname); goto done; }
    did = H5Dopen2(fid, dsname, H5P_DEFAULT);
    if (did < 0) { fprintf(stderr, "[%s] H5Dopen2 failed\n", dsname); goto done; }
    if (H5Dread(did, mem_type, H5S_ALL, H5S_ALL, rdxpl, rbuf) < 0) {
        fprintf(stderr, "[%s] H5Dread failed\n", dsname); goto done;
    }
    rc = 0;
done:
    if (did >= 0)  H5Dclose(did);
    if (dcpl >= 0) H5Pclose(dcpl);
    if (sid >= 0)  H5Sclose(sid);
    if (fid >= 0)  H5Fclose(fid);
    return rc;
}

#endif /* H5ZCOMP_TEST_COMMON_H */
