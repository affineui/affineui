#!/usr/bin/env python3
"""Compute the next affineui version.

Usage:
    python scripts/next_version.py --last v1.2.3     --bump minor --mode release
    python scripts/next_version.py --last v1.2.3     --bump minor --mode rc
    python scripts/next_version.py --last v1.2.4-rc.1 --bump none  --mode rc
    python scripts/next_version.py --last v1.2.4-rc.2 --bump none  --mode release
    python scripts/next_version.py --last ""         --bump none  --mode release

Outputs `next=<VERSION>` to stdout (and $GITHUB_OUTPUT when set) — bare, no
leading `v`. Downstream steps prefix the `v` when they build a tag name.

Semantics
=========
The last-tag input defines the reference point. It can be:
  * empty string        → first release ever; treated as if last was 0.0.0
  * a stable tag        → e.g. v1.2.3
  * a pre-release tag   → e.g. v1.2.4-rc.1

`bump` selects which core segment (MAJOR.MINOR.PATCH) gets incremented off
the last stable core:

  major:  X.Y.Z → (X+1).0.0
  minor:  X.Y.Z → X.(Y+1).0
  patch:  X.Y.Z → X.Y.(Z+1)
  none:   X.Y.Z → X.Y.Z         (rely on the mode change alone)

`mode` selects the pre-release suffix:

  release:  no suffix       (e.g. 2.0.0)
  rc:       -rc.N
  beta:     -beta.N
  alpha:    -alpha.N

Counter N: if the LAST tag is already <target_core>-<mode>.M then N = M+1
(same series, next candidate). Otherwise N = 1 (fresh series).

Common workflows
================
Start a new patch RC series:
  last=v1.2.3, bump=patch, mode=rc     → 1.2.4-rc.1

Next RC in same series:
  last=v1.2.4-rc.1, bump=none, mode=rc → 1.2.4-rc.2

Promote RC to stable release:
  last=v1.2.4-rc.2, bump=none, mode=release → 1.2.4

Cut a fresh minor release with no RC dance:
  last=v1.2.3, bump=minor, mode=release → 1.3.0

Edge cases
==========
* Empty --last  → 0.0.0 core. Then apply bump + mode as normal.
* Non-canonical last (e.g. missing `v`, has +build metadata) → tolerated:
  leading `v` stripped, build metadata (+…) ignored.
* Requested next == last → error (nothing to publish). Callers should
  bump or change mode.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# Same regex as scripts/set_version.py — semver 2.0.0. Kept private here
# to avoid a cross-file import in CI (scripts/ is not a package).
_SEMVER_RE = re.compile(
    r"^(?P<major>0|[1-9]\d*)"
    r"\.(?P<minor>0|[1-9]\d*)"
    r"\.(?P<patch>0|[1-9]\d*)"
    r"(?:-(?P<pre>[0-9A-Za-z.-]+))?"
    r"(?:\+(?P<build>[0-9A-Za-z.-]+))?$"
)


def parse(v: str) -> tuple[int, int, int, str | None]:
    """(major, minor, patch, pre-or-None). Build metadata is discarded."""
    m = _SEMVER_RE.match(v.lstrip("v"))
    if not m:
        raise SystemExit(f"[next_version] invalid semver: {v!r}")
    return int(m["major"]), int(m["minor"]), int(m["patch"]), m["pre"]


def prerelease_counter(pre: str | None, mode: str) -> int | None:
    """If `pre` is already `<mode>.N`, return N. Otherwise None.

    Compares only the FIRST dot-separated token so a build tag like
    `-rc.1.build.42` still counts as the `rc` series (rare, but be
    tolerant).
    """
    if pre is None:
        return None
    parts = pre.split(".", 1)
    if parts[0] != mode:
        return None
    if len(parts) < 2:
        return None
    tail = parts[1].split(".", 1)[0]
    if not tail.isdigit():
        return None
    return int(tail)


def compute(last: str, bump: str, mode: str) -> str:
    if bump not in {"none", "patch", "minor", "major"}:
        raise SystemExit(f"[next_version] bad --bump: {bump!r}")
    if mode not in {"release", "rc", "beta", "alpha"}:
        raise SystemExit(f"[next_version] bad --mode: {mode!r}")

    if last.strip() == "":
        major, minor, patch, last_pre = 0, 0, 0, None
    else:
        major, minor, patch, last_pre = parse(last)

    # Apply the bump. `none` leaves the core alone — the mode change alone
    # differentiates the next version (e.g. going from -rc.1 to -rc.2, or
    # promoting -rc.N to a plain triplet).
    if bump == "major":
        major, minor, patch = major + 1, 0, 0
    elif bump == "minor":
        minor, patch = minor + 1, 0
    elif bump == "patch":
        patch += 1

    core = f"{major}.{minor}.{patch}"

    if mode == "release":
        next_v = core
    else:
        # Continue an existing series iff the core stayed the same. When
        # bump != none the core moved, so we start fresh at N=1.
        if bump == "none":
            n = prerelease_counter(last_pre, mode)
            next_n = (n + 1) if n is not None else 1
        else:
            next_n = 1
        next_v = f"{core}-{mode}.{next_n}"

    # Refuse a no-op. Otherwise the caller ends up trying to tag an
    # already-existing tag and CI fails obscurely.
    if last.strip() != "" and next_v == last.lstrip("v").split("+", 1)[0]:
        raise SystemExit(
            f"[next_version] next == last ({next_v!r}); "
            f"change --bump or --mode"
        )
    return next_v


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--last", required=True, help="last tag (may be empty)")
    ap.add_argument("--bump", required=True, choices=["none", "patch", "minor", "major"])
    ap.add_argument("--mode", required=True, choices=["release", "rc", "beta", "alpha"])
    args = ap.parse_args()

    next_v = compute(args.last, args.bump, args.mode)
    # `core` = MAJOR.MINOR.PATCH, stripped of any -pre / +build. The
    # release-notes workflow keys the notes file on core so v1.2.4-rc.1,
    # v1.2.4-rc.2, and v1.2.4 all share `docs/release-notes/v1.2.4.md`.
    core = next_v.split("-", 1)[0].split("+", 1)[0]
    lines = [f"next={next_v}", f"core={core}"]
    for line in lines:
        print(line)
    gh_out = os.environ.get("GITHUB_OUTPUT")
    if gh_out:
        with open(gh_out, "a") as f:
            f.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
