#!/usr/bin/env python3
"""Tests for scripts/repackage_release.py — the compile-once guarantee.

`Publish a release` never compiles: it re-stamps the pre-release's artifacts and
ships those bytes. So a bug here ships a corrupt wheel to PyPI. These tests check
the invariants that actually matter, not the happy path:

  * the compiled payload survives byte-for-byte
  * the wheel is INSTALLABLE — RECORD hashes must validate, which is what pip
    checks; a stale RECORD is the likeliest way to ship a broken wheel
  * the nupkg's OPC core-properties (.psmdcp) are rewritten too, not just the
    .nuspec — otherwise the package contradicts itself
  * a swapped binary is CAUGHT (an earlier version of this script matched
    artifacts on hash-SETS and would happily accept a wheel whose extension
    module came from a different Python version)
  * a dropped artifact is CAUGHT — a partial promotion must not ship

Run: python -m pytest tests/test_repackage_release.py -v
"""

from __future__ import annotations

import base64
import csv
import hashlib
import io
import json
import subprocess
import sys
import zipfile
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
SCRIPT = REPO / "scripts" / "repackage_release.py"
sys.path.insert(0, str(REPO / "scripts"))
import repackage_release as rr  # noqa: E402


def ext_bytes(tag: str) -> bytes:
    """A distinct fake compiled payload per ABI, so a swap is detectable."""
    return b"\x7fELF\x02\x01\x01" + f"compiled-{tag}".encode() * 64


def _row(path: str, data: bytes) -> tuple[str, str, str]:
    d = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
    return (path, f"sha256={d}", str(len(data)))


def make_wheel(tmp: Path, version: str, abi: str = "cp312", dist: str = "affineui") -> Path:
    """A wheel shaped like a real cibuildwheel output."""
    di = f"{dist}-{version}.dist-info"
    ext = f"_affineui.{abi}-win_amd64.pyd"
    files = {
        f"{dist}/__init__.py": b"from ._affineui import *\n",
        ext: ext_bytes(abi),
        f"{di}/METADATA": (
            f"Metadata-Version: 2.1\nName: {dist}\nVersion: {version}\n"
            f"Summary: GPU-accelerated HTML/CSS UI\n\nLong description.\n"
        ).encode(),
        f"{di}/WHEEL": b"Wheel-Version: 1.0\nGenerator: test\nRoot-Is-Purelib: false\n",
    }
    rows = [_row(p, d) for p, d in files.items()]
    rows.append((f"{di}/RECORD", "", ""))
    buf = io.StringIO()
    csv.writer(buf, lineterminator="\n").writerows(rows)
    files[f"{di}/RECORD"] = buf.getvalue().encode()

    path = tmp / f"{dist}-{version}-{abi}-{abi}-win_amd64.whl"
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for n, d in files.items():
            z.writestr(n, d)
    return path


def make_nupkg(tmp: Path, version: str, pkg: str = "AffineUI") -> Path:
    """A nupkg shaped like a real `dotnet pack` output — including the .psmdcp."""
    nuspec = (
        '<?xml version="1.0"?>\n<package><metadata>\n'
        f"  <id>{pkg}</id>\n  <version>{version}</version>\n"
        '  <dependencies><dependency id="Other" version="9.9.9" /></dependencies>\n'
        "</metadata></package>\n"
    ).encode()
    # dotnet pack always emits this; it carries its OWN <version>.
    psmdcp = (
        '<?xml version="1.0"?>\n'
        '<coreProperties xmlns="http://schemas.openxmlformats.org/package/2006/metadata/core-properties">\n'
        f"  <version>{version}</version>\n"
        f"  <identifier>{pkg}</identifier>\n"
        "</coreProperties>\n"
    ).encode()

    path = tmp / f"{pkg}.{version}.nupkg"
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(f"{pkg}.nuspec", nuspec)
        z.writestr("package/services/metadata/core-properties/abc123.psmdcp", psmdcp)
        z.writestr("lib/net8.0/AffineUI.dll", b"MZ" + b"managed" * 32)
        z.writestr("runtimes/win-x64/native/affineui_c.dll", ext_bytes("win"))
        z.writestr("runtimes/linux-x64/native/libaffineui_c.so", ext_bytes("linux"))
        z.writestr("runtimes/osx-arm64/native/libaffineui_c.dylib", ext_bytes("osx"))
    return path


