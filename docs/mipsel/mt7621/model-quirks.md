# Model quirks — MT7621

Things where the model knowingly differs from the SoC, and why.

## 1. Reported data-cache line size is 2, not 4 (`board=lg-gapm7200`)

`Config1.Dl = 4` (32-byte lines) is the hardware value and what the MIPS
kernel expects.  The parallel-NAND bootloader for this board, however, has a
bug in its DMA allocator:

```
n = size - 1 + 2 * line_size
p = kmalloc(n)
aligned = (p + line_size - 1) & ~(line_size - 1)
memset(aligned, 0, n)          /* n bytes from the *aligned* pointer */
```

The `memset` starts at `aligned`, not at `p`, so it writes
`round_up(p, line) - p` bytes past the allocation.  Its next free-chunk header
sits right there, so the following allocation fails and the bootloader prints
"unable to allocate fifos".  With `line <= 8` the pointer is already aligned
and nothing is overwritten, which is why a reported line size of 8 boots while
16/32/64 do not.

The model reports `Dl = 2` for this board while it runs without a data cache;
with `MT7621_DCACHE=1` it reports the hardware value and boots anyway (see the
resolution at the end of this section).  `MT7621_DL` overrides it for
experiments.

### Compared against real hardware, 2026-11-15

A B70 running Breed r1286 (the image in `full.bin`) was queried over its shell.
Every static input matches:

| field | hardware | model |
|---|---|---|
| Clocks | CPU 880, DDR 1200, Bus 220, Ref 40 | same |
| DRAM | 256MB, `80000000-8fffffff` | same |
| Kernel | 248KB, `8ffc0000-8fffe25f` | same |
| Boot parameters | 256KB, `8fd7be3c-8fdbbe3b` | same |
| Managed memory | 2MB, `8fdbbf3c-8ffbbf3b` | same |
| Cached DMA line size (`0x8fffc624`) | 32 | 32 |

Two things the comparison settled:

