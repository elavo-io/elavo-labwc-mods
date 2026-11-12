#!/bin/bash
# Copy the mods into a labwc tree checked out at LABWC_REF and apply the
# patches in order. assemble.sh calls this after the checkout; it also runs
# on any other copy of that tree.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=${1:?usage: apply-mods.sh LABWC_TREE}

rm -rf "$work/src/elavo-mods"
mkdir -p "$work/src/elavo-mods"
cp -a "$root/mods/." "$work/src/elavo-mods/"
for patch in "$root"/patches/*.patch; do
  git -C "$work" apply "$patch"
done

# SceneFX replaces the wlroots scene graph: corner radii, shadow and blur
# nodes. ELAVO_SCENEFX=0 builds the previous renderer path (strip-clipped
# corners, buffer shadows, the render-pass blur) for comparison.
if [ "${ELAVO_SCENEFX:-1}" != 0 ]; then
  for patch in "$root"/patches/scenefx/*.patch; do
    git -C "$work" apply "$patch"
  done
  "$root/scripts/swap-scenefx-includes.sh" "$work"
fi
