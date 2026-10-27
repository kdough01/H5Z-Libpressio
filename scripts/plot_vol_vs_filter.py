#!/usr/bin/env python3
"""
plot_vol_vs_filter.py -- the VOL experiments vs the same experiments through H5Zcomp.

    python3 scripts/plot_vol_vs_filter.py \
        --vol-matrix  <vol>/tests/results/20260811_140354_52888 \
        --vol-devres  <vol>/tests/results/device_resident_tests \
        --vol-prog    <vol>/tests/results/prog_20260818_140442_53500 [--vol-prog ...] \
        --filter      results/vs_vol_<jobid>        (jobs/vol_compare.pbs output; repeatable)
        [--filter-baseline results/final]           (the Sep 2026 per-chunk sweep)
        --out         results/vs_vol_<jobid>/plots

Every input is optional; a figure is drawn from whatever matches, and a series
with no data is listed as "not run" in its panel rather than silently missing.

Figures (PNG + PDF), and summary.md with the numbers behind them:
  1_ratio_parity       filter ratio vs VOL ratio, one compression unit (N=1)
  2_matrix_time        filter / VOL end-to-end write and read time, per codec,
                       N=1 and N=8, per-chunk path vs dataset-level path
  3_decouple_<codec>   ratio and write time vs number of storage chunks:
                       VOL (native, vol_N8/N16), filter one-unit-per-chunk,
                       filter one unit over all chunks ("h5z:group":"all")
  4_residency_<codec>  write time in the three residency arms, VOL vs filter
  5_progressive        bytes read, read time and RMSE vs requested percentage

Times are wall clock (steady_clock in both harnesses). "write" is the
end-to-end write: H5Fcreate .. H5Dwrite .. flush .. fsync .. H5Fclose .. fsync
(create+write+flush+sync+close+csync), so a codec that runs at H5Fclose is not
missed; "read" is open+read on a cold cache. Reps are averaged.

Needs numpy + matplotlib (python3 -m pip install --user numpy matplotlib).
"""
import argparse
import csv
import glob
import math
import os
import re
import sys
from collections import defaultdict

try:
    import numpy as np
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D
except ImportError as e:  # pragma: no cover
    sys.exit(f"needs numpy + matplotlib ({e}); python3 -m pip install --user numpy matplotlib")

# ---------------------------------------------------------------------------
# style: one fixed hue per system, the same in every figure
# ---------------------------------------------------------------------------
C_VOL, C_CHUNK, C_DIRECT = "#2a78d6", "#eb6834", "#1baf7a"   # VOL / per-chunk filter / dataset-level filter
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e4e3df"
SERIES = {
    "vol":    dict(color=C_VOL,    label="VOL (CompVOL)", marker="o"),
    "chunk":  dict(color=C_CHUNK,  label="filter, per-chunk path", marker="s"),
    "direct": dict(color=C_DIRECT, label="filter, dataset-level path", marker="D"),
    "group":  dict(color=C_DIRECT, label='filter, one unit over all chunks ("h5z:group":"all")', marker="D"),
}
plt.rcParams.update({
    "font.size": 9, "axes.titlesize": 9.5, "axes.labelsize": 9, "legend.fontsize": 8,
    "axes.edgecolor": INK2, "axes.labelcolor": INK, "xtick.color": INK2, "ytick.color": INK2,
    "text.color": INK, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6,
    "axes.spines.top": False, "axes.spines.right": False, "lines.linewidth": 2,
    "lines.markersize": 5, "figure.dpi": 150, "savefig.bbox": "tight", "legend.frameon": False,
})
DS_ORDER = ["miranda", "s3d", "scale-T", "nyx_baryon", "ocean_temp", "einspline37"]
DS_LABEL = {"einspline37": "QMC (einspline37)", "nyx_baryon": "NYX baryon", "ocean_temp": "ocean temp"}
ARMS = ["devres", "rtrip", "hostsim"]


def f(x):
    try:
        v = float(x)
        return v if math.isfinite(v) else None
    except (TypeError, ValueError):
        return None


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None


def gmean(xs):
    xs = [x for x in xs if x is not None and x > 0]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else None