- The board's NAND is an **ESMT** part.  The device's own heap names it ("ESMT
  NAND 128MiB 3.3V 8-bit") while the model answered with the Samsung ID
  (`0xec`) for every board.  Vendor `0xc8` is now the `lg-gapm7200` default and
  `-M mt7621,nand-id=...` covers other parts (see 1d).
- The first 512 bytes of the heap (`0x8fdbbf3c`..`0x8fdbc13c`) are identical,
  chunk sizes included - the same `mt7621-uart`, `reset`, `sys`, `red`
  allocations and the same flash descriptor.

The Ethernet path is where they part.  The DMA wrapper's clear starts at
`round_up(p, line)` and runs `n = size - 1 + 2 * line` bytes; the allocator only
guarantees 8-byte alignment, so at `line = 32` the start is 0, 8, 16 or 24
bytes into a line.  For the first 1 KiB buffer (`n = 0x43f`, 1088 usable bytes)
only 0 fits:

- `0` - exact, nothing overruns.
- `8` - `0x43f + 8 - 1088 = 7` bytes past the allocation: 3 bytes reach the
  next chunk's `size`, and the following `malloc` returns NULL ("unable to
  allocate fifos").  16 and 24 are worse.

Measured here (`MT7621_DL=4`, Samsung and ESMT identity alike): the buffer is
at `0x8fddcd58`, i.e. 8 bytes into a line, and the boot fails.  The board boots
this same image at `Dl = 4`, so on the board that allocation must be 32-byte
aligned - which means the pre-Ethernet allocation sequence still differs, not
the arithmetic.

What the model allocates before the Ethernet setup is now 15 chunks totalling
`0x20dbc` requested (it was 19 chunks / `0x21744` before the CS0 fix removed
the duplicate page buffer), and the heap's first 512 bytes match the board's
dump chunk for chunk.  The remaining difference is somewhere after the flash
descriptor at `0x8fdbc13c`, i.e. in the `0x200`/`0xf4`/`0x844` page-buffer
group or the `0x20000` environment buffer.

Note for anyone re-testing: an earlier reading of "the ESMT ID fixes Dl=4" was
wrong.  That run did not pass `MT7621_DL`, so the board override below (`Dl=2`)
was still in force and the boot succeeded at `Dl = 2`.  The identity was a
fidelity fix only.

`MT7621_CFGLOG=1` prints the value the guest will actually see, at reset:

    cpu: Config1=fea3498f Dl=2 line=8        (default on lg-gapm7200)
    cpu: Config1=fea3518f Dl=4 line=32       (MT7621_DL=4)

Assert on that line in scripts instead of trusting that the environment
variable was passed through - the false positive above came from a loop that
printed the intended Dl without setting it.

### What the board's own heap says (2026-11-15, after the dumps)

The board's chain at `Dl = 4` is identical to the model's: first DMA chunk
`0x8fddcd50` size `0x449`, DMA pointer `afddcd60`, then `0x8fddd198` (second
buffer), `0x8fddd5e0` (0x100 table), `0x8fddd6e8` (ring, size `0x18c69`).  So
the layout and the overrun are the same on both sides; there is no 24-byte
difference to find - the earlier theory was wrong.

What differs is the allocator's state.  Breed's allocator (TLSF, control block
at `0x8fff0000`, `sbrk` bookkeeping at `0x8fffcd30`) ends up with:

| | board | model (Dl=2) | model (Dl=4, at the DMA alloc) |
|---|---|---|---|
| heap base / limit | `8fdbbf3c` / `8ffbbf3c` | same | same |
| `sbrk` cursor | `8fe51000` | `8fe09000` | `8fdde000` |
| allocator counter at `8fff0f8c` | `0x53` | `0x4c` | `0x48` |

The board does about 0x48000 more transient allocation over boot than the model
does at `Dl = 2`; that is a separate fidelity gap and not the `Dl = 4` cause
(zeroing three bytes kills any chunk size below 16 MiB, whatever the cursor
is).  `sysinfo`'s "Managed memory used" is not comparable between the two:
it includes the DHCP/telnet buffers, and it moved from 860.87KB to 588.08KB on
the board between two measurements of the same boot.

At the moment of the first DMA allocation the model's free block after the
buffer is `0x8fddd198` with size `0xe68` (it ends exactly at the cursor).  The
wrapper clears `n = 0x43f` bytes from `round_up(p,32) = 0x8fddcd60`, i.e.
through `0x8fddd19e` - three bytes of the next chunk's size field, so the
header `0x00000e69` becomes 0, the allocator sees a zero-sized last block, its
pool extension is not adjacent to that block and it returns NULL.

### Resolved: the model had no data cache (2026-11-15)

The board's chain is identical to the model's - same chunks, same alignment,
same clear - so the difference was never the heap layout.  It is the cache.
Breed clears through the **uncached** alias (`0xA0000000|p`) and the allocator
reads the header through the **cached** one; on silicon the line stays in the
L1D, so the allocator still sees the old size and the overrun costs nothing.
QEMU had no cache, so the zero was immediately visible.

The machine now models the 1004Kc L1 data cache for real (`MT7621_DCACHE=1`,
`modules/dcache.c`, `patches/0005-mt7621-data-cache-model.patch`):

- 32 KiB, 4-way, 256 sets, line size from `Config1.Dl`, true LRU per set.  A
  miss fills from memory and, because a store may miss too, a store allocates
  the line (write-allocate) as well as writing memory.
- **Write-through**, and the memory write is performed by QEMU, not by the
  model: doing it in the helper skips QEMU's dirty/TB bookkeeping, and
  invalidating a TB from inside a store helper makes QEMU re-execute the
  instruction, so the store lands twice.  That double store was what zeroed the
  chunk header the allocator had just written (measured: every address written
  twice in the window trace).
- Uncached accesses (KSEG1, device mappings, uncached TLB pages) never reach
  the model; they go to memory and leave whatever the cache holds untouched -
  which is exactly why Breed's clear leaves a stale line behind.
- DRAM aliases: the machine mirrors RAM above the installed size so that Breed
  can measure it.  An alias is the same physical line, so the model indexes
  with the **wrapped** address; otherwise the mirror probe reads the cache and
  Breed measures 448 MiB instead of 256 MiB.
- `cache` instructions and `synci` are applied: hit invalidate and hit
  writeback-invalidate drop the line (writeback is a no-op in a write-through
  cache), index operations invalidate the addressed set.  `ll`/`sc` bypass the
  cache by construction (QEMU masks `TLB_FORCE_SLOW` in the atomic path), so
  `sc` is followed by a line invalidate in `gen_st_cond`.

How an access is routed: `mips_cpu_tlb_fill()` sets `MemTxAttrs.force_slow` for
a cacheable page, and `tlb_set_page_full()` turns that into `TLB_FORCE_SLOW` in
the TLB comparator, so every data access to that page takes the slow path where
the model serves it.  Two details matter and both were bugs first:

- `TLB_FORCE_SLOW` is `1 << 8`, but `CPUTLBEntryFull.slow_flags[]` is `uint8_t`,
  so the original code stored zero there and *nothing was ever forced*.  The
  cache therefore only saw TLB misses and the accesses after its own flush -
  which is why the model appeared to need `-icount` and behaved erratically.
  The request now travels in `MemTxAttrs.force_slow`, which
  `tlb_set_compare()` honours.
- the handler also runs on an ordinary TLB miss, so it checks the mapping's
  cacheability itself.  Cacheability comes from the segment/page tables:
  kseg0..kseg3 use their `SegCtl` `C` field, useg/sseg consult the addressing
  mode and then the TLB entry's `C`.  Treating an address by its numeric range
  alone made **KSEG1 look cached**, and Breed's DRAM probe (write through
  KSEG1, read back) then observed the cache and measured 448 MiB.

