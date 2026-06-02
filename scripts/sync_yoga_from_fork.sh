#!/usr/bin/env bash
# external/yoga is a git SUBTREE of the affineui_yoga fork (branch
# `affineui`). It is the canonical, in-tree, editable Yoga source - extend
# layout behavior here, build, and it ships in the repo.
#
#   Pull the fork's latest into the subtree:   scripts/sync_yoga_from_fork.sh
#   Push local Yoga changes back to the fork:
#       git subtree push --prefix=external/yoga <fork-path> affineui
#
# Pass an explicit fork path or set AFFINEUI_YOGA_FORK if it isn't a sibling.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FORK="${1:-${AFFINEUI_YOGA_FORK:-${ROOT}/../affineui_yoga}}"
cd "$ROOT"
git subtree pull --prefix=external/yoga "$FORK" affineui --squash
echo "Pulled affineui_yoga (branch affineui) into external/yoga."
