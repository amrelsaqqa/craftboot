# Changelog

## v0.3.0 — 2026-11-16

The MT7621 machine can now run the board's real cache geometry (`Config1.Dl=4`,
32-byte lines) instead of the `Dl=2` workaround it had been pinned to since the
board bring-up.

### Added

- **L1 data-cache model for the MT7621 (MIPS 1004Kc)**, opt-in with
  `MT7621_DCACHE=1`: 32 KiB, 4-way, LRU per set, line size from `Config1`,
  write-through.  There is one DRAM; the model tracks what the cache *holds*
  (residency) and keeps only the bytes an uncached store made stale, because a
  write-through resident line is otherwise bit-identical to memory.
  - uncached stores (the DMA aliases, `0xA0000000|p`) bypass the cache, as on
    silicon, and leave a stale copy behind when they hit a resident line;
  - loads are routed through the model only on pages that hold a stale line;
    everywhere else QEMU's own load path returns exactly what the hardware
    would;
  - `cache` instructions and `synci` drop lines; `sc` invalidates after the
    atomic.
  - Result: Breed boots this firmware at the hardware `Dl=4` — its DMA clear
    keeps the allocator's chunk header in the cache, so the following `malloc`
    finds a valid block instead of failing with "unable to allocate fifos".
- `MT7621DL`, `MT7621_CFGLOG` and `MT7621_EXCLOG` knobs for A/B testing line
  size, asserting what the guest sees, and logging exceptions (including DSP)
  with the instruction words.
- Test tooling: `tools/expect.py` (pass/fail from serial patterns, guest-time
  and stall budgets, serial socket so it can answer "press Enter"),
  `tools/mt7621-boot.sh` (the `base`/`dl4`/`cache`/`reboot` cases with their
  expectations), `tools/guest-peek.py`, `tools/fdt-mem.py`,
  `tools/dump-on-panic.py`, `tools/mipsel/mt7621/breed-dl-probe.sh`.
- The binaries report the CraftBoot build in `--version`:

  ```
  Built by CraftBoot v0.3.0
  QEMU emulator version 10.2.2
  Copyright (c) 2003-2025 Fabrice Bellard and the QEMU Project developers
  ```

  The version comes from `git describe --tags --match 'v[0-9]*'` at build time
  (`CRAFTBOOT_VERSION=...` overrides it), so nothing is hard-coded; the core
  change is a *global* patch (`patches/generic/`) and the include is
  conditional, so a QEMU built without the generated header prints what it
  always did.

### Fixed

Three defects stood between the cache model and a working boot, all found by
instrumentation rather than theory:

- **Force-slow routing never worked.** `TLB_FORCE_SLOW` is `1 << 8` while
  `CPUTLBEntryFull.slow_flags[]` is `uint8_t`, so the request stored zero and
  no access was ever forced — the model only saw TLB misses.  It now travels in
  `MemTxAttrs` and is honoured when the TLB comparator is built.
- **Every cached store happened twice.** The model performed the store and
  invalidated translated blocks from inside the helper, so QEMU re-executed the
  instruction; that is what zeroed the chunk header the allocator had just
  written.  Stores are now handed back to QEMU.
- **KSEG1 looked cacheable.** Cacheability was decided from the address range,
  so Breed's DRAM probe (write through KSEG1, read back) measured 448 MiB on a
  256 MiB board, which aliases physical memory onto itself.  Cacheability now
  follows the actual mapping (`SegCtl` for kseg*, the TLB entry for useg).

Also: DRAM aliases are indexed by their wrapped physical address (a mirror
probe sees one line), and write-allocate on a store miss is kept because the
firmware demonstrates it — without it Breed fails at "unable to allocate
fifos" again.

### Changed

- The board **still reports `Dl=2` by default**; with `MT7621_DCACHE=1` it
  reports the hardware `Dl=4`.  Nothing changes for an ordinary boot.
- NAND: the ID probe is answered on CS0 only (fixes a duplicate `Flash:` line),
  and the part is per board (`lg-gapm7200` = ESMT `0xc8`), overridable with
  `-M mt7621,nand-id=...`.
- `lg-gapm7200` no longer registers the SPI controller; clocks report
  880/1200/220 MHz with a 40 MHz crystal like the real boards.
- Core carries **hooks only** (force-slow routing, the access handler being
  given the resolved physical address, the per-page slow hook, mapping
  cacheability).  No TCG code generation is patched.

### Performance

Measured on one pinned core, wall clock to `br-lan: link becomes ready`
(three runs each):

| | time |
|---|---|
| no cache model | 36.8–39.9 s |
| `MT7621_DCACHE=1` | 67.6–70.1 s |

i.e. ≈1.78×, and the whole difference is the per-store entry into QEMU's slow
path.  The model's own share is a 32-byte set, one SSE2 four-way tag compare
and one LRU stamp; several cheaper designs were built, measured and rejected
(an inline residency tracker in generated code: 85.5 s, and inexact without
true LRU), and they are documented in `docs/mipsel/mt7621/model-quirks.md` so
they are not rediscovered.

Correctness check over a boot: of 8584 reads served from stale snapshots, only
three differed from memory — Breed's two DMA buffers and one kernel read of a
DMA buffer before its invalidation, which is what the hardware returns too.

### Verified

- `Dl=4` + model: Breed → Linux → shell, `Memory: 249772K/262144K` (the right
  256 MiB), network up, `uptime`/`free` answer, no panics; a 900 s run held at
  the shell.
- `Dl=4` without the model fails at "unable to allocate fifos" — the defect the
  model exists to explain.
- `make TARGETS=mipsel,arm check-patches` applies both series cleanly on
  QEMU v10.2.2.

## v0.2.0 and earlier

See the git history; v0.2.0 was the state before the MT7621 cache work.
