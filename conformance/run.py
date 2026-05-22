#!/usr/bin/env python3
"""Conformance runner: A/B a test (or all tests) in a real browser vs AffineUI.

For each test it replays the same ordered steps on both sides, captures a
snapshot at every `snapshot` marker, pixel-diffs each pair, and writes an
HTML report. Designed so many agents can each own one test in parallel:

    python run.py --test buttons      # one test (an agent's inner loop)
    python run.py                      # all tests
    python run.py --filter form        # tests whose name contains "form"

Exit code is non-zero if any snapshot exceeds its test's threshold.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from diff import diff_images
from PIL import Image

ROOT = Path(__file__).resolve().parent           # conformance/
REPO = ROOT.parent
CASES = ROOT / "cases"
OUT = ROOT / "out"

DEFAULTS = {"width": 1024, "height": 768, "dpi": 1.0, "tolerance": 2, "threshold": 5.0, "steps": []}


def find_tool() -> Path:
    hits = sorted(REPO.glob("build/**/tools/conformance/conformance_test.exe"))
    if not hits:
        sys.exit("conformance_test.exe not found — build it first "
                 "(cmake --build build/ninja --target conformance_test).")
    return hits[0]


def load_case(case_dir: Path) -> dict:
    cfg = dict(DEFAULTS)
    j = case_dir / "case.json"
    if j.exists():
        cfg.update(json.loads(j.read_text(encoding="utf-8")))
    return cfg


def snapshot_names(steps: list[dict]) -> list[str]:
    names = [s["snapshot"] for s in steps if "snapshot" in s]
    return names or ["default"]


def run_test(name: str, tool: Path, channel: str) -> list[dict]:
    # Both drivers self-load <cases>/<name>/case.json; the runner only needs it
    # for the snapshot names + diff config (tolerance/threshold).
    cfg = load_case(CASES / name)
    snaps = snapshot_names(cfg["steps"])
    OUT.mkdir(parents=True, exist_ok=True)

    # AffineUI side (loads case.json itself).
    def err_rows(reason: str) -> list[dict]:
        print(f"[ERROR] {name}: {reason}")
        return [{"test": name, "snapshot": s, "passed": False, "pct": 100.0,
                 "mean": 0.0, "max": 0, "threshold": cfg["threshold"], "error": reason,
                 "browser": "", "affineui": "", "diff": ""} for s in snaps]

    # Either driver may crash on a not-yet-supported feature — capture it as a
    # failure and keep going so one bad test never aborts the suite.
    try:
        subprocess.run([str(tool), "--test", name, "--cases-dir", str(CASES), "--out-dir", str(OUT)],
                       check=True, timeout=120)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        return err_rows(f"AffineUI render failed: {e}")
    try:
        subprocess.run(["node", str(ROOT / "browser" / "shot.js"), "--test", name,
                        "--cases-dir", str(CASES), "--out-dir", str(OUT), "--channel", channel],
                       check=True, timeout=120)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        return err_rows(f"browser render failed: {e}")

    results = []
    for snap in snaps:
        aff_ppm = OUT / f"{name}.affineui.{snap}.ppm"
        aff_png = OUT / f"{name}.affineui.{snap}.png"
        br_png  = OUT / f"{name}.browser.{snap}.png"
        diff_png = OUT / f"{name}.diff.{snap}.png"
        if not aff_ppm.exists() or not br_png.exists():
            results += err_rows(f"missing snapshot '{snap}'")[: 1]
            continue
        Image.open(aff_ppm).save(aff_png)               # for the HTML report
        r = diff_images(br_png, aff_ppm, diff_png, cfg["tolerance"])
        passed = r.size_matched and r.pct_changed <= cfg["threshold"]
        results.append({
            "test": name, "snapshot": snap, "passed": passed,
            "pct": r.pct_changed, "mean": r.mean_delta, "max": r.max_delta,
            "threshold": cfg["threshold"], "error": "",
            "browser": br_png.name, "affineui": aff_png.name, "diff": diff_png.name,
        })
        flag = "PASS" if passed else "FAIL"
        print(f"[{flag}] {name}/{snap}: {r.pct_changed:.2f}% changed "
              f"(mean {r.mean_delta:.1f}, max {r.max_delta}, thr {cfg['threshold']}%)")
    return results


def write_report(rows: list[dict]) -> Path:
    def cell(p): return f'<td><img src="{p}" width="320"></td>'
    trs = []
    for r in rows:
        color = "#1e8e3e" if r["passed"] else "#d93025"
        err = r.get("error") or ""
        status = "PASS" if r["passed"] else ("ERROR" if err else "FAIL")
        meta = (f'{r["pct"]:.2f}% Δ<br>mean {r["mean"]:.1f}<br>max {r["max"]}'
                if not err else f'<i>{err}</i>')
        trs.append(
            f'<tr><td><b>{r["test"]}</b><br>{r["snapshot"]}<br>'
            f'<span style="color:{color}">{status}</span><br>{meta}</td>'
            f'{cell(r["browser"])}{cell(r["affineui"])}{cell(r["diff"])}</tr>')
    html = ("<!doctype html><meta charset=utf-8><title>AffineUI conformance</title>"
            "<style>body{font-family:sans-serif;background:#111;color:#ddd}"
            "table{border-collapse:collapse}td{border:1px solid #333;padding:6px;vertical-align:top}"
            "th{padding:6px}</style>"
            "<h1>AffineUI conformance</h1>"
            "<table><tr><th>test</th><th>browser (Chrome)</th><th>AffineUI</th><th>diff</th></tr>"
            + "".join(trs) + "</table>")
    out = OUT / "report.html"
    out.write_text(html, encoding="utf-8")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--test", help="run a single test by name")
    ap.add_argument("--filter", help="run tests whose name contains this substring")
    ap.add_argument("--channel", default="chrome", help="browser channel (chrome|chromium|msedge)")
    args = ap.parse_args()

    tool = find_tool()
    if args.test:
        names = [args.test]
    else:
        names = sorted(d.name for d in CASES.iterdir() if (d / "index.html").exists())
        if args.filter:
            names = [n for n in names if args.filter in n]
    if not names:
        sys.exit("no matching tests")

    rows: list[dict] = []
    for n in names:
        try:
            rows += run_test(n, tool, args.channel)
        except Exception as e:                       # never let one test abort the suite
            print(f"[ERROR] {n}: {e}")
            rows.append({"test": n, "snapshot": "-", "passed": False, "pct": 100.0,
                         "mean": 0.0, "max": 0, "threshold": 0, "error": str(e),
                         "browser": "", "affineui": "", "diff": ""})
    report = write_report(rows)

    failed = [r for r in rows if not r["passed"]]
    print(f"\n{len(rows) - len(failed)}/{len(rows)} snapshots passed.  report: {report}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
