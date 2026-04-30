#!/usr/bin/env python3
"""Generate a markdown perf report from a snapshot CSV.

Usage:
    gen_report.py <snapshot.csv> [<previous.csv>]

Writes to <snapshot>.report.md (sibling of the CSV).

Layout:
  1. Run metadata (commit, platform, compiler).
  2. Per-op sections.  For each op, per-shape sorted leaderboards from
     fastest to slowest format.  Marks the format with the smallest
     wire size (where wire is uniform across the op).
  3. Aggregate rankings — for each op, each format's average rank across
     all shapes, plus best/worst/firsts/lasts counts.
  4. Overall summary — average rank across every (op, shape) cell.
  5. Optional delta-vs-previous: cells whose ns_median moved >= 2x.

Re-runnable: same input → same output.
"""
from __future__ import annotations

import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

# --- helpers ---------------------------------------------------------------


def col_key(r: dict) -> str:
    return f"psio::{r['format']}" if r["library"] == "psio" else r["library"]


def load(p: Path) -> list[dict]:
    with p.open() as f:
        return list(csv.DictReader(f))


def fmt_ns(v: float | None) -> str:
    if v is None:
        return "-"
    if v < 10:
        return f"{v:.2f}"
    if v < 100:
        return f"{v:.1f}"
    return f"{v:.0f}"


def fmt_rank(v: float | None, w: int = 5) -> str:
    if v is None:
        return "-".rjust(w)
    return f"{v:.2f}".rjust(w)


def md_inline_safe(text: str) -> str:
    """Make a string safe to inline inside a markdown table cell.

    Wraps in backticks so HTML-like fragments (e.g. `view<T, Fmt>`) and
    pipe characters survive strict GFM parsers (stackedit, etc).  Empty
    strings stay empty.  Backticks inside the input get doubled so
    they're escaped within the surrounding pair.
    """
    if not text:
        return ""
    if "`" in text:
        text = text.replace("`", "``")
        return f"`` {text} ``"
    return f"`{text}`"


def collect_shapes(rows: list[dict]) -> list[str]:
    seen, ordered = set(), []
    for r in rows:
        s = r["shape"]
        if s not in seen:
            seen.add(s)
            ordered.append(s)
    return ordered


def collect_ops(rows: list[dict]) -> list[str]:
    canonical = [
        "view_one", "decode_then_view", "decode",
        "encode_rvalue", "encode_sink", "validate", "size_of",
    ]
    present = {r["op"] for r in rows}
    return [o for o in canonical if o in present]


def collect_formats(rows: list[dict]) -> list[str]:
    seen, ordered = set(), []
    for r in rows:
        k = col_key(r)
        if k not in seen:
            seen.add(k)
            ordered.append(k)
    return sorted(ordered)


# --- per-op sorted leaderboards -------------------------------------------


def section_run_meta(rows: list[dict], csv_path: Path) -> str:
    m = rows[0]
    return (
        f"# psio-vs-externals — perf snapshot {m['timestamp_utc']}\n"
        f"\n"
        f"- **Commit:** `{m['commit']}`\n"
        f"- **Platform:** {m['os']}/{m['arch']} · {m['compiler']} · "
        f"{m['cxx_std']} · `{m['build_type']}`\n"
        f"- **CSV:** `{csv_path.as_posix()}` ({len(rows)} rows)\n"
        f"\n"
    )


