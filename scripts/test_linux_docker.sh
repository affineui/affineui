#!/usr/bin/env bash
# Build + test AffineUI on Linux locally without leaving macOS / Windows.
# Reproduces the .github/workflows/ci.yml `linux` job inside a Docker
# container — same Ubuntu image, same packages, same configure/build/test
# commands. Use when you want a fast feedback loop on a Linux-only
# regression before pushing to CI.
#
# Usage:
#   scripts/test_linux_docker.sh                   # build + ctest
#   scripts/test_linux_docker.sh shell             # drop into a shell
#   scripts/test_linux_docker.sh -- ctest -R foo   # forward args to ctest
#
# Requirements: Docker (or Podman aliased as docker) installed and running.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${AFFINEUI_LINUX_IMAGE:-ubuntu:24.04}"
CONTAINER_WORK="/src"

if [[ "${1:-}" == "shell" ]]; then
    exec docker run --rm -it \
        -v "${REPO}:${CONTAINER_WORK}" \
        -w "${CONTAINER_WORK}" \
        "${IMAGE}" bash
fi

# All remaining args go to the script that runs inside the container.
EXTRA_ARGS=()
if [[ "${1:-}" == "--" ]]; then
    shift
    EXTRA_ARGS=("$@")
fi

# The build/ output ends up inside the mounted repo, so it persists between
# runs. To match the linux CI job, use a dedicated `build-linux/` dir so it
# doesn't clobber a local macOS/Windows build/.
BUILD_DIR="build-linux"

docker run --rm \
    -v "${REPO}:${CONTAINER_WORK}" \
    -w "${CONTAINER_WORK}" \
    -e DEBIAN_FRONTEND=noninteractive \
    "${IMAGE}" bash -lc "
        set -euo pipefail
        apt-get update -qq
        apt-get install -y --no-install-recommends \
            build-essential cmake ninja-build \
            libx11-dev libxi-dev libxcursor-dev \
            libgl1-mesa-dev libasound2-dev \
            xvfb >/dev/null
        cmake -S . -B ${BUILD_DIR} -G Ninja -DCMAKE_BUILD_TYPE=Release
        cmake --build ${BUILD_DIR} --parallel
        # Wrap ctest in xvfb-run so any test that touches sokol_app's X
        # connection has a display server. Headless layout / DOM tests
        # don't need it but xvfb-run is harmless for them.
        xvfb-run -a ctest --test-dir ${BUILD_DIR} --output-on-failure ${EXTRA_ARGS[@]:-}
    "