def write_total(r):
    parts = [f(r.get(k)) for k in ("create_ms", "write_ms", "flush_ms", "sync_ms", "close_ms", "csync_ms")]
    return sum(p for p in parts if p is not None and p >= 0) if all(p is not None for p in parts) else None


def read_total(r):
    o, rd = f(r.get("open_ms")), f(r.get("read_ms"))
    return (o or 0.0) + rd if rd is not None else None


def family(comp):
    return comp.split("_")[0] + ("_gpu" if "_gpu_" in comp else "_cpu" if "_cpu_" in comp else "")


# ---------------------------------------------------------------------------
# loading
# ---------------------------------------------------------------------------
class Store:
    """rows keyed by (system, dataset, compressor, N, arm); values averaged over reps."""

    def __init__(self):
        self.raw = defaultdict(list)
        self.fail = []
        self.sources = defaultdict(set)

    def add(self, system, r, n, arm="hostsim", src=""):
        key = (system, r["dataset"], r["compressor"], int(n), arm)
        if r.get("rep") == "-1":
            self.fail.append(key)
            return
        self.raw[key].append({
            "ratio": f(r.get("ratio")), "write": write_total(r), "read": read_total(r),
            "write_call": f(r.get("write_ms")), "rmse": f(r.get("rmse")), "maxae": f(r.get("maxae")),
            "stored": f(r.get("stored_bytes")), "logical": f(r.get("logical_bytes")),
        })
        self.sources[system].add(src)

    def get(self, system, ds, comp, n, arm="hostsim", field="ratio"):
        rs = self.raw.get((system, ds, comp, int(n), arm))
        return mean([r[field] for r in rs]) if rs else None

    def keys(self, system=None):
        return [k for k in self.raw if system is None or k[0] == system]


VOL_SUFFIX = re.compile(r"_(vol|pressio)_N(\d+)$")


def load_vol_matrix(store, d):
    for p in sorted(glob.glob(os.path.join(d, "results_ext_*.csv"))):
        label = os.path.basename(p)[len("results_ext_"):-4]
        if "_pjson" in label or "_vjson" in label or "_hostout" in label:
            continue
        m = VOL_SUFFIX.search(label)
        if m and m.group(1) == "pressio":
            continue                      # LibPressio-internal chunking: no filter analogue
        n = int(m.group(2)) if m else 1
        for r in csv.DictReader(open(p, newline="")):
            store.add("vol", r, n, src=d)


def load_vol_devres(store, d):
    for p in sorted(glob.glob(os.path.join(d, "results_ext_*.csv"))):
        label = os.path.basename(p)[len("results_ext_"):-4]
        arm = label.rsplit("_", 1)[-1]
        if arm not in ARMS or label.startswith("probe_"):
            continue
        for r in csv.DictReader(open(p, newline="")):
            store.add("vol", r, 1, arm=arm, src=d)


def load_filter(store, d, baseline=False):
    paths = [os.path.join(d, "results_h5zcomp_ext.csv")] if baseline else \
        sorted(glob.glob(os.path.join(d, "results_ext_*.csv")))
    for p in paths:
        if not os.path.exists(p):
            continue
        label = os.path.basename(p)
        for r in csv.DictReader(open(p, newline="")):
            if r.get("compressor", "").endswith(("_vjson", "_pjson")):
                continue
            arm = r.get("arm") or "hostsim"
            path = r.get("path") or "chunk"
            group = r.get("group") or ""
            system = "group" if group else path
            if "progressive_" in label:
                system = "prog_write"     # the write half of a progressive run
            store.add(system, r, int(r["chunk_n"]), arm=arm, src=d)


