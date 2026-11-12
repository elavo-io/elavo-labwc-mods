#!/bin/sh
# Point every scene-graph include in a labwc tree at SceneFX's header.
#
# SceneFX re-exports the wlr_scene_* API with extra fields and node types,
# and its header uses the same include guard as the wlroots one, so a file
# that reaches the wlroots header first would see the wrong struct layouts.
# Every include is swapped, including files upstream adds later, and the
# script fails if one is left. Adapted from swap-scenefx-includes.sh in
# grigio/labwc (labwc-blur, GPL-2.0-only).
set -eu

work=${1:?usage: swap-scenefx-includes.sh LABWC_TREE}
old='#include <wlr/types/wlr_scene.h>'
new='#include <scenefx/types/wlr_scene.h>'

files=$(grep -rlF "$old" "$work/src" "$work/include" || true)
if [ -n "$files" ]; then
  echo "$files" | xargs sed -i "s|$old|$new|"
fi
if grep -rlF 'wlr/types/wlr_scene.h' "$work/src" "$work/include"; then
  echo "swap-scenefx-includes: wlroots scene header still included above" >&2
  exit 1
fi
echo "swap-scenefx-includes: $(echo "$files" | grep -c . || true) files"