Measured with `MT7621_DCACHE=1`, no `-icount`:

| run | result |
|---|---|
| `Dl=2` | Breed, Linux, shell |
| `Dl=4` | Breed, Linux, shell - **the hardware value, no override** |

Cost and routing, measured on one pinned core:

| milestone | no cache | cache model | ratio |
|---|---|---|---|
| `ubi0: attached` | 11 s | 13-16 s | ~1.2-1.4x |
| `procd: - init -` | 22-26 s | 29-31 s | ~1.3x |
| `br-lan: link becomes ready` | 36.8-39.9 s | 67.6-70.1 s | ~1.78x |

The `br-lan` row is three runs each; the spread is +-2 s, so a difference under
about 3% cannot be told apart from noise on this host.  (A single 64.3 s sample
earlier was luck, not a gain.)

Measurement caveat: the same build measures 67.6-70.1 s in one session and
71.5-71.7 s in another, so this host drifts about 5% and anything smaller is
noise.  Two "optimisations" (hoisting the geometry into statics, finding a free
way with one SSE compare) were tried and reverted for exactly that reason: they
measured as a regression in one session and as neutral in the next, and the
simpler code won.

What the model actually does to the guest, from `MT7621_STALETRACE=1` over a
boot to `br-lan`:

```
hits 1.82e9   fills 3.1e7 (1.7% miss)   stores 1.85e9
stale_made 1625   stale_reads 8584   invalidates 1.26e6
```

1.26M cache-op invalidations cost about 0.04 s, so they are not a lever.  Of the
8584 reads served from a stale snapshot, only **three** differed from memory:
two are Breed's DMA buffers (the allocator header the whole model exists for)
and one is a kernel read of a DMA buffer before its invalidation - a case where
the hardware returns the cached copy too.  That is the check that matters: the
model is not inventing stale reads anywhere else.

Everything in the cost is the per-store slow-path entry, and the model side of
it has been taken to its floor: one 32-byte set per cache set with four tags,
four LRU stamps and a stale bit per way; the four tags compared at once with
SSE2; no bytes copied on a fill (a write-through resident line equals memory, so
only lines an uncached store made stale keep bytes, in a small direct-mapped
side table); tags holding line+1 so zero means invalid; counters only maintained
under `MT7621_STALETRACE`; the LRU stamp skipped when the way is already the
most recent.  FIFO replacement (`MT7621_FIFO=1`) measured *worse* - 69.5 s
against 67.6 - and stays a knob: true LRU is what keeps the allocator header
resident across Breed's clear.

The whole remaining gap is the per-store slow-path entry.  Measured with
`MT7621_DCACHENOSTORE=1` (diagnostic: no store tracking at all, residency
assumed), the same boot takes 45 s - i.e. **the model would be free if store
accounting cost nothing**.  That option is not shippable because assuming
residency is exactly what lets a stale value be invented, but it fixes the
target for the next step: keep the store in the fast path and record the
stored line's tag there.

