# H5Z-Libpressio

A LibPressio-backed HDF5 filter (`H5Zcomp`, filter id 33010, placeholder). One filter works with any LibPressio compressor, CPU or GPU (sz3, zfp, szx, sperr, bzip2, cuszp, cusz, nvcomp, …), and uses the RFC-HDFG-2026 filter **blob** so codec configs can be larger than `cd_values` allows.

This repo is the filter-side twin of [`vol-external-passthrough`](../vol-external-passthrough). The tests and benchmarks mirror that repo's tests and benchmarks, so the VOL and filter results can be compared row for row.

> **Requires the blob-branch HDF5.** It builds against `brtnfld/hdf5`, branch `blob` (developed at `e77c5db26d`). A stock HDF5 release does not have `H5Z_class3_t` or the filter blob API, and configure will stop with a message saying so.

## Layout

```
src/H5Z_comp.cc          the filter (moved from hdf5@blobComp:H5Zcomp/)
include/H5Zpressio.h     public API: H5Pset_comp, H5Pset_comp_dxpl, H5Pget_comp_json, H5Z_register_comp,
                         H5Pset_comp_native_buffer, H5Pset_comp_progressive[_roi], H5Pset_comp_direct
examples/                ex_roundtrip, ex_config_size, ex_large_config, NOTES.md (moved as-is)
                         ex_native_gpu, ex_decoupled_chunks, ex_progressive (dataset-level path, see below)
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
| — | `test_dset_io.c` | `ci.dset_io` | The dataset-level path: `[direct]` either path reads the other's files; `[native]` buffer used in place, unservable layouts refused; `[group]` unit ≠ chunk, hyperslabs, ragged edges, blob config with no app help; `[progressive]` fewer chunks read, error falls with the percentage, ROI. Skipped (77) on an HDF5 without `dset_write`/`dset_read`. |
| — | `test_large_config.c` | `ci.large_config` | Config-size paths: inline-vs-blob routing, a 32 KiB inline config (past the 256-value getter limit), the blob surviving the file byte for byte, and round-trips with noop and sz3. |

Every test exits with code 77 (reported by ctest as *skipped*) when an SDRBench path isn't reachable. Codecs missing from your LibPressio build are skipped, not failed. On a laptop, that means `ctest -L ci` runs whatever codecs you have.

### Chunking and how it compares to the VOL

The VOL compresses a whole dataset, or `vol:chunk_n` slabs. A filter compresses HDF5 **chunks**, so:

- The tests default to **one chunk = the whole dataset**. Set `H5ZCOMP_CHUNK_N=n` to split into n chunks.
- `bench_comp_timing` maps each entry's `vol:chunk_n` (or the `VOL_COMP_CHUNK_N` env var) to the HDF5 chunk count, splitting the slowest dimension first. `--chunk-n N` overrides it for all entries.
- `*_pjson` entries use LibPressio's internal chunking, which has no filter analogue. They're skipped unless you pass `--include-pjson`.
- A single chunk over 4 GiB (einspline37 at N=1) can't be written by the per-chunk filter. It's recorded as a `rep=-1` row, as in the VOL repo's filter runs. (With `"h5z:group"` the unit is striped over smaller chunks instead; see the next section. `bench_comp_timing` does not use it yet.)
- GPU codecs pay a host→device and device→host copy for every chunk inside the per-chunk filter (see `examples/NOTES.md` §2). So compare them against the VOL's `rtrip`/`hostsim` arms, not `devres`. The device-resident counterpart is `H5Pset_comp_native_buffer` (next section), which the benchmark harness does not exercise yet.

## Dataset-level path: native GPU buffers, decoupled chunks, progressive reads

Three things the VOL adaptor does and a per-chunk filter cannot. They need **one addition to the blob branch** (in `kdough01/hdf5-blob-extensions`, branch `blob-dset-io`): `H5Z_class3_t` gains two optional callbacks, `dset_write` and `dset_read`. When a chunked dataset's pipeline is a single filter that defines them, `H5Dwrite`/`H5Dread` offer the whole request to the filter *before* the library touches the buffer or splits the selection into chunks. The filter returns 1 (done), 0 (decline: the normal per-chunk path runs) or -1. It stores and fetches data with the existing `H5Dwrite_chunk` / `H5Dread_chunk`, so **the file format does not change** and everything still lives in the dataset's own chunks.

CMake reports which HDF5 it found (`dataset-level callbacks ... ENABLED`), and `H5Zcomp_has_dset_io()` reports it at run time. Against an HDF5 without the callbacks, the filter builds and behaves exactly as before, and `ci.dset_io` and the three examples skip.

```bash
# build the patched HDF5 instead of upstream blob
HDF5_BLOB_REPO=https://github.com/kdough01/hdf5-blob-extensions HDF5_BLOB_BRANCH=blob-dset-io \
HDF5_BLOB_SRC=$HOME/src/hdf5-blob-dset-io bash scripts/build_hdf5_blob.sh $HOME/sw/hdf5-blob-dset-io origin/blob-dset-io
```

The callbacks engage **only when asked**: a dataset with `"h5z:group"`, or a DXPL carrying one of the properties below. Every other request is declined, so the existing tests and benchmark rows go through the per-chunk filter unchanged.

| What | How you ask | Example |
|---|---|---|
| **Native (GPU) buffer.** `H5Dwrite`/`H5Dread` take a device pointer. The codec works on it in place in the named LibPressio domain. Only compressed bytes cross to the host. | `H5Pset_comp_native_buffer(dxpl, "cudamalloc")` | `ex_native_gpu.c` |
| **Compression unit ≠ storage chunk.** One unit spans several storage chunks. Its stream is striped, in order, across those chunks. | `"h5z:group":"4,1,1"` in the options JSON (`0` = every chunk along that dimension, `"all"` = whole dataset) | `ex_decoupled_chunks.c` |
| **Progressive read.** Read and decode only the first *p*% of each unit's stream. With striping, that means reading the first storage chunks of each unit. | write with `"h5z:progressive":true` (sperr), read with `H5Pset_comp_progressive(dxpl, p)` or `H5Pset_comp_progressive_roi(...)` | `ex_progressive.c` |
| Ordinary host I/O through the same path (compress straight from the caller's buffer inside `H5Dwrite`, no chunk cache). Off by default. | `H5Pset_comp_direct(dxpl, 1)` or `H5ZCOMP_DIRECT=1` | `test_dset_io.c [direct]` |

`H5Zcomp_get_io_stats()` reports what the last transfer did: units, storage chunks touched, bytes read from the file, and bytes staged through host memory (0 when every unit was coded in place).

**The `"h5z:"` keys are part of the filter's config.** They ride in the same options JSON as the codec's own options, inline in `cd_values` or in the blob, and are stripped before LibPressio sees the rest. Nothing new is stored in the file. A config with `h5z:` keys that needs more than 256 `cd_values` goes to the blob, and the filter's `init` callback reads it back, so such a dataset needs no `H5Pget_comp_json` step in any reader.

**On disk.**
- *Ungrouped* datasets written through this path are byte-compatible with the per-chunk path (one stream per chunk). Either path reads the other's files, and so does an HDF5 without the callbacks.
- *Grouped* datasets: each member chunk of a unit holds a 40+16·rank byte header and its slice of the unit's stream. They are readable only where the callbacks exist. Elsewhere the per-chunk filter refuses them with a message; it never returns wrong data. `h5dump`, `h5diff` and `h5repack` work with the plugin on `HDF5_PLUGIN_PATH` (checked).
- No stored chunk exceeds 4 GiB however large the unit is, so a unit is no longer capped by the chunk limit.

**Limits.**
- Native buffers: memory type must equal the dataset type, whole units must be selected, and each unit must be one contiguous block of the buffer (chunks or units spanning all but the first dimension). Anything else is refused with a message, never dereferenced.
- Grouped datasets: a write must cover whole units (a whole unit can be rewritten on its own; part of one is refused). Reads take any `all`/hyperslab selection; point selections are refused. Do not resize after writing. H5Zcomp must be the only filter in the pipeline. `H5Dread_multi` with more than one dataset and VDS source datasets do not reach the callbacks.
- Progressive reads need SPERR at build time (`pkg-config SPERR`) and 3-D units. The read granularity is one storage chunk: use many small storage chunks per unit. On an *ungrouped* progressive dataset the whole chunk is still read and only the decode is shortened.
- **Not run on a GPU yet.** The native path was exercised here with the host domain (`"malloc"`) standing in for a device. For a device buffer the codec receives a non-owning `cudamalloc` input (compress) or output (decompress) pointing at the caller's memory. A codec that works in that domain (cuszp calls `make_readable`/`make_writeable` for `cudamalloc`) uses it in place. A codec that answers in a host buffer of its own (any CPU codec) is routed through LibPressio's `malloc>cudamalloc` send. This follows LibPressio's domain API as read from its source, but it needs `ctest -L gpu` (`gpu.ex_native_cuszp`) on Swing before it counts as working.

Numbers from the examples on synthetic fields (a sandbox run, not SDRBench):

```
ex_decoupled_chunks sz3 abs=1e-3, 64x96x96 f32      chunks  units   stored    ratio
  A  4-plane chunks, per chunk                          16     16   52383 B   45.0x
  B  4-plane chunks, "h5z:group":"all"                  16      1   10715 B  220.2x
  C  4-plane chunks, "h5z:group":"4,1,1"                16      4   18725 B  126.0x
  D  one chunk (whole dataset)                           1      1    9307 B  253.5x

