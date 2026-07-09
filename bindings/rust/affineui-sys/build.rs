// affineui-sys build script.
//
// Two build modes, gated by cargo features:
//
//   default = ["vendored"] — build affineui_c from the vendored C++
//     source in `vendor/` via cmake. Ships everything the C ABI needs
//     to link. Requires `cmake` + a C++20 compiler on the user's
//     machine (Rust convention for -sys crates). First build is slow
//     (~1-2 min on a laptop) because cmake compiles the whole engine;
//     subsequent builds hit the cargo cache.
//
//   ["system"] (no default) — link against a system-installed
//     libaffineui_c located via AFFINEUI_LIB_DIR. Fast build but the
//     caller has to have provided the shared library themselves.
//
// The vendored source is populated by `scripts/vendor.sh` at cargo
// publish time (from the parent monorepo). For an in-tree dev build
// where `vendor/` isn't populated, build.rs falls back to the
// monorepo's own CMakeLists.txt at ../../../ so `cargo build` inside
// a fresh clone just works.

use std::env;
use std::path::{Path, PathBuf};

fn shared_lib_name() -> &'static str {
    if cfg!(target_os = "windows") {
        "affineui_c.dll"
    } else if cfg!(target_os = "macos") {
        "libaffineui_c.dylib"
    } else {
        "libaffineui_c.so"
    }
}

fn import_lib_name() -> &'static str {
    // On Windows, the linker consumes `affineui_c.lib` (the import lib).
    // On Unix, there's no separate import lib — the shared object is
    // both the linkage target and the runtime target.
    if cfg!(target_os = "windows") {
        "affineui_c.lib"
    } else {
        shared_lib_name()
    }
}

fn main() {
    println!("cargo:rerun-if-env-changed=AFFINEUI_LIB_DIR");
    println!("cargo:rerun-if-changed=build.rs");

    // Feature negotiation. `system` alone (default-features = false)
    // means "no vendored build, only look for a pre-built system lib".
    let system_only = cfg!(feature = "system") && !cfg!(feature = "vendored");

    if system_only {
        link_system_lib_or_fail();
        return;
    }

    // Vendored path — build affineui_c from C++ source via cmake.
    match locate_cmake_source() {
        Some(src_dir) => build_vendored(&src_dir),
        None => {
            // No vendored source AND no monorepo checkout — try the
            // system-installed lib as a last resort. Useful for
            // advanced users who unpacked the crate manually and set
            // AFFINEUI_LIB_DIR themselves.
            eprintln!(
                "affineui-sys: vendored source not found under either \
                 `vendor/` or the monorepo checkout path; falling back \
                 to a system-installed libaffineui_c."
            );
            link_system_lib_or_fail();
        }
    }
}

// ─── locate + build vendored source ──────────────────────────────────

fn locate_cmake_source() -> Option<PathBuf> {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());

    // 1. Published-crate case: vendor/ populated by scripts/vendor.sh.
    let vendor = manifest.join("vendor");
    if vendor.join("CMakeLists.txt").is_file() {
        println!("cargo:rerun-if-changed=vendor/CMakeLists.txt");
        return Some(vendor);
    }

    // 2. Monorepo dev case: repo root is three levels up
    //    (bindings/rust/affineui-sys → repo root).
    let candidate = manifest.join("..").join("..").join("..");
    if let Ok(repo_root) = candidate.canonicalize() {
        if repo_root.join("CMakeLists.txt").is_file() {
            return Some(repo_root);
        }
    }

    None
}

