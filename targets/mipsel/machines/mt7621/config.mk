# SPDX-License-Identifier: GPL-2.0-or-later
# Machine: -M mt7621 (MediaTek MT7621A, MIPS 1004Kc).
# Board presets are runtime options: -M mt7621,board=<preset>

# --- how these sources reach the build tree --------------------------------
#
# mt7621_MACHINE_SRCS is the only list of what this machine is made of.
# Every entry is "<source>:<destination>": the source is relative to this
# directory, the destination relative to the root of the QEMU tree the build
# uses.
#
#   make prep            recreate build/<target>/src from the pinned tag and
#                        copy every pair in (mkdir -p on the destination first)
#   make build           copy only the pairs whose content changed, then ninja
#   make sync            build's copy step without configure/prep: the fast
#                        edit loop for one module
#   make export-patches  run the same list backwards: the files are excluded
#                        from the generated patch series and copied out of the
#                        dev checkout (DEV=...) back into this directory
#
# The copy is a real copy, not a symlink: the build tree is disposable and
# prep makes it a git repository, so the machine sources appear there as
# ordinary files.  Nothing in this directory is patched -- the series only
# carries core changes plus the meson/Kconfig hunk that lists the .c files.
#
# When adding a file, add it here *and* to that hook patch's files(...) list.
# An entry missing here means the build silently keeps the previous copy.
#
# One file today; the layout conventions (and the copy rules above) are in
# docs/machines.md.
mt7621_MACHINE_SRCS := \
    mt7621.c:hw/mips/mt7621.c \
    mt7621.h:hw/mips/mt7621/mt7621.h \
    modules/uart.c:hw/mips/mt7621/uart.c \
    modules/regfile.c:hw/mips/mt7621/regfile.c \
    modules/wdt.c:hw/mips/mt7621/wdt.c \
    modules/systick.c:hw/mips/mt7621/systick.c
mt7621_BOARDS      := lg-gapm7200 rb750gr3
mt7621_DEV_BRANCH  := mt7621-model
