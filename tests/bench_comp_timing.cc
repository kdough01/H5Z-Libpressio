/* ============================================================================
 * bench_comp_timing.cc  --  the CompVOL experiment matrix, through H5Zcomp
 *
 * Port of the VOL repo's bench_filter_timing.cc (whose --libpressio backend was
 * a stub "not wired up"). Here every BENCH_COMPRESSORS entry -- sz3, zfp
 * (cpu + cuda), szx, cuszp, sperr, bzip2, noop -- runs through ONE filter,
 * H5Zcomp, with the SAME libpressio id + options JSON the VOL uses. So ratio,
 * fidelity and timing rows are codec-matched against bench_vol_timing.
 *
 * Kept identical to the VOL harness so the CSVs merge row-for-row:
 *   - bench_config.h (datasets, compressor table, xforms, bounds)  -- verbatim
 *   - bench_timing.h                                               -- verbatim
 *   - XCSV schema: the VOL's 24 columns, same order
 *   - durability: fsync after flush + after close, page-cache evict before
 *     read (BENCH_FSYNC / BENCH_SYNC_DIR / BENCH_DROP_CACHE, default on)
 *   - fidelity gate: maxae <= 2x bound, lossless => bit-exact
 *
 * Filter-specific differences (all recorded in the CSV, never hidden):
 *   - chunk_n: the VOL's "vol:chunk_n" (opts_json / VOL_COMP_CHUNK_N env)
 *     becomes the HDF5 chunk count, split slowest-dimension first. --chunk-n
 *     overrides it for every entry. chunk_n=1 = one chunk = whole dataset.
 *   - *_pjson entries (LibPressio's internal chunking) have no filter
 *     analogue and are skipped unless --include-pjson.
 *   - codec_kind "gpu" rows pay per-chunk H2D + D2H inside the filter; there
 *     is no devres arm. d2h_ms is 0 (the copy is inside write_ms).
 *   - A dataset whose single chunk exceeds 4 GiB (einspline37 at N=1) cannot
 *     be written by any filter; it is recorded as a rep=-1 row.
 *   - Entries whose codec is not in this LibPressio build are skipped (not
 *     failed) with a [skip] line.
 *
 * Usage:
 *   bench_comp_timing [--only DS[,DS]] [--comp NAME] [--chunk-n N] [--reps R]
 *                     [--h5 FILE] [--out CSV] [--ext XCSV] [--include-pjson]
 *                     [--deflate [--level L]] [--list]
 *   env: BENCH_ONLY, BENCH_COMP, BENCH_DEBUG, BENCH_KEEP_H5, BENCH_CHUNKINFO,
 *        BENCH_FSYNC, BENCH_SYNC_DIR, BENCH_DROP_CACHE
 * ==========================================================================*/
#include <hdf5.h>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libpressio/libpressio.h>
#include "H5Zpressio.h"

#define BENCH_CONFIG_ENABLE_HDF5
#include "bench_config.h"
#include "bench_timing.h"

static void bench_crash_handler(int sig) {
    void *bt[64];
    int n = backtrace(bt, 64);
    fprintf(stderr, "\n*** caught signal %d ***\n", sig);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    _exit(128 + sig);
}

/* ------------------------------------------------------------------------
 * DURABILITY INSTRUMENTATION -- identical to bench_vol_timing.cc /
 * bench_filter_timing.cc, or the numbers are not comparable.
 * --------------------------------------------------------------------- */
static inline double bench_wall_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1.0e3 + (double)ts.tv_nsec * 1.0e-6;
}
struct BenchWallTimer {
    double t0;
    void   start()   { t0 = bench_wall_now_ms(); }
    double stop_ms() { return bench_wall_now_ms() - t0; }
};

static int bench_env_int(const char *name, int dflt) {
    const char *s = std::getenv(name);
    if (!s || !*s) return dflt;
    return std::atoi(s);
}
static int bench_do_fsync(void)     { return bench_env_int("BENCH_FSYNC", 1) != 0; }
static int bench_do_dropcache(void) { return bench_env_int("BENCH_DROP_CACHE", 1) != 0; }
static int bench_do_syncdir(void)   { return bench_env_int("BENCH_SYNC_DIR", 1) != 0; }
static int bench_keep_h5(void)      { return bench_env_int("BENCH_KEEP_H5", 0) != 0; }

