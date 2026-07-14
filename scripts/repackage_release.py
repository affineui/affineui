#!/usr/bin/env python3
"""Re-stamp a pre-release's artifacts to the final version, WITHOUT recompiling.

    repackage_release.py manifest  rc/*.whl rc/*.nupkg -o rc/hashes.json
    repackage_release.py promote   rc/ 0.5.0-rc.2 0.5.0 -o dist/ --manifest rc/hashes.json

`Publish a release` runs no compiler. It takes the artifacts `Build a
pre-release` produced, rewrites the handful of metadata strings that carry the
version, and ships those exact bytes. The thing you tested is the thing that
goes out — not "the same source, rebuilt", but literally the same compiled files.

Why that's sound
================
The pre-release suffix NEVER REACHES THE COMPILER.

The five manifests carry the release core (`0.5.0`) even while cutting
`0.5.0-rc.2` — see set_version.py --verify. The suffix is appended at build time
and lands only in packaging metadata. CMake gets the bare core (project(VERSION)
rejects a suffix anyway), so AFFINEUI_VERSION_{MAJOR,MINOR,PATCH} — the only
version the compiler ever sees — is identical for the rc and the final.

So `affineui_version()` in an rc binary already returns "0.5.0". It does not
start lying when promoted. The suffix lives ONLY here:

    wheel   METADATA `Version:`, the .dist-info/ dirname, RECORD, the filename
    nupkg   the .nuspec <version>, the .psmdcp <version>, the filename

Everything else — the extension module, the native libs, the managed assembly —
is copied byte-for-byte, and `promote` PROVES it: every compiled member is
hashed before and after, and any drift is a hard error. If someone ever plumbs
the suffix into a compile definition, this script starts failing instead of
quietly shipping a binary that disagrees with its own version string.
"""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import io
import json
import re
import sys
import zipfile
from pathlib import Path

# Members whose bytes we promise not to touch. Deliberately broad: a false
# positive costs nothing (we hash a text file too), a false negative would let a
# compiled artifact change without tripping the check.
COMPILED_SUFFIXES = (".pyd", ".so", ".dylib", ".dll", ".a", ".lib", ".exe")


def is_compiled(name: str) -> bool:
    n = name.lower()
    return n.endswith(COMPILED_SUFFIXES) or ".so." in n


def compiled_hashes(archive: Path) -> dict[str, str]:
    """{member name -> sha256} for every compiled member of a zip."""
    with zipfile.ZipFile(archive) as z:
        return {
            i.filename: hashlib.sha256(z.read(i.filename)).hexdigest()
            for i in z.infolist()
            if is_compiled(i.filename)
        }


def die(msg: str) -> None:
    print(f"::error::{msg}", file=sys.stderr)
    sys.exit(1)


# ── version spellings ────────────────────────────────────────────────────────
# PEP 440 normalises `0.5.0-rc.2` to `0.5.0rc2`, so a wheel's filename and
# METADATA use that spelling while the nupkg keeps the semver one. Both map to
# the same final `0.5.0`, which is spelled identically in every ecosystem.

def pep440(v: str) -> str:
    """`0.5.0-rc.2` -> `0.5.0rc2` (what pip/build actually write)."""
    return re.sub(r"-(rc|a|b|alpha|beta)\.?(\d+)$", r"\1\2", v).replace("-", "")


def assert_final(v: str) -> str:
    """The promotion target must be a bare triplet — we only promote TO a final."""
    v = v.strip().lstrip("v")
    if not re.fullmatch(r"\d+\.\d+\.\d+", v):
        die(f"target must be a bare MAJOR.MINOR.PATCH, got {v!r}")
    return v


# ── wheel ────────────────────────────────────────────────────────────────────
# A wheel is a zip:
#   affineui-<VER>.dist-info/METADATA   `Version: <VER>`
#   affineui-<VER>.dist-info/RECORD     csv of (path, sha256=<b64>, size)
#   _affineui.<abi>.pyd                 the compiled payload
# Filename: {dist}-{version}-{python}-{abi}-{platform}.whl

WHEEL_RE = re.compile(r"^(?P<dist>[^-]+)-(?P<ver>[^-]+)-(?P<rest>.+\.whl)$")


def record_row(path: str, data: bytes) -> tuple[str, str, str]:
    digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=")
    return (path, f"sha256={digest.decode()}", str(len(data)))


