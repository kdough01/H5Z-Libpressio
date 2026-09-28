# H5Z-Libpressio

A LibPressio-backed HDF5 filter (`H5Zcomp`, filter id 33010, placeholder). One filter works with any LibPressio compressor, CPU or GPU (sz3, zfp, szx, sperr, bzip2, cuszp, cusz, nvcomp, …), and uses the RFC-HDFG-2026 filter **blob** so codec configs can be larger than `cd_values` allows.

This repo is the filter-side twin of [`vol-external-passthrough`](../vol-external-passthrough). The tests and benchmarks mirror that repo's tests and benchmarks, so the VOL and filter results can be compared row for row.

> **Requires the blob-branch HDF5.** It builds against `brtnfld/hdf5`, branch `blob` (developed at `e77c5db26d`). A stock HDF5 release does not have `H5Z_class3_t` or the filter blob API, and configure will stop with a message saying so.

## Layout

```
src/H5Z_comp.cc          the filter (moved from hdf5@blobComp:H5Zcomp/)
include/H5Zpressio.h     public API: H5Pset_comp, H5Pset_comp_dxpl, H5Pget_comp_json, H5Z_register_comp
examples/                ex_roundtrip, ex_config_size, ex_large_config, NOTES.md (moved as-is)
tests/                   ctest suite + bench_comp_timing (see table below)
jobs/                    PBS jobs for Swing: ci_swing, bench_comp_timing, chunk_ladder
scripts/                 build_hdf5_blob.sh, env.sh, compare_vol_filter.py
```

## Build on the cluster

```bash
# 1. once: build the blob HDF5 (read access to brtnfld/hdf5 is enough)
bash scripts/build_hdf5_blob.sh              # -> ~/sw/hdf5-blob-e77c5db

# 2. configure + build against it and your LibPressio spack view
source scripts/env.sh                        # edit paths at the top if yours differ
h5zc_build build                             # = cmake -DHDF5_ROOT=... -DCMAKE_PREFIX_PATH=$LP_VIEW; cmake --build

# 3. test
ctest --test-dir build -L ci   --output-on-failure    # data-free, runs anywhere
ctest --test-dir build -L data --output-on-failure    # needs SDRBench (BENCH_DATA_ROOT)
```

Or submit it as a batch job: `qsub jobs/ci_swing.pbs` (add `-v USE_CUDA=1` for the GPU tests).

`build/lib/libH5Zcomp.so` works in two ways:

- **As a plugin.** Run `export HDF5_PLUGIN_PATH=$PWD/build/lib`. Then any HDF5 reader (h5dump, h5py, your code) can decode small-config datasets with no code changes.
- **As a library.** Link it and `#include "H5Zpressio.h"` to write datasets:

```c
hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
H5Pset_chunk(dcpl, 3, chunk);
H5Pset_comp(dcpl, "sz3", "{\"sz3:error_bound_mode_str\":\"abs\",\"sz3:abs_error_bound\":1e-3}");
```

## Tests: how they map to the VOL repo

