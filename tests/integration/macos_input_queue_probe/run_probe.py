#!/usr/bin/env python3
"""Run the macOS input/frame scheduler acceptance probe."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


RESULT_RE = re.compile(r"^RESULT (?P<fields>.+)$", re.MULTILINE)


def parse_result(output: str) -> dict[str, float]:
    match = RESULT_RE.search(output)
    if not match:
        raise RuntimeError(f"probe produced no RESULT line:\n{output}")
    result: dict[str, float] = {}
    for field in match.group("fields").split():
        name, value = field.split("=", 1)
        result[name] = float(value)
    return result


def run_case(executable: Path, name: str, nested: bool) -> dict[str, float]:
    env = os.environ.copy()
    if nested:
        env.pop("AFFINEUI_PROBE_NO_NESTED", None)
    else:
        env["AFFINEUI_PROBE_NO_NESTED"] = "1"
    completed = subprocess.run(
        [str(executable)],
        env=env,
        capture_output=True,
        text=True,
        timeout=15,
        check=False,
    )
    combined = completed.stdout + completed.stderr
    if completed.returncode != 0:
        raise RuntimeError(
            f"{name}: exit {completed.returncode}\n{combined}"
        )
    result = parse_result(combined)
    failures: list[str] = []
    for field, expected in (
        ("tagged", 300),
        ("missing", 0),
        ("duplicates", 0),
        ("out_of_order", 0),
        ("max_frame_depth", 1),
        ("events_during_frame", 0),
    ):
        if result.get(field) != expected:
            failures.append(f"{field}={result.get(field)} expected {expected}")
    if result.get("max_age_ms", float("inf")) > 250:
        failures.append(f"max_age_ms={result.get('max_age_ms')} > 250")
    if result.get("mouse_up_age_ms", float("inf")) > 150:
        failures.append(
            f"mouse_up_age_ms={result.get('mouse_up_age_ms')} > 150"
        )
    print(f"{name}: {RESULT_RE.search(combined).group(0)}")
    if failures:
        raise RuntimeError(f"{name}: " + "; ".join(failures))
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    try:
        run_case(args.executable.resolve(), "queue", nested=False)
        run_case(args.executable.resolve(), "nested", nested=True)
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print("PASS: native input stayed ordered and bounded; frames stayed serialized")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
