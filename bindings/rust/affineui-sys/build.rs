// Locate and link the affineui_c shared library.
//
// Resolution order (docs/LANGUAGE_BINDINGS.md §4.1):
//   1. AFFINEUI_LIB_DIR environment variable.
//   2. The repo-checkout dev path (<repo>/build/ninja) so
//      `build.ps1` / cmake --build followed by `cargo build` just works.
//
// Running binaries additionally needs affineui_c.dll / libaffineui_c.so
// on the loader path; see bindings/rust/README.md.

use std::env;
use std::path::{Path, PathBuf};

fn import_lib_name() -> &'static str {
    if cfg!(target_os = "windows") {
        "affineui_c.lib"
    } else if cfg!(target_os = "macos") {
        "libaffineui_c.dylib"
    } else {
        "libaffineui_c.so"
    }
}

fn shared_lib_name() -> &'static str {
    if cfg!(target_os = "windows") {
        "affineui_c.dll"
    } else if cfg!(target_os = "macos") {
        "libaffineui_c.dylib"
    } else {
        "libaffineui_c.so"
    }
}

// Copy the shared library next to every built binary so `cargo run` /
// `cargo test` find it without any PATH setup. Windows searches only the
// exe's own directory, and cargo scatters binaries across
// target/<profile>, its examples/ subdir, and deps/ (test binaries).
// OUT_DIR is target/<profile>/build/<pkg>-<hash>/out — three ancestors up.
fn copy_shared_lib_to_target(search_dir: &Path) {
    let src = search_dir.join(shared_lib_name());
    if !src.exists() {
        return;
    }
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let Some(profile_dir) = out_dir.ancestors().nth(3) else { return };
    for dir in [profile_dir.to_path_buf(), profile_dir.join("examples"), profile_dir.join("deps")] {
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

fn main() {
    println!("cargo:rerun-if-env-changed=AFFINEUI_LIB_DIR");

    let mut search_dir = env::var_os("AFFINEUI_LIB_DIR").map(PathBuf::from);

    if search_dir.is_none() {
        // bindings/rust/affineui-sys -> repo root is three levels up.
        let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
        let dev = manifest.join("..").join("..").join("..").join("build").join("ninja");
        if dev.join(import_lib_name()).exists() {
            search_dir = Some(dev);
        }
    }

    match search_dir {
        Some(dir) => {
            println!("cargo:rustc-link-search=native={}", dir.display());
            copy_shared_lib_to_target(&dir);
        }
        None => {
            println!(
                "cargo:warning=affineui_c library not found. Set AFFINEUI_LIB_DIR \
                 to the directory containing {} (e.g. <repo>/build/ninja after \
                 `cmake --build build/ninja --target affineui_c` with \
                 -DAFFINEUI_BUILD_C_SHARED=ON).",
                import_lib_name()
            );
        }
    }
    println!("cargo:rustc-link-lib=dylib=affineui_c");
}
