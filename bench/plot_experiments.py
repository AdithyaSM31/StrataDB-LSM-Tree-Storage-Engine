#!/usr/bin/env python3
"""Draws the five experiment charts in docs/BENCHMARKS.md from the CSVs in docs/results.

Usage: plot_experiments.py [--results docs/results] [--output docs/images]
Needs matplotlib.
"""

import argparse
import csv
import os
import statistics
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402
from matplotlib.ticker import FuncFormatter  # noqa: E402

# Each engine keeps its colour in every chart.
STRATA = "#2a78d6"
LEVELDB = "#eb6834"
NOMMAP = "#1baf7a"
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"
BEFORE = "#c3c2b7"

SLOWDOWN_THRESHOLD = 2000  # ops/s. Under this, a 200 ms sample is inside the L0 slowdown.
L0_SLOWDOWN_FILES = 8


def load(path):
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def compact(value, _position=None):
    if value >= 1000:
        return f"{value / 1000:g}K"
    return f"{value:g}"


def style(ax, grid_axis="y"):
    ax.set_facecolor(SURFACE)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(AXIS)
    ax.spines["bottom"].set_linewidth(1)
    ax.tick_params(colors=INK_MUTED, length=0, labelsize=9)
    ax.grid(axis=grid_axis, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)


def new_figure(title, subtitle, nrows=1, ncols=1, size=(9.5, 4.8), **kwargs):
    fig, axes = plt.subplots(nrows, ncols, figsize=size, facecolor=SURFACE, **kwargs)
    fig.text(0.015, 0.97, title, fontsize=13, fontweight="semibold", color=INK, va="top")
    fig.text(0.015, 0.915, subtitle, fontsize=9.5, color=INK_SECONDARY, va="top")
    return fig, axes


def save(fig, output, name):
    fig.savefig(os.path.join(output, name), dpi=170, facecolor=SURFACE)
    plt.close(fig)


def line(ax, xs, ys, colour, marker="o", label=None, hollow=None, size=7, zorder=3):
    ax.plot(xs, ys, color=colour, linewidth=2, solid_capstyle="round", label=label, zorder=zorder)
    for i, (x, y) in enumerate(zip(xs, ys)):
        face = SURFACE if hollow and hollow[i] else colour
        ax.plot(x, y, marker=marker, markersize=size, markerfacecolor=face, markeredgecolor=colour,
                markeredgewidth=1.6, zorder=zorder + 1, linestyle="none")


def experiment_1(results, output):
    rows = load(os.path.join(results, "experiments", "exp1_bloom.csv"))
    groups = defaultdict(list)
    for row in rows:
        groups[(row["engine"], int(row["bloom_bits"]))].append(row)
    fig, ax = new_figure(
        "Experiment 1: more bloom bits per key, faster missing-key lookups",
        "readmissing, 10M keys, warm page cache, median of 3 runs. "
        "Hollow points started with L0 files, which every lookup has to check.")
    fig.subplots_adjust(left=0.08, right=0.97, top=0.82, bottom=0.12)
    for engine, colour, marker, label in (("StrataDB", STRATA, "o", "StrataDB"),
                                          ("LevelDB-nommap", NOMMAP, "s", "LevelDB no mmap")):
        bits = sorted(b for e, b in groups if e == engine)
        ops = [statistics.median(float(r["ops_per_sec"]) for r in groups[(engine, b)]) for b in bits]
        hollow = [not groups[(engine, b)][0]["levels_at_start"].startswith("0/") for b in bits]
        line(ax, bits, ops, colour, marker=marker, label=label, hollow=hollow)
    style(ax)
    ax.set_ylim(bottom=0)
    ax.yaxis.set_major_formatter(FuncFormatter(compact))
    ax.set_xticks(sorted({b for _, b in groups}))
    ax.set_xlabel("Bloom bits per key", color=INK_SECONDARY, fontsize=9.5)
    ax.set_ylabel("ops/s", color=INK_SECONDARY, fontsize=9.5)
    handles = [
        Line2D([], [], color=STRATA, marker="o", linewidth=2, label="StrataDB"),
        Line2D([], [], color=NOMMAP, marker="s", linewidth=2, label="LevelDB no mmap"),
        Line2D([], [], color=INK_MUTED, marker="o", markerfacecolor=SURFACE, linestyle="none",
               label="Started with L0 files"),
    ]
    ax.legend(handles=handles, frameon=False, loc="upper left", fontsize=9, labelcolor=INK_SECONDARY)
    save(fig, output, "exp1_bloom.png")


