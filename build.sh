#!/usr/bin/env bash
#
# AffineUI task runner (Linux / macOS / WSL / git-bash).
# Windows users: use build.ps1 (sets up the MSVC environment).
#
#   ./build.sh                  compile-check the prebuilt dist/ codefiles
#                               (uses the committed dist/; does NOT regenerate)
#   ./build.sh codefiles        (re)generate dist/affineui.{h,cpp}  (REQUIRES clang)
#   ./build.sh examples         build every example app
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

# Primary example (what `run` launches with no name) + the full set.
PRIMARY="hello"
EXAMPLES=(hello bootstrap hello_sdl media imm_counter imm_todo text_flow forms_focus bootstrap_kitchen embed_d3d11 bootstrap_dashboard decius_game_editor decius_video_editor decius_synth_dark decius_synth_skeuo command_panel decius_dender affine_2600 virtual_list)

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

# Example targets that actually exist in the configured build (some are
# optional, e.g. hello_sdl is skipped when SDL2 isn't found).
example_targets() {
    ensure_configured
    # NB: feed grep from a here-string, not a pipe. `grep -q` exits on
    # first match and closes stdin, which SIGPIPEs the upstream printf;
    # combined with `set -o pipefail` that used to make the `&&` branch
    # silently skip and mark the target as "not available".
    local present; present="$(ninja -C "$BUILD" -t targets all 2>/dev/null | sed 's/:.*//')"
    local e
    for e in "${EXAMPLES[@]}"; do
        # `if`, not `&&`: the loop's status is its last command's, so a final
        # example that ISN'T present would leave the function returning 1 and
        # `set -e` would kill the caller mid-assignment — silently.
        if grep -qx "$e" <<< "$present"; then
            echo "$e"
        fi
    done
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
    if [[ ! " ${EXAMPLES[*]} " == *" $name "* ]]; then
        echo "unknown example '$name'." >&2
        echo "available: ${EXAMPLES[*]}" >&2
        exit 1
    fi
    # Materialize the target list first, then grep the string — piping
    # into `grep -q` short-circuits `example_targets`, which trips
    # `pipefail` and makes valid targets read as "not available".
    local available; available="$(example_targets)"
    if ! grep -qx "$name" <<< "$available"; then
        echo "example '$name' isn't available in this build (an optional dep, e.g. SDL2, wasn't found at configure)." >&2
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
