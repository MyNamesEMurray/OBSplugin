#!/usr/bin/env python3
"""LensLink pipeline benchmark report.

Turns a before/after pair of the plugin's benchmark sample files into a
statistical comparison: a markdown report plus a self-contained HTML file
with charts. Pure standard library — python3 is the only requirement.

Usage:
    python3 bench-report.py                  # pick files interactively
    python3 bench-report.py BEFORE.csv AFTER.csv

The sample files are written by OBS while "Log pipeline benchmark
numbers" is enabled (Tools -> LensLink Settings): one row per second of
live video, named bench-<pipeline>-<epoch>.csv in the plugin's config
directory. Recipe: stream ~10 s or more on your normal settings, switch
pipelines (restart OBS), stream the same scene again, then run this.
"""

import csv
import html
import os
import statistics
import sys
from datetime import datetime

METRICS = [
    # (csv column, label, unit, lower_is_better)
    ("avg_cost_ms", "Video-path cost per frame", "ms", True),
    ("max_cost_ms", "Video-path cost per frame (worst)", "ms", True),
    ("copy_mb_s", "Pixels crossing system memory", "MB/s", True),
    ("obs_cpu_pct", "OBS process CPU", "%", True),
    ("latency_ms", "Capture→decode latency", "ms", True),
    ("fps", "Decoded frame rate", "fps", False),
]


def config_dirs():
    home = os.path.expanduser("~")
    return [
        os.path.join(os.environ.get("APPDATA", ""), "obs-studio",
                     "plugin_config", "lenslink"),
        os.path.join(home, "Library", "Application Support", "obs-studio",
                     "plugin_config", "lenslink"),
        os.path.join(home, ".config", "obs-studio", "plugin_config",
                     "lenslink"),
    ]


def find_sample_files():
    found = []
    for d in config_dirs():
        if not d or not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            if name.startswith("bench-") and name.endswith(".csv"):
                path = os.path.join(d, name)
                found.append((os.path.getmtime(path), path))
    found.sort(reverse=True)
    return [p for _, p in found]


def pick(files, label):
    print(f"\nSelect the {label} run:")
    for i, path in enumerate(files, 1):
        stamp = datetime.fromtimestamp(os.path.getmtime(path))
        print(f"  [{i}] {os.path.basename(path)}"
              f"  ({stamp:%Y-%m-%d %H:%M:%S})")
    while True:
        choice = input(f"{label} file number: ").strip()
        if choice.isdigit() and 1 <= int(choice) <= len(files):
            return files[int(choice) - 1]
        print("Not a valid number, try again.")


def load(path):
    rows = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            try:
                rows.append({
                    "time_s": float(row["time_s"]),
                    "pipeline": row["pipeline"],
                    "width": int(row["width"]),
                    "height": int(row["height"]),
                    "fps": float(row["fps"]),
                    "avg_cost_ms": float(row["avg_cost_ms"]),
                    "max_cost_ms": float(row["max_cost_ms"]),
                    "copy_mb_s": float(row["copy_mb_s"]),
                    "latency_ms": float(row["latency_ms"]),
                    "obs_cpu_pct": float(row["obs_cpu_pct"]),
                })
            except (KeyError, ValueError) as err:
                raise SystemExit(f"{path}: not a LensLink benchmark file "
                                 f"({err})")
    if not rows:
        raise SystemExit(f"{path}: no samples (was video streaming while "
                         "the benchmark was on?)")
    return rows


def describe(rows):
    pipelines = {r["pipeline"] for r in rows}
    dims = {(r["width"], r["height"]) for r in rows}
    w, h = max(dims)
    return ("/".join(sorted(pipelines)),
            f"{w}x{h}",
            f"{len(rows)} s of samples")


def stats(values):
    ordered = sorted(values)
    p95 = ordered[min(len(ordered) - 1, int(round(0.95 * len(ordered))) - 1)]
    return {
        "mean": statistics.fmean(values),
        "median": statistics.median(values),
        "p95": p95,
        "min": ordered[0],
        "max": ordered[-1],
    }


def fmt(x):
    return f"{x:,.2f}"


