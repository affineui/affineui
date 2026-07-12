#!/usr/bin/env bash
#
# AffineUI task runner (Linux / macOS / WSL / git-bash).
# Windows users: use build.ps1 (sets up the MSVC environment).
#
#   ./build.sh                  compile-check the prebuilt dist/ codefiles
#                               (uses the committed dist/; does NOT regenerate)
#   ./build.sh codefiles        (re)generate dist/affineui.{h,cpp}  (REQUIRES clang)
#   ./build.sh examples         build every example app
#   ./build.sh list             list every runnable example (C++ and Python)
#   ./build.sh run [name]       build + run one example  (default: hello)
#                               C++:    ./build.sh run decius_dender
#                               Python: ./build.sh run py_component_gallery
#   ./build.sh test             build + run the unit tests (ctest)
#   ./build.sh configure        cmake configure (Ninja) into ./build
#   ./build.sh clean            remove ./build
#   ./build.sh sync-nanovg      vendor the affineui_nanovg fork into external/nanovg
#   ./build.sh sync-lexbor      vendor the affineui_lexbor fork into external/lexbor
#   ./build.sh help             show this help
#
# 'codefiles' (the amalgamator) stages Lexbor as C++ and depends on clang's
# diagnostics; it fails fast if clang is absent. The no-arg default instead
# compile-checks the prebuilt dist/ with the platform compiler + native backend.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${ROOT}/build"
DIST="${ROOT}/dist"
SMOKE="${ROOT}/build/smoke"

# Primary example (what `run` launches with no name). The full set is NOT
# hardcoded — it's read from the manifest CMake writes at configure time
# (examples/CMakeLists.txt), so it can't drift out of date as examples are
# added, and optional ones only appear when their deps were actually found.
PRIMARY="hello"

# Python examples (bindings/python/examples), addressed as py_<script>. These
# are discovered from disk for the same reason the C++ list is generated: a
# hardcoded list rots. The Python bindings are OFF in the default configure
# (they need pybind11), so they get their own build dir and are only built when
# a py_* example is actually run — a plain C++ build pays nothing.
PY_EXAMPLES_DIR="${ROOT}/bindings/python/examples"
PY_BUILD="${ROOT}/build-python"
PY_STAGE="${PY_BUILD}/pystage"
PY_VENV="${PY_BUILD}/venv"
PY_BIN=""