def load_prog(path, system):
    """progressive CSV (VOL run_progressive.sh format, or the filter's) -> {(ds,comp): {pct: [rows]}}"""
    out = defaultdict(lambda: defaultdict(list))
    ds_default = comp_default = None
    d = os.path.dirname(path)
    m = re.match(r"progressive_(?:(.+?)_)?(sperr_\w+)\.csv$", os.path.basename(path))
    if m:
        comp_default = m.group(2)
        ds_default = m.group(1)
    if ds_default is None:   # VOL: dataset is in the run's results_ext files
        for x in glob.glob(os.path.join(d, "results_ext_*p100*.csv")):
            for r in csv.DictReader(open(x, newline="")):
                ds_default = r["dataset"]
                break
            break
    for r in csv.DictReader(open(path, newline="")):
        ds = r.get("dataset") or ds_default
        comp = r.get("compressor") or comp_default
        out[(ds, comp)][int(r["pct"])].append(r)
    return out


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
def save(fig, out, name, made):
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(out, f"{name}.{ext}"))
    plt.close(fig)
    made.append(name)


def note_missing(ax, names):
    if names:
        ax.text(0.98, 0.02, "not run: " + ", ".join(names), transform=ax.transAxes,
                ha="right", va="bottom", fontsize=7, color=INK2, style="italic")


def ds_sorted(dss):
    return sorted(dss, key=lambda d: (DS_ORDER.index(d) if d in DS_ORDER else 99, d))


def smart_log(ax, axis="y"):
    """log scale only when the data span more than 3x; plain tick labels either way."""
    from matplotlib.ticker import ScalarFormatter, NullFormatter
    vals = []
    for ln in ax.get_lines():
        d = ln.get_ydata() if axis == "y" else ln.get_xdata()
        vals += [v for v in np.asarray(d, dtype=float) if np.isfinite(v) and v > 0]
    if vals and max(vals) / min(vals) > 3:
        (ax.set_yscale if axis == "y" else ax.set_xscale)("log")
    fmt = ScalarFormatter(useOffset=False)
    fmt.set_scientific(False)
    target = ax.yaxis if axis == "y" else ax.xaxis
    target.set_major_formatter(fmt)
    target.set_minor_formatter(NullFormatter())


def legend_handles(keys):
    return [Line2D([0], [0], color=SERIES[k]["color"], marker=SERIES[k]["marker"], lw=2,
                   label=SERIES[k]["label"]) for k in keys]


# ---------------------------------------------------------------------------
# figure 1: ratio parity
# ---------------------------------------------------------------------------
def fig_ratio_parity(S, out, made, md):
    fig, ax = plt.subplots(figsize=(4.6, 4.2))
    lo, hi, missing = 1e9, 0, []
    rows = []
    for sysname in ("chunk", "direct"):
        xs, ys = [], []
        for (s, ds, comp, n, arm) in S.keys(sysname):
            if n != 1 or arm != "hostsim":
                continue
            v, fl = S.get("vol", ds, comp, 1), S.get(sysname, ds, comp, 1)
            if v and fl:
                xs.append(v); ys.append(fl); rows.append((sysname, ds, comp, v, fl))
        if not xs:
            missing.append(SERIES[sysname]["label"])
            continue
        lo, hi = min(lo, *xs, *ys), max(hi, *xs, *ys)
        ax.scatter(xs, ys, s=22, color=SERIES[sysname]["color"], marker=SERIES[sysname]["marker"],
                   edgecolor="white", linewidth=0.6, label=f'{SERIES[sysname]["label"]} ({len(xs)})', zorder=3)
    if hi > 0:
        ax.plot([lo / 1.5, hi * 1.5], [lo / 1.5, hi * 1.5], color=INK2, lw=1, ls="--", zorder=1)
        ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_xlabel("VOL compression ratio"); ax.set_ylabel("filter compression ratio")
    ax.set_title("Same codec, same unit (N=1): ratio parity")
    ax.legend(loc="upper left")
    note_missing(ax, missing)
    save(fig, out, "1_ratio_parity", made)
    if rows:
        dev = [abs(fl / v - 1) for (_, _, _, v, fl) in rows]
        md.append(f"## 1. Ratio parity (N=1)\n\n{len(rows)} pairs; median |filter/VOL - 1| = "
                  f"{sorted(dev)[len(dev)//2]*100:.2f}%, worst {max(dev)*100:.2f}%.\n\n")
        worst = sorted(rows, key=lambda t: -abs(t[4] / t[3] - 1))[:5]
        md.append("| path | dataset | codec | VOL ratio | filter ratio |\n|---|---|---|---|---|\n" +
                  "".join(f"| {a} | {b} | {c} | {d:.2f} | {e:.2f} |\n" for a, b, c, d, e in worst) + "\n")


