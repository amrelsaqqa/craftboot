#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Regenerate hw/<arch>/<machine>/meson.build from a machine's config.mk list.
#
#   tools/machine-meson.sh <out> <machine> <srcs> <ss> <subdir>
#
# <srcs> is the machine's "<src>:<dst>" list, <ss> the meson source set the
# machine's sources belong to (arm_common_ss, mips_ss, ...) and <subdir> the
# module directory (hw/arm/ipq4019).  The generated file lists only the module
# .c files -- the machine file stays in the parent meson.build and the machine
# hook patch only does subdir('<machine>'), so neither has to change when a
# module is added.
#
# Writes only when the content changed, so ninja does not reconfigure on every
# sync.  A machine without <machine>_MACHINE_SS (not migrated yet) or without
# module sources produces no file, and that is not an error.
set -eu

out=$1
m=$2
srcs=$3
ss=$4
sub=$5

[ -n "$ss" ] || exit 0
cfg=CONFIG_$(printf '%s' "$m" | tr '[:lower:]' '[:upper:]')
files=

for p in $srcs; do
    d=${p#*:}
    case $d in
        "$sub"/*.c) files="${files:+$files, }'${d##*/}'" ;;
    esac
done

# The hook patch does subdir('<machine>'), so the directory must be named
# after the machine; catching a mismatch here beats a meson error later.
if [ "$(basename "$sub")" != "$m" ]; then
    echo "machine-meson: $m: subdir '$sub' must end in /$m" >&2
    exit 1
fi

# A machine that drops its last module still needs a valid meson file: the
# hook patch already enters this directory, so leave an empty list rather
# than a stale file from the previous build (or none at all).
if [ -n "$files" ]; then
    new=$(printf '%s\n' \
        '# Generated from config.mk by the build; do not edit.' \
        "$ss.add(when: '$cfg', if_true: files($files))")
else
    new=$(printf '%s\n' \
        '# Generated from config.mk by the build; do not edit.' \
        '# (no module sources: the machine file is added by the hook patch)')
fi

mkdir -p "$(dirname "$out")"
if [ -f "$out" ] && [ "$new" = "$(cat "$out")" ]; then
    exit 0
fi
printf '%s\n' "$new" >"$out"
