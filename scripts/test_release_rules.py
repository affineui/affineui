#!/usr/bin/env python3
"""Tests for the release rulebook (resolve_release.py + set_version.py --verify).

These two scripts decide what gets published to three immutable registries. If
they are wrong, the mistake cannot be taken back — so they get tested like
anything else that can't be undone.

Runs against a throwaway git repo with synthetic tags. No network, no registry.

    python scripts/test_release_rules.py
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RESOLVE = REPO / "scripts" / "resolve_release.py"

_failures: list[str] = []
_passes = 0


def sh(*args: str, cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True)


def resolve(cwd: Path, version: str, mode: str, source_tag: str = ""):
    return sh(
        sys.executable, str(RESOLVE),
        "--version", version, "--mode", mode, "--source-tag", source_tag,
        cwd=cwd,
    )


def ok(cond: bool, what: str) -> None:
    global _passes
    if cond:
        _passes += 1
        print(f"  ok    {what}")
    else:
        _failures.append(what)
        print(f"  FAIL  {what}")


def make_repo(tmp: Path, tags: list[str]) -> Path:
    r = tmp / "r"
    r.mkdir(parents=True)
    sh("git", "init", "-q", cwd=r)
    sh("git", "config", "user.email", "t@t", cwd=r)
    sh("git", "config", "user.name", "t", cwd=r)
    (r / "f").write_text("1")
    sh("git", "add", "-A", cwd=r)
    sh("git", "commit", "-qm", "c1", cwd=r)
    for t in tags:
        sh("git", "tag", t, cwd=r)
    return r


def outputs(p: subprocess.CompletedProcess) -> dict[str, str]:
    return dict(
        line.split("=", 1) for line in p.stdout.splitlines() if "=" in line
    )


def main() -> int:
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)

        # ── the counter auto-increments ─────────────────────────────────────
        r = make_repo(tmp / "a", ["v0.4.2"])
        p = resolve(r, "0.5.0", "rc")
        ok(p.returncode == 0 and outputs(p).get("tag") == "v0.5.0-rc.1",
           "first rc -> rc.1")

        sh("git", "tag", "v0.5.0-rc.1", cwd=r)
        p = resolve(r, "0.5.0", "rc")
        ok(p.returncode == 0 and outputs(p).get("tag") == "v0.5.0-rc.2",
           "second rc auto-increments -> rc.2")

        # alpha/beta keep independent counters
        p = resolve(r, "0.5.0", "beta")
        ok(p.returncode == 0 and outputs(p).get("tag") == "v0.5.0-beta.1",
           "beta counter is independent of rc")

        # ── a release MUST promote an RC ────────────────────────────────────
        p = resolve(r, "0.5.0", "release")
        ok(p.returncode != 0 and "must promote" in p.stderr,
           "release with no source_tag is refused")

        p = resolve(r, "0.5.0", "release", "v0.4.2")
        ok(p.returncode != 0 and "not a pre-release" in p.stderr,
           "release promoting a FINAL tag is refused")

        p = resolve(r, "0.5.0", "release", "v0.5.0-rc.1")
        ok(p.returncode == 0 and outputs(p).get("tag") == "v0.5.0",
           "release promoting a real RC is accepted")

        # ── THE core guarantee: the release ships the RC's commit ───────────
        r2 = make_repo(tmp / "b", ["v0.4.2"])
        rc_sha = sh("git", "rev-parse", "HEAD", cwd=r2).stdout.strip()
        sh("git", "tag", "v0.5.0-rc.1", cwd=r2)
        # main moves on
        (r2 / "g").write_text("2")
        sh("git", "add", "-A", cwd=r2)
        sh("git", "commit", "-qm", "later", cwd=r2)
        head = sh("git", "rev-parse", "HEAD", cwd=r2).stdout.strip()
        p = resolve(r2, "0.5.0", "release", "v0.5.0-rc.1")
        got = outputs(p).get("source_ref")
        ok(p.returncode == 0 and got == rc_sha and got != head,
           "release tags the RC's COMMIT even when main has advanced")

        # ── versions only go up ─────────────────────────────────────────────
        p = resolve(r, "0.4.2", "rc")
        ok(p.returncode != 0 and "not greater" in p.stderr,
           "reusing the last released version is refused")

        p = resolve(r, "0.4.0", "rc")
        ok(p.returncode != 0 and "not greater" in p.stderr,
           "going backwards is refused")

        p = resolve(r, "0.4.3", "rc")
        ok(p.returncode == 0, "a higher version is accepted")

        # ── you type the number, not the counter ────────────────────────────
        p = resolve(r, "0.5.0-rc.1", "rc")
        ok(p.returncode != 0 and "must not carry a pre-release suffix" in p.stderr,
           "typing a suffix is refused")

        p = resolve(r, "not-a-version", "rc")
        ok(p.returncode != 0, "garbage version is refused")

        # ── never re-tag ────────────────────────────────────────────────────
        p = resolve(r, "0.5.0", "release", "v0.5.0-rc.1")
        sh("git", "tag", "v0.5.0", cwd=r)
        p = resolve(r, "0.5.0", "release", "v0.5.0-rc.1")
        ok(p.returncode != 0, "re-tagging an existing version is refused")

    print()
    if _failures:
        print(f"{len(_failures)} FAILED, {_passes} passed")
        for f in _failures:
            print(f"  - {f}")
        return 1
    print(f"all {_passes} release-rule tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
