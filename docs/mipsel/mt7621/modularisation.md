# Splitting the mt7621 machine: notes for the next agent

**Status: in progress on `refactor/mt7621-modules`.**  The Ethernet subsystem
is out (`modules/eth.c`, 1,456 lines, plus the private `modules/eth.h`), which
was the largest block; so are the CM/CPC (`modules/cm.c`) and the EIP-93
crypto engine (`modules/crypto.c`).  The loader (`modules/loader.c`), the
NAND attach (`modules/nand.c`) and the debug knobs (`modules/dump.c`) followed.
`mt7621.c` is **1,082 lines from 5,469** -- below ipq4019's 2,244 for the same
kind of machine.

Out so far:
`modules/uart.c` (TX-only 16550 mirror), `modules/regfile.c` (the shared
register-file ops plus the GPIO semantics inside it), `modules/wdt.c` (timer 1
of the system controller), `modules/systick.c` (cevt-systick, its timers and
the boot hand-over), `modules/board.c` (presets, aliases, image probe and
`chip_mode`), `modules/flash.c` (SPI NOR controller + W25Q128FV die) and
`modules/nand.c` (NFI + K9F1G08U0B die, dump loader, persistence).
Every step passed check-patches, a fresh prep + build, boot-smoke 3/3 and
net-ping 5/5 (the tap is now usable on this host).

Two moves are not quite the pure moves the plan assumed:

- **GPIO is not a leaf.**  It has no register window of its own; its direction
  masking and DSET/DCLR handling live inside the sysctl register file, which
  the DRAM controller page also uses.  So the move took the shared
  `mt7621_reg_ops` (exported in the header) rather than inventing a GPIO
  `MemoryRegionOps`.
- **Watchdog resets are a machine decision.**  The module arms and fires;
  `wdt_rst_cause` is still consumed by the machine's reset path, so only the
  timer and ops moved.

What is left in `mt7621.c` is the machine proper: memory map, RAM and KSEG1
alias windows, boot stages, reset sequencing, `-M` properties and the
command-line injection knob.  The split is structurally done; further work
would be ripple (splitting `sysctl_defaults`/`dramc_defaults` out of the
machine, say), not structure.

Notes from the Ethernet move, since it is the pattern for a subsystem:

- Private register maps go in `modules/<block>.h` when there are many (109
  lines of ETH/PDMA/QDMA/TX_DMA/RX_DMA bits here); the machine must not see
  them.
- Two machine entry points fell out naturally: `mt7621_eth_init()` (port
  selection, NIC, MMIO + SRAM windows, nitest timer) and
  `mt7621_eth_dev_reset()` (ring state, switch/PHY defaults).  Reset was the
  only place the machine touched the frame engine directly.
- Definition order inside the new file matters: `phy/switch defaults` call
  `mt7621_sw_word()`, so they must come after it, not before.

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
- The build system (`prep`, `build`, `sync`, `export-patches`) supports
  several sources per machine.  `hw/mips/mt7621/meson.build` is **generated**
  from `mt7621_MACHINE_SRCS` by `tools/machine-meson.sh`; the two variables it
  reads are `mt7621_MACHINE_SS` (the meson source set, `mips_ss` here) and
  `mt7621_MACHINE_SUBDIR` (`hw/mips/mt7621`).  Adding a module means editing
  `config.mk` only -- headers are copied but never listed.

## The dev checkout

The target series (0001-0003) is the only thing generated from a QEMU tree:
`~/qemu-mt7621/qemu`, branch `mt7621-model` (`mt7621_DEV_BRANCH`).  Note the
path: `~/qemu-mt7621` is the old repository (its own `dcache-work`/`master`),
while the checkout is the `qemu/` inside it.

```sh
make TARGET=mipsel export-patches DEV=~/qemu-mt7621/qemu
```

That command produces the three arch-wide patches and nothing else.  The
machine series is repository-owned and must never appear on that branch:
`export-patches` does not generate machine patches, and a machine commit there
would only invite it to be regenerated into the wrong series.

If the branch is lost, recreate it from the pinned tag plus the three global
changes as commits (1004Kc cache geometry / icache shadow / GIC coupling,
64-bit shared counter, per-VP compare) and re-export to prove the result
matches.  Losing it costs regeneration, not correctness: builds, machine work
and the machine series are unaffected, and `check-patches` still proves the
committed series applies on its own.

## What the split has hit so far

- **Split by generality.**  The target series keeps what any machine of this
  target can use -- generic MIPS/GIC mechanics -- and is named accordingly
  (`mips: 1004Kc cache geometry, icache shadow and GIC counter frequency`,
  `mips-gic: 64-bit shared counter`, `mips-gic: 64-bit per-VP compare`).  Every
  `MT7621_*` knob and tracer lives in the machine series, because only this
  machine's guests use it.

- **The build hook is per machine.**  The target series is core changes only
  (0001-0003); everything mt7621-specific is in
  the machine series, `0001` being the base hook: `CONFIG_MT7621` in
  `hw/mips/Kconfig`, `files('mt7621.c')` in `hw/mips/meson.build`, and
  `subdir('mt7621')`; `0002-0004` are `bring-up tracing`, `syslog trace` and
  `cachelog`.  The module list inside
  `hw/mips/mt7621/meson.build` is generated from `mt7621_MACHINE_SRCS` (see
  `mt7621_MACHINE_SS` and `mt7621_MACHINE_SUBDIR`).  Same shape as ipq4019,
  so `docs/machines.md`'s canonical hook describes both, and a second mipsel
  machine adds its own series instead of touching the shared one.

  This shape is required, not cosmetic: the target series is applied whether
  or not a given machine is selected, so a second mipsel machine's stanza
  there would reference sources that `prep` never copied for this build and
  fail at configure time.  Machine series are applied only when selected.

  Ordering constraint, already satisfied: `subdir('mt7621')` fails at
  configure until the generator has written `hw/mips/mt7621/meson.build`.
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