def section_per_op_leaderboards(rows: list[dict],
                                 ops: list[str],
                                 shapes: list[str]) -> str:
    """For each op, for each shape, emit a sorted leaderboard."""
    out: list[str] = ["# Per-op leaderboards (fastest to slowest)\n"]
    for op in ops:
        out.append(f"\n## {op}\n")
        for shape in shapes:
            cells: list[tuple[float, str, int | None, str]] = []
            for r in rows:
                if r["op"] != op or r["shape"] != shape:
                    continue
                try:
                    ns = float(r["ns_median"])
                except (ValueError, KeyError):
                    continue
                try:
                    wb = int(r["wire_bytes"])
                except (ValueError, KeyError):
                    wb = None
                cells.append((ns, col_key(r), wb, r.get("notes", "")))
            if not cells:
                continue
            cells.sort()
            min_wire = (
                min((c[2] for c in cells if c[2] is not None), default=None)
            )
            out.append(f"\n### {shape}\n")
            out.append("| rank | format | ns | wire B | notes |")
            out.append("|-----:|--------|---:|-------:|-------|")
            for i, (ns, fmt, wb, notes) in enumerate(cells, start=1):
                wire = "-" if wb is None else str(wb)
                wire_marker = "" if wb != min_wire or wb is None else " ★"
                out.append(
                    f"| {i} | `{fmt}` | {fmt_ns(ns)} | {wire}{wire_marker} "
                    f"| {md_inline_safe(notes)} |"
                )
        out.append("")
    return "\n".join(out)


# --- per-op aggregate rankings --------------------------------------------


def per_op_rankings(rows: list[dict],
                    ops: list[str],
                    shapes: list[str]) -> dict[str, dict[str, dict]]:
    """For each op, compute each format's rank-per-shape and aggregate stats.

    Returns {op: {format: {avg, median, best, worst, n, firsts, lasts}}}.
    """
    out: dict[str, dict[str, dict]] = {}
    for op in ops:
        # ranks_by_format: format → list of (shape, rank, total_in_shape)
        ranks: dict[str, list[tuple[str, int, int]]] = defaultdict(list)
        for shape in shapes:
            cells: list[tuple[float, str]] = []
            for r in rows:
                if r["op"] != op or r["shape"] != shape:
                    continue
                try:
                    cells.append((float(r["ns_median"]), col_key(r)))
                except (ValueError, KeyError):
                    pass
            cells.sort()
            n = len(cells)
            for rank, (_, fmt) in enumerate(cells, start=1):
                ranks[fmt].append((shape, rank, n))
        agg: dict[str, dict] = {}
        for fmt, rs in ranks.items():
            ranks_only = [r for (_, r, _) in rs]
            agg[fmt] = {
                "avg":    sum(ranks_only) / len(ranks_only),
                "median": statistics.median(ranks_only),
                "best":   min(ranks_only),
                "worst":  max(ranks_only),
                "n":      len(ranks_only),
                "firsts": sum(1 for r in ranks_only if r == 1),
                "lasts":  sum(1 for (s, r, n_in_shape) in rs
                              if r == n_in_shape),
            }
        out[op] = agg
    return out


def section_per_op_aggregate(rankings: dict[str, dict[str, dict]],
                              ops: list[str]) -> str:
    out: list[str] = ["\n# Per-op aggregate rankings (lower avg rank = better)\n"]
    for op in ops:
        agg = rankings.get(op, {})
        if not agg:
            continue
        sorted_fmts = sorted(agg.items(), key=lambda kv: kv[1]["avg"])
        out.append(f"\n## {op}\n")
        out.append("| format | avg rank | median rank | best | worst | "
                   "shapes | firsts | lasts |")
        out.append("|--------|---------:|------------:|-----:|------:|"
                   "-------:|-------:|------:|")
        for fmt, s in sorted_fmts:
            out.append(
                f"| `{fmt}` | {fmt_rank(s['avg'])} "
                f"| {fmt_rank(s['median'])} "
                f"| {s['best']} | {s['worst']} | {s['n']} "
                f"| {s['firsts']} | {s['lasts']} |"
            )
    return "\n".join(out) + "\n"


# --- overall summary across all ops ---------------------------------------