def experiment_2(results, output):
    rows = load(os.path.join(results, "experiments", "exp2_group_commit.csv"))
    groups = defaultdict(list)
    for row in rows:
        groups[(row["engine"], row["group_commit"], int(row["threads"]))].append(
            float(row["ops_per_sec"]))
    fig, ax = new_figure(
        "Experiment 2: group commit scales synced writes about 6.7x at 16 threads",
        "fillsync, 100K writes in total, median of 3 runs. LevelDB has no switch, "
        "so it only appears with group commit on.")
    fig.subplots_adjust(left=0.08, right=0.97, top=0.82, bottom=0.12)
    threads = sorted({t for _, _, t in groups})
    series = (("StrataDB", "1", STRATA, "o", 8, "StrataDB, group commit on", False),
              ("LevelDB", "1", LEVELDB, "s", 4.5, "LevelDB", False),
              ("StrataDB", "0", STRATA, "o", 8, "StrataDB, group commit off", True))
    for engine, commit, colour, marker, size, label, hollow in series:
        ops = [statistics.median(groups[(engine, commit, t)]) for t in threads]
        line(ax, threads, ops, colour, marker=marker, label=label, size=size,
             hollow=[hollow] * len(threads), zorder=4 if engine == "LevelDB" else 3)
    style(ax)
    ax.set_xscale("log", base=2)
    ax.set_xticks(threads)
    ax.set_xticklabels([str(t) for t in threads])
    ax.minorticks_off()
    ax.set_ylim(bottom=0)
    ax.yaxis.set_major_formatter(FuncFormatter(compact))
    ax.set_xlabel("Writer threads", color=INK_SECONDARY, fontsize=9.5)
    ax.set_ylabel("ops/s", color=INK_SECONDARY, fontsize=9.5)
    handles = [
        Line2D([], [], color=STRATA, marker="o", linewidth=2, label="StrataDB, group commit on"),
        Line2D([], [], color=LEVELDB, marker="s", markersize=4.5, linewidth=2, label="LevelDB"),
        Line2D([], [], color=STRATA, marker="o", markerfacecolor=SURFACE, linewidth=2,
               label="StrataDB, group commit off"),
    ]
    ax.legend(handles=handles, frameon=False, loc="upper left", fontsize=9, labelcolor=INK_SECONDARY)
    save(fig, output, "exp2_group_commit.png")


def slowdown_share(rows):
    elapsed = [float(r["elapsed_sec"]) for r in rows]
    done = [int(r["ops_done"]) for r in rows]
    slow = 0.0
    for i in range(1, len(rows)):
        span = elapsed[i] - elapsed[i - 1]
        if span > 0 and (done[i] - done[i - 1]) / span < SLOWDOWN_THRESHOLD:
            slow += span
    return elapsed[-1], slow


