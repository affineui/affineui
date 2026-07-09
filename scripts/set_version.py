#!/usr/bin/env python3
"""Patch the affineui version across every binding + CMakeLists.

Usage:
    python scripts/set_version.py 1.2.3
    python scripts/set_version.py 1.2.3-rc.1
    python scripts/set_version.py --check 1.2.3-rc.1   # validate + classify only

Called by .github/workflows/release.yml + wheels.yml right before build/publish
so the built artifacts carry the tag's version. Also useful locally for cutting
a release candidate — run it, commit, tag `v<VERSION>`, push.

Version input is bare (no leading `v`, but a leading `v` is tolerated and
stripped). Format is validated as

    MAJOR.MINOR.PATCH[-PRE][+BUILD]

with MAJOR/MINOR/PATCH being non-negative integers. Anything after `-` is
carried through verbatim to every ecosystem (PEP 440, semver, NuGet all accept
the `-rc.1`/`-alpha.1` style). Build metadata after `+` is stripped for
downstream consumers that don't accept it.

Files patched:
    bindings/python/pyproject.toml       version = "..."
    bindings/rust/Cargo.toml             workspace.package.version = "..."
    bindings/rust/affineui/Cargo.toml    affineui-sys = { ..., version = "..." }
    bindings/csharp/AffineUI/AffineUI.csproj    <Version>...</Version>
    CMakeLists.txt                       project(... VERSION x.y.z ...)

The C# csproj Version is not stripped for NuGet — NuGet accepts the same
semver-ish shape (`1.2.3-rc.1`). The CMake project VERSION() macro does NOT
accept pre-release suffixes, so we strip everything after `-` for that
one file only (CMake gets 1.2.3 from 1.2.3-rc.1).

--check mode
------------
Prints `version=…` and `is_prerelease=true|false` (both to stdout for local
runs; also appended to $GITHUB_OUTPUT when set). Used by release.yml + wheels.yml
to derive `is_prerelease` from THIS parser instead of a separate shell regex,
so the two never drift. Crucially, `is_prerelease` reflects the SEMVER `-pre`
group only — build metadata after `+` (which is also allowed to contain `-`)
does NOT flip the flag.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

SEMVER_RE = re.compile(
    r"^(?P<major>0|[1-9]\d*)"
    r"\.(?P<minor>0|[1-9]\d*)"
    r"\.(?P<patch>0|[1-9]\d*)"
    r"(?:-(?P<pre>[0-9A-Za-z.-]+))?"
    r"(?:\+(?P<build>[0-9A-Za-z.-]+))?$"
)


def core_version(version: str) -> str:
    """Return MAJOR.MINOR.PATCH — no pre-release / build metadata."""
    m = SEMVER_RE.match(version)
    if not m:
        raise ValueError(f"invalid semver: {version!r}")
    return f"{m['major']}.{m['minor']}.{m['patch']}"


def patch(rel_path: str, pattern: str, replacement: str) -> None:
    """Apply a single-file regex substitution and confirm exactly one match.

    Fail loudly if the pattern doesn't match — that means the file was
    reformatted since this script was written and needs a look, not a silent
    no-op that ships the wrong version.
    """
    p = REPO_ROOT / rel_path
    text = p.read_text()
    new_text, count = re.subn(pattern, replacement, text, flags=re.MULTILINE)
    if count == 0:
        raise SystemExit(f"[set_version] no match for pattern in {rel_path}: {pattern}")
    if count > 1:
        raise SystemExit(
            f"[set_version] {count} matches for pattern in {rel_path}, "
            f"expected exactly 1: {pattern}"
        )
    p.write_text(new_text)
    print(f"[set_version] patched {rel_path}")


def classify(version: str) -> tuple[str, bool]:
    """Validate and classify a semver string.

    Returns (version_without_v_prefix, is_prerelease). `is_prerelease` is
    True iff the SEMVER `-pre` group is present — NOT for build metadata
    after `+` (which is also `-`-separated but semantically distinct).
    """
    v = version.lstrip("v")
    m = SEMVER_RE.match(v)
    if not m:
        raise SystemExit(f"[set_version] invalid semver: {version!r}")
    return v, m["pre"] is not None


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "--check":
        if len(sys.argv) != 3:
            raise SystemExit("[set_version] --check requires a VERSION argument")
        v, is_pre = classify(sys.argv[2])
        # `core` = MAJOR.MINOR.PATCH, no `-pre`, no `+build`. Notes files
        # in docs/release-notes/ are keyed on core, so v1.2.4-rc.1 and
        # v1.2.4-rc.2 and v1.2.4 all share the same notes file.
        core = core_version(v)
        lines = [
            f"version={v}",
            f"is_prerelease={'true' if is_pre else 'false'}",
            f"core={core}",
        ]
        # $GITHUB_OUTPUT is the runner's per-step outputs file. When present
        # (any GitHub Actions step), also append to it so `steps.<id>.outputs`
        # picks the values up. Local invocations just get the stdout lines.
        gh_out = os.environ.get("GITHUB_OUTPUT")
        if gh_out:
            with open(gh_out, "a") as f:
                f.write("\n".join(lines) + "\n")
        for line in lines:
            print(line)
        return 0

    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    version, _is_pre = classify(sys.argv[1])
    cmake_v = core_version(version)  # CMake project() rejects -rc.1

    # Python (pyproject.toml). scikit-build-core reads `version` at build.
    patch(
        "bindings/python/pyproject.toml",
        r'^version = "[^"]*"$',
        f'version = "{version}"',
    )

    # Rust workspace.package version — inherited by both crates via
    # `version.workspace = true`.
    patch(
        "bindings/rust/Cargo.toml",
        r'^version = "[^"]*"$',
        f'version = "{version}"',
    )
    # affineui/Cargo.toml also pins affineui-sys by version — bump that
    # in lock-step so cargo publish accepts the workspace.
    patch(
        "bindings/rust/affineui/Cargo.toml",
        r'^affineui-sys = \{ path = "\.\./affineui-sys", version = "[^"]*" \}$',
        f'affineui-sys = {{ path = "../affineui-sys", version = "{version}" }}',
    )

    # C# csproj: <Version>...</Version> — NuGet supports semver pre-release.
    patch(
        "bindings/csharp/AffineUI/AffineUI.csproj",
        r"^(\s*)<Version>[^<]*</Version>$",
        rf"\1<Version>{version}</Version>",
    )

    # CMakeLists.txt project() only accepts MAJOR.MINOR.PATCH (or a subset)
    # — no pre-release suffix. Emit core_version there.
    patch(
        "CMakeLists.txt",
        r"^(\s*VERSION )\d+\.\d+\.\d+(\s*)$",
        rf"\g<1>{cmake_v}\g<2>",
    )

    print(f"[set_version] all files set to {version} (CMake: {cmake_v})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
