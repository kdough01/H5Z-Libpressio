#!/usr/bin/env python3
"""
compare_vol_filter.py -- line up VOL and H5Zcomp benchmark rows.

    python3 scripts/compare_vol_filter.py VOL_EXT.csv [VOL_EXT2.csv ...] -- FILTER_EXT.csv [...]
    python3 scripts/compare_vol_filter.py vol.csv -- filt.csv --out compare.csv

Both inputs are the "ext" CSVs written by bench_vol_timing (VOL repo) and
bench_comp_timing (this repo). Rows are joined on (dataset, compressor,
chunk_n). Reps are averaged; rep=-1 failure rows are reported separately.
Standard library only.

VOL-side handling:
  - VOL sweeps also run each compressor with VOL_COMP_CHUNK_N=8/16 under the
    SAME compressor name; joining without chunk_n averaged those into the N=1
    row (skewed ratios/times). chunk_n is therefore part of the key.
  - VOL results from before the bench_config.h vol:chunk_n parsing fix report
    chunk_n=1 for *_vjson rows; those are corrected to 8.
  - Files matching *_pressio_N* or *_pjson* (LibPressio-internal chunking, no
    filter analogue) are skipped unless --keep-pressio.
  - --alias qmc=einspline37 renames a VOL dataset (repeatable).
"""
import csv
import sys
from collections import defaultdict

NUM = ["ratio", "write_ms", "read_ms", "rmse", "maxae", "stored_bytes"]


BY_CHUNK = True
KEEP_PRESSIO = False
ALIAS = {}


def load(paths, vol_side=False):
    import os
    rows, fails = defaultdict(list), []
    for p in paths:
        b = os.path.basename(p)
        if vol_side and not KEEP_PRESSIO and ("_pressio_N" in b or "_pjson" in b):
            continue
        with open(p, newline="") as f:
            for r in csv.DictReader(f):
                if vol_side:
                    r["dataset"] = ALIAS.get(r["dataset"], r["dataset"])
                    if r["compressor"].endswith("_vjson") and r["chunk_n"] == "1":
                        r["chunk_n"] = "8"
                key = (r["dataset"], r["compressor"]) + ((r["chunk_n"],) if BY_CHUNK else ())
                if r["rep"] == "-1":
                    fails.append(key)
                    continue
                rows[key].append(r)
    agg = {}
    for k, rs in rows.items():
        agg[k] = {c: sum(float(r[c]) for r in rs) / len(rs) for c in NUM}
        agg[k]["bound_ok"] = min(int(r["bound_ok"]) for r in rs)
        agg[k]["n"] = len(rs)
        agg[k]["chunk_n"] = "/".join(sorted({r["chunk_n"] for r in rs}))
    return agg, fails


def main(argv):
    global BY_CHUNK, KEEP_PRESSIO
    if "--no-chunk" in argv:
        BY_CHUNK = False
        argv = [a for a in argv if a != "--no-chunk"]
    argv = [a for a in argv if a != "--by-chunk"]   # default now; accepted for compatibility
    if "--keep-pressio" in argv:
        KEEP_PRESSIO = True
        argv = [a for a in argv if a != "--keep-pressio"]
    while "--alias" in argv:
        i = argv.index("--alias")
        a, b = argv[i + 1].split("=", 1)
        ALIAS[a] = b
        argv = argv[:i] + argv[i + 2:]
    out = None
    if "--out" in argv:
        i = argv.index("--out")
        out = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    if "--" not in argv:
        sys.exit(__doc__)
    i = argv.index("--")
    vol, vfail = load(argv[:i], vol_side=True)
    fil, ffail = load(argv[i + 1:])

    keys = sorted(set(vol) | set(fil))
    hdr = ["dataset", "compressor", "chunk_n",
           "vol_ratio", "filt_ratio", "ratio_filt/vol",
           "vol_write_ms", "filt_write_ms", "write_filt/vol",
           "vol_read_ms", "filt_read_ms", "read_filt/vol",
           "vol_maxae", "filt_maxae", "vol_ok", "filt_ok"]
    table = []
    for k in keys:
        v, f = vol.get(k), fil.get(k)
        def g(d, c):
            return d[c] if d else float("nan")
        def q(a, b):
            return a / b if (a == a and b == b and b) else float("nan")
        cn = k[2] if BY_CHUNK else f"{v['chunk_n'] if v else '-'}/{f['chunk_n'] if f else '-'}"
        table.append([k[0], k[1], cn,
                      g(v, "ratio"), g(f, "ratio"), q(g(f, "ratio"), g(v, "ratio")),
                      g(v, "write_ms"), g(f, "write_ms"), q(g(f, "write_ms"), g(v, "write_ms")),
                      g(v, "read_ms"), g(f, "read_ms"), q(g(f, "read_ms"), g(v, "read_ms")),
                      g(v, "maxae"), g(f, "maxae"),
                      v["bound_ok"] if v else "", f["bound_ok"] if f else ""])

    w = [12, 18, 6, 9, 9, 8, 11, 11, 8, 11, 11, 8]
    print(" ".join(h[:x].ljust(x) for h, x in zip(hdr, w)))
    for row in table:
        cells = []
        for val, x in zip(row, w):
            cells.append((f"{val:.3f}" if isinstance(val, float) else str(val))[:x].ljust(x))
        print(" ".join(cells))

    only_v = [k for k in keys if k in vol and k not in fil]
    only_f = [k for k in keys if k in fil and k not in vol]
    if only_v:
        print(f"\n{len(only_v)} config(s) only in VOL results (e.g. {only_v[0]})")
    if only_f:
        print(f"{len(only_f)} config(s) only in filter results (e.g. {only_f[0]})")
    if vfail or ffail:
        print("\nrecorded failures (rep=-1):")
        for k in sorted(set(vfail)):
            print("  VOL   ", *k)
        for k in sorted(set(ffail)):
            print("  filter", *k)

    if out:
        with open(out, "w", newline="") as fh:
            cw = csv.writer(fh)
            cw.writerow(hdr)
            cw.writerows(table)
        print(f"\nwrote {out}")


if __name__ == "__main__":
    main(sys.argv[1:])