# ---------------------------------------------------------------------------
# figure 2: time ratios over the matrix
# ---------------------------------------------------------------------------
def fig_matrix_time(S, out, made, md):
    comps = sorted({k[2] for k in S.keys("vol") if k[4] == "hostsim"},
                   key=lambda c: (family(c), c))
    Ns = [1, 8]
    fig, axes = plt.subplots(len(Ns), 2, figsize=(11, 3.3 * len(Ns)), sharey=True, squeeze=False)
    md.append("## 2. Filter / VOL time (geometric mean over datasets)\n\n"
              "| N | codec | write chunk | write direct | read chunk | read direct |\n|---|---|---|---|---|---|\n")
    table = defaultdict(dict)
    for i, n in enumerate(Ns):
        for j, metric in enumerate(("write", "read")):
            ax = axes[i][j]
            missing = []
            for k, sysname in enumerate(("chunk", "direct")):
                xs, ys, any_pt = [], [], False
                for ci, comp in enumerate(comps):
                    vals = []
                    for ds in {k2[1] for k2 in S.keys("vol") if k2[2] == comp}:
                        v, fl = S.get("vol", ds, comp, n, field=metric), S.get(sysname, ds, comp, n, field=metric)
                        if v and fl:
                            vals.append(fl / v)
                    if vals:
                        any_pt = True
                        x = ci + (k - 0.5) * 0.32
                        ax.scatter([x] * len(vals), vals, s=10, color=SERIES[sysname]["color"],
                                   alpha=0.55, linewidth=0, zorder=2)
                        g = gmean(vals)
                        ax.plot([x - 0.13, x + 0.13], [g, g], color=SERIES[sysname]["color"], lw=2.4, zorder=3)
                        table[(n, comp)][f"{metric} {sysname}"] = g
                if not any_pt:
                    missing.append(SERIES[sysname]["label"])
            ax.axhline(1, color=INK2, lw=1, ls="--", zorder=1)
            ax.set_yscale("log")
            ax.set_xticks(range(len(comps)))
            ax.set_xticklabels(comps, rotation=60, ha="right", fontsize=7)
            ax.set_title(f"{metric} time, filter / VOL  (N={n})")
            if j == 0:
                ax.set_ylabel("filter / VOL  (below 1 = filter faster)")
            note_missing(ax, missing)
    fig.legend(handles=legend_handles(["chunk", "direct"]) +
               [Line2D([0], [0], color=INK2, lw=2.4, label="bar = geometric mean, dots = datasets")],
               loc="upper center", ncol=3, bbox_to_anchor=(0.5, 1.02))
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    save(fig, out, "2_matrix_time", made)
    for (n, comp), d in sorted(table.items()):
        g = lambda k: f"{d[k]:.2f}" if k in d else "-"
        md.append(f"| {n} | {comp} | {g('write chunk')} | {g('write direct')} | {g('read chunk')} | {g('read direct')} |\n")
    md.append("\n")


