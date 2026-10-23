#!/bin/bash
# Check out the pinned labwc mirror, copy the mods in, apply the patches in order, and build.
# The mirror is never modified. Dev builds and the image use this script.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
ref=$(tr -d '[:space:]' < "$root/LABWC_REF")
work=${1:-$root/build/labwc}

if [ -n "${ELAVO_LABWC_MIRROR:-}" ]; then
  mirror=$ELAVO_LABWC_MIRROR
elif [ -d "$root/../elavo-labwc/.git" ]; then
  mirror=$(cd "$root/../elavo-labwc" && pwd)
else
  mirror=https://github.com/elavo-io/elavo-labwc.git
fi

rm -rf "$work"
mkdir -p "$(dirname "$work")"
if [ -d "$mirror/.git" ]; then
  # --no-local copies objects. --local hardlinks, which fail when the
  # mirror and the work tree are on different filesystems.
  git clone --no-local "$mirror" "$work"
else
  git clone "$mirror" "$work"
fi
git -C "$work" checkout --detach "$ref"
rm -rf "$work/src/elavo-mods"
mkdir -p "$work/src/elavo-mods"
cp -a "$root/mods/." "$work/src/elavo-mods/"
for patch in "$root"/patches/*.patch; do
  git -C "$work" apply "$patch"
done

meson setup "$work/build" "$work" --buildtype=release
meson compile -C "$work/build"
echo "built $work/build/labwc"