def build_markdown(before, after, before_path, after_path):
    b_pipe, b_dims, b_len = describe(before)
    a_pipe, a_dims, a_len = describe(after)

    lines = [
        "# LensLink pipeline benchmark report",
        "",
        f"Generated {datetime.now():%Y-%m-%d %H:%M:%S}",
        "",
        f"- **Before:** `{os.path.basename(before_path)}` — "
        f"pipeline `{b_pipe}`, {b_dims}, {b_len}",
        f"- **After:** `{os.path.basename(after_path)}` — "
        f"pipeline `{a_pipe}`, {a_dims}, {a_len}",
        "",
    ]
    if b_dims != a_dims:
        lines += [
            "> **Warning:** the two runs use different resolutions — the "
            "comparison is not apples-to-apples.",
            "",
        ]

    lines += [
        "| Metric | Before (mean) | After (mean) | Change | "
        "Before p95 | After p95 |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for key, label, unit, lower_better in METRICS:
        b = stats([r[key] for r in before])
        a = stats([r[key] for r in after])
        if b["mean"] > 0:
            pct = (a["mean"] - b["mean"]) / b["mean"] * 100.0
            if abs(pct) < 2.0:
                change = f"{pct:+.1f}% (≈ no change)"
            else:
                better = (pct < 0) == lower_better
                change = f"{pct:+.1f}% {'✅' if better else '⚠️'}"
        else:
            change = "n/a" if a["mean"] == 0 else f"+{fmt(a['mean'])} {unit}"
        lines.append(
            f"| {label} ({unit}) | {fmt(b['mean'])} | {fmt(a['mean'])} | "
            f"{change} | {fmt(b['p95'])} | {fmt(a['p95'])} |")

    lines += [
        "",
        "Mean/median/p95 are computed over 1-second samples. "
        "\"Pixels crossing system memory\" at 0.00 on a GPU-pipeline run "
        "confirms the zero-copy path actually engaged; a nonzero value "
        "there means the automatic CPU fallback ran and the runs are not "
        "comparing what you think.",
    ]
    return "\n".join(lines) + "\n"


def svg_series(before, after, key, label, unit, width=640, height=200):
    """Two time-series polylines on one inline-SVG chart."""
    pad = 40
    all_vals = [r[key] for r in before] + [r[key] for r in after]
    top = max(all_vals) * 1.15 or 1.0

    def poly(rows, color):
        n = len(rows)
        if n == 1:
            rows = rows * 2
            n = 2
        pts = []
        for i, r in enumerate(rows):
            x = pad + i / (n - 1) * (width - pad - 10)
            y = height - pad - (r[key] / top) * (height - pad - 10)
            pts.append(f"{x:.1f},{y:.1f}")
        return (f'<polyline fill="none" stroke="{color}" stroke-width="2" '
                f'points="{" ".join(pts)}"/>')

    grid = []
    for frac in (0.0, 0.5, 1.0):
        y = height - pad - frac * (height - pad - 10)
        grid.append(f'<line x1="{pad}" y1="{y:.1f}" x2="{width - 10}" '
                    f'y2="{y:.1f}" stroke="#ccc" stroke-width="1"/>')
        grid.append(f'<text x="4" y="{y + 4:.1f}" font-size="11" '
                    f'fill="#666">{top * frac:.1f}</text>')

    return f"""
<h3>{html.escape(label)} ({unit}) — per second</h3>
<svg viewBox="0 0 {width} {height}" width="{width}" height="{height}"
     role="img">
  {''.join(grid)}
  {poly(before, '#c0392b')}
  {poly(after, '#27ae60')}
  <text x="{pad}" y="16" font-size="12" fill="#c0392b">— before</text>
  <text x="{pad + 90}" y="16" font-size="12" fill="#27ae60">— after</text>
</svg>"""


def build_html(markdown_text, before, after):
    charts = "".join(
        svg_series(before, after, key, label, unit)
        for key, label, unit, _ in METRICS)
    table = "<pre>" + html.escape(markdown_text) + "</pre>"
    return f"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<title>LensLink pipeline benchmark report</title>
<style>
 body {{ font: 14px/1.5 system-ui, sans-serif; margin: 2em auto;
        max-width: 720px; color: #222; }}
 h1 {{ font-size: 1.4em; }} h3 {{ margin-bottom: 4px; }}
 pre {{ background: #f6f6f6; padding: 1em; overflow-x: auto; }}
</style></head><body>
<h1>LensLink pipeline benchmark report</h1>
{table}
{charts}
</body></html>"""


def main():
    if len(sys.argv) == 3:
        before_path, after_path = sys.argv[1], sys.argv[2]
    elif len(sys.argv) == 1:
        files = find_sample_files()
        if len(files) < 2:
            raise SystemExit(
                "Fewer than two benchmark files found. Enable 'Log "
                "pipeline benchmark numbers' in Tools -> LensLink "
                "Settings, stream once per pipeline, then rerun — or "
                "pass two CSV paths directly.")
        before_path = pick(files, "BEFORE")
        after_path = pick(files, "AFTER")
    else:
        raise SystemExit(__doc__)

    before = load(before_path)
    after = load(after_path)

    markdown_text = build_markdown(before, after, before_path, after_path)
    html_text = build_html(markdown_text, before, after)

    md_out = "lenslink-bench-report.md"
    html_out = "lenslink-bench-report.html"
    with open(md_out, "w") as f:
        f.write(markdown_text)
    with open(html_out, "w") as f:
        f.write(html_text)

    print("\n" + markdown_text)
    print(f"Wrote {md_out} and {html_out} (charts) to "
          f"{os.getcwd()}")


if __name__ == "__main__":
    main()