def rc_set(tmp: Path, rc: str = "0.5.0-rc.2") -> tuple[Path, Path, str]:
    """A pre-release directory + its manifest, as `Build a pre-release` leaves it."""
    src = tmp / "rc"
    src.mkdir()
    pep = rr.pep440(rc)
    make_wheel(src, pep, "cp311")
    make_wheel(src, pep, "cp312")
    make_nupkg(src, rc)
    manifest = src / "rc-hashes.json"
    rr.cmd_manifest(sorted(src.glob("*.whl")) + sorted(src.glob("*.nupkg")), manifest)
    return src, manifest, rc


# ── the wheel ────────────────────────────────────────────────────────────────

def test_wheel_preserves_the_compiled_payload(tmp_path):
    src = make_wheel(tmp_path, "0.5.0rc2")
    dst = rr.promote_wheel(src, "0.5.0", tmp_path / "out")
    assert dst.name == "affineui-0.5.0-cp312-cp312-win_amd64.whl"
    with zipfile.ZipFile(dst) as z:
        assert z.read("_affineui.cp312-win_amd64.pyd") == ext_bytes("cp312")


def test_wheel_rewrites_only_the_version_metadata(tmp_path):
    src = make_wheel(tmp_path, "0.5.0rc2")
    dst = rr.promote_wheel(src, "0.5.0", tmp_path / "out")
    with zipfile.ZipFile(dst) as z:
        names = z.namelist()
        meta = z.read("affineui-0.5.0.dist-info/METADATA").decode()
    assert not any("0.5.0rc2" in n for n in names), "no rc-versioned paths may survive"
    assert "Version: 0.5.0\n" in meta and "0.5.0rc2" not in meta
    assert "Summary: GPU-accelerated HTML/CSS UI" in meta   # untouched
    assert "Long description." in meta


def test_wheel_record_hashes_validate(tmp_path):
    """RECORD must be internally consistent — this is what pip verifies on install."""
    src = make_wheel(tmp_path, "0.5.0rc2")
    dst = rr.promote_wheel(src, "0.5.0", tmp_path / "out")
    checked = 0
    with zipfile.ZipFile(dst) as z:
        for row in csv.reader(io.StringIO(z.read("affineui-0.5.0.dist-info/RECORD").decode())):
            if not row:
                continue
            path, digest, size = row
            if path.endswith("/RECORD"):
                assert digest == "" and size == ""
                continue
            data = z.read(path)   # KeyError if RECORD references a stale path
            want = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
            assert digest == f"sha256={want}", f"stale RECORD hash for {path}"
            assert int(size) == len(data)
            checked += 1
    assert checked >= 3


# ── the nupkg ────────────────────────────────────────────────────────────────

def test_nupkg_rewrites_nuspec_and_psmdcp(tmp_path):
    """Both version elements. Rewriting only the nuspec ships a self-contradicting package."""
    src = make_nupkg(tmp_path, "0.5.0-rc.2")
    dst = rr.promote_nupkg(src, "0.5.0", tmp_path / "out")
    assert dst.name == "AffineUI.0.5.0.nupkg"
    with zipfile.ZipFile(dst) as z:
        nuspec = z.read("AffineUI.nuspec").decode()
        psmdcp = z.read("package/services/metadata/core-properties/abc123.psmdcp").decode()
    assert "<version>0.5.0</version>" in nuspec and "0.5.0-rc.2" not in nuspec
    assert "<version>0.5.0</version>" in psmdcp and "0.5.0-rc.2" not in psmdcp
    assert 'version="9.9.9"' in nuspec, "a <dependency version=...> attribute must not be rewritten"


def test_nupkg_preserves_all_three_natives(tmp_path):
    src = make_nupkg(tmp_path, "0.5.0-rc.2")
    dst = rr.promote_nupkg(src, "0.5.0", tmp_path / "out")
    with zipfile.ZipFile(dst) as z:
        assert z.read("runtimes/win-x64/native/affineui_c.dll") == ext_bytes("win")
        assert z.read("runtimes/linux-x64/native/libaffineui_c.so") == ext_bytes("linux")
        assert z.read("runtimes/osx-arm64/native/libaffineui_c.dylib") == ext_bytes("osx")


# ── the guarantee ────────────────────────────────────────────────────────────