def section_overall_summary(rows: list[dict], ops: list[str],
                             shapes: list[str]) -> str:
    """Three-signal aggregate, replacing the older avg-rank summary:

      1. Geomean ratio vs best — for each cell `r = ns/best_ns`, take
         geomean across cells.  1.00 = "always tied with leader".
         Magnitude-aware and scale-invariant.
      2. Rank distribution (min, p25, median, p75, max) — shows
         consistency vs spikiness.  Two formats with the same avg rank
         can have very different distributions.
      3. Win-rate at three tolerances — % of cells the format is
         (a) leader, (b) within 5% of leader, (c) within 2× of leader.
         Measures "how often is this format competitive".

    Cells are excluded from a format's stats if the format is absent
    (e.g., flatbuf only covers a subset of shapes); the comparison is
    still apples-to-apples within each cell because we re-rank only
    the formats present.
    """
    import math
    # Build per-(op, shape) cell: list of (ns, format), sorted ascending.
    cells: dict[tuple[str, str], list[tuple[float, str]]] = defaultdict(list)
    for r in rows:
        try:
            ns = float(r["ns_median"])
        except (ValueError, KeyError):
            continue
        cells[(r["op"], r["shape"])].append((ns, col_key(r)))
    for k in cells:
        cells[k].sort()

    # Per-format aggregations.
    ratios:  dict[str, list[float]] = defaultdict(list)
    ranks:   dict[str, list[int]]   = defaultdict(list)
    leader_count: dict[str, int]    = defaultdict(int)
    within5_count: dict[str, int]   = defaultdict(int)
    within2x_count: dict[str, int]  = defaultdict(int)
    cell_count: dict[str, int]      = defaultdict(int)

    for (op, shape), entries in cells.items():
        if not entries:
            continue
        best = entries[0][0]
        if best <= 0:
            continue
        for rank, (ns, fmt) in enumerate(entries, start=1):
            ratio = ns / best
            ratios[fmt].append(ratio)
            ranks[fmt].append(rank)
            cell_count[fmt] += 1
            if rank == 1:
                leader_count[fmt] += 1
            if ratio <= 1.05:
                within5_count[fmt] += 1
            if ratio <= 2.0:
                within2x_count[fmt] += 1

    formats = sorted(ratios.keys())

    def geomean(xs: list[float]) -> float:
        if not xs:
            return float("nan")
        return math.exp(sum(math.log(x) for x in xs) / len(xs))

    def percentile(xs: list[int], p: float) -> float:
        if not xs:
            return float("nan")
        sxs = sorted(xs)
        k = (len(sxs) - 1) * p
        lo = int(k)
        hi = min(lo + 1, len(sxs) - 1)
        return sxs[lo] + (sxs[hi] - sxs[lo]) * (k - lo)

    rows_out = []
    for fmt in formats:
        rs = ratios[fmt]
        rk = ranks[fmt]
        n = cell_count[fmt]
        rows_out.append({
            "fmt":      fmt,
            "geomean":  geomean(rs),
            "min_rank": min(rk),
            "p25":      percentile(rk, 0.25),
            "median":   percentile(rk, 0.50),
            "p75":      percentile(rk, 0.75),
            "max_rank": max(rk),
            "leader":   leader_count[fmt],
            "within5":  within5_count[fmt],
            "within2x": within2x_count[fmt],
            "cells":    n,
        })

    # Sort by geomean ratio (lower = better).
    rows_out.sort(key=lambda r: r["geomean"])

    out: list[str] = [
        "\n# Overall summary — geomean ratio + rank distribution + win-rate\n",
        ("\n*The overall ranking is geomean of `ns / best_ns` across "
         "every (op, shape) cell where the format participates.  "
         "`1.00` means the format ties for fastest in every cell; "
         "`2.50` means it averages 2.5× slower than the per-cell leader.*\n"),
        "",
        ("| format | geomean ratio | rank dist (min/p25/med/p75/max) "
         "| leader | within 5% | within 2× | cells |"),
        ("|--------|--------------:|:---:"
         "|-------:|----------:|----------:|------:|"),
    ]
    for r in rows_out:
        rank_dist = (f"{r['min_rank']}/{r['p25']:.0f}/{r['median']:.0f}/"
                     f"{r['p75']:.0f}/{r['max_rank']}")
        n = r["cells"]
        leader_pct  = 100.0 * r["leader"]   / n if n else 0.0
        within5_pct = 100.0 * r["within5"]  / n if n else 0.0
        within2x_pct = 100.0 * r["within2x"] / n if n else 0.0
        out.append(
            f"| `{r['fmt']}` | {r['geomean']:.2f}× | {rank_dist} "
            f"| {r['leader']} ({leader_pct:.0f}%) "
            f"| {r['within5']} ({within5_pct:.0f}%) "
            f"| {r['within2x']} ({within2x_pct:.0f}%) "
            f"| {n} |"
        )
    out.append("")
    out.append("**Reading guide:**")
    out.append("- `geomean ratio 1.00` → format is always tied with leader.")
    out.append("- `rank dist 1/1/2/2/5` → consistently top-2 with one spike "
               "to rank 5.")
    out.append("- `within 5%` is the count of cells where the format is "
               "within 5% of the cell's leader (effectively tied).")
    out.append("- `within 2×` is the count where format is no worse than "
               "2× slower than the leader (still competitive).")
    return "\n".join(out) + "\n"


