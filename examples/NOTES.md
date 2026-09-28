# H5Zcomp — what it can and can't do (v2, H5Z_class3_t)

> **Updated after the first builds (H5Z-Libpressio repo):** the inline ceiling
> is ~60 KiB, not ~256 KiB — the whole pipeline is one object-header message
> and those are capped at 64 KiB (`H5O_MESG_MAX_SIZE`). And the DXPL is **not**
> needed on write: the chunk cache usually runs the forward filter at
> `H5Dclose`/`H5Fclose`, where the DXPL is `H5P_DEFAULT`, so the filter now
> keeps a content-tagged registry of blob configs instead. A *different*
> process reading a large-config dataset calls `H5Pget_comp_json()` once
> (which registers it) and then reads normally. Details: README "Changes".

## 1. Config size — now supports arbitrarily large configs

Two paths, chosen automatically by `H5Pset_comp` based on size:

- **Small config (fits cd_values, ≤ ~60 KiB):** carried inline in `cd_values`.
  Fully **autonomous** — a plain `H5Dread` just works, because `cd_values`
  reach the decode callback. This is every normal CPU/GPU codec's options bag.
  (`ex_roundtrip.c`.)

- **Large config (> ~60 KiB — ROIBIN mask, SZ4 JIT source):** stored in the
  filter **blob** (persists in the file, self-describing), and delivered to the
  decode callback through a **DXPL property**, because the library never hands
  the blob to `filter2`. This needs light app cooperation, no HDF5 change:
  - write: `H5Pset_comp_dxpl(dxpl, json)` before `H5Dwrite`
  - read:  `H5Pget_comp_json(dcpl, &json)` (pulls it from the file) → `H5Pset_comp_dxpl(rdxpl, json)` before `H5Dread`

  (`ex_large_config.c` runs the whole flow; `ex_config_size.c` shows the
  inline→blob boundary.)

So **yes**, ROIBIN and SZ4-shaped multi-megabyte configs can be carried. The
only non-transparency is that a large-config dataset needs that 2-line DXPL
preamble before `H5Dread` — a plain `h5dump`/`h5py` read won't decode it. The
fully transparent version is the one library change: deliver the recovered
blob straight to `filter2`.

## 2. GPU codecs — always pay the device↔host copies

Not a v1/v2 limitation — **structural to every H5Z filter.** The pipeline hands
a filter a **host** buffer and requires a **host** buffer back, so a GPU codec
in a filter always does, per chunk:

```
host chunk --H2D--> device --GPU (de)compress--> device --D2H--> host bytes
```

The uncompressed chunk arrives on the host, so the H2D is unavoidable; the
result must return on the host, so the D2H is too. The **VOL** avoids this by
sitting *above* the chunk cache: it controls both endpoints, keeps data
device-resident through compression, and transfers only the (small) compressed
bytes — the uncompressed data never touches the host. Position determines
residency, and a filter's position forces host endpoints.

## Mapping to the CompVOL limitations

| # | Limitation | H5Zcomp v2 |
|---|---|---|
| 1 | host residency (device↔host copies) | **open** — filters always copy; §2 |
| 2 | synchronous per-chunk | open |
| 3 | config capped at ~1 KB `cd_values` | **closed** — inline ≤ ~60 KiB (autonomous), larger via blob+DXPL; §1 |
| 4 | compression unit = storage chunk | open |