static double bench_fsync_path(const char *path, int sync_dir) {
    BenchWallTimer t; t.start();
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "[warn] fsync open failed %s: %s\n", path, std::strerror(errno));
        return 0.0;
    }
    if (::fsync(fd) != 0)
        std::fprintf(stderr, "[warn] fsync failed %s: %s\n", path, std::strerror(errno));
    ::close(fd);
    if (sync_dir) {
        char dir[1024];
        std::snprintf(dir, sizeof(dir), "%s", path);
        char *slash = std::strrchr(dir, '/');
        if (slash) { if (slash == dir) dir[1] = '\0'; else *slash = '\0'; }
        else       { std::snprintf(dir, sizeof(dir), "."); }
        int dfd = ::open(dir, O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) { (void)::fsync(dfd); ::close(dfd); }
    }
    return t.stop_ms();
}

static double bench_evict_path(const char *path) {
    BenchWallTimer t; t.start();
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return 0.0;
    (void)::fsync(fd);
    if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) != 0)
        std::fprintf(stderr, "[warn] fadvise DONTNEED failed %s: %s\n", path, std::strerror(errno));
    ::close(fd);
    return t.stop_ms();
}

/* Same 24 columns, same order, as bench_vol_timing.cc's XCSV_HEADER. */
static const char *XCSV_HEADER =
    "dataset,compressor,codec_kind,chunk_n,rep,"
    "logical_bytes,stored_bytes,ratio,"
    "create_ms,write_ms,flush_ms,sync_ms,close_ms,csync_ms,"
    "evict_ms,open_ms,read_ms,"
    "rmse,abs_thresh,maxae,bound_ok,write_wall_ms,d2h_ms,read_wall_ms\n";

static void xcsv_failure_row(FILE *xcsv, const char *dataset, const char *comp,
                             const char *kind, int chunk_n, unsigned long long logical) {
    std::fprintf(xcsv,
        "%s,%s,%s,%d,-1,%llu,0,0,"
        "-1,-1,-1,-1,-1,-1,"
        "-1,-1,-1,"
        "-1,-1,-1,0,-1,-1,-1\n",
        dataset, comp, kind, chunk_n, logical);
    std::fflush(xcsv);
}

typedef struct { double min, max, mean, rmse, maxae; } bench_stats;

static bench_stats compute_stats(const void *orig, const void *dec, size_t nelem, bench_dtype_t dt) {
    double mn = DBL_MAX, mx = -DBL_MAX, sum = 0.0, se = 0.0, maxae = 0.0;
    for (size_t i = 0; i < nelem; ++i) {
        double o = (dt == BENCH_F64) ? ((const double *)orig)[i] : (double)((const float *)orig)[i];
        double v = (dt == BENCH_F64) ? ((const double *)dec)[i]  : (double)((const float *)dec)[i];
        if (o < mn) mn = o;
        if (o > mx) mx = o;
        sum += o;
        double e = o - v;
        se += e * e;
        double ae = std::fabs(e); if (ae > maxae) maxae = ae;
    }
    bench_stats s;
    s.min = mn; s.max = mx;
    s.mean  = sum / (double)nelem;
    s.rmse  = std::sqrt(se / (double)nelem);
    s.maxae = maxae;
    return s;
}

/* 1 if this LibPressio build provides the codec. */
static int have_codec(const char *id) {
    struct pressio *lib = pressio_instance();
    if (!lib) return 0;
    struct pressio_compressor *c = pressio_get_compressor(lib, id);
    int ok = (c != NULL);
    if (c) pressio_compressor_release(c);
    pressio_release(lib);
    return ok;
}

static int is_pjson(const bench_compressor_t *c) {
    return c->opts_json && std::strstr(c->opts_json, "\"vol:chunking_mode\":\"pressio\"");
}

