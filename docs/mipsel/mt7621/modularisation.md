# Splitting the mt7621 machine: notes for the next agent

**Status: in progress on `refactor/mt7621-modules`.**  Out so far:
`modules/uart.c` (TX-only 16550 mirror), `modules/regfile.c` (the shared
register-file ops plus the GPIO semantics inside it), `modules/wdt.c` (timer 1
of the system controller), `modules/systick.c` (cevt-systick, its timers and
the boot hand-over), `modules/board.c` (presets, aliases, image probe and
`chip_mode`), `modules/flash.c` (SPI NOR controller + W25Q128FV die) and
`modules/nand.c` (NFI + K9F1G08U0B die, dump loader, persistence).
`mt7621.c` is down to 3,654 lines; the modules total 1,744.  Every step passed
check-patches, a fresh prep + build, boot-smoke 3/3 and net-ping 5/5.

Two moves are not quite the pure moves the plan assumed:

- **GPIO is not a leaf.**  It has no register window of its own; its direction
  masking and DSET/DCLR handling live inside the sysctl register file, which
  the DRAM controller page also uses.  So the move took the shared
  `mt7621_reg_ops` (exported in the header) rather than inventing a GPIO
  `MemoryRegionOps`.
- **Watchdog resets are a machine decision.**  The module arms and fires;
  `wdt_rst_cause` is still consumed by the machine's reset path, so only the
  timer and ops moved.

Next: PDMA/QDMA plus the MT7530 switch glue as one subsystem, with a private
`modules/eth.h` for its register/state types (the QPIC precedent).  Then the
CM/CPC block, the EIP-93 crypto engine, and whatever the loader/ELF path still
needs once those are gone.

Two more interface notes from this round: `mt7621_pwrite_all()` had to become
public because the machine's NAND flush uses the same helper, and both die
loaders stay callable from machine init (`mt7621_nand_load()`), since `-kernel`
is what fills them.

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

## What the split has hit so far

- The `meson.build` stanza that lists `mt7621.c` lives in the *target* series
  (`targets/mipsel/patches/0001-...`), not in a machine hook patch as on
  ipq4019.  Rather than hand-editing a generated patch, the machine carries
  `patches/0001-hw-mips-meson-list-modules.patch` (and `series`) that rewrites
  the stanza.  Fold it into the dev branch on the next `export-patches`, then
  the machine series can go away.
- `mt7621.h` lands in `hw/mips/mt7621/`, not `hw/mips/`: a module's
  `#include "mt7621.h"` resolves as its sibling, and the machine includes
  `"mt7621/mt7621.h"`.  This is the ipq4019 layout (`hw/arm/ipq4019/ipq4019.h`).
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