def promote_wheel(src: Path, final: str, outdir: Path) -> Path:
    final = assert_final(final)   # we only ever promote TO a bare triplet
    m = WHEEL_RE.match(src.name)
    if not m:
        die(f"not a wheel filename: {src.name}")
    dist, old_ver, rest = m["dist"], m["ver"], m["rest"]

    # The artifact must belong to the cycle we're promoting. Guards against
    # pointing a promotion at a stale directory from an earlier release.
    if not old_ver.startswith(final):
        die(
            f"{src.name} is not an artifact of {final} — refusing to promote an "
            f"artifact from a different release cycle."
        )

    dst = outdir / f"{dist}-{final}-{rest}"
    old_di, new_di = f"{dist}-{old_ver}.dist-info", f"{dist}-{final}.dist-info"

    before = compiled_hashes(src)
    if not before:
        die(f"{src.name} has no compiled member — a pure-python wheel is never what we ship")

    rows: list[tuple[str, str, str]] = []
    staged: list[tuple[zipfile.ZipInfo, str, bytes | None]] = []

    with zipfile.ZipFile(src) as zin:
        for info in zin.infolist():
            name = info.filename
            new_name = new_di + name[len(old_di):] if name.startswith(old_di + "/") else name

            if name == f"{old_di}/RECORD":
                staged.append((info, f"{new_di}/RECORD", None))  # written last
                continue

            data = zin.read(name)
            if name == f"{old_di}/METADATA":
                data, n = re.subn(
                    rb"(?mi)^Version:[ \t]*\S+[ \t]*$",
                    f"Version: {final}".encode(),
                    data,
                    count=1,
                )
                if n != 1:
                    die(f"expected 1 `Version:` line in {name}, rewrote {n}")

            staged.append((info, new_name, data))
            rows.append(record_row(new_name, data))

    # RECORD's own row carries no hash/size (PEP 427).
    rows.append((f"{new_di}/RECORD", "", ""))
    buf = io.StringIO()
    csv.writer(buf, lineterminator="\n").writerows(rows)
    record = buf.getvalue().encode()

    outdir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
        for info, new_name, data in staged:
            # Preserve external_attr so the executable bit on .so survives.
            ni = zipfile.ZipInfo(new_name, date_time=info.date_time)
            ni.external_attr = info.external_attr
            ni.create_system = info.create_system
            ni.compress_type = zipfile.ZIP_DEFLATED
            zout.writestr(ni, record if data is None else data)

    check_unchanged(before, compiled_hashes(dst), src, dst, old_di, new_di)
    return dst


# ── nupkg ────────────────────────────────────────────────────────────────────
# The version appears in TWO places inside a .nupkg, and missing the second one
# ships a package whose OPC core-properties disagree with its own nuspec:
#   <id>.nuspec                                          <version>
#   package/services/metadata/core-properties/*.psmdcp   <version>

NUPKG_RE = re.compile(r"^(?P<id>.+?)\.(?P<ver>\d+\.\d+\.\d+(?:-[0-9A-Za-z.\-]+)?)\.nupkg$")


def promote_nupkg(src: Path, final: str, outdir: Path) -> Path:
    final = assert_final(final)   # we only ever promote TO a bare triplet
    m = NUPKG_RE.match(src.name)
    if not m:
        die(f"not a nupkg filename: {src.name}")

    if not m["ver"].startswith(final):
        die(
            f"{src.name} is not an artifact of {final} — refusing to promote an "
            f"artifact from a different release cycle."
        )

    dst = outdir / f"{m['id']}.{final}.nupkg"
    before = compiled_hashes(src)
    if not before:
        die(f"{src.name} has no native library — the NuGet package must carry runtimes/*/native/*")

    rewrote = 0
    outdir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
        for info in zin.infolist():
            data = zin.read(info.filename)
            low = info.filename.lower()
            if low.endswith(".nuspec") or low.endswith(".psmdcp"):
                # Element-anchored, so a <dependency version="..."/> ATTRIBUTE
                # is not touched.
                data, n = re.subn(
                    rb"<version>[^<]*</version>",
                    f"<version>{final}</version>".encode(),
                    data,
                    count=1,
                )
                rewrote += n
            ni = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            ni.external_attr = info.external_attr
            ni.create_system = info.create_system
            ni.compress_type = zipfile.ZIP_DEFLATED
            zout.writestr(ni, data)

    # nuspec + psmdcp. If a future pack drops the psmdcp this catches it rather
    # than silently shipping a half-rewritten package.
    if rewrote < 1:
        die(f"{src.name}: no <version> element rewritten")

    check_unchanged(before, compiled_hashes(dst), src, dst)
    return dst


# ── the guarantee ────────────────────────────────────────────────────────────

def check_unchanged(
    before: dict[str, str],
    after: dict[str, str],
    src: Path,
    dst: Path,
    old_di: str | None = None,
    new_di: str | None = None,
) -> None:
    """Every compiled member must survive repackaging byte-for-byte."""
    def norm(k: str) -> str:
        if old_di and new_di and k.startswith(old_di + "/"):
            return new_di + k[len(old_di):]
        return k

    expected = {norm(k): v for k, v in before.items()}
    if set(expected) != set(after):
        die(
            f"compiled members changed between {src.name} and {dst.name}\n"
            f"  missing: {sorted(set(expected) - set(after))}\n"
            f"  added:   {sorted(set(after) - set(expected))}"
        )
    for name, want in expected.items():
        if after[name] != want:
            die(
                f"BIT-IDENTITY VIOLATED for {name}\n"
                f"  tested:   {want}\n"
                f"  shipping: {after[name]}\n"
                f"The compiled payload changed during repackaging. Refusing to publish.\n"
                f"If a compile definition now carries the pre-release suffix, the "
                f"compile-once model is invalid — see docs/RELEASING.md."
            )