fn build_vendored(src_dir: &Path) {
    // `cmake` crate builds into $OUT_DIR/build. `build_target` runs
    // `cmake --build --target affineui_c` (skips install; we don't
    // need install rules).
    //
    // The `-DAFFINEUI_BUILD_*=OFF` chain turns off every top-level
    // affineui target that isn't affineui_c. The top-level CMakeLists
    // still `add_subdirectory`'s them (extras/skeuo, extras/tools),
    // but with no targets emitted; the vendored source tree includes
    // those subdirectories precisely so the add_subdirectory calls
    // don't error.
    let dst = cmake::Config::new(src_dir)
        .define("AFFINEUI_BUILD_C_SHARED", "ON")
        .define("AFFINEUI_BUILD_EXAMPLES", "OFF")
        .define("AFFINEUI_BUILD_TESTS", "OFF")
        .define("AFFINEUI_BUILD_TOOLS", "OFF")
        .define("AFFINEUI_INSTALL", "OFF")
        // The Roboto-Regular.ttf blob in assets/ is only used by the
        // embed-font code path; the crate lets the host provide fonts
        // instead, so we skip the asset entirely. Keeps the vendored
        // source tree smaller and avoids one more moving part.
        .define("AFFINEUI_NO_EMBEDDED_FONTS", "ON")
        .define("CMAKE_BUILD_TYPE", "Release")
        .build_target("affineui_c")
        .build();

    let build_dir = dst.join("build");

    // The library might land in either the top of the build tree
    // (single-config generators — Ninja, Make) or under a subdir
    // (multi-config generators — Xcode, VS).
    let shared_lib_search = find_containing_dir(&build_dir, shared_lib_name())
        .unwrap_or_else(|| build_dir.clone());

    println!(
        "cargo:rustc-link-search=native={}",
        shared_lib_search.display()
    );
    println!("cargo:rustc-link-lib=dylib=affineui_c");

    // Copy the shared lib next to the eventual Rust binary so
    // `cargo run` / `cargo test` find it at runtime without any
    // PATH setup (matches the previous DX).
    copy_shared_lib_to_target(&shared_lib_search);
}

// ─── system-installed fallback (feature = "system", or vendored miss) ─

fn link_system_lib_or_fail() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());

    let mut search_dir = env::var_os("AFFINEUI_LIB_DIR").map(PathBuf::from);

    if search_dir.is_none() {
        // Same convenience path the old build.rs used: repo checkout
        // dev build at <repo>/build/ninja.
        let dev = manifest
            .join("..").join("..").join("..")
            .join("build").join("ninja");
        if dev.join(import_lib_name()).exists() {
            search_dir = Some(dev);
        }
    }

    match search_dir {
        Some(dir) => {
            println!("cargo:rustc-link-search=native={}", dir.display());
            copy_shared_lib_to_target(&dir);
            println!("cargo:rustc-link-lib=dylib=affineui_c");
        }
        None => {
            panic!(
                "affineui-sys: cannot locate affineui_c to link against. \
                 Either build the vendored source (default features — \
                 requires cmake + C++20 compiler), or set AFFINEUI_LIB_DIR \
                 to a directory containing {}.",
                import_lib_name()
            );
        }
    }
}

// ─── shared helpers ──────────────────────────────────────────────────

fn find_containing_dir(root: &Path, filename: &str) -> Option<PathBuf> {
    // BFS through the cmake build tree looking for the shared lib.
    // Cap depth so we don't wander into deeply-vendored deps.
    let mut queue = vec![(root.to_path_buf(), 0usize)];
    while let Some((dir, depth)) = queue.pop() {
        if depth > 4 {
            continue;
        }
        let entries = match std::fs::read_dir(&dir) {
            Ok(e) => e,
            Err(_) => continue,
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                queue.push((path, depth + 1));
            } else if path.file_name().and_then(|n| n.to_str()) == Some(filename) {
                return path.parent().map(Path::to_path_buf);
            }
        }
    }
    None
}

/// Copy the shared library next to every built binary so `cargo run` /
/// `cargo test` find it without any PATH setup. Windows searches only
/// the exe's own directory; cargo scatters binaries across
/// target/<profile>, target/<profile>/examples, and target/<profile>/deps.
/// OUT_DIR is target/<profile>/build/<pkg>-<hash>/out — three ancestors up.
fn copy_shared_lib_to_target(search_dir: &Path) {
    let src = search_dir.join(shared_lib_name());
    if !src.exists() {
        return;
    }
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let Some(profile_dir) = out_dir.ancestors().nth(3) else { return };
    for dir in [
        profile_dir.to_path_buf(),
        profile_dir.join("examples"),
        profile_dir.join("deps"),
    ] {
        let _ = std::fs::create_dir_all(&dir);
        let dst = dir.join(shared_lib_name());
        if let Err(e) = std::fs::copy(&src, &dst) {
            println!(
                "cargo:warning=could not copy {} to {} ({e}); add {} to your \
                 loader path to run",
                shared_lib_name(),
                dir.display(),
                search_dir.display()
            );
        }
    }
}
