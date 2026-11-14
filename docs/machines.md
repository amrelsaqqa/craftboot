# Machine source layout

Every machine in this repository is one SoC model plus a board layer.  The
SoC is fixed (same die, same controllers, same addresses); a board is a data
table (which dies hang off which bus, which pins drive the lamps, which PHYs
sit on the MDIO bus).  That split is the reason a machine is not one file:

- **the machine file** is the board-independent machine: init, memory map,
  reset, boot stages, the `-M` properties;
- **a module per controller** owns one address block end to end: its register
  offsets, its `MemoryRegionOps`, its state, and one constructor;
- **a board module** holds the presets and the merge order (explicit property
  beats preset beats env), so adding a board is a data change, not code.

`ipq4019` is the **worked example** (eleven modules, `ipq4019.c` down from
11.8k to 2.2k lines).  This document is the rule set extracted from that work,
kept generic so the other machines can be moved the same way; `mt7621` is the
next one to apply it to (its own notes are in
`docs/mipsel/mt7621/modularisation.md`).

### The worked example: ipq4019

| file | lines | owns |
|---|---|---|
| `ipq4019.c` | 2,244 | machine: init, memory map, reset, boot stages, regfile/gcnt, `-M` properties |
| `ipq4019.h` | 826 | the interface between the blocks |
| `modules/flash.c` | 2,104 | QUP SPI, NOR die, SPI NAND, die store |
| `modules/eth.c` | 1,732 | IPQESS, host netdev, MDIO/PHY, board ports |
| `modules/qpic.c` | 1,424 | QPIC parallel NAND (die, controller, BAM) |
| `modules/qpic.h` | 238 | that block's private register map and state |
| `modules/gpio.c` | 841 | TLMM, LED panel, keys |
| `modules/bam.c` | 757 | BLSP1 BAM |
| `modules/uart.c` | 650 | UARTDM console |
| `modules/stub.c` | 631 | stubs, regfile, power-hold, watchdog, memory window, clocks |
| `modules/board.c` | 480 | presets, merge order, env shim |
| `modules/dump.c` | 248 | whole-machine state dump |

Before: one 11,802-line file.  After: a 2,244-line machine plus ten
controllers and one private header.  The split was done as nineteen
behaviour-neutral commits, each boot-tested, and merged with `--no-ff` so the
per-module history stays bisectable.

## Tree layout

```
targets/<arch>/machines/<soc>/
├── config.mk          sources (src:dest pairs), board names, dev branch
├── <soc>.c            the machine
├── <soc>.h            the interface between the blocks
├── modules/
│   ├── <block>.c
│   └── <block>.h      only when a block's private interface is large
├── patches/
│   └── 0001-…-hooks.patch   the meson/Kconfig hunk (lists every .c)
└── series
```

Nothing is patched under `modules/`: the machine sources are *copied* into
the QEMU tree by `prep`/`build`/`sync` and excluded from
`export-patches` (they are the machine, not a core change).

## Build mapping

`config.mk` maps every source to its destination explicitly, because the
repository layout and the QEMU tree differ:

```make
ipq4019_MACHINE_SRCS := \
    ipq4019.c:hw/arm/ipq4019.c \
    ipq4019.h:hw/arm/ipq4019/ipq4019.h \
    modules/uart.c:hw/arm/ipq4019/uart.c \
    modules/gpio.c:hw/arm/ipq4019/gpio.c \
    ...
```

`prep`, `build`, `sync` and `export-patches` all parse the same list; the
copy creates the destination directory, and a missing entry means the build
silently compiles the previous copy (see *Pitfalls*).

The resulting tree mirrors the repository:

```
hw/arm/ipq4019.c
hw/arm/ipq4019/{ipq4019.h, uart.c, gpio.c, …}
```

and the machine hook patch is static -- it adds the machine file and enters
the machine's directory:

```meson
arm_common_ss.add(when: 'CONFIG_IPQ4019', if_true: files('ipq4019.c'))
subdir('ipq4019')
```

`hw/arm/ipq4019/meson.build` is **generated**, never edited: during
`prep`/`build`/`sync`, `tools/machine-meson.sh` writes it from
`<machine>_MACHINE_SRCS`:

```meson
# Generated from config.mk by the build; do not edit.
arm_common_ss.add(when: 'CONFIG_IPQ4019', if_true: files(
    'gpio.c', 'uart.c', 'bam.c', 'eth.c', 'stub.c', 'board.c',
    'flash.c', 'qpic.c', 'dump.c'))
```

`<machine>_MACHINE_SS` in `config.mk` names the meson source set
(`arm_common_ss`, `mips_ss`, …).  Adding a module therefore touches exactly one
file, `config.mk`; no patch is edited and no list is copied.

### Include rules

Everything resolves without `..` and without touching `include/`:

| file | includes the interface as |
|---|---|
| `hw/arm/ipq4019.c` | `#include "ipq4019/ipq4019.h"` |
| `hw/arm/ipq4019/uart.c` | `#include "ipq4019.h"` |
| a private header (`qpic.h`) | `#include "ipq4019.h"` |

## What belongs where

**The machine file** (`<soc>.c`) keeps only what is *the machine*: memory
map and RAM windows, reset sequencing, the boot-stage/ELF loader, the `-M`
properties and the machine class.  If a function manipulates one controller's
registers, it belongs in that controller's module.

**A module** (`modules/<block>.c`):

- owns one address block or one bus/controller (UART, the pin controller, the
  Ethernet DMA engine, a flash stack);
- defines its `MemoryRegionOps` and its state **in the module**, except for
  the state the state dump reads (see below);
- exposes one constructor (`add_<block>()`) called from init, and a
  `<block>_dev_reset()` if it has reset state beyond zeroing;
- may own its whole instantiation: prefer `add_pshold()`/`add_wdt()` over
  exporting a `MemoryRegionOps` for init to wire up.

**The interface header** (`<soc>.h`) holds:

- the SoC/board state structs and the file-scope machine pointers;
- the base addresses and constants two or more blocks share;
- the small helpers every block uses (`env_u32`, `env_true`, the log hooks);
- prototypes for everything that crosses a file boundary, and nothing else.

It should stay small.  When one block's private interface is genuinely large,
give it its own header (`modules/qpic.h`) instead of growing the shared one:
the QPIC's register map and opcode/state types are ~240 lines that no other
block needs.  The machine includes it only where it needs the types (the state
dump).

**A board module** (`modules/board.c`) holds the preset table, the pending
config the property setters record into, the merge order and the env shim.
The `-M` property setters stay in the machine file (they are the machine
class); they write through the small merge state exported in the header.

### State structs and the state dump

State structs live in the shared header *even though the code that owns them
lives in a module*.  That is deliberate: the whole-machine dump
(`QEMU_IPQ_STATE_DUMP`) reads every block's registers at the boot handoff, so
making the structs private would only move the coupling into a second
interface.  Moving the dump to per-block `dump(FILE *)` callbacks would be the
cleaner end state, but it is a much larger change and it buys nothing until
the dump itself is split.

## Naming

- `modules/<block>.c`, one block per file; the file name is the block, not
  the chip.
- `add_<block>()` for constructors called from init; `*_dev_reset()` for reset
  state; the register-level helpers stay `static` in the module.
- Shared types keep their `<SOC>…` prefix (`IPQEdma`, `IPQQupl`).

## Moving a block out: the recipe

1. **Map the file first.**  List the section banners and the top-level
   functions.  The file order is *not* the module order: in `ipq4019` the SPI
   register code sits at the top, its opcode state machine in "building the
   board's part" after the QPIC block, and the die constructors in between.
   That is why the flash move was four ranges.
2. **Extract line-based, with assertions.**  String search for a banner is
   fragile (`rfind` on a banner string silently lands on the wrong section);
   index by line, assert the expected anchor, and slice.
3. **Move, do not copy.**  The recipe is `lines[:start] + lines[end:]` *after*
   the extracted text has been written to the new file.  Copying instead of
   moving shows up as "static declaration follows non-static" at the next
   build.
4. **Move the definitions with the code.**  Cut the block's `struct`s/defines
   into the new module (or the header, if shared), then de-`static` exactly
   the cross-file interface and add its prototypes to the header.