def cmd_manifest(archives: list[Path], out: Path) -> int:
    manifest = {a.name: compiled_hashes(a) for a in archives}
    empty = [n for n, h in manifest.items() if not h]
    if empty:
        die(f"these artifacts contain no compiled member: {empty}")
    out.write_text(json.dumps(manifest, indent=2, sort_keys=True))
    total = sum(len(v) for v in manifest.values())
    print(f"[repackage] {len(manifest)} artifact(s), {total} compiled member(s) -> {out}")
    return 0


def cmd_promote(srcdir: Path, rc: str, final: str, outdir: Path, manifest_path: Path) -> int:
    """Re-stamp every rc artifact to `final`, then prove nothing compiled moved.

    The manifest is keyed on the RC's filenames. Rather than guessing which rc
    entry a promoted file came from (matching on hash-sets is unsound — a wheel
    cross-contaminated with another wheel's extension module would still
    "match"), we map each output back to its exact source and compare the full
    name->hash dict. We also assert every manifest entry was consumed, so a
    dropped artifact can't silently ship an incomplete release.
    """
    final = assert_final(final)
    rc = rc.strip().lstrip("v")
    recorded: dict[str, dict[str, str]] = json.loads(manifest_path.read_text())

    wheels = sorted(srcdir.glob("*.whl"))
    nupkgs = sorted(srcdir.glob("*.nupkg"))
    if not wheels or not nupkgs:
        die(
            f"incomplete pre-release: {len(wheels)} wheel(s), {len(nupkgs)} nupkg(s) "
            f"in {srcdir}. Refusing to publish a partial release."
        )

    # Every artifact must belong to the rc we were told to promote. Guards
    # against a stale download directory, or a `--manifest` from another cycle.
    rc_pep = pep440(rc)
    consumed: set[str] = set()
    for src in wheels + nupkgs:
        if src.name not in recorded:
            die(f"{src.name} is not in the manifest — it was not part of the tested pre-release")
        if rc not in src.name and rc_pep not in src.name:
            die(
                f"{src.name} is not an artifact of {rc} (expected {rc!r} or {rc_pep!r} "
                f"in the filename) — refusing to promote artifacts from another release."
            )

        dst = promote_wheel(src, final, outdir) if src.suffix == ".whl" \
            else promote_nupkg(src, final, outdir)

        # The real check: compare against THIS artifact's recorded hashes, by
        # member name — not against "some entry that happens to have the same set
        # of hashes", which would wave through a wheel cross-contaminated with
        # another wheel's extension module.
        #
        # Compared verbatim: compiled members live outside .dist-info (the only
        # path the repackaging renames), so promotion never moves one. If a
        # compiled member's PATH ever changes, that is itself a thing to catch,
        # not to normalise away.
        want = recorded[src.name]
        got = compiled_hashes(dst)
        if got != want:
            die(
                f"{dst.name} does not match the tested {src.name}\n"
                f"  tested:   {sorted(want.items())}\n"
                f"  shipping: {sorted(got.items())}"
            )
        consumed.add(src.name)
        print(f"[repackage] {src.name} -> {dst.name}  ({len(got)} compiled member(s) preserved)")

    # Coverage: nothing the pre-release built may be silently dropped.
    missed = set(recorded) - consumed
    if missed:
        die(
            f"{len(missed)} artifact(s) from the tested pre-release were not promoted: "
            f"{sorted(missed)}\nRefusing to publish an incomplete release."
        )

    print(f"[repackage] all {len(consumed)} artifact(s) verified byte-for-byte identical to the tested pre-release")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("manifest", help="hash the compiled members of the rc's artifacts")
    m.add_argument("archives", type=Path, nargs="+")
    m.add_argument("-o", "--out", type=Path, required=True)

    p = sub.add_parser("promote", help="re-stamp every rc artifact to the final version")
    p.add_argument("srcdir", type=Path)
    p.add_argument("rc", help="the pre-release version, e.g. 0.5.0-rc.2")
    p.add_argument("final", help="the release version, e.g. 0.5.0")
    p.add_argument("-o", "--outdir", type=Path, required=True)
    p.add_argument("--manifest", type=Path, required=True)

    a = ap.parse_args()
    if a.cmd == "manifest":
        return cmd_manifest(a.archives, a.out)
    return cmd_promote(a.srcdir, a.rc, a.final, a.outdir, a.manifest)


if __name__ == "__main__":
    sys.exit(main())