def experiment_3(results, output):
    fig, axes = new_figure(
        "Experiment 3: StrataDB spends about twice as long held back by the L0 slowdown",
        "fillrandom, 10M keys, sampled every 200 ms, run 1 of 3. "
        "Throughput falls to about 1K ops/s whenever L0 reaches 8 files.",
        nrows=2, ncols=2, size=(9.5, 5.6), sharex=True, sharey="row",
        gridspec_kw={"height_ratios": [3, 1.3]})
    fig.subplots_adjust(left=0.08, right=0.98, top=0.78, bottom=0.09, hspace=0.12, wspace=0.05)
    for col, (name, label, colour) in enumerate((("strata", "StrataDB", STRATA),
                                                 ("leveldb", "LevelDB", LEVELDB))):
        rows = load(os.path.join(results, "experiments", f"exp3_timeseries_{name}_run1.csv"))
        elapsed = [float(r["elapsed_sec"]) for r in rows]
        done = [int(r["ops_done"]) for r in rows]
        l0 = [int(r["l0_files"]) for r in rows]
        rate = [(done[i] - done[i - 1]) / (elapsed[i] - elapsed[i - 1]) if elapsed[i] > elapsed[i - 1]
                else 0 for i in range(1, len(rows))]
        total, slow = slowdown_share(rows)
        top, bottom = axes[0][col], axes[1][col]
        top.step(elapsed[1:], rate, color=colour, linewidth=1.1, where="pre")
        top.set_title(f"{label}: {total:.0f} s, {slow:.0f} s of it in the slowdown ({100 * slow / total:.0f}%)",
                      fontsize=10, color=INK, loc="left")
        bottom.step(elapsed, l0, color=colour, linewidth=1.4, where="post")
        bottom.axhline(L0_SLOWDOWN_FILES, color=INK_MUTED, linewidth=0.8)
        style(top)
        style(bottom)
        top.set_ylim(bottom=0)
        top.yaxis.set_major_formatter(FuncFormatter(compact))
        bottom.set_ylim(0, 10)
        bottom.set_yticks([0, 4, 8])
        bottom.set_xlabel("Seconds", color=INK_SECONDARY, fontsize=9.5)
    axes[0][0].set_ylabel("ops/s", color=INK_SECONDARY, fontsize=9.5)
    axes[1][0].set_ylabel("L0 files", color=INK_SECONDARY, fontsize=9.5)
    save(fig, output, "exp3_l0_timeseries.png")


def experiment_4(results, output):
    rows = load(os.path.join(results, "experiments", "exp4_block_cache.csv"))
    fig, axes = new_figure(
        "Experiment 4: a bigger block cache hits more often but doesn't read faster",
        "readrandom, 10M uniform keys, cold start under a 512 MB memory limit, one run per size.",
        ncols=3, size=(9.5, 4.2))
    fig.subplots_adjust(left=0.07, right=0.98, top=0.72, bottom=0.14, wspace=0.26)
    panels = (("Hit rate (%)", lambda r: 100 * int(r["cache_hits"])
               / (int(r["cache_hits"]) + int(r["cache_misses"]))),
              ("ops/s", lambda r: float(r["ops_per_sec"])),
              ("p99 latency (µs)", lambda r: float(r["p99_us"])))
    for ax, (ylabel, value) in zip(axes, panels):
        for engine, colour, marker, label in (("StrataDB", STRATA, "o", "StrataDB"),
                                              ("LevelDB-nommap", NOMMAP, "s", "LevelDB no mmap")):
            group = [r for r in rows if r["engine"] == engine]
            sizes = [int(r["block_cache_size"]) / 1048576 for r in group]
            line(ax, sizes, [value(r) for r in group], colour, marker=marker, label=label, size=6)
        style(ax)
        ax.set_xscale("log", base=2)
        ax.set_xticks([8, 32, 64, 128, 256])
        ax.set_xticklabels(["8", "32", "64", "128", "256"])
        ax.minorticks_off()
        ax.set_ylim(bottom=0)
        ax.yaxis.set_major_formatter(FuncFormatter(compact))
        ax.set_title(ylabel, fontsize=10, color=INK, loc="left")
        ax.set_xlabel("Block cache (MB)", color=INK_SECONDARY, fontsize=9)
    axes[0].legend(frameon=False, loc="upper left", fontsize=9, labelcolor=INK_SECONDARY)
    save(fig, output, "exp4_block_cache.png")


