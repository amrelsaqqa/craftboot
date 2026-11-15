#!/bin/sh
# Write the CraftBoot version header for the QEMU build, only when it changes,
# so a rebuild of system/vl.c happens when the version actually moved.
#
#   scripts/version-header.sh <path-to-header> <version>
set -e
out=$1
ver=$2
tmp=$out.tmp
[ -n "$out" ] && [ -n "$ver" ] || { echo "usage: $0 <header> <version>" >&2; exit 2; }
mkdir -p "$(dirname "$out")"
printf '#define CRAFTBOOT_VERSION "%s"\n' "$ver" > "$tmp"
if cmp -s "$tmp" "$out"; then
    rm -f "$tmp"
else
    mv "$tmp" "$out"
    echo "  version $ver"
fi
