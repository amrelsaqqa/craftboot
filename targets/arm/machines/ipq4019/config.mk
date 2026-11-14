# SPDX-License-Identifier: GPL-2.0-or-later
# Machine: -M ipq4019 (Qualcomm IPQ4019/IPQ4018, Cortex-A7).  Boards: default, hap-ac2, hap-ac3.
#
# --- how these sources reach the build tree --------------------------------
#
# ipq4019_MACHINE_SRCS is the only list of what this machine is made of.
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
# The machine file stays in hw/arm; the blocks live under hw/arm/ipq4019/ so
# their includes are plain ("ipq4019.h") and the build tree groups them the way
# the repository does.  The conventions behind this layout are in
# docs/machines.md.
ipq4019_MACHINE_SRCS := \
    ipq4019.c:hw/arm/ipq4019.c \
    ipq4019.h:hw/arm/ipq4019/ipq4019.h \
    modules/gpio.c:hw/arm/ipq4019/gpio.c \
    modules/uart.c:hw/arm/ipq4019/uart.c \
    modules/bam.c:hw/arm/ipq4019/bam.c \
    modules/eth.c:hw/arm/ipq4019/eth.c \
    modules/stub.c:hw/arm/ipq4019/stub.c \
    modules/board.c:hw/arm/ipq4019/board.c \
    modules/flash.c:hw/arm/ipq4019/flash.c \
    modules/qpic.c:hw/arm/ipq4019/qpic.c \
    modules/qpic.h:hw/arm/ipq4019/qpic.h \
    modules/dump.c:hw/arm/ipq4019/dump.c
ipq4019_BOARDS       := hap-ac2 hap-ac3
ipq4019_DEV_BRANCH   := ipq4019-model