ex_progressive sperr pwe=1e-4, 128^3 f32, 32 storage chunks, 1 unit
   5%   2/32 chunks    6.3% of stored bytes   rmse 6.6e-03
  25%   8/32 chunks   25.0%                   rmse 8.8e-04
 100%  32/32 chunks  100.0%                   rmse 3.6e-05   max err 1.0e-04
  ROI (4 units x 8 chunks): 11/32 chunks, 34.4% of bytes; bound met in the ROI unit
```

## Comparing against the VOL experiments

`jobs/vol_compare.pbs` reruns the VOL's experiments through the filter, and `scripts/plot_vol_vs_filter.py` puts the two side by side. Each phase mirrors one job in `vol-external-passthrough/tests`:

| phase | VOL job (run used) | filter side |
|---|---|---|
| `matrix` | `bench_vol_timing.pbs` (52888): 6 datasets × 15 codecs, native and `vol_N8`/`vol_N16` | same matrix at N = 1, 8, 16, per-chunk path and dataset-level path (`--path direct`) |
| `decouple` | the same run's native vs `vol_N` rows | N = 1..32 storage chunks: one unit per chunk vs one unit over all of them (`--group all`) |
| `residency` | `device_resident.pbs` (`device_resident_tests`): GPU codecs × arms `devres`/`rtrip`/`hostsim` | the same three arms (`--arm`); `devres` hands `H5Dwrite` a `cudaMalloc` pointer through `H5Pset_comp_native_buffer` |
| `progressive` | `run_progressive.sh` (53500 scale-T, 53492/53488 s3d) | the same datasets, codecs, percentages and reps (`--progressive`), one unit striped over 98 / 100 chunks |

```bash
export HDF5_BLOB=$HOME/sw/hdf5-blob-dset-io; source scripts/env.sh   # + the SPERR exports
qsub -V jobs/vol_compare.pbs                                  # all four phases (12 h)
qsub -V -v PHASES=residency jobs/vol_compare.pbs              # or one at a time
```

Results land in `results/vs_vol_<jobid>/`. The job ends by running the plot script against the VOL result folders, which default to `$HOME/hdf5_projs/vol-external-passthrough/tests/results/...`; override them with `VOL_RESULTS`, `VOL_MATRIX`, `VOL_DEVRES` and `VOL_PROG`. The figures and a `summary.md` of the numbers behind them go to `results/vs_vol_<jobid>/plots/`:

- `1_ratio_parity`: filter ratio vs VOL ratio at N=1.
- `2_matrix_time`: filter ÷ VOL end-to-end write and read time, per codec, both paths.
- `3_decouple_<codec>`: ratio and write time vs storage-chunk count.
- `4_residency_<codec>`: write time in the three arms.
- `5_progressive`: bytes read, read time and RMSE vs requested percentage.

Times are wall clock. Write is end to end (create through the final fsync), so codec work done at `H5Fclose` is counted; read is open plus read on a cold cache. Make sure `BENCH_FSYNC`, `BENCH_DROP_CACHE` and `BENCH_SYNC_DIR` match the VOL runs (all on by default).

`results/vs_vol_baseline_sep2026/plots/` is the same script run on what existed before this job: the VOL runs above plus the September per-chunk sweep (`results/final`). Every series that needs the dataset-level path is marked "not run" there.

The script also works by hand, with any subset of inputs:

```bash
python3 scripts/plot_vol_vs_filter.py --vol-matrix <vol>/tests/results/20260811_140354_52888 \
    --vol-devres <vol>/tests/results/device_resident_tests --vol-prog <vol>/tests/results/prog_20260818_140442_53500 \
    --filter results/vs_vol_<jobid> --filter-baseline results/final --out plots/
```

`bench_comp_timing` flags behind the job: `--arm hostsim|devres|rtrip`, `--path chunk|direct`, `--group SPEC`, `--progressive 5,10,... --prog-csv FILE`. The ext CSV keeps the VOL's 24 columns and appends `arm,path,group,units,chunks_io,bytes_io,bytes_staged`. `devres`/`rtrip` need `-DUSE_CUDA=ON`; `BENCH_DEVRES_HOST=1` runs `devres` on a host buffer to check the plumbing.

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
