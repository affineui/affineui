#!/usr/bin/env bash
# Reproduce the .github/workflows/wheels.yml Linux job locally — build a
# manylinux wheel for the affineui Python bindings, smoke-test it inside
# the same container the CI uses. Useful for fixing a Linux wheel failure
# without round-tripping through GitHub Actions.
#
# Usage:
#   scripts/test_linux_wheel_docker.sh                  # build cp312 wheel
#   scripts/test_linux_wheel_docker.sh cp310 cp312      # build several
#   scripts/test_linux_wheel_docker.sh --keep-output    # leave wheelhouse/ behind
#
# Prerequisites:
#   - Docker Desktop / Colima / Podman running locally.
#   - Python 3 with `pip` (cibuildwheel pulls itself in).
#
# What it actually does:
#   1. Pip-installs cibuildwheel into a venv at /tmp/affineui-cibw if absent.
#   2. Runs `cibuildwheel --only cp312-manylinux_x86_64 bindings/python`
#      (or whatever Python tags you passed). cibuildwheel spawns the
#      manylinux_2_28 container per tag, installs deps, builds, and runs
#      `xvfb-run -a pytest -x bindings/python/tests` inside.
#   3. Drops the result into ./wheelhouse/.
#
# The full CI matrix is 15 wheels; this script defaults to one to keep
# the local loop short. Pass extra tags as args to widen.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "${REPO}"

# --keep-output retains wheelhouse/ across runs; default cleans it so a
# stale "previously-built" wheel doesn't mask a failure to rebuild.
KEEP=0
TAGS=()
for arg in "$@"; do
    case "$arg" in
        --keep-output) KEEP=1 ;;
        cp*) TAGS+=("$arg") ;;
        *) echo "unknown arg: $arg (expected --keep-output or cpXY tag)" >&2; exit 2 ;;
    esac
done
if [[ ${#TAGS[@]} -eq 0 ]]; then
    TAGS=("cp312")  # newest Python that matches a stable Python release.
fi

# Docker required — give a clear hint if the daemon isn't reachable.
if ! docker version >/dev/null 2>&1; then
    echo "error: docker daemon not reachable." >&2
    echo "  On macOS:    start Docker Desktop or Colima." >&2
    echo "  On Linux:    sudo systemctl start docker" >&2
    exit 1
fi

# cibuildwheel lives in its own venv so we don't pollute the system /pyenv.
VENV="${REPO}/.cibw-venv"
if [[ ! -d "${VENV}" ]]; then
    python3 -m venv "${VENV}"
    "${VENV}/bin/pip" install --upgrade pip cibuildwheel >/dev/null
fi

[[ ${KEEP} -eq 1 ]] || rm -rf wheelhouse

# Build the listed tags only, all targeted at manylinux_x86_64. cibuildwheel's
# tag syntax is `<pythontag>-<platformtag>`; we expand each Python tag once.
ONLY=""
for t in "${TAGS[@]}"; do
    ONLY="${ONLY:+${ONLY} }${t}-manylinux_x86_64"
done

# Mirror the wheels.yml env so the local run matches CI byte-for-byte. The
# Linux package install + xvfb-run test wrapper come from wheels.yml.
# manylinux_2_28 (AlmaLinux 9 + gcc-toolset-13) is required — cibuildwheel's
# default manylinux2014 (CentOS 7 + GCC 10) lacks <source_location>.
export CIBW_BUILD="${ONLY}"
export CIBW_MANYLINUX_X86_64_IMAGE="manylinux_2_28"
export CIBW_BEFORE_ALL_LINUX="yum install -y libX11-devel libXi-devel libXcursor-devel libxkbcommon-devel mesa-libGL-devel alsa-lib-devel ninja-build xorg-x11-server-Xvfb"
export CIBW_ENVIRONMENT="CMAKE_POLICY_VERSION_MINIMUM=3.5"
export CIBW_TEST_REQUIRES="pytest"
export CIBW_TEST_COMMAND_LINUX="xvfb-run -a pytest -x {package}/tests"

"${VENV}/bin/cibuildwheel" --output-dir wheelhouse bindings/python

echo
echo "Wheels in ./wheelhouse/:"
ls -1 wheelhouse/