def test_promote_full_set(tmp_path):
    src, manifest, rc = rc_set(tmp_path)
    out = tmp_path / "dist"
    assert rr.cmd_promote(src, rc, "0.5.0", out, manifest) == 0
    assert {p.name for p in out.glob("*.whl")} == {
        "affineui-0.5.0-cp311-cp311-win_amd64.whl",
        "affineui-0.5.0-cp312-cp312-win_amd64.whl",
    }
    assert (out / "AffineUI.0.5.0.nupkg").exists()


def test_a_swapped_binary_is_caught(tmp_path):
    """THE regression test.

    An earlier version matched artifacts on hash-SETS, so a cp312 wheel carrying
    the cp311 extension module verified clean. Promotion must compare each
    artifact against ITS OWN manifest entry, by member name.
    """
    src, manifest, rc = rc_set(tmp_path)
    victim = src / "affineui-0.5.0rc2-cp312-cp312-win_amd64.whl"
    with zipfile.ZipFile(victim) as z:
        members = {i.filename: z.read(i.filename) for i in z.infolist()}
    # Cross-contaminate: the cp312 wheel now holds cp311's compiled payload.
    members["_affineui.cp312-win_amd64.pyd"] = ext_bytes("cp311")
    with zipfile.ZipFile(victim, "w") as z:
        for n, d in members.items():
            z.writestr(n, d)

    with pytest.raises(SystemExit):
        rr.cmd_promote(src, rc, "0.5.0", tmp_path / "dist", manifest)


def test_a_dropped_artifact_is_caught(tmp_path):
    """Every artifact the rc built must be promoted — a partial release must not ship."""
    src, manifest, rc = rc_set(tmp_path)
    (src / "affineui-0.5.0rc2-cp311-cp311-win_amd64.whl").unlink()
    with pytest.raises(SystemExit):
        rr.cmd_promote(src, rc, "0.5.0", tmp_path / "dist", manifest)


def test_an_unknown_artifact_is_caught(tmp_path):
    """A file that wasn't part of the tested pre-release must not sneak in."""
    src, manifest, rc = rc_set(tmp_path)
    make_wheel(src, "0.5.0rc2", "cp313")     # never manifested
    with pytest.raises(SystemExit):
        rr.cmd_promote(src, rc, "0.5.0", tmp_path / "dist", manifest)


def test_missing_wheels_or_nupkg_is_caught(tmp_path):
    src = tmp_path / "rc"
    src.mkdir()
    make_wheel(src, "0.5.0rc2")
    manifest = src / "m.json"
    rr.cmd_manifest(sorted(src.glob("*.whl")), manifest)
    with pytest.raises(SystemExit):     # no nupkg at all
        rr.cmd_promote(src, "0.5.0-rc.2", "0.5.0", tmp_path / "dist", manifest)


def test_bit_identity_violation_is_fatal(tmp_path):
    with pytest.raises(SystemExit):
        rr.check_unchanged({"lib.so": "a" * 64}, {"lib.so": "b" * 64},
                           Path("rc.whl"), Path("final.whl"))


def test_promoting_to_a_prerelease_is_refused(tmp_path):
    src = make_wheel(tmp_path, "0.5.0rc2")
    with pytest.raises(SystemExit):
        rr.promote_wheel(src, "0.5.0-rc.3", tmp_path / "out")


def test_cross_cycle_promotion_is_refused(tmp_path):
    """An artifact from the 0.4.0 cycle must not be promotable to 0.5.0."""
    with pytest.raises(SystemExit):
        rr.promote_wheel(make_wheel(tmp_path, "0.4.0rc1"), "0.5.0", tmp_path / "out")
    with pytest.raises(SystemExit):
        rr.promote_nupkg(make_nupkg(tmp_path, "0.4.0-rc.1"), "0.5.0", tmp_path / "out")


def test_pep440_normalisation():
    assert rr.pep440("0.5.0-rc.2") == "0.5.0rc2"
    assert rr.pep440("1.2.3-beta.10") == "1.2.3beta10"
    assert rr.pep440("0.5.0") == "0.5.0"


# ── CLI ──────────────────────────────────────────────────────────────────────

def test_cli_end_to_end(tmp_path):
    src, manifest, rc = rc_set(tmp_path)
    out = tmp_path / "dist"
    r = subprocess.run(
        [sys.executable, str(SCRIPT), "promote", str(src), rc, "0.5.0",
         "-o", str(out), "--manifest", str(manifest)],
        capture_output=True, text=True,
    )
    assert r.returncode == 0, r.stderr
    assert "verified byte-for-byte identical" in r.stdout
    assert (out / "AffineUI.0.5.0.nupkg").exists()