def memory_medians(path):
    groups = defaultdict(list)
    for row in load(path):
        groups[(row["engine"], row["workload"])].append(float(row["ops_per_sec"]))
    return {key: statistics.median(values) for key, values in groups.items()}


def experiment_5(results, output):
    before = memory_medians(os.path.join(results, "before", "memory.csv"))
    after = memory_medians(os.path.join(results, "after", "memory.csv"))
    workloads = sorted({w for e, w in after if e == "StrataDB"},
                       key=lambda w: after[("StrataDB", w)] / before[("StrataDB", w)])
    fig, axes = new_figure(
        "Experiment 5: hardware CRC sped up StrataDB and closed much of the gap to LevelDB",
        "Memory tier, 10M keys, median of 3 runs. LevelDB also moved to hardware CRC for the after run.",
        ncols=2, size=(9.5, 5.4), sharey=True)
    fig.subplots_adjust(left=0.15, right=0.97, top=0.80, bottom=0.10, wspace=0.08)
    left, right = axes
    positions = range(len(workloads))
    gains = [100 * (after[("StrataDB", w)] / before[("StrataDB", w)] - 1) for w in workloads]
    left.barh(positions, gains, height=0.42, color=STRATA)
    for y, gain in zip(positions, gains):
        left.text(gain + 3 if gain >= 0 else 3, y, f"{gain:+.0f}%", va="center", fontsize=8.5,
                  color=INK_SECONDARY)
    left.set_yticks(list(positions))
    left.set_yticklabels(workloads, fontsize=9, color=INK_SECONDARY)
    left.set_title("StrataDB throughput change", fontsize=10, color=INK, loc="left")
    left.set_xlim(-10, 175)
    left.set_xticks([0, 50, 100, 150])
    left.xaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:g}%"))
    style(left, grid_axis="x")
    left.spines["bottom"].set_visible(False)

    gap_before = [before[("LevelDB", w)] / before[("StrataDB", w)] for w in workloads]
    gap_after = [after[("LevelDB", w)] / after[("StrataDB", w)] for w in workloads]
    for y, (old, new) in enumerate(zip(gap_before, gap_after)):
        right.plot([old, new], [y, y], color=GRID, linewidth=2, zorder=2)
        right.plot(old, y, "o", markersize=7, color=BEFORE, zorder=3)
        right.plot(new, y, "o", markersize=8, color=STRATA, markeredgecolor=SURFACE,
                   markeredgewidth=1.5, zorder=4)
    right.axvline(1, color=INK_MUTED, linewidth=0.8)
    right.set_title("How many times faster default LevelDB is", fontsize=10, color=INK, loc="left")
    right.set_xlim(0.5, 4.6)
    right.set_xticks([1, 2, 3, 4])
    right.xaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:g}x"))
    style(right, grid_axis="x")
    right.spines["bottom"].set_visible(False)
    handles = [Line2D([], [], color=BEFORE, marker="o", linestyle="none", label="Before"),
               Line2D([], [], color=STRATA, marker="o", linestyle="none", label="After")]
    right.legend(handles=handles, frameon=False, loc="lower right", fontsize=9,
                 labelcolor=INK_SECONDARY)
    save(fig, output, "exp5_crc_fix.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--results", default="docs/results")
    parser.add_argument("--output", default="docs/images")
    args = parser.parse_args()
    os.makedirs(args.output, exist_ok=True)
    plt.rcParams["font.family"] = "sans-serif"
    plt.rcParams["font.sans-serif"] = ["DejaVu Sans"]
    experiment_1(args.results, args.output)
    experiment_2(args.results, args.output)
    experiment_3(args.results, args.output)
    experiment_4(args.results, args.output)
    experiment_5(args.results, args.output)


if __name__ == "__main__":
    main()
