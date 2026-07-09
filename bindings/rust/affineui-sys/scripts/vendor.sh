#!/usr/bin/env bash
# Populate bindings/rust/affineui-sys/vendor/ with everything the crate
# needs to build affineui_c from source when installed from crates.io.
#
# Run this before `cargo publish` (release.yml wires it in automatically).
# The output tree is intentionally gitignored — it's regenerated per
# release from whatever is on the current ref.
set -euo pipefail

# Absolute path to this script's own directory, resolved via readlink
# so the script works no matter the caller's cwd.
script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
crate_dir="$( cd "$script_dir/.." && pwd )"
repo_root="$( cd "$crate_dir/../../.." && pwd )"

vendor="$crate_dir/vendor"
echo "▸ vendoring source from $repo_root into $vendor"

# Purge any prior vendor tree so a partial removal upstream doesn't
# leave dangling files behind.
rm -rf "$vendor"
mkdir -p "$vendor"

# Everything cmake needs to build the `affineui_c` target — headers,
# implementation, cmake glue, and the whole vendored deps tree.
copy_paths=(
  "CMakeLists.txt"
  "LICENSE"
  "cmake"
  "include"
  "src"
  "external"
  "extras"                   # extras/skeuo + extras/tools are
                             # `add_subdirectory`'d unconditionally by
                             # the top-level CMakeLists; the crate turns
                             # their targets off via -D options but the
                             # directories themselves must exist.
)

for p in "${copy_paths[@]}"; do
  src="$repo_root/$p"
  if [ ! -e "$src" ]; then
    echo "  ✗ missing $p in repo root — skipping"
    continue
  fi
  echo "  ▸ $p"
  # rsync is used over cp -R for the --exclude flags and consistent
  # behavior across macOS + Linux.
  rsync -a \
    --exclude='__pycache__' \
    --exclude='.git*' \
    --exclude='.DS_Store' \
    --exclude='build/' \
    --exclude='node_modules/' \
    "$src" "$vendor/"
done

echo "▸ vendor tree size:"
du -sh "$vendor"
echo "▸ file counts:"
find "$vendor" -type f | wc -l | awk '{print "  files:", $1}'