py_example_targets() {
    [ -d "$PY_EXAMPLES_DIR" ] || return 0
    local f
    for f in "$PY_EXAMPLES_DIR"/*.py; do
        [ -f "$f" ] || continue
        local base; base="$(basename "$f" .py)"
        echo "py_${base}"
    done
    return 0
}

is_py_example() {
    grep -qx "$1" <<< "$(py_example_targets)"
}

py()    { command -v python3 || command -v python; }

find_clang() { command -v clang++ 2>/dev/null || command -v clang 2>/dev/null || true; }

require_clang() {
    local c; c="$(find_clang)"
    if [ -z "$c" ]; then
        echo "error: 'codefiles' (amalgamation) requires clang, which was not found on PATH." >&2
        echo "       The amalgamator stages Lexbor as C++ and depends on clang's diagnostics;" >&2
        echo "       other compilers are not supported for this step. Install LLVM/clang and retry." >&2
        exit 1
    fi
    echo "$c"
}

do_configure() {
    cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="RelWithDebInfo"
}
ensure_configured() { [ -f "$BUILD/CMakeCache.txt" ] || do_configure; }

codefiles() {
    local clang; clang="$(require_clang)"
    "$(py)" "$ROOT/tools/amalgamate.py" --root "$ROOT" --out "$DIST" --cxx "$clang"
}

smoke() {
    # Compile-check the *prebuilt* (committed) two-file SDK in dist/ with the
    # platform compiler + native backend. Does NOT regenerate dist/ (`codefiles`).
    if [ ! -f "$DIST/affineui.cpp" ] || [ ! -f "$DIST/affineui.h" ]; then
        echo "error: no prebuilt codefiles in dist/ (expected affineui.cpp + affineui.h)." >&2
        echo "       generate them first:  ./build.sh codefiles   (requires clang)" >&2
        exit 1
    fi
    mkdir -p "$SMOKE"
    local defs langflags
    case "$(uname -s)" in
        Darwin)   defs="-DSOKOL_METAL -DAFFINEUI_BACKEND_METAL -DSOKOL_NO_ENTRY"; langflags="-x objective-c++" ;;
        *)        defs="-DSOKOL_GLCORE -DAFFINEUI_BACKEND_GL -DSOKOL_NO_ENTRY";   langflags="" ;;
    esac
    local cxx
    case "${TOOLCHAIN:-auto}" in
        auto)  if [ "$(uname -s)" = Darwin ]; then cxx="${CXX:-clang++}"; else cxx="${CXX:-c++}"; fi ;;
        clang) cxx="$(command -v clang++ 2>/dev/null || command -v clang 2>/dev/null || true)" ;;
        gcc)   cxx="$(command -v g++ 2>/dev/null || command -v gcc 2>/dev/null || true)" ;;
        msvc)  echo "error: --toolchain=msvc is Windows-only (use build.ps1)" >&2; exit 1 ;;
        *)     echo "error: unknown --toolchain='${TOOLCHAIN}' (use auto|clang|gcc)" >&2; exit 1 ;;
    esac
    [ -n "$cxx" ] || { echo "error: --toolchain='${TOOLCHAIN}' compiler not found on PATH" >&2; exit 1; }
    echo "smoke: compiling dist/affineui.cpp ($cxx) ..."
    "$cxx" -std=c++20 -c $langflags $defs -I "$DIST" "$DIST/affineui.cpp" -o "$SMOKE/affineui.o"
    printf '#include "affineui.h"\nint main(){ affineui::Ui ui; (void)ui; return 0; }\n' > "$SMOKE/smoke_main.cpp"
    echo "smoke: compiling a consumer that includes affineui.h ..."
    "$cxx" -std=c++20 -c $defs -I "$DIST" "$SMOKE/smoke_main.cpp" -o "$SMOKE/smoke_main.o"
    echo "smoke: OK"
}

# Example targets that actually exist in the configured build, straight from
# the manifest CMake wrote at configure time. Optional examples (hello_sdl,
# embed_d3d11) are absent from it when their deps weren't found, so whatever
# this prints is genuinely runnable.
example_targets() {
    ensure_configured
    local manifest="$BUILD/examples/examples.txt"
    [ -f "$manifest" ] || return 0
    # `return 0`, not the grep's status: a manifest with nothing to strip makes
    # grep exit 1, and under `set -e` that would kill the caller mid-assignment
    # — silently. (Same trap origin/main hit from the other direction.)
    grep -v '^[[:space:]]*$' "$manifest" || true
    return 0
}

build_examples() {
    local tgts; tgts="$(example_targets)"
    [ -n "$tgts" ] || { echo "no example targets in this build."; return; }
    # shellcheck disable=SC2086
    cmake --build "$BUILD" --target $tgts --parallel
}

list_examples() {
    echo "C++ examples    (./build.sh run <name>)"
    example_targets | sed 's/^/    /'
    echo
    echo "Python examples (./build.sh run <name>)"
    py_example_targets | sed 's/^/    /'
}

# ── Python examples ──────────────────────────────────────────────────
# Run in a private venv, NOT the ambient interpreter. An `affineui` installed
# with `pip install -e` registers a sys.meta_path finder, and a meta_path finder
# BEATS PYTHONPATH — so an editable install (even a stale one pointing at a
# different worktree) silently wins over anything we stage, and the examples end
# up running against another checkout entirely. The venv has no affineui in it,
# so what we build here is what runs.
ensure_python_venv() {
    if [ ! -x "$PY_VENV/bin/python" ]; then
        echo "creating the Python example venv ($PY_VENV) ..."
        "$(py)" -m venv "$PY_VENV"
        "$PY_VENV/bin/python" -m pip install -q --upgrade pip
        # pybind11 is a BUILD dependency: it ships the CMake config package.
        "$PY_VENV/bin/python" -m pip install -q pybind11
    fi
    PY_BIN="$PY_VENV/bin/python"
}

ensure_python_configured() {
    ensure_python_venv
    [ -f "$PY_BUILD/CMakeCache.txt" ] && return 0
    local pybind_dir
    pybind_dir="$("$PY_BIN" -c 'import pybind11; print(pybind11.get_cmake_dir())')"
    cmake -S "$ROOT" -B "$PY_BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE="RelWithDebInfo" \
        -DAFFINEUI_BUILD_PYTHON_BINDINGS=ON \
        -DAFFINEUI_BUILD_EXAMPLES=OFF \
        -DAFFINEUI_BUILD_TESTS=OFF \
        -DAFFINEUI_BUILD_TOOLS=OFF \
        -DPython_EXECUTABLE="$PY_BIN" \
        -Dpybind11_DIR="$pybind_dir"
}

stage_python() {
    ensure_python_configured
    cmake --build "$PY_BUILD" --parallel
    rm -rf "$PY_STAGE"
    mkdir -p "$PY_STAGE/affineui"
    cp -R "$ROOT/bindings/python/python/affineui/." "$PY_STAGE/affineui/"
    rm -rf "$PY_STAGE/affineui/__pycache__"
    # The package does `from ._affineui import ...`, so the extension has to sit
    # INSIDE the package directory.
    local core; core="$(find "$PY_BUILD" \( -name '_affineui*.so' -o -name '_affineui*.pyd' \) -print -quit)"
    [ -n "$core" ] || { echo "error: no _affineui module was built under $PY_BUILD" >&2; exit 1; }
    cp "$core" "$PY_STAGE/affineui/"
    # photo_core (the photo sample's raster core) must land in the SAME directory
    # as _affineui: they are pybind11 modules sharing process-global state, and
    # the sample deliberately refuses to load a photo_core from anywhere else
    # rather than segfault on a mismatched pair.
    local photo; photo="$(find "$PY_BUILD" \( -name 'photo_core*.so' -o -name 'photo_core*.pyd' \) -print -quit)"
    [ -n "$photo" ] && cp "$photo" "$PY_STAGE/affineui/"
    return 0
}

run_python_example() {
    local script="$PY_EXAMPLES_DIR/${1#py_}.py"
    [ -f "$script" ] || { echo "error: no such Python example: $script" >&2; exit 1; }
    stage_python
    echo "running $script"
    # Assets resolve relative to the repo root. PYTHONPATH is SET, not appended
    # to: an inherited one could point at another checkout's bindings.
    ( cd "$ROOT" && PYTHONPATH="$PY_STAGE" "$PY_BIN" "$script" )
}

run_example() {
    local name="${1:-$PRIMARY}"
    if is_py_example "$name"; then
        run_python_example "$name"
        return
    fi
    # Materialize the list first, then grep the string — piping into `grep -q`
    # short-circuits `example_targets` (grep closes stdin on first match), which
    # trips `pipefail` and makes valid targets read as "not available".
    local available; available="$(example_targets)"
    if ! grep -qx "$name" <<< "$available"; then
        echo "unknown example '$name'." >&2
        echo "available:" >&2
        sed 's/^/  /' <<< "$available" >&2
        py_example_targets | sed 's/^/  /' >&2
        exit 1
    fi
    cmake --build "$BUILD" --target "$name" --parallel
    local exe; exe="$(find "$BUILD/examples" -type f -name "$name" -perm -u+x 2>/dev/null | head -1)"
    [ -n "$exe" ] || { echo "built '$name' but no executable found under $BUILD/examples" >&2; exit 1; }
    echo "running $exe"
    ( cd "$ROOT" && "$exe" )   # assets/ resolve relative to repo root
}

run_tests() {
    ensure_configured
    cmake --build "$BUILD" --parallel
    ctest --test-dir "$BUILD" --output-on-failure
}

usage() { sed -n '2,/^set -euo/p' "$ROOT/build.sh" | sed 's/^#\s\?//; /^set -euo/d'; }

# Parse GNU-style flags (e.g. --toolchain=clang) out of the args, anywhere.
TOOLCHAIN="auto"
POSITIONAL=()
for arg in "$@"; do
    case "$arg" in
        --toolchain=*) TOOLCHAIN="${arg#*=}" ;;
        --*)           echo "warning: unknown option '$arg'" >&2 ;;
        *)             POSITIONAL+=("$arg") ;;
    esac
done
set -- ${POSITIONAL[@]+"${POSITIONAL[@]}"}

cmd="${1:-}"; shift || true
case "$cmd" in
    "")          smoke ;;
    codefiles)   codefiles ;;
    examples)    build_examples ;;
    list)        list_examples ;;
    run)         run_example "${1:-}" ;;
    test)        run_tests ;;
    conformance) echo "conformance harness is Windows/D3D11-only for now — use build.ps1 conformance (see docs/CONFORMANCE.md)" >&2; exit 1 ;;
    configure)   do_configure ;;
    clean)       rm -rf "$BUILD" ;;
    sync-nanovg) bash "$ROOT/scripts/sync_nanovg_from_fork.sh" ;;
    sync-lexbor) bash "$ROOT/scripts/sync_lexbor_from_fork.sh" ;;
    help)        usage ;;
    *)           echo "unknown command '$cmd'"; echo; usage; exit 1 ;;
esac