| VOL repo (`tests/`) | Here (`tests/`) | ctest | What it checks through the filter |
|---|---|---|---|
| `test_ci.c` | `test_ci.c` | `ci.roundtrip` | noop/bzip2/sz3/zfp round-trips with 1 chunk, 8 chunks, and ragged (padded edge) chunks. Rejects an unknown compressor at `H5Dcreate`. Checks the pipeline carries the filter once, with chunk dims. |
| `test_cpu_write.c` | `test_cpu_write.c` | `ci.cpu_write` | The same 7 datasets (bzip2 l5/l9, sz3 1e-3/1e-6, zfp accuracy/rate, noop). Now asserts error bounds instead of only printing values. |
| `test_error_miranda.c` | `test_error_miranda.c` | `ci.errors`, `ci.errors_chunked` | The same 5 checks, plus two filter-only ones: a blob-config dataset writes and reads with no DXPL, and bad JSON in the blob is rejected. Uses Miranda when present, otherwise a synthetic field. |
| `test_time_miranda.c` | `test_time_miranda.c` | `data.miranda_timing[_8chunks]` | The same 7 fields × (noop, cuszp, sz3 1e-3/1e-6, bzip2), timed. Fidelity is gated. |
| `test_time_hurricane.c` | `test_time_hurricane.c` | `data.hurricane_gpu` (`USE_CUDA=ON`) | The same GPU codecs (nvcomp, cusz, cuszp) on CLOUDf01. |
| `bench_filter_timing.cc` / `bench_vol_timing.cc` | `bench_comp_timing.cc` | `ci.bench_list`, `data.miranda_bench_smoke` | The full `bench_config.h` matrix through H5Zcomp. Writes the same 24-column ext CSV. |
| `bench_config.h`, `bench_timing.h` | same files, copied verbatim | | Datasets, compressor table, bounds, xforms |
| `tier2_h5repack_roundtrip.pbs` | `test_plugin_read.c` | `ci.plugin_autoload` | A reader linked against neither H5Zcomp nor LibPressio decodes the data through `HDF5_PLUGIN_PATH` (including a 32 KiB inline config), and refuses a blob config cleanly. |
| — | `test_large_config.c` | `ci.large_config` | Config-size paths: inline-vs-blob routing, a 32 KiB inline config (past the 256-value getter limit), the blob surviving the file byte for byte, and round-trips with noop and sz3. |

Every test exits with code 77 (reported by ctest as *skipped*) when an SDRBench path isn't reachable. Codecs missing from your LibPressio build are skipped, not failed. On a laptop, that means `ctest -L ci` runs whatever codecs you have.

### Chunking and how it compares to the VOL

The VOL compresses a whole dataset, or `vol:chunk_n` slabs. A filter compresses HDF5 **chunks**, so:

- The tests default to **one chunk = the whole dataset**. Set `H5ZCOMP_CHUNK_N=n` to split into n chunks.
- `bench_comp_timing` maps each entry's `vol:chunk_n` (or the `VOL_COMP_CHUNK_N` env var) to the HDF5 chunk count, splitting the slowest dimension first. `--chunk-n N` overrides it for all entries.
- `*_pjson` entries use LibPressio's internal chunking, which has no filter analogue. They're skipped unless you pass `--include-pjson`.
- A single chunk over 4 GiB (einspline37 at N=1) can't be written by any filter. It's recorded as a `rep=-1` row, as in the VOL repo's filter runs.
- GPU codecs pay a host→device and device→host copy for every chunk inside the filter (see `examples/NOTES.md` §2). So compare them against the VOL's `rtrip`/`hostsim` arms, not `devres`.

## Benchmarks

```bash
qsub jobs/bench_comp_timing.pbs                                   # the whole matrix
qsub -v BENCH_ONLY=miranda,s3d,BENCH_COMP=sz3_1e3 jobs/bench_comp_timing.pbs
qsub -v BENCH_DATASET=s3d,BENCH_COMP=sz3_1e3 jobs/chunk_ladder.pbs  # ratio/time vs chunk size

# line up against a VOL run (bench_vol_timing's ext CSV)
python3 scripts/compare_vol_filter.py <vol>/results_vol.csv.ext.csv -- results/<tag>/results_h5zcomp_ext.csv
```

The durability settings (`BENCH_FSYNC`, `BENCH_DROP_CACHE`, `BENCH_SYNC_DIR`) default to on, as in the VOL harness. They must match between the two runs, or the numbers aren't comparable.

## Changes from the in-tree version (`hdf5@blobComp` c9f4a0d)

The in-tree filter had never been compiled. Building it and running this suite against blob@e77c5db (and blob HEAD) turned up the following. All are fixed in `src/H5Z_comp.cc`, and each has a test that covers it:

| # | Problem | Effect | Fix | Covered by |
|---|---|---|---|---|
| 1 | Missing `#include <libpressio_ext/json/pressio_options_json.h>` | Did not compile | Added | build |
| 2 | `set_local` stored the **dataset extent** as the codec shape | Any dataset with more than one chunk failed on `H5Dwrite` (shape/size mismatch) | Uses `H5Pget_chunk` | `ci.roundtrip` (8chunks, ragged), `[pipeline]` check |
| 3 | `H5Pget_filter_by_id2` refuses `*cd_nelmts > 256` ("probable uninitialized"), while `H5Pset_filter` accepts 65535 | `set_local`/`can_apply` could not read the filter's own cd_values, so **every** `H5Dcreate` failed | Reads ≤256 values directly. Larger ones go through a DCPL property that `H5Pset_comp` sets and HDF5 copies into the dataset DCPL | all `ci.*`; `[mid/*]` 32 KiB config |
| 4 | Pipeline = one object-header message, capped at 64 KiB (`H5O_MESG_MAX_SIZE`) | The design's "≤256 KiB inline" limit is unreachable. Configs above ~64 KB fail at `H5Dcreate` | Inline ceiling is 60 KiB. Anything larger goes to the blob | `[routing]`, `[mid/*]` |
| 5 | Large-config path: `H5Pset_filter` **plus** `H5Pappend_filter_blob` | Filter entered the pipeline twice. `H5Pget_comp_json` read the entry that had no blob | `append_filter_blob` then `H5Pmodify_filter` on the same entry | `[routing] exactly once` |
| 6 | Blob config delivered only on the DXPL | The chunk cache runs the forward filter at `H5Dclose`/`H5Fclose`, where the context DXPL is `H5P_DEFAULT`, so **large-config writes always failed** | cd_values carry a content tag (`@fnv1a64:…`) of the blob JSON. The filter keeps a tag→JSON registry, filled by `H5Pset_comp`, `can_apply`, `H5Pset_comp_dxpl` and `H5Pget_comp_json`. A DXPL JSON that doesn't match the tag is rejected | `ci.errors` test 6, `ci.ex_large_config`, `ci.plugin_autoload` (`blob_noop` refused cleanly in a fresh process) |
| 7 | `H5Pget_comp_json` assumed pipeline index 0 | Wrong or missing blob when other filters are present | Looks the index up | `[blob/*]` |
| 8 | Bad compressor id or bad JSON only failed at the first write. Bad JSON was ignored silently | Unlike the VOL | `can_apply` builds the codec at `H5Dcreate` | `ci.errors` tests 1, 2, 7 |
| 9 | `H5Z_class3_t` layout changed on `blob` after e77c5db (description moved, `init`/`term` added, filter2 gained `state`) | Positional initializer broke | Struct filled field by field. CMake detects the layout | built and `ctest -L ci` passing on **both** e77c5db and blob HEAD (69f191c) |

One harness fix: `bench_config.h`'s `bench_compressor_chunk_n()` found the `:` inside `"vol:chunk_n"`, so every `*_vjson` entry reported `chunk_n=1`. The same line is in the VOL repo's copy, which means **the VOL XCSV `chunk_n` column is wrong for vjson rows**. The connector itself parses the JSON separately, so the VOL measurements are fine. Only the label is off. `compare_vol_filter.py` therefore joins on (dataset, compressor) by default.

### What the large-config route looks like now

- **Writer:** `H5Pset_comp(dcpl, id, big_json)` → `H5Dcreate` → `H5Dwrite`. Nothing else is needed.
- **Another process reading it:** call `H5Pget_comp_json(H5Dget_create_plist(dset), &json)` once, then `H5Dread` as normal.
- **A reader that doesn't know about H5Zcomp** (h5dump, h5py): large-config datasets fail cleanly. Configs up to ~60 KiB are fully transparent.

## Notes / next steps

- On the newer `blob` layout, the `init(file, dcpl, …, &state)` callback can read the filter blob once per dataset and hand it to filter2 as `state`. That would make large-config reads transparent for *any* reader, and the tag registry and `H5Pget_comp_json` step would go away. It's worth raising with Scot Breitenfeld, along with #3 (the 256-value getter guard) and #4 (the 64 KiB message cap versus `H5Z_MAX_CD_NELMTS`), before deciding which commit to pin.
- Register a permanent filter id with The HDF Group before publishing files.