That step is host code generation, not machine code: `tgen_qemu_st()` in
`tcg/i386/tcg-target.c.inc` emits the fast store itself, so recording a line
would be ~11 hand-written x86 instructions (LEA the host address, subtract the
RAM base, bounds check, shift to a line number, store the tag) using only the
two reserved scratch registers.  That was implemented and compiled, and then
withdrawn, because it cannot be exact:

- the log can only record **stores**; the real cache also fills and evicts on
  **loads**, and loads are the bulk of the traffic.  So a line that the cache
  evicted long ago stays marked in the log, and the model would then serve a
  stale copy for a line the hardware no longer holds - invented staleness,
  exactly the failure mode this model exists to avoid;
- bounding that with a window ("stored within the last N stores") is the
  ageing heuristic again, and evicting from the log by store pressure alone
  still forgets more slowly than the cache does.

Making residency exact therefore needs an inline *cache lookup* on every load
and store - tag compares and a copy in generated code instead of one helper
call - which is a real TCG cache model, and it would itself cost something.
A second, non-TCG attempt went the same way: gate store tracking on pages that
have actually been accessed through an uncached mapping (only such a page can
receive a store that bypasses the cache).  Breed boots on it - no fifo error -
but the kernel livelocks just after `sched_clock`, because a page becomes
bypass-capable at `tlb_fill()` time and flushing the TLB from there storms
during early init.  It is also an approximation: a page the guest only ever
touches through cached mappings would get no staleness at all.

### The way to exactness at speed, scoped

The exact way to know residency without a slow path per store is to let the
emulated cache track itself in generated code, which is what a real cache does
per access.  Design, in stages:

1. **Reserve one host register** for the model (a callee-saved one, already
   pushed by TCG's prologue), remove it from `tcg_target_available_regs` so the
   allocator never hands it to a guest value, and keep the tracker base there.
2. **Inline on both loads and stores.** From `haddr` (`base + index + ofs`,
   already computed by `prepare_host_addr`): subtract the RAM base, bounds
   check, shift to a line number, mask to a set.  Then the four ways of that
   set are one 16-byte SIMD compare against the line (`pcmpeqd` +
   `pmovmskb`), so "resident?" is one branch.
3. **On a miss, insert** into the set's round-robin way (a two-bit counter per
   set, incremented inline).  Round-robin rather than LRU is a replacement
   policy choice, not a bias: it is what many real caches do, and it keeps the
   tracker's capacity equal to the hardware's 1024 lines.
4. **The C model shrinks** to what the inline tracker cannot do: snapshot the
   pre-store bytes when an uncached store hits a line the tracker says is
   resident, serve loads on pages that hold such a snapshot, and drop both on
   `cache`/`synci` (which must also clear the tracker's tag, so an invalidated
   line stops counting as resident).
5. **Validation before trust**: while both mechanisms exist, have the C model
   (still seeing every store) compare its own residency against the tracker on
   each store and count disagreements.  They must agree except immediately
   after an invalidation, which is a bug in the wrong direction.

The store half of this was implemented and measured: the tracker costs about
6% of a boot (71 s instead of 67 s to `br-lan ready`), i.e. far less than the
40 ns per-store slow path it was meant to replace.  Three codegen bugs it
exposed are worth recording, because they are the class of mistake this work
invites:

- `OPC_SUB_GvEv` is the `sub r, r/m` form, so the destination is the *first*
  register: `sub L0, L1` needs `(TCG_REG_L0, TCG_REG_L1)`, not the reverse.
  The reversed form computed `ram_base - haddr`, so nothing was recorded and
  every access fell through the bounds check;
