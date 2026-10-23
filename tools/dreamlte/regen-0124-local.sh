#!/bin/bash
# Regenerate xnu/patches/0124-dreamlte-s8.patch on the Mac from the tree that
# build-xnu-local.sh left in research/xnu-local/dxb/xnu (all patches applied,
# including the current 0124): reverse 0124, run apply-dreamlte.py, and write
# the difference in the format export-patches.sh produces. The tree's HEAD is
# not touched (snapshots go through a temporary index), so the next
# build-xnu-local.sh run resets it as usual.
#
#   tools/dreamlte/regen-0124-local.sh
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
x=$repo/research/xnu-local/dxb/xnu
patch=$repo/xnu/patches/0124-dreamlte-s8.patch
idx=$(mktemp -d)/index
trap 'rm -rf "$(dirname "$idx")"' EXIT

cd "$x"
git apply -R --check "$patch"
git apply -R "$patch"
snap() { GIT_INDEX_FILE=$idx git read-tree HEAD && GIT_INDEX_FILE=$idx git add -A && GIT_INDEX_FILE=$idx git write-tree; }
base=$(snap)
python3 "$repo/tools/dreamlte/apply-dreamlte.py" .
new=$(snap)
git diff --no-color --no-prefix --abbrev=7 "$base" "$new" |
	sed -e 's#^diff --git \([^ ]*\) \([^ ]*\)$#diff --git a/\1 b/\2#' -e 's#^--- \([^/]\)#--- a/\1#' -e 's#^+++ \([^/]\)#+++ b/\1#' > "$patch"
echo "wrote $patch ($(grep -c '^@@' "$patch") hunks)"
