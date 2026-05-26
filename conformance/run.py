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
import html as html_lib
import json
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

from diff import diff_images
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent           # conformance/
REPO = ROOT.parent
CASES = ROOT / "cases"
OUT = ROOT / "out"

DEFAULTS = {"width": 1024, "height": 768, "dpi": 1.0, "tolerance": 2, "threshold": 5.0, "steps": []}
RESAMPLE = getattr(Image, "Resampling", Image).LANCZOS


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
    names: list[str] = []
    for step in steps:
        if "snapshot" in step:
            names.append(step["snapshot"])
        elif (("mouse_path" in step) or ("mouse_recording" in step)) and step.get("snapshot_prefix"):
            trace = step.get("mouse_recording", step.get("mouse_path", []))
            count = len(trace) if isinstance(trace, list) else 0
            names.extend(f'{step["snapshot_prefix"]}_{i:03d}' for i in range(count))
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

    html_for_affineui = None
    if cfg.get("hydrate_with_browser"):
        html_for_affineui = OUT / f"{name}.hydrated.html"
        try:
            subprocess.run(["node", str(ROOT / "browser" / "hydrate.js"), "--test", name,
                            "--cases-dir", str(CASES), "--out-html", str(html_for_affineui),
                            "--channel", channel],
                           check=True, timeout=120)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
            return err_rows(f"browser hydration failed: {e}")

    # Either driver may crash on a not-yet-supported feature — capture it as a
    # failure and keep going so one bad test never aborts the suite.
    try:
        aff_cmd = [str(tool), "--test", name, "--cases-dir", str(CASES), "--out-dir", str(OUT)]
        if html_for_affineui is not None:
            aff_cmd += ["--html", str(html_for_affineui)]
        subprocess.run(aff_cmd, check=True, timeout=120)
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


def make_filmstrip(test: str, rows: list[dict], kind: str, frame_size: tuple[int, int]) -> str:
    frames = [r for r in rows if r.get(kind) and not r.get("error")]
    if len(frames) <= 1:
        return ""

    label_h = 24
    thumb_w, thumb_h = frame_size

    strip = Image.new("RGB", (thumb_w * len(frames), label_h + thumb_h), (17, 17, 17))
    draw = ImageDraw.Draw(strip)
    for i, row in enumerate(frames):
        x = i * thumb_w
        draw.rectangle((x, 0, x + thumb_w - 1, label_h - 1), fill=(35, 35, 35))
        draw.text((x + 6, 5), str(row["snapshot"]), fill=(225, 225, 225))
        with Image.open(OUT / row[kind]) as im:
            frame = im.convert("RGB").resize((thumb_w, thumb_h), RESAMPLE)
        strip.paste(frame, (x, label_h))

    out_name = f"{test}.{kind}.filmstrip.png"
    strip.save(OUT / out_name)
    return out_name


def write_report(rows: list[dict]) -> Path:
    def esc(value): return html_lib.escape(str(value), quote=True)
    def cell(p): return f'<td><img src="{esc(p)}" width="320"></td>' if p else "<td></td>"

    grouped: dict[str, list[dict]] = defaultdict(list)
    for r in rows:
        grouped[r["test"]].append(r)

    film_trs = []
    for test, group in grouped.items():
        if len([r for r in group if not r.get("error")]) <= 1:
            continue
        first_image = next((r.get("browser") or r.get("affineui") or r.get("diff")
                            for r in group if not r.get("error")), "")
        if not first_image:
            continue
        with Image.open(OUT / first_image) as first:
            aspect = first.height / first.width if first.width else 1.0
        thumb_w = 240
        frame_size = (thumb_w, max(1, round(thumb_w * aspect)))

        strips = {kind: make_filmstrip(test, group, kind, frame_size) for kind in ("browser", "affineui", "diff")}
        if not all(strips.values()):
            continue
        failed = [r for r in group if not r["passed"]]
        color = "#1e8e3e" if not failed else "#d93025"
        status = "PASS" if not failed else "FAIL"
        worst = max(group, key=lambda r: r["pct"])
        meta = f'{len(group)} frames<br>{worst["pct"]:.2f}% max &Delta;'
        film_trs.append(
            f'<tr><td><b>{esc(test)}</b><br>'
            f'<span style="color:{color}">{status}</span><br>{meta}</td>'
            f'<td class="timeline"><img src="{esc(strips["browser"])}"></td>'
            f'<td class="timeline"><img src="{esc(strips["affineui"])}"></td>'
            f'<td class="timeline"><img src="{esc(strips["diff"])}"></td></tr>')

    trs = []
    for r in rows:
        color = "#1e8e3e" if r["passed"] else "#d93025"
        err = r.get("error") or ""
        status = "PASS" if r["passed"] else ("ERROR" if err else "FAIL")
        meta = (f'{r["pct"]:.2f}% &Delta;<br>mean {r["mean"]:.1f}<br>max {r["max"]}'
                if not err else f'<i>{esc(err)}</i>')
        trs.append(
            f'<tr><td><b>{esc(r["test"])}</b><br>{esc(r["snapshot"])}<br>'
            f'<span style="color:{color}">{status}</span><br>{meta}</td>'
            f'{cell(r["browser"])}{cell(r["affineui"])}{cell(r["diff"])}</tr>')

    film_section = ""
    if film_trs:
        film_section = (
            "<h2>Filmstrips</h2>"
            "<p>Generated for tests with multiple snapshots. Browser, AffineUI, and diff strips use the same frame slots.</p>"
            "<table><tr><th>test</th><th>browser timeline</th><th>AffineUI timeline</th><th>diff timeline</th></tr>"
            + "".join(film_trs) + "</table>")

    html = ("<!doctype html><meta charset=utf-8><title>AffineUI conformance</title>"
            "<style>body{font-family:sans-serif;background:#111;color:#ddd}"
            "table{border-collapse:collapse}td{border:1px solid #333;padding:6px;vertical-align:top}"
            "th{padding:6px}.timeline img{max-width:960px;width:100%;height:auto}</style>"
            "<h1>AffineUI conformance</h1>"
            + film_section +
            "<h2>Snapshots</h2>"
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