/* Chunk layout: slowest-first split into ~chunk_n pieces (same rule as the VOL
 * repo's bench_filter_timing so the two filter harnesses agree). */
static void chunk_layout(const bench_dataset_t *d, int chunk_n, hsize_t *chunk) {
    hsize_t dims[BENCH_MAX_RANK];
    bench_dataset_h5dims(d, dims);
    for (int i = 0; i < d->rank; ++i) chunk[i] = dims[i];
    if (chunk_n > 1) {
        hsize_t remaining = (hsize_t)chunk_n;
        for (int i = 0; i < d->rank && remaining > 1; ++i) {
            hsize_t split = (dims[i] < remaining) ? dims[i] : remaining;
            if (split < 1) split = 1;
            chunk[i]  = (dims[i] + split - 1) / split;
            if (chunk[i] < 1) chunk[i] = 1;
            remaining = (remaining + split - 1) / split;
        }
    }
    size_t itemsz = bench_dtype_size(d->dtype);
    unsigned long long celems = 1ULL, nch = 1ULL;
    for (int i = 0; i < d->rank; ++i) {
        celems *= (unsigned long long)chunk[i];
        nch    *= (unsigned long long)((dims[i] + chunk[i] - 1) / chunk[i]);
    }
    unsigned long long cbytes = celems * (unsigned long long)itemsz;
    std::fprintf(stdout, "CHUNKINFO dataset=%s chunk_n=%d nchunks_actual=%llu "
                 "chunk_elems=%llu chunk_bytes=%llu\n", d->name, chunk_n, nch, celems, cbytes);
    std::fflush(stdout);
    const char *cipath = std::getenv("BENCH_CHUNKINFO");
    if (cipath && *cipath) {
        FILE *cf = std::fopen(cipath, "a");
        if (cf) { std::fprintf(cf, "%s,%d,%llu,%llu,%llu\n", d->name, chunk_n, nch, celems, cbytes); std::fclose(cf); }
    }
}

/* c == NULL => deflate baseline (different codec; overhead comparison only). */
static hid_t make_dcpl(const bench_dataset_t *d, const bench_compressor_t *c,
                       int chunk_n, int deflate_level) {
    hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
    hsize_t chunk[BENCH_MAX_RANK];
    chunk_layout(d, chunk_n, chunk);
    H5Pset_chunk(dcpl, d->rank, chunk);

    /* Match the VOL: no fill-value writes, allocate late. */
    H5Pset_fill_time(dcpl, H5D_FILL_TIME_NEVER);
    H5Pset_alloc_time(dcpl, H5D_ALLOC_TIME_LATE);

    if (!c) {
        H5Pset_deflate(dcpl, (unsigned)deflate_level);
        return dcpl;
    }

    /* Exactly the options JSON the VOL harness hands the connector (zfp rate
     * entries get zfp:type / zfp:dims spliced in). The VOL-only "vol:*" keys
     * ride along; LibPressio ignores keys it does not own. */
    char opts[2048];
    bench_compressor_opts_json(c, d, opts, sizeof(opts));
    const char *oj = (std::strcmp(opts, "{}") == 0) ? NULL : opts;
    if (std::getenv("BENCH_DEBUG"))
        std::fprintf(stderr, "[dbg dcpl] %s/%s id=%s chunk_n=%d opts=%s\n",
                     d->name, c->name, c->pressio_id, chunk_n, oj ? oj : "(default)");

    if (H5Pset_comp(dcpl, c->pressio_id ? c->pressio_id : "noop", oj) < 0) {
        std::fprintf(stderr, "[ERR] H5Pset_comp(%s) failed\n", c->name);
        H5Pclose(dcpl);
        return H5I_INVALID_HID;
    }
    return dcpl;
}