- a reserved host register must be added to `s->reserved_regs` *after*
  `tcg_target_init()` rebuilds that set from scratch - reserving before the
  rebuild silently leaves the register allocatable, and the tracker then
  clobbers a live guest value (the symptom was Breed reporting "Bootloader data
  is corrupted": every stored value came out wrong);
- the tracker must record **every** store width: Breed writes its chunk header
  with a sub-word store, so a 4/8-byte-only tracker has no residency for the
  one line the whole model exists to protect.

Even with those fixed, it was withdrawn, and the second round narrowed the
reason precisely:

- it had to record **loads** as well as stores, or lines the CPU keeps reading
  have no residency at all;
- the set block stride must match the model exactly.  It went from a
  five-word (four tags + counter) layout to a four-tag layout while the emitter
  still multiplied by 20, so every tag landed in the wrong block and residency
  was never found - visible as a set block full of `ffffffff` with three
  million bounds checks having passed;
- round-robin insertion was replaced by a **FIFO with move-to-front on a hit**,
  which is the cheap LRU approximation.

That still failed for a reason worth recording: at Breed's DMA clear the set
held the neighbouring lines but not the header, which four insertions had
pushed out - because stores on a page that already holds a stale record take
the *slow* path, so the generated code never sees them.  Once the model's own
store path also recorded residency, Breed booted on the tracker.

It was still withdrawn, on measurement rather than principle:

| | `br-lan: link becomes ready` |
|---|---|
| no cache model | 41.1 s |
| inline tracker (loads+stores, move-to-front) | **85.5 s** |
| exact model with the per-store slow path | 71 s |

Move-to-front costs four compares plus up to eight memory moves and two stack
parkings *per access*, on both the load and the store path, which is more than
the 40 ns slow-path entry it replaces.  Two consequences for anyone picking
this up:

- a reserved host register is not an option: taking one register out of
  `tcg_target_available_regs` breaks TCG's constraint sets and QEMU dies with
  no diagnostic.  The emitted code must live with `TCG_REG_L0`/`L1` plus the
  host stack;
- the inline update has to be a few instructions, not tens.  That means a
  **PLRU bit tree** (three bits per set, updated with and/or shifts on a hit,
  ~6 instructions) rather than move-to-front, and recording stores only if the
  load side cannot be made that cheap.

So the honest position is: **about 1.48-1.75x is what an exact per-access cache
costs in this TCG**.  The 1.0x ceiling requires either assuming residency
(which invents stale values, the failure this model exists to prevent) or
inlining a full cache lookup on both load and store paths.  The model in the
tree pays the exact price, and both faster attempts are recorded here so they
are not rediscovered.

## 1b. Clocks: crystal and DDR/bus decode

Real boards (a K2P log Breed 1.2 prints) report `CPU 880, DDR 1200, Bus 293,
Ref 40`.  The model now matches the first and last of those: SYSCFG0[8:6] is 4
(40 MHz crystal, per clk-mt7621's 0-2 = 20 MHz, 3-5 = 40 MHz, 6-7 = 25 MHz) and
the CPU PLL in MEMC 0x648 is FBDIV 21 / PREDIV 0, i.e. `(21 + 1) * 40 / 2^0 =
880 MHz`.

Two decodes are still wrong, and they come from registers whose real values are
not known:

- **DDR** is decoded from MEMC 0x618: `0x64 -> 960`, `0x78 -> 1120`,
  `0x80 -> 1280`, `0x96 -> 1440`, `0x190 -> 4000` MHz, i.e.
  `ref * 4 * (v >> 4)`, halved when 0x604 bit 2 is set.  The pair shipped now
  is `0x604 = 0x4`, `0x618 = 0xf0`, which prints **DDR 1200** - the rate the
  reference board reports.
- **Bus** decodes as `CPU / 4` in these images (220 MHz at 880).  The 293 MHz
  in the K2P log comes from Breed **r1126** (2017); the dumps here are r1280 /
  r1286 / r1416, so the difference may be the bootloader build rather than the
  SoC.  Nothing modelled moves it: SYSCFG0 bits 9-31, CLKCFG0 (all four
  selector values), CLKCFG1, CUR_CLK_STS and MEMC 0x604/0x618 were all swept.
  A K2P-class dump (r1126) would settle whether the model needs a divider
  register we have not found.

`CUR_CLK_STS` itself is decoded: bits 1:0 multiply the CPU clock and bits 9:8
divide it (`0x101 -> 880`, `0x102 -> 1760`, `0x201 -> 440`, `0x301 -> 293`).

All of these are reachable at run time for further measurements:
`MT7621_MEMC604`, `MT7621_MEMC618`, `MT7621_DDR_PLL`, `MT7621_CLKCFG` and
`MT7621_CLKSTS`.

## 1c. No SPI NOR on lg-gapm7200

The B70's only flash is the parallel NAND on the NFI.  The machine used to
register the SPI controller for every board, so firmware probing it found a
W25Q128 die that is not soldered on this board.  `mt7621_spi_init()` now
registers the controller only for boards that have one (`rb750gr3`); on
`lg-gapm7200` reads in that window hit the sysctl region and report nothing,
which is what the real board does.

This does not by itself change the DMA-allocator outcome at Dl=4 (measured: the
first 1 KiB buffer did not move), so it is a fidelity fix, not the allocator
one.

## 1d. The NAND answers on CS0 only

Breed probes both chip selects during its flash scan.  The model answered the
ID command on CS0 *and* CS1, so the bootloader "found" the same NAND twice
(two identical `Flash:` lines) and allocated a second page buffer for the
phantom die.  `mt7621_nfi_cmd()` now answers only when `NFI_CSEL` selects CS0,
which is the board's wiring: one `Flash:` line, and ~0x9a0 less heap before
the Ethernet setup (measured: the first 1 KiB DMA buffer moves from
`8fddd6f8` to `8fddcd58`).

The identity is also the board's part now: an ESMT 128 MiB chip (vendor
`0xc8`), as the device's own heap dump shows, instead of the Samsung `0xec`
the model used to hard-code.  Boards differ, so this is a per-board default
overridden by `-M mt7621,nand-id=...`.

Dl=4 still fails after this: the buffer lands 24 bytes into a 32-byte line (8
bytes to the next boundary), which is 4 bytes too far for Breed's overrun; see
the measurements in section 1.

`MT7621_CSLOG=1` prints every ID probe and the chip select it used.

## 2. DSA receive path: `fport` encoding and the delayed RX interrupt

The frame engine fills RX descriptor word 3 with the receiving port, and the
two software stacks that run here read it differently:

| stack | field | value |
|---|---|---|
| Linux `mtk_eth_soc` | `RX_DMA_FPORT_SHIFT` 19, `mac = fport - 1` | `1 << 19` (MAC0) |
| other (`raeth` style) | `ext(rxd4, 18, 3)` | `5 << 18` (CPU port) |

Using one encoding for the other stack is not "the wrong port": Linux computes
`mac = -1` and releases the descriptor before any counter moves, which looks
exactly like a silent backend.  The model picks the encoding from `qtx_en`
(only the Linux driver enables the QDMA transmit engine); `MT7621_RX_FPORT`
forces the port number.

Drivers also move the RX rings to *delayed* interrupts after PHY bring-up
(`PDMA_RX_DLY_INT_CFG`, `MTK_PDMA_INT_MASK = MTK_RX_DONE_DLY`, bit 30).  The
model raises the delayed bit as well as `RX_DONE_INT0`, otherwise NAPI never
polls and filled descriptors are never consumed.

## 3. Guest time: CPU clock, GIC counter width, watchdog

Two independent things made guest time wrong (`sched_clock` at 25 MHz,
timestamps jumping, minute-long NAND scans, RCU stalls):

- **CPU clock.**  `clk-mt7621` computes the CPU rate from `MEMC[0x648]`
  (`FBDIV` bits 10:4, `PREDIV` 13:12).  The model programs FBDIV 69 /
  PREDIV 1 → 875 MHz, matching the counter.
- **GIC counter width.**  The SoC's GIC has a 64-bit shared counter, and Linux
  registers sched_clock with 64 bits regardless of the advertised width.  The
  model advertises `COUNTERBITS = 8`, implements `COUNTERHI`, and treats the
  per-VP compare as a full 64-bit deadline.  Compare semantics follow the
  width: a 32-bit counter wraps and re-arms after each expiry; a 64-bit
  counter has no next lap, so a deadline at or behind the counter is
  considered delivered and the timer disarms until the guest writes a new
  compare.

The counter width is a device property (`counter-bits`, default 32-bit); the
MT7621 machine sets 64.  `MT7621_GIC_COUNTER_BITS` and `MT7621_CPU_PLL` exist
only for experiments.

The guest's own watchdog pets its timer for a while and then stops while
loading modules; emulation is slow enough to exceed the guest's timeout, so
the model warns instead of resetting.  `MT7621_WDT_RESET=1` restores the
reset.

## 4. NAND command 0xEC

The 5.15 NAND driver issues `0xEC` (cache read) during its bad-block scan and
polls `NFI_PIO_DIRDY`.  It is handled like `0x30`; without it every miss burned
the driver's PIO timeout and produced RCU stalls.
