#!/usr/bin/env bash
#
# AffineUI task runner (Linux / macOS / WSL / git-bash).
# Windows users: use build.ps1 (sets up the MSVC environment).
#
#   ./build.sh                  compile-check the prebuilt dist/ codefiles
#                               (uses the committed dist/; does NOT regenerate)
#   ./build.sh codefiles        (re)generate dist/affineui.{h,cpp}  (REQUIRES clang)
#   ./build.sh examples         build every example app
#   ./build.sh list             list every runnable example in this build
#   ./build.sh run [name]       build + run one example  (default: hello)
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

run_example() {
    local name="${1:-$PRIMARY}"
    # Materialize the list first, then grep the string — piping into `grep -q`
    # short-circuits `example_targets` (grep closes stdin on first match), which
    # trips `pipefail` and makes valid targets read as "not available".
    local available; available="$(example_targets)"
    if ! grep -qx "$name" <<< "$available"; then
        echo "unknown example '$name'." >&2
        echo "available:" >&2
        sed 's/^/  /' <<< "$available" >&2
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
    list)        example_targets ;;
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