static int run_one(const bench_dataset_t *din, const bench_compressor_t *c, const char *h5path,
                   int chunk_n, int rep, int deflate_level, FILE *csv, FILE *xcsv) {
    const char *cname = c ? c->name : "deflate";
    const char *kind  = c ? bench_codec_kind_name(c->kind) : "cpu";

    bench_dataset_t d;
    size_t raw = 0;
    void *hbuf = bench_load_field(din, &d, &raw);   /* resolves HDF5-sourced dims */
    if (!hbuf) {
        std::fprintf(stderr, "[skip] %s: load failed\n", din->name);
        xcsv_failure_row(xcsv, din->name, cname, kind, chunk_n, 0ULL);
        return -1;
    }
    if (d.xform != BENCH_XFORM_NONE)     /* same preprocessing as the VOL harness */
        bench_apply_xform(hbuf, bench_num_elements(&d), d.dtype, d.xform);

    double range = 0.0;
    {
        size_t ne = bench_num_elements(&d);
        double mn = DBL_MAX, mx = -DBL_MAX;
        for (size_t i = 0; i < ne; ++i) {
            double v = (d.dtype == BENCH_F64) ? ((const double *)hbuf)[i] : (double)((const float *)hbuf)[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        range = mx - mn;
    }

    void *rbuf = std::malloc(raw);
    if (!rbuf) { std::fprintf(stderr, "[skip] %s: OOM rbuf\n", d.name); std::free(hbuf); return -1; }

    hsize_t dims[BENCH_MAX_RANK];
    bench_dataset_h5dims(&d, dims);
    hid_t ntype = bench_dataset_h5native(&d);
    hid_t ftype = bench_dataset_h5type(&d);

    const int    lossless = c ? c->lossless : 1;
    const double thr      = c ? bench_abs_threshold(c, &d, range) : 0.0;
    const int    do_fsync = bench_do_fsync();
    const int    do_evict = bench_do_dropcache();
    const int    do_sdir  = bench_do_syncdir();

    double create_ms = 0, write_ms = 0, flush_ms = 0, sync_ms = 0;
    double close_ms = 0, csync_ms = 0, evict_ms = 0, open_ms = 0, read_ms = 0;
    double write_wall_ms = 0, read_wall_ms = 0;
    unsigned long long stored = 0, filesize = 0;
    int rc = 0;

    /* ---------------- WRITE ---------------- */
    {
        BenchCpuTimer ct; ct.start();
        hid_t file = H5Fcreate(h5path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
        create_ms = ct.stop_ms();
        if (file < 0) { std::fprintf(stderr, "[ERR] H5Fcreate %s\n", h5path); rc = -1; goto cleanup; }

        hid_t space = H5Screate_simple(d.rank, dims, NULL);
        hid_t dcpl  = make_dcpl(&d, c, chunk_n, deflate_level);
        if (dcpl < 0) { H5Sclose(space); H5Fclose(file); rc = -1; goto cleanup; }
        hid_t dset  = H5Dcreate2(file, d.name, ftype, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
        if (dset < 0) {
            std::fprintf(stderr, "[ERR] H5Dcreate2 %s/%s N=%d -- chunk >4GiB, or codec "
                                 "rejected by can_apply?\n", d.name, cname, chunk_n);
            H5Pclose(dcpl); H5Sclose(space); H5Fclose(file); rc = -1; goto cleanup;
        }

        BenchCpuTimer wt; wt.start();
        BenchWallTimer ww; ww.start();
        herr_t wret = H5Dwrite(dset, ntype, H5S_ALL, H5S_ALL, H5P_DEFAULT, hbuf);
        write_wall_ms = ww.stop_ms();
        write_ms = wt.stop_ms();

        BenchWallTimer ft; ft.start();
        (void)H5Fflush(file, H5F_SCOPE_LOCAL);
        flush_ms = ft.stop_ms();
        if (do_fsync) sync_ms = bench_fsync_path(h5path, do_sdir);

        stored = (unsigned long long)H5Dget_storage_size(dset);  /* == VOL metric */
        H5Dclose(dset); H5Pclose(dcpl); H5Sclose(space);

        BenchWallTimer clt; clt.start();
        H5Fclose(file);
        close_ms = clt.stop_ms();
        if (do_fsync) csync_ms = bench_fsync_path(h5path, do_sdir);

        if (wret < 0) { std::fprintf(stderr, "[ERR] H5Dwrite %s/%s\n", d.name, cname); rc = -1; goto cleanup; }
    }

    /* ---------------- READ (cold cache) ---------------- */
    {
        std::memset(rbuf, 0, raw);
        if (do_evict) evict_ms = bench_evict_path(h5path);

        BenchWallTimer ot; ot.start();
        hid_t file = H5Fopen(h5path, H5F_ACC_RDONLY, H5P_DEFAULT);
        open_ms = ot.stop_ms();
        if (file < 0) { std::fprintf(stderr, "[ERR] H5Fopen %s\n", h5path); rc = -1; goto cleanup; }
        H5Fget_filesize(file, (hsize_t *)&filesize);

        hid_t dset = H5Dopen2(file, d.name, H5P_DEFAULT);
        if (dset < 0) { std::fprintf(stderr, "[ERR] H5Dopen2 %s\n", d.name); H5Fclose(file); rc = -1; goto cleanup; }

        BenchCpuTimer rt; rt.start();
        BenchWallTimer rw; rw.start();
        herr_t rret = H5Dread(dset, ntype, H5S_ALL, H5S_ALL, H5P_DEFAULT, rbuf);
        read_wall_ms = rw.stop_ms();
        read_ms = rt.stop_ms();

        H5Dclose(dset); H5Fclose(file);
        if (rret < 0) { std::fprintf(stderr, "[ERR] H5Dread %s/%s\n", d.name, cname); rc = -1; goto cleanup; }
    }

    /* ---------------- FIDELITY + EMIT ---------------- */
    {
        size_t nelem = bench_num_elements(&d);
        bench_stats st = compute_stats(hbuf, rbuf, nelem, d.dtype);
        double ratio = stored ? (double)raw / (double)stored : 0.0;
        int bound_ok = (thr > 0.0) ? (st.maxae <= 2.0 * thr) : (st.maxae == 0.0);
        if (c && !bench_bound_is_checkable(c)) bound_ok = 1;   /* rate mode: reported, not judged */

        if (lossless && st.maxae != 0.0)
            std::fprintf(stderr, "[FAIL] %s/%s declared lossless but maxae=%.6e\n", d.name, cname, st.maxae);
        else if (thr > 0.0 && st.maxae > 2.0 * thr)
            std::fprintf(stderr, "[FAIL] %s/%s maxae=%.6e exceeds 2x bound %.6e\n", d.name, cname, st.maxae, thr);

        bench_csv_row(csv, d.name, cname, "h5zcomp", "write", "total", write_ms, ratio, -1.0);
        bench_csv_row(csv, d.name, cname, "h5zcomp", "write", "flush", flush_ms, -1.0, -1.0);
        bench_csv_row(csv, d.name, cname, "h5zcomp", "write", "sync",  sync_ms,  -1.0, -1.0);
        bench_csv_row(csv, d.name, cname, "h5zcomp", "write", "close", close_ms, -1.0, -1.0);
        bench_csv_row(csv, d.name, cname, "h5zcomp", "write", "csync", csync_ms, -1.0, -1.0);
        bench_csv_row(csv, d.name, cname, "h5zcomp", "read",  "evict", evict_ms, -1.0, -1.0);
        bench_csv_row(csv, d.name, cname, "h5zcomp", "read",  "total", read_ms,  -1.0, st.rmse);

        std::fprintf(xcsv,
            "%s,%s,%s,%d,%d,%llu,%llu,%.4f,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%.4f,%.4f,%.4f,"
            "%.6e,%.6e,%.6e,%d,%.4f,%.4f,%.4f\n",
            d.name, cname, kind, chunk_n, rep,
            (unsigned long long)raw, stored, ratio,
            create_ms, write_ms, flush_ms, sync_ms, close_ms, csync_ms,
            evict_ms, open_ms, read_ms,
            st.rmse, thr, st.maxae, bound_ok, write_wall_ms, 0.0, read_wall_ms);
        std::fflush(xcsv);

        std::printf("  %-14s %-16s N=%-5d r=%d C=%6.2f W=%9.2f F=%8.2f S=%8.2f "
                    "X=%7.2f S2=%7.2f | E=%6.2f O=%6.2f R=%9.2f ms  "
                    "ratio=%7.2fx  file=%.1f MiB  RMSE=%.3e maxae=%.3e%s\n",
                    d.name, cname, chunk_n, rep, create_ms, write_ms, flush_ms,
                    sync_ms, close_ms, csync_ms, evict_ms, open_ms, read_ms,
                    ratio, filesize / (1024.0 * 1024.0), st.rmse, st.maxae,
                    (d.xform != BENCH_XFORM_NONE) ? " (log-space)" : "");
        std::fflush(stdout);
    }

cleanup:
    if (rc != 0) {
        xcsv_failure_row(xcsv, d.name, cname, kind, chunk_n, (unsigned long long)raw);
        std::fprintf(stderr, "[RECORDED FAILURE] %s/%s N=%d\n", d.name, cname, chunk_n);
    }
    std::free(rbuf);
    std::free(hbuf);
    return rc;
}

static int name_selected(const char *sel, const char *name) {
    if (!sel || !*sel) return 1;
    /* comma-separated exact match */
    std::string s(sel);
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find(',', pos);
        if (e == std::string::npos) e = s.size();
        if (s.compare(pos, e - pos, name) == 0 && std::strlen(name) == e - pos) return 1;
        pos = e + 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGSEGV, bench_crash_handler);
    signal(SIGABRT, bench_crash_handler);
    const char *h5path   = "bench_comp.h5";
    const char *csvpath  = "results_h5zcomp.csv";
    const char *xcsvpath = NULL;
    const char *only_cmp = std::getenv("BENCH_COMP");
    const char *only_ds  = std::getenv("BENCH_ONLY");
    int chunk_override = 0, deflate = 0, deflate_level = 6, reps = 1, include_pjson = 0, list = 0;
    int nfail = 0, nskip = 0, nrun = 0;

    for (int i = 1; i < argc; ++i) {
        if      (!std::strcmp(argv[i], "--deflate"))       deflate = 1;
        else if (!std::strcmp(argv[i], "--include-pjson")) include_pjson = 1;
        else if (!std::strcmp(argv[i], "--list"))          list = 1;
        else if (!std::strcmp(argv[i], "--comp")    && i + 1 < argc) only_cmp = argv[++i];
        else if (!std::strcmp(argv[i], "--only")    && i + 1 < argc) only_ds  = argv[++i];
        else if (!std::strcmp(argv[i], "--out")     && i + 1 < argc) csvpath  = argv[++i];
        else if (!std::strcmp(argv[i], "--ext")     && i + 1 < argc) xcsvpath = argv[++i];
        else if (!std::strcmp(argv[i], "--h5")      && i + 1 < argc) h5path   = argv[++i];
        else if (!std::strcmp(argv[i], "--chunk-n") && i + 1 < argc) chunk_override = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--reps")    && i + 1 < argc) reps = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--level")   && i + 1 < argc) deflate_level = std::atoi(argv[++i]);
        else { std::fprintf(stderr, "unknown argument '%s' (see header of bench_comp_timing.cc)\n", argv[i]); return 2; }
    }
    if (reps < 1) reps = 1;

    if (H5Z_register_comp() < 0 || H5Zfilter_avail(H5Z_FILTER_COMP) <= 0) {
        std::fprintf(stderr, "FATAL: H5Zcomp filter %d not available\n", H5Z_FILTER_COMP);
        return 1;
    }

    if (list) {
        std::printf("%-18s %-7s %-5s %-6s %-9s %s\n", "compressor", "id", "kind", "chunkN", "available", "opts");
        for (int ci = 0; ci < BENCH_NUM_COMPRESSORS; ++ci) {
            const bench_compressor_t *c = &BENCH_COMPRESSORS[ci];
            std::printf("%-18s %-7s %-5s %-6d %-9s %s%s\n", c->name, c->pressio_id,
                        bench_codec_kind_name(c->kind), bench_effective_chunk_n(c),
                        have_codec(c->pressio_id) ? "yes" : "NO",
                        c->opts_json ? c->opts_json : "{}", is_pjson(c) ? "   [pjson: skipped by default]" : "");
        }
        bench_datasets_validate();
        return 0;
    }

    char xdefault[512];
    if (!xcsvpath) { std::snprintf(xdefault, sizeof(xdefault), "%s.ext.csv", csvpath); xcsvpath = xdefault; }

    std::fprintf(stderr, "[h5zcomp] backend=%s filter_id=%d chunk_n=%s reps=%d\n",
                 deflate ? "deflate(baseline)" : "H5Zcomp", H5Z_FILTER_COMP,
                 chunk_override ? std::to_string(chunk_override).c_str() : "per-entry (vol:chunk_n, default 1)",
                 reps);
    std::fprintf(stderr,
        "[h5zcomp] durability: fsync=%d sync_dir=%d drop_cache=%d "
        "(BENCH_FSYNC / BENCH_SYNC_DIR / BENCH_DROP_CACHE) -- these MUST match "
        "the VOL run or the two are not comparable\n",
        bench_do_fsync(), bench_do_syncdir(), bench_do_dropcache());
    if (deflate)
        std::fprintf(stderr, "NOTE: deflate is a DIFFERENT codec from the VOL entries. "
                             "Use for the overhead claim only, never ratio or fidelity.\n");
    if (std::getenv("BENCH_DEBUG")) bench_datasets_validate();

    FILE *csv = std::fopen(csvpath, "w");
    if (!csv) { std::perror("csv"); return 1; }
    bench_csv_header(csv);
    FILE *xcsv = std::fopen(xcsvpath, "w");
    if (!xcsv) { std::perror("xcsv"); std::fclose(csv); return 1; }
    std::fputs(XCSV_HEADER, xcsv);

    const size_t mem_avail = bench_mem_available_bytes();

    for (int di = 0; di < BENCH_NUM_DATASETS; ++di) {
        const bench_dataset_t *d = &BENCH_DATASETS[di];
        if (!name_selected(only_ds, d->name)) continue;
        if (access(d->path, R_OK) != 0) {
            std::fprintf(stderr, "[skip] %s: %s not readable\n", d->name, d->path);
            continue;
        }
        if (d->src != BENCH_SRC_HDF5 && mem_avail) {
            size_t need = 2 * bench_num_bytes(d);
            if (need > (size_t)(0.9 * (double)mem_avail)) {
                std::fprintf(stderr, "[SKIP] %s needs %.1f GiB, %.1f GiB available\n",
                             d->name, bench_gib(need), bench_gib(mem_avail));
                continue;
            }
        }

        if (deflate) {
            int n = chunk_override > 0 ? chunk_override : 1;
            for (int r = 0; r < reps; ++r) {
                nrun++;
                if (run_one(d, NULL, h5path, n, r, deflate_level, csv, xcsv) != 0) nfail++;
            }
        } else {
            for (int ci = 0; ci < BENCH_NUM_COMPRESSORS; ++ci) {
                const bench_compressor_t *c = &BENCH_COMPRESSORS[ci];
                if (!name_selected(only_cmp, c->name)) continue;
                if (is_pjson(c) && !include_pjson) continue;
                if (!have_codec(c->pressio_id)) {
                    std::fprintf(stderr, "[skip] %s: codec '%s' not in this LibPressio build\n",
                                 c->name, c->pressio_id);
                    nskip++;
                    continue;
                }
                int n = chunk_override > 0 ? chunk_override : bench_effective_chunk_n(c);
                for (int r = 0; r < reps; ++r) {
                    nrun++;
                    if (run_one(d, c, h5path, n, r, deflate_level, csv, xcsv) != 0) nfail++;
                }
            }
        }
        if (!bench_keep_h5()) std::remove(h5path);
    }

    std::fclose(csv);
    std::fclose(xcsv);
    std::fprintf(stderr, "wrote %s and %s (%d run, %d failed configuration(s) recorded, %d codec skip(s))\n",
                 csvpath, xcsvpath, nrun, nfail, nskip);
    if (nrun == 0) return 77;   /* nothing reachable: ctest reports SKIPPED */
    return nfail ? 2 : 0;       /* non-zero so the PBS script notices */
}
