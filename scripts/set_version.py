#!/usr/bin/env python3
"""Patch the affineui version across every binding + CMakeLists.

Usage:
    python scripts/set_version.py 1.2.3
    python scripts/set_version.py 1.2.3-rc.1

Called by .github/workflows/release.yml right before build/publish so the
built artifacts carry the tag's version. Also useful locally for cutting a
release candidate — run it, commit, tag `v<VERSION>`, push.

Version input is bare (no leading `v`). Format is validated as
    MAJOR.MINOR.PATCH[-PRE][+BUILD]
with MAJOR/MINOR/PATCH being non-negative integers. Anything after `-` is
carried through verbatim to every ecosystem (PEP 440, semver, NuGet all
accept the `-rc.1`/`-alpha.1` style).

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
"""

from __future__ import annotations

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


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    version = sys.argv[1].lstrip("v")  # tolerate `v1.2.3`
    if not SEMVER_RE.match(version):
        raise SystemExit(f"[set_version] invalid semver: {version!r}")
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
