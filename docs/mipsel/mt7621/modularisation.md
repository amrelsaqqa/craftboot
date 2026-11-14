# Splitting the mt7621 machine: notes for the next agent

**Status: not started.  `mt7621.c` is untouched (one 5,469-line file).**

The split of the IPQ4019 machine is the worked example for this.  Read
`docs/machines.md` first -- it has the conventions, the move recipe, the
verification steps and the pitfalls (banner slicing, copy-vs-move, and a
missing `config.mk` entry compiling a stale copy).  This file only adds what
is specific to mt7621.

## Already in place (no work needed)

- `config.mk` already uses the `<source>:<destination>` list and documents the
  copy process inline; today it has one entry:
  `mt7621.c:hw/mips/mt7621.c`.  Add a module by adding a pair:
  `modules/uart.c:hw/mips/mt7621/uart.c` (the directory is created by the
  copy rule).
- The build system (`prep`, `build`, `sync`, `export-patches`) already
  supports several sources per machine; the hook patch's `meson.build` hunk
  must list each new `.c`.
- Tests: `tests/mipsel/mt7621/boot-smoke.sh <image> [seconds]` checks the
  console banners; `tests/mipsel/mt7621/net-ping.sh <image> <guest-ip>`
  verifies the Ethernet path.  Run both after any move that touches flash,
  NAND, IRQ or Ethernet; boot-smoke alone is enough for the leaves.

## The block map

The file is a sequence of these areas (line counts are today's; re-measure
after each move, they shift):

| area | notes |
|---|---|
| UART / serial console | leaf; good first move |
| GPIO / pinmux | leaf |
| timers / watchdog | leaf |
| SPI flash controller + NOR die | includes the JEDEC override knob and the write path |
| MTK NAND controller + die | register map matches Linux `mtk_nand`; die model with its store |
| PDMA (RX) + QDMA (TX) + switch glue | the largest and most cross-referenced: one Ethernet subsystem, take it after the rest |
| PCIe | leaf once the PHY/link model is understood |
| GCR / CPC clocks + reset | blocks that other areas read; keep the reset sequencing with the machine |
| crypto (AES/DES/TDES/MD5/SHA) | leaf, self-contained |
| board presets (`lg-gapm7200`, `rb750gr3`) | move to `modules/board.c` as data + merge order |
| machine init / memory map | **stays in `mt7621.c`** -- see the caution below |

## Suggested order

1. `mt7621.h` + the build wiring (no code move): state structs, base
   addresses, shared helpers; confirm a `sync` build and boot-smoke are
   unchanged.
2. `modules/uart.c`, `modules/gpio.c`, `modules/timers.c` -- leaves.
3. `modules/flash.c` (SPI + NOR) and `modules/nand.c` (controller + die).
4. `modules/board.c` (presets, merge order, env shim).
5. `modules/eth.c` (PDMA/QDMA + switch glue) -- give it a private
   `modules/eth.h` if the register/state types are large, the way the QPIC
   has `qpic.h`.
6. `modules/pcie.c`, `modules/clocks.c`, `modules/crypto.c` as time allows.

## mt7621-specific cautions

- **KSEG0/KSEG1 physical aliasing** is cross-cutting: the alias table maps the
  addresses the CPU can generate to the device they hit.  It stays with the
  machine (or in the header), not in a controller module.
- **The two DMA engines are one subsystem.**  PDMA is the RX ring, QDMA the TX
  ring; moving one without the other would put the frame path in two files.
  Move them together with the switch glue, and test with `net-ping.sh`.
- **MIPS, not ARM.**  Do not carry ARM headers (`target/arm/...`, `cpu.h` from
  an ARM machine) into modules by copying the ipq4019 files; use what
  `mt7621.c` already includes.
- **Board presets are runtime options** (`-M mt7621,board=<preset>`), so the
  board module stays data-only; the `-M` property setters remain in
  `mt7621.c` and write through the merge state exported by the header.
- **Cross-block statics are the compiler's job.**  Move a block, de-`static`
  only what fails to link, and add exactly those prototypes to `mt7621.h`.
  Keep the header small.

## Per-step acceptance

- `make TARGET=mipsel/mt7621 sync` builds clean;
- `tests/mipsel/mt7621/boot-smoke.sh <image>` passes (and `net-ping.sh` after
  the Ethernet move);
- `make TARGET=mipsel/mt7621 check-patches` passes;
- one fresh `prep` + `build` after any file-list change;
- one commit per block, **pure move, no behaviour change**, message
  `mt7621: move <block> out`.
