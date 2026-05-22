#!/usr/bin/env python3
"""Worktree-friendly conformance self-check for a single test.

A background fixing agent works in an isolated git worktree that does NOT have
the Playwright/Node browser deps installed. The browser side of a conformance
test is deterministic (real Chrome rendering the same HTML), so the agent does
not need to re-run it — it can render only the AffineUI side here and diff it
against the **pre-generated browser reference PNG** kept in a shared reference
directory (normally the main checkout's conformance/out/).

This is the agent's inner loop: fix -> rebuild conformance_test -> run this ->
read pct_changed -> repeat. It also appends a one-line heartbeat to a shared
status board so the run can be monitored from the main checkout while it works.

    python conformance/agent_check.py --test html_overflow
    python conformance/agent_check.py --test bs_grid --ref-dir <abs path to refs>

Exit code is 0 if every snapshot is at/under the test's threshold, else 1.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import subprocess
import sys
from pathlib import Path

from diff import diff_images

ROOT = Path(__file__).resolve().parent           # conformance/
REPO = ROOT.parent                                # repo root (this worktree)
CASES = ROOT / "cases"
OUT = ROOT / "out"

# Where the deterministic browser reference PNGs live. Defaults to this
# checkout's out/, but a worktree agent points it at the main checkout via
# --ref-dir or $AFFINEUI_BROWSER_REF so it can diff without running Chrome.
DEFAULT_REF = os.environ.get("AFFINEUI_BROWSER_REF", str(OUT))

DEFAULTS = {"tolerance": 2, "threshold": 5.0, "steps": [{"snapshot": "default"}]}


def find_tool() -> Path:
    hits = sorted(REPO.glob("build/**/tools/conformance/conformance_test.exe"))
    if not hits:
        sys.exit("conformance_test.exe not found — build it first "
                 "(cmake --build build/ninja --target conformance_test).")
    return hits[-1]


def load_case(name: str) -> dict:
    cfg = dict(DEFAULTS)
    j = CASES / name / "case.json"
    if j.exists():
        cfg.update(json.loads(j.read_text(encoding="utf-8")))
    return cfg


def snapshot_names(steps: list[dict]) -> list[str]:
    return [s["snapshot"] for s in steps if "snapshot" in s] or ["default"]


def board_path() -> Path:
    # Shared, gitignored status board — agents append here, the orchestrator
    # tails it. Lives in the reference (main) checkout so all worktrees write
    # to the same file.
    board_dir = Path(os.environ.get("AFFINEUI_AGENT_STATUS",
                                    str(Path(DEFAULT_REF).parent.parent / ".agent-status")))
    board_dir.mkdir(parents=True, exist_ok=True)
    return board_dir / "BOARD.md"


def post(test: str, msg: str) -> None:
    ts = _dt.datetime.now().strftime("%H:%M:%S")
    try:
        with board_path().open("a", encoding="utf-8") as f:
            f.write(f"- `{ts}` **{test}** — {msg}\n")
    except OSError:
        pass  # never let status logging break the loop


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--test", required=True, help="test name under conformance/cases/")
    ap.add_argument("--ref-dir", default=DEFAULT_REF,
                    help="dir holding <test>.browser.<snap>.png references")
    ap.add_argument("--quiet-board", action="store_true",
                    help="do not append to the shared status board")
    args = ap.parse_args()

    tool = find_tool()
    cfg = load_case(args.test)
    snaps = snapshot_names(cfg["steps"])
    ref_dir = Path(args.ref_dir)
    OUT.mkdir(parents=True, exist_ok=True)

    try:
        subprocess.run([str(tool), "--test", args.test,
                        "--cases-dir", str(CASES), "--out-dir", str(OUT)],
                       check=True, timeout=120)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        if not args.quiet_board:
            post(args.test, f"[BUILD/RENDER FAILED] {e}")
        sys.exit(f"AffineUI render failed: {e}")

    all_pass = True
    worst = 0.0
    lines = []
    for snap in snaps:
        aff_ppm = OUT / f"{args.test}.affineui.{snap}.ppm"
        br_png = ref_dir / f"{args.test}.browser.{snap}.png"
        diff_png = OUT / f"{args.test}.diff.{snap}.png"
        if not aff_ppm.exists():
            print(f"[ERROR] {args.test}/{snap}: AffineUI did not emit a snapshot")
            all_pass = False
            continue
        if not br_png.exists():
            print(f"[ERROR] {args.test}/{snap}: no browser reference at {br_png}\n"
                  f"        (generate it once in the main checkout: "
                  f"python conformance/run.py --test {args.test})")
            all_pass = False
            continue
        r = diff_images(br_png, aff_ppm, diff_png, cfg["tolerance"])
        passed = r.size_matched and r.pct_changed <= cfg["threshold"]
        all_pass = all_pass and passed
        worst = max(worst, r.pct_changed)
        flag = "PASS" if passed else "FAIL"
        size = "" if r.size_matched else f"  SIZE MISMATCH {r.a_size} vs {r.b_size}"
        msg = (f"[{flag}] {args.test}/{snap}: {r.pct_changed:.2f}% changed "
               f"(mean {r.mean_delta:.1f}, max {r.max_delta}, thr {cfg['threshold']}%){size}")
        print(msg)
        lines.append(f"{snap} {r.pct_changed:.2f}%")

    if not args.quiet_board:
        verdict = "[PASS]" if all_pass else f"[WIP {worst:.2f}% / thr {cfg['threshold']}%]"
        post(args.test, f"{verdict} — " + ", ".join(lines))
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