5. **Wire it:** add the pair to `config.mk` and the source set
   (`<machine>_MACHINE_SS`); the generated `meson.build` follows.  A machine
   hook patch only gains `subdir('<soc>')` once, when the split starts.
6. **Build with `sync`, fix the interface errors**, and iterate.  The compiler
   is the checklist: implicit declaration, unknown type, conflicting
   types, undefined reference.
7. **Test:** boot the board from a die copy and check the guest's own log
   (for `ipq4019`: RouterBOOT banner, `loading kernel... OK`, the yaffs mount,
   and no `qpic: anomaly:` lines).  For a flash/CPU/IRQ-touching move also do
   one install or one full workload.
8. **Validate the series:** `make TARGET=<target> check-patches`, then one
   fresh `prep` + `build` (the file list lives in three places; only a clean
   `prep` proves they agree).
9. **Commit one block per commit**, message "move <block> out", and say that
   it is a pure move.  No behaviour change in the same commit — that is what
   keeps `git diff -M` reviewable and a bisect meaningful.

## Pitfalls (all hit for real)

- **A missing `config.mk` entry** makes `sync` skip the file, so the build
  compiles the *previous* copy in the tree.  The errors look like code
  problems (static/duplicate definitions) but the fix is the build list.
  When an error does not match the source you are reading, check
  `build/<target>/src/...`.
- **Banner `rfind`** can cut across a neighbouring section (it removed the
  whole QPIC block once).  Use line indices and assert the first line of the
  slice.
- **Duplicated definitions**: a struct or helper cut into the header but left
  in the module is a hard error ("redefinition"), and a function definition
  dragged into a header is a multiple-definition link error.  Cut, don't
  copy.
- **The state dump is the reason structs are shared.**  Do not "clean up" by
  making them private without also giving the dump a per-block interface.
- **The file's own ordering** may put one block's pieces far apart (see the
  flash move).  Plan the ranges before editing.

## The next machine: mt7621

`ipq4019` is the worked example; `mt7621` is the next application of these
rules.  Take the biggest, most self-contained leaves first; they prove the
build plumbing with the least interface churn, and each one shrinks the file
enough to see the next boundary.

`mt7621` is one 5,469-line file today.  The blocks the code already separates
are: UART, GPIO/pinmux, the SPI flash controller and NOR die, the
MTK NAND controller and die, PDMA/QDMA Ethernet with its switch glue, PCIe,
GCR/CPC clocks, crypto, timers/watchdog, and the board presets (`lg-gapm7200`,
`rb750gr3`).

A workable order, mirroring `ipq4019`:

1. build plumbing + `mt7621.h` (state, base addresses, shared helpers);
2. `modules/uart.c`, `modules/gpio.c`, `modules/timers.c` — leaves;
3. `modules/flash.c` (SPI + NOR) and `modules/nand.c` (controller + die);
4. `modules/board.c` (presets, merge order, env shim);
5. `modules/eth.c` (PDMA/QDMA + switch glue) — the largest, and the one most
   likely to need its own private header;
6. `modules/pcie.c`, `modules/clocks.c`, `modules/crypto.c` as time allows.

The same rules apply verbatim: one block per file, constructors named
`add_<block>()`, state structs in `mt7621.h`, one `-M` machine class in
`mt7621.c`, presets as data, and one boot-tested commit per move.  Budget one
session per 2–3 modules; the last two (`eth`, and anything with cross-block
state) take as long as the first five together.  `mt7621` has the same
build plumbing already (`mt7621_MACHINE_SRCS` is a one-entry pair list), so
step 1 is mostly the header and the file map.

## Checklist

- [ ] file map written down; ranges verified by line, not by banner text
- [ ] block's types/defines moved with it
- [ ] only the interface de-`static`ed; prototypes in the header
- [ ] constructor moved into the module (`add_<block>()`)
- [ ] `config.mk` updated (and `<machine>_MACHINE_SS` set); the generated
      `meson.build` follows -- no patch edited
- [ ] `sync` builds clean
- [ ] boot test passes (guest log + no anomalies)
- [ ] `check-patches` passes
- [ ] fresh `prep` + `build` passes
- [ ] one commit, pure move, no behaviour change
