#!/usr/bin/env python3
"""
assemble_final.py -- build ONE curated filter result set from several runs.

    python3 scripts/assemble_final.py results/MANIFEST.txt

MANIFEST.txt, one run per line (later lines override earlier ones for the
same (dataset, compressor, chunk_n)); '#' starts a comment:

    h5zcomp_miranda      exclude=^zfp_gpu_     # pre-fix zfp CUDA rows are wrong
    zfpgpu_miranda                             # the fixed zfp_gpu rows

Each <run> is a folder under results/ holding results_h5zcomp_ext.csv. An
optional exclude=<regex> drops rows whose compressor matches it.

Writes results/final/:
    results_h5zcomp_ext.csv   the curated rows (one per key)
    SOURCES.csv               which run every row came from
    MANIFEST.txt              copy of the manifest used
Standard library only.
"""
import csv
import os
import re
import shutil
import sys


def main(manifest):
    root = os.path.dirname(os.path.abspath(manifest))
    out_dir = os.path.join(root, "final")
    os.makedirs(out_dir, exist_ok=True)

    rows, source, header = {}, {}, None
    for n, raw in enumerate(open(manifest), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        run, excl = parts[0], None
        for p in parts[1:]:
            if p.startswith("exclude="):
                excl = re.compile(p[len("exclude="):])
        path = os.path.join(root, run, "results_h5zcomp_ext.csv")
        if not os.path.exists(path):
            sys.exit(f"{manifest}:{n}: missing {path}")
        kept = dropped = 0
        with open(path, newline="") as f:
            rd = csv.DictReader(f)
            header = header or rd.fieldnames
            for r in rd:
                if excl and excl.search(r["compressor"]):
                    dropped += 1
                    continue
                k = (r["dataset"], r["compressor"], r["chunk_n"], r["rep"])
                rows[k] = r
                source[k] = run
                kept += 1
        print(f"{run:32s} kept {kept:4d}  excluded {dropped:3d}")

    out = os.path.join(out_dir, "results_h5zcomp_ext.csv")
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=header)
        w.writeheader()
        for k in sorted(rows):
            w.writerow(rows[k])
    with open(os.path.join(out_dir, "SOURCES.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["dataset", "compressor", "chunk_n", "rep", "run"])
        for k in sorted(source):
            w.writerow(list(k) + [source[k]])
    shutil.copy(manifest, os.path.join(out_dir, "MANIFEST.txt"))

    fails = sum(1 for k in rows if k[3] == "-1")
    print(f"\nwrote {out}: {len(rows)} rows ({fails} recorded failures)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
