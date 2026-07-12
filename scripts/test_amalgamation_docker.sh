#!/usr/bin/env bash
# Generate the two-file SDK (affineui.h + affineui.cpp) and compile it on
# Linux with BOTH gcc and clang.
#
# Why this exists: ci.yml's `amalgamate` job only ever compiles the
# amalgamation on macOS (clang, Metal/GL). The two-file form is the default
# integration story for end users, so it also has to compile under gcc — the
# permissive compiler that lets through what MSVC rejects, and vice versa.
# Nothing verified that until this script.
#
# Note the split of roles:
#   - GENERATING the amalgamation needs clang. amalgamate.py compiles the
#     staged Lexbor tree to repair it and to scan objects for unprefixed
#     globals, and it passes clang-only flags (-Wno-c++11-narrowing) to do
#     it. That is a release-machine-only requirement.
#   - CONSUMING the generated affineui.cpp must work on gcc, clang, and MSVC.
#     That is what this script actually proves.
#
# Usage:
#   scripts/test_amalgamation_docker.sh            # generate + compile (gcc, clang)
#   scripts/test_amalgamation_docker.sh shell      # drop into a shell
#
# Requirements: Docker (or Podman aliased as docker).

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

# Keep the generated artifacts out of the Windows/macOS build/ tree.
OUT_DIR="build-amalg"

# The container runs as root and writes ${OUT_DIR} through the bind mount, so on
# Linux/macOS it would leave root-owned artifacts the invoking user can't delete.
# Hand ownership back on the way out. (No-op on Docker Desktop for Windows, where
# the mount is already remapped — hence the `|| true`.)
HOST_IDS="$(id -u 2>/dev/null || echo 0):$(id -g 2>/dev/null || echo 0)"

docker run --rm \
    -v "${REPO}:${CONTAINER_WORK}" \
    -w "${CONTAINER_WORK}" \
    -e DEBIAN_FRONTEND=noninteractive \
    "${IMAGE}" bash -lc "
        set -euo pipefail
        trap 'chown -R ${HOST_IDS} ${OUT_DIR} 2>/dev/null || true' EXIT
        apt-get update -qq
        apt-get install -y --no-install-recommends \
            build-essential clang cmake ninja-build git ca-certificates \
            python3 \
            libx11-dev libxi-dev libxcursor-dev \
            libgl1-mesa-dev libasound2-dev >/dev/null

        echo '=== gcc/clang versions ==='
        gcc --version | head -1
        clang++ --version | head -1

        # --- Generate ------------------------------------------------------
        # amalgamate.py needs clang to repair the staged Lexbor C++ tree.
        echo
        echo '=== Generating amalgamation (clang) ==='
        mkdir -p ${OUT_DIR}
        python3 tools/amalgamate.py \
            --root . \
            --out ${OUT_DIR} \
            --cxx clang++

        ls -la ${OUT_DIR}/affineui.h ${OUT_DIR}/affineui.cpp

        # --- Compile: the part CI has never done ---------------------------
        # GL backend is the Linux default. SOKOL_NO_ENTRY keeps sokol from
        # claiming main(). Compile only (-c): we are proving the translation
        # unit is valid under each compiler, not linking an app.
        FLAGS='-std=c++20 -DSOKOL_GLCORE -DSOKOL_NO_ENTRY -DAFFINEUI_BACKEND_GL'

        echo
        echo '=== Compiling amalgamation with g++ (GL) ==='
        g++ \$FLAGS -I ${OUT_DIR} -c ${OUT_DIR}/affineui.cpp -o ${OUT_DIR}/amalg_gcc.o
        echo 'g++: OK'

        echo
        echo '=== Compiling amalgamation with clang++ (GL) ==='
        clang++ \$FLAGS -I ${OUT_DIR} -c ${OUT_DIR}/affineui.cpp -o ${OUT_DIR}/amalg_clang.o
        echo 'clang++: OK'

        # --- Adapter variant (AFFINEUI_WITH_SOKOL) -------------------------
        echo
        echo '=== Compiling amalgamation with g++ (adapter, AFFINEUI_WITH_SOKOL) ==='
        g++ \$FLAGS -DAFFINEUI_WITH_SOKOL \
            -I ${OUT_DIR} -c ${OUT_DIR}/affineui.cpp -o ${OUT_DIR}/amalg_gcc_adapter.o
        echo 'g++ adapter: OK'

        echo
        echo '=== Sizes ==='
        ls -la ${OUT_DIR}/*.o
        echo
        echo 'AMALGAMATION LINUX CHECK: PASS (gcc + clang)'
    "