# --- delta vs previous -----------------------------------------------------


def section_delta(new_rows: list[dict], old_rows: list[dict]) -> str:
    """Cells whose ns_median moved >= 2x between old and new."""
    new_ix = {(r["op"], r["shape"], col_key(r)): float(r["ns_median"])
              for r in new_rows
              if r.get("ns_median") not in ("", None)}
    old_ix = {(r["op"], r["shape"], col_key(r)): float(r["ns_median"])
              for r in old_rows
              if r.get("ns_median") not in ("", None)}
    movers = []
    for k, vn in new_ix.items():
        if k not in old_ix:
            continue
        vo = old_ix[k]
        if vo < 0.1:
            continue
        ratio = vn / vo
        if ratio < 0.5 or ratio > 2.0:
            movers.append((ratio, k, vo, vn))
    movers.sort()
    out = ["\n# Delta vs previous run (≥2× moves)\n",
           "\n| op | shape | format | old | new | × |",
           "|----|-------|--------|----:|----:|--:|"]
    if not movers:
        out.append("| (no cells moved ≥2×) | | | | | |")
    else:
        for ratio, (op, shape, fmt), vo, vn in movers[:60]:
            out.append(f"| `{op}` | `{shape}` | `{fmt}` "
                       f"| {fmt_ns(vo)} | {fmt_ns(vn)} "
                       f"| {ratio:.2f}x |")
    return "\n".join(out) + "\n"


# --- entry point -----------------------------------------------------------


def main(argv: list[str]) -> int:
    if len(argv) < 2 or len(argv) > 3:
        print(__doc__, file=sys.stderr)
        return 1

    csv_path = Path(argv[1])
    if not csv_path.exists():
        print(f"error: CSV not found: {csv_path}", file=sys.stderr)
        return 1

    rows = load(csv_path)
    if not rows:
        print(f"error: empty CSV: {csv_path}", file=sys.stderr)
        return 1

    shapes = collect_shapes(rows)
    ops    = collect_ops(rows)

    out_path = csv_path.with_suffix(".report.md")
    sections: list[str] = [
        section_run_meta(rows, csv_path),
        section_per_op_leaderboards(rows, ops, shapes),
    ]

    rankings = per_op_rankings(rows, ops, shapes)
    sections.append(section_per_op_aggregate(rankings, ops))
    sections.append(section_overall_summary(rows, ops, shapes))

    if len(argv) == 3:
        prev_path = Path(argv[2])
        if prev_path.exists():
            prev_rows = load(prev_path)
            sections.append(
                f"\n# Comparison baseline: `{prev_path.as_posix()}`\n"
            )
            sections.append(section_delta(rows, prev_rows))
        else:
            print(f"warning: previous CSV not found: {prev_path}",
                  file=sys.stderr)

    out_path.write_text("\n".join(sections))
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