# ---------------------------------------------------------------------------
# figure 3: decoupling
# ---------------------------------------------------------------------------
def fig_decouple(S, out, made, md, comps_hint):
    comps = [c for c in comps_hint if any(k[2] == c for k in S.keys("vol"))]
    md.append("## 3. Decoupling: ratio vs number of storage chunks\n\n")
    for comp in comps:
        dss = ds_sorted({k[1] for k in S.keys() if k[2] == comp and k[4] == "hostsim"})
        if not dss:
            continue
        fig, axes = plt.subplots(2, len(dss), figsize=(2.55 * len(dss), 5.2), squeeze=False)
        md.append(f"**{comp}**\n\n| dataset | N | VOL ratio | filter per-chunk | filter group=all |\n|---|---|---|---|---|\n")
        for j, ds in enumerate(dss):
            missing = []
            for i, metric in enumerate(("ratio", "write")):
                ax = axes[i][j]
                for sysname in ("vol", "chunk", "group"):
                    pts = sorted((k[3], S.get(sysname, ds, comp, k[3], field=metric))
                                 for k in S.keys(sysname) if k[1] == ds and k[2] == comp and k[4] == "hostsim")
                    pts = [(n, v) for n, v in pts if v]
                    if not pts:
                        if i == 0 and sysname != "vol":
                            missing.append({"chunk": "per-chunk", "group": "group=all"}[sysname])
                        continue
                    ax.plot([p[0] for p in pts], [p[1] for p in pts], color=SERIES[sysname]["color"],
                            marker=SERIES[sysname]["marker"], ls="-" if len(pts) > 1 else "none",
                            markeredgecolor="white", markeredgewidth=0.6)
                ax.set_xscale("log", base=2)
                ax.set_xticks([1, 2, 4, 8, 16, 32])
                ax.get_xaxis().set_major_formatter(matplotlib.ticker.ScalarFormatter())
                smart_log(ax)
                if i == 0:
                    ax.set_title(DS_LABEL.get(ds, ds))
                    note_missing(ax, missing)
                else:
                    ax.set_xlabel("storage chunks (N)")
                if j == 0:
                    ax.set_ylabel("compression ratio" if i == 0 else "write time (ms, end to end)")
            for n in sorted({k[3] for k in S.keys() if k[1] == ds and k[2] == comp and k[4] == "hostsim"}):
                g = lambda s: (lambda v: f"{v:.2f}" if v else "-")(S.get(s, ds, comp, n))
                md.append(f"| {ds} | {n} | {g('vol')} | {g('chunk')} | {g('group')} |\n")
        fig.legend(handles=legend_handles(["vol", "chunk", "group"]), loc="upper center", ncol=3,
                   bbox_to_anchor=(0.5, 1.03))
        fig.suptitle(f"{comp}: the VOL decouples the compression unit from storage; "
                     f'"h5z:group" does the same in the filter', y=1.07, fontsize=10)
        fig.tight_layout()
        save(fig, out, f"3_decouple_{comp}", made)
        md.append("\n")


# ---------------------------------------------------------------------------
# figure 4: residency arms
# ---------------------------------------------------------------------------
def arm_write(S, system, ds, comp, arm):
    if system == "vol":
        return S.get("vol", ds, comp, 1, arm, "write")
    # einspline37 cannot be one HDF5 chunk: the job runs it as one unit over 4 chunks
    return S.get("chunk", ds, comp, 1, arm, "write") or S.get("group", ds, comp, 4, arm, "write")


def fig_residency(S, out, made, md):
    comps = sorted({k[2] for k in S.keys("vol") if k[4] in ("devres", "rtrip")})
    if not comps:
        return
    md.append("## 4. Residency arms: end-to-end write time (ms)\n\n"
              "| codec | dataset | arm | VOL | filter | filter / VOL |\n|---|---|---|---|---|---|\n")
    for comp in comps:
        dss = ds_sorted({k[1] for k in S.keys() if k[2] == comp and k[4] in ARMS})
        fig, axes = plt.subplots(1, len(dss), figsize=(2.3 * len(dss), 3.0), squeeze=False)
        for j, ds in enumerate(dss):
            ax = axes[0][j]
            missing = []
            top = max([arm_write(S, s2, ds, comp, a2) or 0 for s2 in ("vol", "filter") for a2 in ARMS] + [1])
            for k, (sysname, label) in enumerate((("vol", "VOL"), ("filter", "filter"))):
                vals = [arm_write(S, sysname, ds, comp, arm) for arm in ARMS]
                xs = np.arange(len(ARMS)) + (k - 0.5) * 0.38
                cols = [C_VOL] * 3 if sysname == "vol" else [C_DIRECT, C_CHUNK, C_CHUNK]
                ax.bar(xs, [v or 0 for v in vals], width=0.36, color=cols,
                       edgecolor="white", linewidth=1.0, zorder=2)
                for x, v in zip(xs, vals):
                    if not v:
                        ax.text(x, top * 0.02, "n/a", ha="center", va="bottom", fontsize=6.5,
                                color=INK2, rotation=90)
                if not any(vals):
                    missing.append(label)
            for arm in ARMS:
                v, fl = arm_write(S, "vol", ds, comp, arm), arm_write(S, "filter", ds, comp, arm)
                if v or fl:
                    fmt = lambda x: f"{x:.1f}" if x else "-"
                    md.append(f"| {comp} | {ds} | {arm} | {fmt(v)} | {fmt(fl)} | "
                              f"{(f'{fl / v:.2f}' if v and fl else '-')} |\n")
            ax.set_xticks(range(len(ARMS)))
            ax.set_xticklabels(ARMS)
            ax.set_title(DS_LABEL.get(ds, ds))
            ax.grid(axis="x", visible=False)
            if j == 0:
                ax.set_ylabel("write time (ms, end to end)")
        fig.legend(handles=[Line2D([0], [0], color=C_VOL, lw=6, label="VOL"),
                            Line2D([0], [0], color=C_DIRECT, lw=6, label="filter, native-buffer path (devres)"),
                            Line2D([0], [0], color=C_CHUNK, lw=6, label="filter, per-chunk path (rtrip, hostsim)")],
                   loc="upper center", ncol=3, bbox_to_anchor=(0.5, 1.06))
        fig.suptitle(f"{comp}: devres = device buffer kept on the GPU; rtrip = app copies D2H first; "
                     "hostsim = host buffer", y=1.12, fontsize=9.5)
        fig.tight_layout()
        save(fig, out, f"4_residency_{comp}", made)
    md.append("\n")


