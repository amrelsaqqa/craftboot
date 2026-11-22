# Changelog

## v0.4.0 — 2026-11-22

The MT7621 machine now runs the *installed* system end to end: netinstall over
the tapped PHY, the installed RouterOS booting from the NAND die image, and
repeated reboots (including `/system reboot` through the watchdog) without
panics or lost state.  The 1004Kc is modelled as what it is (two cores, two VPs
each), the SoC resets completely instead of inheriting latched state from the
boot it replaced, and the NAND path is modelled closely enough to serve a real
yaffs filesystem - which is what made the last class of failures visible.

### Added

- **1004Kc multi-threading bring-up** (2 cores x 2 VPs, `{2,2} total 4`):
  `VPECONTROL.TARGTC` is core-local, the MTTR/MFTR gate asks whether the TC
  exists in the processor, `DVPE`/`EVPE` is per processor, `CPC_Cx_CONFIG.PVPE`
  and `MVPConf0.PVPE` agree with the topology, `GCR_CL_ID`/`GCR_CL_COHERENCE`
  are per core, and per-VP reset state is modelled.
- **CP0 count/compare routed through the GIC** on CM parts: the compare is the
  VP-local `GIC_LOCAL_INT_TIMER` (2), latched in `GIC_VP_PEND` and forwarded only
  when the VP mask bit and `GIC_VPE_TIMER_MAP` allow it.  `GIC_VPE_CTL` reports
  `TIMER_ROUTABLE`, so a guest that registers the r4k clockevent gets the GIC
  path the model implements instead of CPU pin 7.
- **NAND: per-sector spare area** (`512 data + 16 spare` per sector, FDM 8 +
  ECC 8), the controller's raw and `AUTO_FMT` page address spaces, yaffs tag
  synthesis for dumps without spare (`MT7621_NAND_YAFFS`), a write-through
  backend (`MT7621_NAND_IMAGE`, `-drive if=none,id=nand`), and a medium snapshot
  at the first real reset (`MT7621_NAND_SNAP`).
- **NAND diagnostics**: `MT7621_TAGLOG`/`MT7621_TAGLOG_BOOT` (per-boot NFI read
  trace, so two boots of the same medium can be diffed), `MT7621_FDMLOG` (the
  tag bytes actually served to the guest, per row), `MT7621_NAND_WRLOG` with
  timestamps, `MT7621_NFIWLOG`, `MT7621_NFILOG`.
- **GIC diagnostics**: `MT7621_GICIRQLOG` reports per-source rates (`gicrate`),
  held-high levels (`giclevel`) and a filtered VP-local transition trace with
  the guest PC, mask/map and pending bits.
- **Ethernet**: the second (QDMA) RX ring and the hand-off latch, the engine is
  quiesced when the guest asks for a reset (so DMA cannot land in freed pages
  during the slow model reset), the bootloader's ring is refused once the kernel
  owns the engine, stale firmware interrupt status is discarded at the driver's
  first unmask, and `MT7621_TXDBG`/`MT7621_RXDBG`/`MT7621_MDIOLOG`/`QDMALOG`
  traces make the phases visible.
- **xHCI/IPPC** (`modules/usb.c`) wired into the build.

### Changed

- **A reset resets every controller, explicitly** - SYSCTL, MEMC, WDT, CM/CPC,
  NFI, SPI, ethernet (incl. the GIC line), xHCI/IPPC, EIP-93, systick, the L1D
  model and the per-boot bring-up latches.  Only DRAM, the flash media and the
  board's strapping properties survive, like the silicon.  `model-quirks.md`
  §17 has the table and the ordering rules.
- **Watchdog**: expiry now resets the SoC (the hardware contract RouterOS relies
  on for `/system reboot`); `MT7621_NO_WDT_RESET=1` turns it back into a warning.
  The armed bark is dropped on reset, so a reboot cannot inherit the previous
  boot's countdown.
- **Warm boot** skips the DDR-init stage and inherits the *trained* MEMC PLL, so
  the booter reports 880 MHz instead of decoding 440 MHz.
- **L1D model on by default at the hardware `Dl=4`** (32-byte lines); the GIC
  counter is clocked at the CPU rate the guest decodes from the PLL.
- CP0 compare writes tolerate the read-arm race (a fresh arm a few microseconds
  behind `Count` expires immediately instead of 2^32 ticks later).

### Fixed

- `irq 7: nobody cared` storm on guests that never route the local timer.
- RCU stalls / `epc=80017a80` hangs caused by unacknowledged VP-local interrupt
  state (and, separately, host starvation - see §18.3).
- `440 MHz` reported on warm boots.
- Netinstall instability: RX DMA into freed pages, a rewound RX index on
  `RX_EN 0->1`, a shadowed `GLO_CFG`, and stale TX/RX status firing `napi` on a
  zeroed `napi_struct`.
- The NAND warm-boot truncation (`chunks=415/416`, silent hang after
  `jumping to kernel code`): the loader now reports it (`INCOMPLETE`, missing
  chunk, `REFUSING: ELF tail missing`) and the *cause* was tracked to an
  offline image-injection page carrying another page's ECC (§18.1).

### Docs

- `model-quirks.md` §14 (topology and the four registers), §15 (compare routing),
  §16 (engine ownership and `GLO_CFG`), §17 (complete reset policy), §18 (the
  truncated-kernel case, its root cause in our offline tooling, and the model's
  ECC limitation).

### Known limits

- The NFI's ECC engine is a stub: `DECENUM` always reads clean, so the model
  cannot reject a page whose spare/ECC is inconsistent.  Images written *inside*
  the emulator therefore carry `0xff` ECC - fine for emulator tests, invalid on
  flash.  A BCH encoder/decoder for `512+8`-byte sectors (strength 4, 8 parity
  bytes) is the missing piece (§18.2).
- A loaded host starves the vCPUs; an RCU stall report from such a run is not a
  guest bug (§18.3).

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
  always did.  That patch also silences QEMU's own `qemu-version.sh`: a
  CraftBoot tree is a fresh repository with no tags, so its `git describe`
  fails by design and used to print a `fatal: No names found` line into every
  build.

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
