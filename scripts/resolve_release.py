#!/usr/bin/env python3
"""Resolve + validate what a release cut will actually publish.

This is the whole rulebook, in one testable place. It works out the tag to
create and the commit to create it on, and refuses anything that would produce a
bad publish. The workflows do no version logic of their own.

Who calls this
--------------
`Build a pre-release` (build-prerelease.yml) calls it with `--mode rc` to pick
the next free counter.

`--mode release` still encodes the rule that a final release must promote a
tested pre-release, and stays here as the tested statement of that rule — but the
release path is now `Publish a release` (publish-release.yml), which enforces a
strictly STRONGER gate: the RC must be the newest one, its tag must exist (which
proves it built, tested, and published cleanly), and its compiled binaries are
re-stamped and hash-verified rather than rebuilt. See docs/RELEASING.md.

Usage:
    resolve_release.py --version 0.5.0 --mode rc
    resolve_release.py --version 0.5.0 --mode rc      --source-tag v0.5.0-rc.1
    resolve_release.py --version 0.5.0 --mode release --source-tag v0.5.0-rc.2

Inputs
------
--version      The RELEASE number, typed by the operator: `0.5.0`. Never a
               suffix — you do not type `rc.2`, the counter is computed.
--mode         alpha | beta | rc | release
--source-tag   The commit to tag. Empty = HEAD. For `release` this is REQUIRED
               and must be a pre-release tag of the same --version: a final
               release republishes an RC's commit, it does not build something
               new.

Outputs (to stdout, and $GITHUB_OUTPUT when set)
------------------------------------------------
    version=0.5.0-rc.2        full version string to publish
    core=0.5.0                MAJOR.MINOR.PATCH — the release-notes key
    tag=v0.5.0-rc.2           tag to create
    source_ref=<sha|HEAD>     commit the tag goes on
    is_prerelease=true|false

The rules enforced here
-----------------------
1.  A final release MUST promote an RC. `--mode release` without a --source-tag
    pointing at a pre-release of the same version is refused. You ship what you
    tested.
2.  The pre-release counter auto-increments. rc.1 -> rc.2, per (version, mode).
3.  The version must be > the last published RELEASE. No going backwards, no
    reusing a burned number. (Pre-releases of the *current* version are fine —
    that's the cycle in progress.)
4.  The tag must not already exist.

The manifest-match check (typed version vs. what's committed) lives in
set_version.py --verify, because that's where the manifest list lives; the
workflow runs both.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

SEMVER_RE = re.compile(
    r"^(?P<major>0|[1-9]\d*)\.(?P<minor>0|[1-9]\d*)\.(?P<patch>0|[1-9]\d*)"
    r"(?:-(?P<pre>[0-9A-Za-z.-]+))?$"
)

MODES = ("alpha", "beta", "rc", "release")


def die(msg: str) -> None:
    print(f"::error::{msg}", file=sys.stderr)
    sys.exit(1)


def git(*args: str) -> str:
    return subprocess.run(
        ["git", *args], capture_output=True, text=True, check=False
    ).stdout.strip()


def parse_core(v: str) -> tuple[int, int, int]:
    """Validate a bare MAJOR.MINOR.PATCH and return it as a sortable tuple."""
    m = SEMVER_RE.match(v)
    if not m:
        die(f"--version must be MAJOR.MINOR.PATCH (got {v!r})")
    if m["pre"]:
        die(
            f"--version must not carry a pre-release suffix (got {v!r}). "
            f"Type the release number and pick --mode; the counter is automatic."
        )
    return int(m["major"]), int(m["minor"]), int(m["patch"])


def all_tags() -> list[str]:
    return [t for t in git("tag", "--list", "v*").splitlines() if t]


def last_stable(tags: list[str]) -> str | None:
    """Highest published FINAL release (no pre-release suffix)."""
    finals = []
    for t in tags:
        m = SEMVER_RE.match(t.lstrip("v"))
        if m and not m["pre"]:
            finals.append(((int(m["major"]), int(m["minor"]), int(m["patch"])), t))
    if not finals:
        return None
    return max(finals)[1]


def next_counter(tags: list[str], core: str, mode: str) -> int:
    """Highest existing <core>-<mode>.N, plus one."""
    pat = re.compile(rf"^v{re.escape(core)}-{re.escape(mode)}\.(\d+)$")
    ns = [int(m.group(1)) for t in tags if (m := pat.match(t))]
    return max(ns) + 1 if ns else 1


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--version", required=True)
    p.add_argument("--mode", required=True, choices=MODES)
    p.add_argument("--source-tag", default="")
    a = p.parse_args()

    version = a.version.strip().lstrip("v")
    mode = a.mode
    source_tag = a.source_tag.strip()

    core_t = parse_core(version)
    core = version
    tags = all_tags()

    # ── rule 3: the number must move forward ────────────────────────────────
    # Compare against the last FINAL release. Pre-releases of the version we're
    # currently cutting are the cycle in progress, not a regression.
    ls = last_stable(tags)
    if ls:
        m = SEMVER_RE.match(ls.lstrip("v"))
        ls_t = (int(m["major"]), int(m["minor"]), int(m["patch"]))
        if core_t <= ls_t:
            die(
                f"version {version} is not greater than the last published "
                f"release {ls}. Versions only go up — registries are immutable, "
                f"so a burned number can never be reused."
            )

    # ── resolve the tag + the commit it goes on ─────────────────────────────
    if mode == "release":
        # ── rule 1: a release promotes an RC ────────────────────────────────
        if not source_tag:
            die(
                f"a final release must promote a tested pre-release: pass "
                f"--source-tag (e.g. v{core}-rc.1). A release ships an RC's "
                f"commit; it does not build something new."
            )
        st = source_tag if source_tag.startswith("v") else f"v{source_tag}"
        if st not in tags:
            die(f"source tag {st} does not exist.")
        m = SEMVER_RE.match(st.lstrip("v"))
        if not m or not m["pre"]:
            die(f"source tag {st} is not a pre-release — nothing to promote.")
        st_core = f"{m['major']}.{m['minor']}.{m['patch']}"
        if st_core != core:
            die(
                f"source tag {st} is a pre-release of {st_core}, not of "
                f"{core}. A release can only promote an RC of its own version."
            )
        full = core
        # THE point of the whole model: tag the RC's commit, not main's HEAD.
        source_ref = git("rev-list", "-n", "1", st)
        is_pre = False
    else:
        n = next_counter(tags, core, mode)
        full = f"{core}-{mode}.{n}"
        source_ref = (
            git("rev-list", "-n", "1", source_tag) if source_tag else git("rev-parse", "HEAD")
        )
        if source_tag and not source_ref:
            die(f"source tag/ref {source_tag} could not be resolved.")
        is_pre = True

    tag = f"v{full}"

    # ── rule 4: never re-tag ────────────────────────────────────────────────
    if tag in tags:
        die(f"tag {tag} already exists.")

    out = {
        "version": full,
        "core": core,
        "tag": tag,
        "source_ref": source_ref,
        "is_prerelease": "true" if is_pre else "false",
    }
    for k, v in out.items():
        print(f"{k}={v}")
    if gh := os.environ.get("GITHUB_OUTPUT"):
        with open(gh, "a", encoding="utf-8") as f:
            for k, v in out.items():
                f.write(f"{k}={v}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