# ---------------------------------------------------------------------------
# figure 5: progressive
# ---------------------------------------------------------------------------
def fig_progressive(vol_prog, filt_prog, out, made, md):
    keys = sorted(set(vol_prog) | set(filt_prog), key=lambda k: (k[0] or "", k[1] or ""))
    if not keys:
        return
    fig, axes = plt.subplots(len(keys), 3, figsize=(11, 2.9 * len(keys)), squeeze=False)
    md.append("## 5. Progressive reads\n\n| dataset | codec | pct | VOL bytes read | filter bytes read | "
              "VOL read ms | filter read ms | VOL RMSE | filter RMSE |\n|---|---|---|---|---|---|---|---|---|\n")
    for i, key in enumerate(keys):
        missing = []
        series = {}
        for name, src in (("vol", vol_prog), ("direct", filt_prog)):
            if key not in src:
                missing.append(SERIES[name]["label"] if name == "vol" else "filter")
                continue
            pcts = sorted(src[key])
            agg = lambda col: [mean([f(r[col]) for r in src[key][p]]) for p in pcts]
            lo = lambda col: [min(f(r[col]) for r in src[key][p]) for p in pcts]
            hi = lambda col: [max(f(r[col]) for r in src[key][p]) for p in pcts]
            frac = [b / s if b and s else None for b, s in zip(agg("bytes_read"), agg("stored_bytes"))]
            series[name] = dict(pcts=pcts, frac=frac, t=agg("read_wall_ms"), tlo=lo("read_wall_ms"),
                                thi=hi("read_wall_ms"), rmse=agg("rmse"))
        for name, s in series.items():
            st = SERIES[name]
            lbl = "VOL" if name == "vol" else 'filter ("h5z:group":"all" + "h5z:progressive")'
            kw = dict(color=st["color"], marker=st["marker"], markeredgecolor="white", markeredgewidth=0.6, label=lbl)
            axes[i][0].plot(s["pcts"], [100 * x if x else None for x in s["frac"]], **kw)
            axes[i][1].plot(s["pcts"], s["t"], **kw)
            axes[i][1].fill_between(s["pcts"], s["tlo"], s["thi"], color=st["color"], alpha=0.15, linewidth=0)
            axes[i][2].plot(s["pcts"], s["rmse"], **kw)
        axes[i][0].plot([0, 100], [0, 100], color=INK2, lw=1, ls="--")
        axes[i][0].set_ylabel(f"{DS_LABEL.get(key[0], key[0])}, {key[1]}\n\n% of stored bytes read")
        axes[i][1].set_ylabel("read time (ms, cold cache)")
        axes[i][2].set_ylabel("RMSE")
        axes[i][2].set_yscale("log")
        for a in axes[i]:
            a.set_xlabel("requested percentage")
        note_missing(axes[i][2], missing)
        if "vol" in series or "direct" in series:
            pcts = sorted(set(series.get("vol", {}).get("pcts", [])) | set(series.get("direct", {}).get("pcts", [])))
            for p in pcts:
                def v(name, field):
                    s = series.get(name)
                    if not s or p not in s["pcts"]:
                        return "-"
                    x = s[field][s["pcts"].index(p)]
                    return "-" if x is None else (f"{100*x:.1f}%" if field == "frac" else
                                                 f"{x:.3e}" if field == "rmse" else f"{x:.0f}")
                md.append(f"| {key[0]} | {key[1]} | {p} | {v('vol','frac')} | {v('direct','frac')} | "
                          f"{v('vol','t')} | {v('direct','t')} | {v('vol','rmse')} | {v('direct','rmse')} |\n")
    fig.legend(handles=[Line2D([0], [0], color=C_VOL, marker="o", lw=2, label="VOL"),
                        Line2D([0], [0], color=C_DIRECT, marker="D", lw=2,
                               label='filter ("h5z:group":"all" + "h5z:progressive")'),
                        Line2D([0], [0], color=INK2, lw=1, ls="--", label="bytes read = requested %")],
               loc="upper center", ncol=3, bbox_to_anchor=(0.5, 1.01))
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    save(fig, out, "5_progressive", made)
    md.append("\n")


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vol-matrix", action="append", default=[])
    ap.add_argument("--vol-devres", action="append", default=[])
    ap.add_argument("--vol-prog", action="append", default=[])
    ap.add_argument("--filter", action="append", default=[])
    ap.add_argument("--filter-baseline", action="append", default=[],
                    help="results/final-style folder (results_h5zcomp_ext.csv): per-chunk rows only")
    ap.add_argument("--decouple-comps", default="sz3_1e3,szx_1e3,zfp_cpu_r8,cuszp_1e3")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    S = Store()
    for d in a.vol_matrix:
        load_vol_matrix(S, d)
    for d in a.vol_devres:
        load_vol_devres(S, d)
    for d in a.filter_baseline:
        load_filter(S, d, baseline=True)
    for d in a.filter:       # later wins only in the sense of being averaged in; jobs don't overlap
        load_filter(S, d)

    vol_prog, filt_prog = {}, {}
    for d in a.vol_prog:
        for p in glob.glob(os.path.join(d, "progressive_*.csv")):
            vol_prog.update(load_prog(p, "vol"))
    for d in a.filter:
        for p in glob.glob(os.path.join(d, "progressive_*.csv")):
            filt_prog.update(load_prog(p, "direct"))

    md = ["# VOL vs H5Zcomp filter\n\n",
          "Inputs:\n\n" + "".join(f"- {k}: {', '.join(sorted(v))}\n" for k, v in sorted(S.sources.items())) +
          "".join(f"- VOL progressive: {d}\n" for d in a.vol_prog) + "\n",
          "Times are wall clock; write = create+write+flush+sync+close+csync, read = open+read (cold cache). "
          "Reps averaged.\n\n"]
    made = []
    fig_ratio_parity(S, a.out, made, md)
    fig_matrix_time(S, a.out, made, md)
    fig_decouple(S, a.out, made, md, a.decouple_comps.split(","))
    fig_residency(S, a.out, made, md)
    fig_progressive(vol_prog, filt_prog, a.out, made, md)
    if S.fail:
        md.append("## Recorded failures (rep=-1)\n\n" +
                  "".join(f"- {s}: {ds} {c} N={n} {arm}\n" for s, ds, c, n, arm in sorted(set(S.fail))) + "\n")
    with open(os.path.join(a.out, "summary.md"), "w") as fh:
        fh.write("".join(md))
    print(f"wrote {len(made)} figure(s) to {a.out}: " + ", ".join(made))
    print(f"      {os.path.join(a.out, 'summary.md')}")


if __name__ == "__main__":
    main()
