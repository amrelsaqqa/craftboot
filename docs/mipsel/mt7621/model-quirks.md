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

**Both boards now run at the hardware `Dl=4` with the L1D model on by
default** (`MT7621_DCACHE=0` opts out, `MT7621_DL=2` restores the old 8-byte
baseline).  `Dl=2` was only ever a Breed-allocator workaround for the uncached
baseline, and it is fatal for Linux guests: `r4k_blast_dcache_setup()` installs
a blast routine only for 16/32/64/128-byte lines, so an 8-byte line leaves
`r4k_blast_dcache`/`icache` NULL and the first flush - typically an IRQ-time
DMA sync - jumps to address 0.  That is how the RouterOS netinstall kernel died
on lg-gapm7200 (`epc=00000000`, `BadVA=00000000`, ExcCode 2, "Fatal exception
in interrupt", followed by `kernel/smp.c:420` "smp_call_function_many from
IRQ").  Measured before this change, no `-icount`:

| run | result |
|---|---|
| `Dl=2` | Breed, Linux, shell - but any cache flush from IRQ context jumps to 0 |
| `Dl=4` | Breed, Linux, shell - **the hardware value, now the default** |

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

**Breed and U-Boot disagree by a factor of two on DDR, and both are right.**
They read the same two registers but decode them differently:

| reader | decode | with `0x618 = 0xf0`, `0x604 = 0x4` |
|---|---|---|
| Breed (measured, see below) | `ref * 4 * fb`, halved when `0x604` bits 2:1 are non-zero | 1200 |
| U-Boot `arch/mips/mach-mt7621/clocks.c` | `fb * xtal`, doubled when `DIV2_SEL` (same bits) is zero | 600 |

Measured on the model by sweeping `MT7621_MEMC604` / `MT7621_MEMC618` and
reading Breed's `Clocks:` line: `0x604 = 0x4` or `0x2` -> `80 * fb`
(`0xf0 -> 1200`, `0x1e0 -> 2400`, `0x78 -> 560`), `0x604 = 0x0` -> `160 * fb`
(`0xf0 -> 2400`, `0x3c -> 480`).  So Breed prints the DDR **data rate**
(1200 MT/s = 2 x 600 MHz) and U-Boot the **clock** - the real B70's Breed log
does say `DDR: 1200MHz`.  The pair shipped here is therefore correct; do not
"reconcile" the two readers by changing `0x618`.  (`MT7621_DDR_PLL`, i.e. the
0x64c register, moves nothing in either decoder - verified.)

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
  (`FBDIV` bits 10:4, `PREDIV` 13:12).  The model programs FBDIV 21 /
  PREDIV 0 with the board's 40 MHz crystal → 880 MHz, the value a real B70
  prints (`Clocks: CPU: 880MHz, DDR: 1200MHz, Bus: 220MHz, Ref: 40MHz`).
  `CLKCFG0` selects that PLL (`CPU_CLK_SEL = 1`) and `CUR_CLK_STS = 0x101`
  keeps the ÷1 ×1 post-divider, so Breed, U-Boot and Linux all read 880.
  The GIC counter is programmed to the same 880 MHz — it used to be left at
  875 MHz, the rate the old 25 MHz-crystal model produced from FBDIV 69 /
  PREDIV 1, which made every guest timebase run 0.57% slow relative to its
  own `udelay()` scale.
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

## 5. NAND page address space: raw page vs AUTO_FMT

The NFI works on 512-byte sectors, and the address the guest puts on the data
port is resolved differently depending on the mode:

| mode | data port layout | spare |
|---|---|---|
| raw (`AUTO_FMT_EN=0`) | `[512 data][16 spare]` x4 = 2112 bytes | interleaved after each sector |
| `AUTO_FMT_EN=1` | four 512-byte data sectors = 2048 bytes | `NFI_FDML/FDMM` only |

Both the MTK U-Boot driver and RouterBOOT read the bad-block mark with a
one-byte access at column `ecc.size + badblockpos` (`512`), which the hardware
resolves to **spare byte 0 of sector 0**.  The model used to treat every
column below 2048 as plain page data, so any block whose byte 512 was not
`0xff` (the first page of a real image, an environment block, ...) was
reported bad and `nand read` returned `0xff`.

`mt7621_nfi_page_read` now builds the raw interleaved page, and the data port
maps the guest's logical offset per mode: byte reads (`BYTE_RW`) use the raw
layout, `AUTO_FMT_EN` reads skip the spare bytes, and raw reads walk the
interleaved layout.  Column `2048..2111` still addresses the main-area dump's
spare area directly, which is the view the older booters use.

This is why U-Boot's `nand bad` is empty and `nand read` returns the real
image bytes; Breed → OpenWrt is unaffected.

## 6. Boot a real B70 die dump: the NFI protocol, the three layouts, and what is still open

This is the state after taking a **real device backup** (`backup-flash-dump-lg7200.bin`,
132 MiB) as the boot image, with our own NFI driver (`~/mt7621-mod/mt7621_nand.c`,
the `mt7621_nfc` module we ported) as the reference for what the hardware must do.
Before this, every attempt at "run the device's own image" died in the booter or
in the filesystem; the list below is why.

### 6.1 A die dump is per-sector raw, not 2048+64

There are three dump layouts and they are not interchangeable:

| layout | page stride | where the spare is | knob |
|---|---|---|---|
| main area only | 2048 | absent, synthesised `0xff` | `MT7621_NAND_SOURCE=data` |
| "mtdall" die | 2112 | 64 bytes at the **end** of the page | `MT7621_NAND_SOURCE=die` |
| controller-raw die | 2112 | 16 bytes **after each 512-byte sector** | `MT7621_NAND_SOURCE=raw` |

A real `nanddump` of this part is the third one.  Reading it as the second puts
everything after the first 512 bytes of each page 16 bytes early - the booter's
stage-1 code then executes `0xffffffff` and the model faithfully reports the
`RI` it takes.  Symptom: Breed boots, "jumping to kernel code", then nothing, or
a `TLBL` loop in the booter's handler.

Diagnosis that settled it: a known byte pattern from a working flat image at
`0x240` appeared in the die dump at `0x250` - one 16-byte spare block later -
and every page's first 512 bytes matched the flat image while the rest was
`0xff`.

The model now keeps `nand_spare` (`NONE`/`PAGE`/`SECTOR`) next to `nand_stride`,
and every path that touches spare bytes goes through one accessor:
`mt7621_nand_oob_byte()` / `mt7621_nand_oob_write()` (image OOB when present,
programmed shadow otherwise, plus the factory bad-block mark and any
`MT7621_NAND_YAFFS` tags).  Writes scatter per sector for the raw layout, because
a data-only program that memcpy's 2048 bytes at the page start overwrites the
neighbouring sectors' spare area.

### 6.2 The BootROM runs stage 1 out of `0xb900_0000`

A NAND uImage at flash 0 is a `uImage` whose name field is overlaid by MikroTik's
`nand_header_t`; `ih_bootloader_offset` (+52) is non-zero and points at the
bootloader inside the image data.  The model used to skip straight to it
("BootROM boot step 2").  On hardware the ROM first copies the image data that
precedes that offset into the BootROM's stage-1 window and runs it there; the
stage-1 image is linked at **`0xb9000000`** (its first instruction is
`j 0xb9000140`, and `0xb9000148` reads `CP0 EBase` and writes `0x7621abcd` to
`0xbe10ad00` as its first hardware poke).  This is the default path (`MT7621_NO_ROM_STAGE1=1` restores the old shortcut):
stage 1 is loaded at `0xb9000000` and entered, the bootloader is pre-placed at
`ih_load` for the hand-over, and the loader's bounds check accepts the KSEG1
window (512 MiB, upper half aliases DRAM) instead of only `ram_size`.

Without this the device's own booter runs into `0xffffffff` at `+0x200` and
loops in its TLB-refill path - the register dump shows `CP0 Status EXL=1`,
`Cause = BD | ExcCode 2`, `EPC = 0xa020…`, `PC` in the exception vector.

### 6.3 What the NFI driver requires (and the model did not do)

Read from `mt7621-mod/mt7621_nand.c`; all four were model bugs, not driver quirks:

1. **Command first, addresses after.**  `nand_read_page_op()` sends `0x30` and
   only then the five address bytes.  The model used to fetch the page inside the
   `0x30` handler, i.e. from the *previous* row.  The fetch is now deferred
   (`nfi_read_pending`) and happens on the first `DATAR` access - and the word
   path has to trigger it too, or AUTO_FMT reads return `0xff` from an empty FIFO
   and the driver spins at `PIO_DIRDY` (that was the intermittent "hang in
   Scanning device for bad blocks").
2. **Every address byte goes through `COLADDR`, then `ADDRNOB=1`.**
   `mt7621_nfc_send_address_byte()` writes `COLADDR=byte; ROWADDR=0; ADDRNOB=1`
   for each cycle, so the column/row split cannot come from `ADDRNOB`; it comes
   from the current NAND command.  The byte must also be latched as the *pending*
   byte or the commit appends a stale value: that made every row assemble to 0,
   so **every erase hit block 0** (`erase row=0 block=0` ×40) and yaffs retired
   the whole partition.
3. **Erase addresses are row-only.**  `0x60` is followed by three row bytes and
   no column; the model assumed two column bytes first.  Handled by
   `nfi_addr_row_only`, set on `0x60` and cleared by `0x00/0x30/0x80/0x90/0xff`.
   The address phase now starts at the first address byte after a command
   (`nfi_addr_fresh`), which covers both orders - the booters send addresses
   before the command, the Linux core after it.
4. **FDM is 8 bytes per sector at an 8-byte register stride.**
   `NFI_FDML(n) = 0xa0 + n*8`, `NFI_FDMM(n) = 0xa4 + n*8`, `NFI_FDM_SIZE = 8`;
   sector *n*'s spare is `[n*16 .. n*16+7]` (the other 8 bytes are ECC parity).
   The model mapped four bytes per register over the whole 64 bytes and, worse,
   only *read* FDM.  Program writes the OOB through FDM (the data port carries
   data only in `AUTO_FMT`), so tags written by yaffs landed nowhere while the
   data page did - the write/read round-trip yaffs checks.
5. Program path detail: the driver skips all-`0xff` pages entirely
   (`check_empty_page`), so a missing program command is expected, not a fault;
   and it forces spare byte 0 to `0xff` on every programmed page so the
   bad-block marker stays pristine (its free area starts at byte 1).

### 6.4 Verification ladder on the die dump

Each step is a real milestone, measured on `MT7621_ROM_STAGE1=1 MT7621_NAND_LAYOUT=raw`:

| step | evidence |
|---|---|
| Breed runs from the die dump | `Board: HiWiFi HC5962 (B70)`, `Clocks: CPU: 880MHz, DDR: 1200MHz, Bus: 220MHz, Ref: 40MHz` |
| loader assembles the kernel object from real OOB | `[NB] obj=00010100 seq=001011ff data=00000869 chunks=0x415` |
| kernel boots | `MIPS: machine is mediatek,mtk7621-soc`, `Memory: 254224K/262144K` |
| NFI driver binds | `mt7621_nfc: ESMT NAND 128MiB`, `ECC strength adjusted to 4 bits` |
| MTD layout | 3 partitions: `RouterBoard NAND 1 Boot`, `RouterBoard NAND 1 Main`, `RouterBoot` |
| yaffs mounts **mtdblock1** | `Attempting MTD mount of 31.1,"mtdblock1"`, `89 blocks to be sorted...`, `is_checkpointed 0` |
| userspace starts | `[NB] init: up`, GPIO/UART/PHY registration, later `crypto: ring status: 0` |

### 6.5 Fixed: the yaffs "block needs retiring" storm

Symptom (hundreds of lines per run, then `init` dying):

```
yaffs: **>> Block 133 retired
yaffs: Block 133 is in state 9 after gc, should be erased
```

yaffs erased a GC block, programmed chunks into it, read them back and found
neither erased data nor valid tags, so it retired the block - and then the next
one, until the filesystem had no blocks left.

Root cause: **the FDM write path was unreachable code.** The capture for
`NFI_FDML/FDMM` writes (spare bytes, where yaffs tags live) had been inserted
between `case NFI_DATAW:` and `case NFI_CMD:`, i.e. after a `return`, so no
program ever captured spare bytes:

```
nand-wr: program row=9976 col=0 len=2048 fdm=0 data0=01000000   <- tags dropped
```

The data page was programmed, the tags were not, so every read-back of a written
page looked corrupt.  As a proper `case 0xa0 ... 0xbf:` the same program shows:

```
nand-wr: program row=9976 col=0 len=2048 fdm=1 data0=01000000
```

and the storm stops:

| | before | after |
|---|---|---|
| `fdm=` on programs | 0 | 1 |
| yaffs retire messages | 971-1416 per run | **0** |
| where it ends | `init` panic | RouterOS userspace (`rb_ioctl`, `RB_GET_TYPE`, `probe_er_radio_data`) |

The lesson is general: the model's spare write path is the only thing that makes
a filesystem's metadata round-trip, so "the data page was written" is not
evidence that a write worked.  `MT7621_NAND_WRLOG=1` prints `fdm=` and the first
spare bytes per program for exactly this check.

### 6.5b Still open

- **Intermittent no-output boots** with an identical command line (sometimes
  nothing after reset, sometimes a stall after `jumping to kernel code`).  Not
  reproducible under `-icount` (the machine is ~5x too slow there), so it needs a
  poll trace: `MT7621_NFISPIN=1` names a register that is read thousands of times
  with the same value.
- **Bad-block scan takes ~16 s** (one read round trip per page through the
  model).
- **Warm reboot** does not yet behave like the device (`machine_hang` vs the real
  reset path) - unverified.

### 6.6 Knob reference (all off by default unless noted)

| knob | meaning |
|---|---|
| `MT7621_NAND_SOURCE=data\|die\|raw` (alias `MT7621_NAND_LAYOUT`) | dump layout (6.1) |
| `MT7621_NAND_BADBLOCKS=a,b,…` | overlay the factory bad-block map |
| `MT7621_NAND_BBLOG=1` | log bad-block queries and the byte path |
| `MT7621_NAND_WRLOG=1` | log program starts and erase row/block |
| `MT7621_NFIWLOG=1` | log every NFI register write (register protocol) |
| `MT7621_NO_ROM_STAGE1=1` | disable the BootROM stage-1 path, i.e. the old jump-straight-to-the-bootloader shortcut (6.2; stage 1 is the default and the only path a real image boots through) |
| `MT7621_BOOTLOG=1` | first 32 PCs after reset, with the entry point |
| `MT7621_TLBLOG=1` | TLB-miss exceptions with `BadVAddr`, `EPC`, faulting word |
| `MT7621_NAND_YAFFS=obj,seq,page:chunk:count,…` | synthesise yaffs tags for main-area dumps |
| `MT7621_DCACHE` / `MT7621_DL` | L1D model (default on) and line size override |

Core changes that came with this: **IBE on instruction fetch from a device or
unassigned region** (`target/mips`, ExcCode 6) instead of QEMU aborting with
`cpu_io_recompile: could not find TB for pc=(nil)`; the machine now reports the
hardware `Dl=4` with the L1D model on by default; and the GIC counter runs at the
CPU rate (880 MHz) the guest decodes.

## 7. MDIO: the MT7530's five PHYs, the busy bit, and the frame's ST field

The die image now boots into RouterOS's userspace (section 6), and the next
things it complains about are Ethernet.  Both items here came out of tracing the
MDIO controller (`MT7621_PHY_IAC` at frame-engine offset `0x10004`) on the real
device image, not out of guessing.

Register layout, from `drivers/net/ethernet/mediatek/mtk_eth_soc.[ch]` (the
driver RouterOS's `raeth` descends from, and the one our model was written
against):

| bit | meaning |
|---|---|
| 31 | `ACCESS`: the transaction.  Write it, the controller runs the frame and clears it. |
| 25..29 | register (5 bits) |
| 20..24 | PHY address (5 bits) |
| 19 / 18 | `READ` / `WRITE` |
| 16 | `ST`: the MDIO frame's start-of-frame field (`01` for clause 22, `00` for clause 45) |
| 15..0 | data (write) / read result |

### 7.1 Only five PHYs and the switch answer

`MT7621_MDIOLOG=1` records every IAC transaction with its decoded PHY and
register.  A full boot of the die image touches exactly these addresses:

```
phy 0 .. 4      (132 / 45 / 45 / 45 / 45 transactions - the MT7530's five
                 internal PHYs, port 0 = WAN, 1..4 = the LAN ports)
phy 31 (0x1f)   (2917 transactions - the switch's indirect register file)
```

Nothing else.  A real MDIO address with no device reads back `0xffff`, and the
model had been answering *every* address 0..31 with a plausible BMCR/BMSR/PHY-ID
set - a "PHY" at every address a driver scans for.  `mt7621_eth_phy_read()` now
returns `0xffff` for `phy > 4`, which is what the bus does.

This is also why RouterOS prints `raeth: mdio timeout` when it does hit an
address that does not answer: it is the driver's word for "read returned
`0xffff`", not a bus hang.  How many of those appear is *not* deterministic
across runs (4 in three runs, 0 in another with an identical command line), so
the count alone says little; the transaction trace is what identifies them.

### 7.2 `ACCESS` is the busy bit; `ST` is not a second "go"

The model used to start a transaction only when a write had **both** `ACCESS`
and `ST` set.  That is right for clause-22 frames (Linux's driver always sets
both), and wrong for the frames the booter sends.  RouterBOOT's etherboot, right
before it prints `Press Ctrl+E to enter etherboot mode`, writes:

```
IAC <- 0xbe000026     ACCESS | reg 0x1f | data 0x26, ST = 0
IAC <- 0xbe040000     ACCESS | reg 0x1f | data 0x0,  ST = 0
```

and then polls `ACCESS`.  Under the old model nothing ever ran, `ACCESS` stayed
set, and the booter spun a bounded ~1200 times per frame before giving up:

```
mdio: read iac=be000026 ACCESS-STILL-SET      (1199 times)
mdio: read iac=be040000 ACCESS-STILL-SET
```

On hardware the access always finishes - `ACCESS` is the busy bit, and `ST` is
just the copy of the frame's start-of-frame field - so `ACCESS` clears and the
poll ends on the first read.  The model now runs any write that sets `ACCESS`:

- `ST` set (clause 22): decoded as before - switch page/data registers at PHY
  31, PHY register reads/writes at 0..4.
- `ST` clear (clause 45 or any other frame): the frame completes and `ACCESS`
  clears; nothing on this bus speaks clause 45, so a read returns `0xffff`,
  exactly like an unpopulated address.

That removes ~2400 poll iterations per boot on the die image, and with it a
source of host-speed-dependent behaviour.  It is also the same bit the driver
polls before *starting* a transaction, so an `ACCESS` left set by a rejected
write stalls the *next* transfer, which is one plausible source of the
intermittent `raeth: mdio timeout` bursts.

### 7.3 What the PHY model still says about link

`up = phy && phy == s->eth_port`: only the switch port wired to the host backend
reports a cable, and PHY address 0 (the WAN PHY) can never report one even when
`MT7621_ETH_PORT=0` asks for it.  The first half is the honest consequence of
modelling a single net backend; the second half is a wart, because address 0 is
a real PHY on this board.  It is left alone for now because RouterBOOT's
etherboot and the netinstall flow probe specific ports to decide whether a cable
is present, so changing it changes which port a booter will use.

### 7.4 Next gap: the SSUSB block

With MDIO quiet, the boot continues into the kernel's own devices and stops at:

```
xhci-mtk 1e1c0000.xhci: clocks are not stable (0x0)
xhci-mtk: probe of 1e1c0000.xhci failed with error -145
```

`xhci-mtk` powers up the ports and then polls `ip_pw_sts1` for
`STS1_SYSPLL_STABLE | STS1_REF_RST | STS1_SYS125_RST | STS1_XHCI_RST`.  The IPPC
block is not modelled at all, so the read returns 0 and the probe fails after a
20 ms timeout.  Per the MT7621 device tree the SSUSB MAC is at `0x1e1c0000` and
its IPPC registers at `0x1e1d0700` (256 bytes: control `0x00..0x0c`, status
`0x10`/`0x14`, `ip_xhci_cap` at `0x24`, four U3 port registers from `0x30`, five
U2 port registers from `0x50`, `u2_phy_pll` at `0x7c`).  A faithful model needs
both halves - the status bits alone move the failure to `xhci_init`, which then
needs a plausible capability register file and a reset/run sequence - so this is
a module of its own, not a one-line stub.

## 8. The board's own device tree, and why the SSUSB block cannot be found from it

Everything above was derived from drivers and from what the guest prints.  The
die image lets us do better: RouterOS's kernel carries its **own flattened
device tree**, so the model can be checked against the tree the firmware
actually boots with instead of against a kernel source tree.

### 8.1 Extracting it from the running guest

The blob is in RAM, so the monitor can hand it over (path quoted or the monitor
tries to parse it as an expression):

```
(qemu) pmemsave 0 0x10000000 "/tmp/ni/ram256.bin"
```

Only one FDT magic is present in 256 MiB of RAM - at physical `0x4cc640`,
9291 bytes, i.e. inside the kernel image the loader placed at `0x80000000`
(physical `0`).  `dtc -I dtb -O dts` then prints the tree.

What it says about the blocks this model implements:

| node | reg | note |
|---|---|---|
| `palmbus@1e000000` | `ranges = <0 0x1e000000 0xfffff>` | children are offsets: `spi@b00`, `nand@3000`, ... |
| `crypto@1e004000` | `0x1e004000 0x1000` | the model maps `0x800` of it |
| `xhci@1e1c0000` | `0x1e1c0000 0x1000`, `0x1e1d0700 0x100` | `reg-names = "mac", "ippc"`; `status = "disabled"` |
| `interrupt-controller@1fbc0000` | `0x1fbc0000 0x80` | GIC base agrees; the DT size is smaller than the block |
| `ethernet@1e100000` | `0x1e100000 0x10000` | frame engine |
| `pcie@1e140000` | ranges to `0x60000000` | |

So for the GIC, crypto, frame engine, NAND and SPI the model's addresses match
the firmware's own tree, which is a much stronger statement than matching a
mainline DTS.

### 8.2 The USB puzzle: `disabled`, yet probed, and not at the DT address

Two observations that do not fit together:

1. The tree marks `xhci@1e1c0000` as **`status = "disabled"`**, so a
   device-tree-driven probe should never happen - yet RouterOS's kernel probes
   `xhci-mtk 1e1c0000.xhci` on every boot.
2. A switch-side model of both blocks (`modules/usb.c`, MAC at `0x1e1c0000`,
   IPPC at `0x1e1d0700`, with `MT7621_USBLOG=1` to name every access) records
   **no access at all** to either address, while `MT7621_PERIPHSPY=1` sees no
   unassigned access anywhere in `0x1e100000..0x1e200000` either.

The probe does run and does read a status register, because it reports a value
that changes from run to run:

```
xhci-mtk 1e1c0000.xhci: clocks are not stable (0x1600003d)
xhci-mtk 1e1c0000.xhci: clocks are not stable (0x24170002)
xhci-mtk 1e1c0000.xhci: clocks are not stable (0x1dc04)
```

Values that differ every boot are what uninitialised DRAM or an unrelated
register file full of DMA pointers looks like - not a modelled status register.
So RouterOS's kernel registers this platform device **itself** (the DT node is
disabled), with resource addresses that are not the ones in the tree, and the
`1e1c0000` in the device name is only the first resource's start.

That makes the SSUSB module a model of a block the firmware believes is
somewhere else: the register file in `modules/usb.c` is written from the
datasheet/spec side (xHCI capability + operational registers, IPPC status bits,
Supported Protocol extended capabilities) and is correct *if* the guest ever
addresses it, but it cannot fix the probe until the address the firmware
actually uses is known.  The instrument for that is the pair of knobs above
(`MT7621_USBLOG` for the model's own blocks, `MT7621_PERIPHSPY` for everything
unassigned in the peripheral window) plus QEMU's trace points, which name the
region and offset of every other access:

```
qemu ... -trace enable=memory_region_ops_read \
              -trace enable=memory_region_ops_write \
              -trace file=/dev/stdout | <filter bursts>
```

because the failing check is a *poll*, i.e. a long burst of identical reads of
one register, which is easy to spot and easy to attribute to a region.

### 8.3 Where the die image stands after the MDIO fix

Four back-to-back runs of the die dump (`MT7621_NAND_LAYOUT=raw`, no other
knobs), counted from the serial log:

| run | mdio timeouts | yaffs retirements | panic | last line |
|---|---|---|---|---|
| 1 | 0 | 0 | 0 | `probe_er_radio_data` @ 78.8 s |
| 2 | 0 | 0 | 0 | `xhci-mtk: probe ... failed` @ 77.8 s |
| 3 | 0 | 0 | 0 | `probe_er_radio_data` @ 77.3 s |
| 4 (with monitor) | 0 | 0 | **1** | `sstore` oops @ 53.0 s, then a reset |

The first three are the state to work from: Breed -> loader (real OOB) ->
kernel -> NFI -> 3 MTD partitions -> yaffs (clean) -> userspace -> switch ->
`rb_ioctl`/`probe_er_radio_data`, with no storage or MDIO complaint anywhere.

The fourth run is the open item: one boot in four dies in RouterOS's `sstore`
("settings store", PID 79) with a **NULL ops pointer**, and the panic then
resets the machine into the booter (whose `flash_ioctl`/`spi_program_sector`
messages appear afterwards without kernel timestamps, and the CPU is left
executing in the `0xbfc0_0000` boot window).

The fault is a three-level ops indirection, which is why it is worth naming:

```
$4 (a0) = 8e1d0840          ; some driver object
0x80266e64  lw  a1, 0x90(a0)   ; a1 = 8f8dc964
0x80266e68  lw  v0, 0x14(a1)   ; v0 = NULL
0x80266e6c  lw  t9, 0x20(v0)   ; -> fault at 0x20
0x80266e78  jr  t9
```

and the caller (`ra = 0x8010be2c`) uses the same `+0x14` ops pointer with
`+0x24` for its own op, i.e. the guest called a driver-core wrapper whose
private object exists but whose ops table was never installed.  That is the
signature of a *probe that has not finished* while `sstore` already talks to
the device, not of a bad register value - `sstore` runs from `/init` while
RouterOS is still loading its out-of-tree modules (`mt7621_nfc` is one of
them), and this model's NAND scan is slow enough (multiple seconds) that the
interleaving differs from the board.

So this is a window in which the guest's own bring-up order matters, not a
wrong register value.

Is it concurrency?  Measured, same image and binary:

| accelerator | runs | panics |
|---|---|---|
| default (multi-threaded TCG, 4 vCPUs) | 4 | 1 |
| `-accel tcg,thread=single` | 4 | 0 |

Serialising the vCPUs made it disappear in four runs, so the panic is a
concurrency effect rather than a deterministic data error.  It is not yet
attributed: a data race inside a shared guest object and a race the *model*
introduces (two vCPUs driving one controller, or one vCPU polling a register
while another owns the transaction) look the same from the serial log.

What the timing shows is that the crash window is RouterOS's settings store
writing to flash, not a random driver:

```
[52.456] new-flash: starting...
[52.458] flash: ptr = (ptrval), base = 1fc00000
[52.478] board name = RouterBOARD 750G r3
[53.338] mt7621-nand 1e003000.nand: wr page 9976 ecc data oob=1 steps=4 ...
[53.4..] ... wr page 9977 .. 9983
```

the panicking run dies at 52.98 s, i.e. as `new-flash`/`sstore` start using the
MTD device, and the first thing they do is program the last pages of block 155.
That puts the NFI program/read round trip - the newest and most stateful part
of the model - at the centre of the investigation, not the Ethernet or USB
code.

Next steps, in order: repeat the MTTCG run enough times to get a rate, then
make the NFI model's transaction state per-context-safe / explicitly locked and
re-measure; if it survives MTTCG, speed the page-read path up (the bad-block
scan and the yaffs sort dominate the boot) so the guest's bring-up order stops
differing from the board's.

## 9a. The die dump's firmware slot predates the board patch

Section 9 ended with the booter looping on a fault.  That fault is real, and it
is in the *image*, not in the model:

- the function is the etherboot timeout loop (`0x81108c58`), whose prologue
  sets `$fp = 0x81110000`;
- the loop's constant load must therefore be `lw v1,-0x70ec($fp)` - which is
  exactly what the other booter build we have (`/tmp/opencode/booter_ram.bin`,
  dumped from a previous session) decodes at the same offset;
- the die dump's booter decodes `lb v1,-0x70ec($t6)` instead, and `$t6` is a
  caller-saved register that the preceding `jalr`s legitimately clobber, so it
  is zero: `BadVAddr = 0xffff8f14`, TLB load fault, and the vector at
  `0x80000180` is `bovc sp,sp,...; jr ra`, which returns to the faulting
  instruction.  Infinite loop, no message.

Proof that this is the image and not the machine: overlay the *known-good*
patched package (`~/tmp/rb-fw-routerboot-full.bin`, sha256 `c74e544f...`,
917504 B for flash `0x140000`-`0x220000`) on the die dump and boot the same
emulator binary with the same command line.  Etherboot then runs:

```
Ethernet link absent...
trying bootp protocol... ... OK
Got IP address: 192.168.33.3
resolved mac address 76:19:B0:2B:86:06
******...            <- TFTP blocks from the host netinstall
```

So the die dump's `0x140000` slot contains an older 3040-byte loader whose
booter image is the pre-patch one; the real board has the package.  The model
needs nothing for this.

Two model fixes were still needed to get that far, both hardware facts:

1. **Autoneg deadline from power-on, not from the first poll** - the PHY used
   to report link-down on the very first BMSR read and only start its 300 ms
   settle then, and RouterBOOT treats that first read as "retry".
2. **The RouterBoot area is mirrored to `0x80e00000`** at load (from the
   board's own `mtd2`, `0x1e0000`, 256 KiB).  Its 0x28-byte header is the
   board descriptor table the booter parses (`+0x14` = config TLV offset,
   `+0x18..+0x24` = geometry windows) and the TLVs follow ("Hard" at
   `+0xf000`).  ROUTERBOOT-NOTES sec.3/sec.15/sec.16 require this; the model
   logs the readback so a wrong offset is visible immediately.

Also confirmed from the booter's own code: its MDIO helper at `0x81110854`
builds the IAC word as `reg<<25 | addr<<18 | ...` with `2<<18` = READ, and the
etherboot path calls it with **PHY 1**, so the default cable must stay on a LAN
PHY (`MT7621_ETH_PORT=1`), not the WAN.

## 9. Etherboot (Ctrl+E) on the die image: link now works, the booter then faults

The netinstall path is RouterBOOT's own: Ctrl+E -> eth init -> BOOTP -> TFTP.
With a host tap attached the booter printed its banner, waited for the key, and
then hung with no further output.  Three separate things were measured.

### 9.1 The wait was a PHY that could never have a cable

A driver trace of the IAC transactions (`MT7621_MDIOLOG=1`) ends with:

```
mdio: w#616 0209ffff phy=00 reg=01 rd  access=0 start=1
mdio: w#617 8209ffff phy=00 reg=01 rd  access=1 start=1   <- BMSR of PHY 0
```

RouterBOOT polls link at **PHY address 0**, the WAN.  The model reported link
only where `phy && phy == MT7621_ETH_PORT`, so address 0 could never have a
cable no matter how the backend was configured.  Fixed: the cable follows
`MT7621_ETH_PORT` for every address, and the `lg-gapm7200` preset now defaults
that to **0**, because the WAN socket is the port a netinstall cable goes into
and the port the booter looks at.  After the fix the booter's own register $s0
holds the BMSR value `0x7965` (link up, autoneg complete), so it does see the
cable.

### 9.2 But it still sends no frame: it takes a TLB fault first

With link up, the booter enters a timer loop and never transmits (`0` frames
with `MT7621_ETHLOG=1`).  QEMU's own exception trace (`-d int`) names the fault:

```
mips_cpu_do_interrupt enter: PC 81108d20 EPC 81108d20 TLB load exception
    S 10000002 C 40000008 A ffff8f14 D 00000000
```

i.e. `BadVAddr = 0xffff8f14`, from the instruction at `0x81108d20`:

```
0x81108d10: lui  a0,0x8110
0x81108d14: lw   v1,0x10(sp)     ; start of the delay window
0x81108d18: lw   a0,0x19ac(a0)
0x81108d1c: subu v0,v0,v1       ; elapsed
0x81108d20: lb   v1,-28908(t6)  ; t6 = 0 -> 0xffff8f14
0x81108d24: mul  v1,v1,a0
0x81108d28: sltu v0,v1,v0
0x81108d2c: beqz v0,0x81108d58   ; timeout exit
...
0x81108d50: jalr s5             ; reporting call
```

The exception vector is not initialised in this image (`0x80000180` holds
`bovc sp,sp,...; jr ra`), so the fault returns straight to the faulting
instruction and the machine loops for ever - no message, no reset, exactly the
"apparent infinite loop".

Two facts narrow the cause:

* `-smp`-independent: this is not the MTTCG race of 8.3, it is deterministic
  (same PC, same BadVAddr in every run).
* The **same instruction differs between the two booters we have**: the die
  image decompresses `lb v1,-0x70ec(t6)` there, while the earlier relocated
  build (`/tmp/opencode/rb-full-reloc-trace-df.bin`, dumped from RAM in an older
  session) has `lw v1,-0x70ec(fp)` in the identical location - a word load from
  the booter's own data, which cannot fault.  The two booters are otherwise
  different builds (2.6 KiB of differing bytes in 256 KiB, including the entry
  stub and a 50 MHz constant), so this is either a different build that expects
  a static base register in `$t6`, or a damaged/patch-variant word in the die
  image.

Since the die image is the source of truth, the next step is to diff its
decompressed booter against a stock RouterBOOT 7.17 for MT7621 rather than to
teach the model anything: a `lb` through a zero base cannot work on silicon
either.  Until then, etherboot from this image is expected to loop.

New knob from this work: `MT7621_TLBLOG_MIN=<pc>` restricts the TLB-miss log to
faults at or above an address (`0x81000000` keeps only the booter's), because
the default sampler is consumed by user-space noise long before a booter fault.

## 10. Matching the model to the board's own register dump (etherboot)

The board's first working etherboot produced a register dump from the booter
itself (the patched build prints `M…` / `G…` lines right before BOOTP).  Diffing
that against the model at the same point is the most direct fidelity test there
is, and it found four differences - all fixed, all board-scoped:

| register | board (working) | model before | model now |
|---|---|---|---|
| `0x1e000010` SYS_SYSCFG0 | `000232c1` | `00000101` | `000232c1` (strap: CHIP_MODE + DDR + 40 MHz xtal in 8:6) |
| `0x1e000058` | `0a210a01` | `00000000` | `0a210a01` |
| `0x1e100a08` PDMA_RST_IDX | `00000000` | `00010000` | self-clearing pulse |
| `0x1e110004` IAC | `0000796d` (data only) | `02097941` (request bits kept) | `0000 <data>` |
| PHY BMSR | `796d` with cable | `7965` / `7941` (bit 3 missing) | `796d` / `7949` |

`MT7621_SYSCFG0` / `MT7621_SYSCTL58` override the straps for A/B.  Normal boot
is unaffected: the die dump still reaches userspace, and with the cable on the
WAN PHY the kernel reports `eth0: phy link up (1000/full)`.

The lg board's default backend port is now **0 (WAN)**: the netinstall cable
goes there on the real board, and that is the PHY RouterBOOT's etherboot reads
(its dump shows the link-up BMSR on that read).

Still different after the fixes (worth chasing next, same method): the booter
programs `PDMA_GLO_CFG = 0x455` (i.e. `MTK_MULTI_EN`, bit 10) on the board and
`0x45` here, reads `0x1e110000` as `45000504` there and zero here, and the
switch matrices read back `000000ff` / `000000ff` on the board versus
`0000003f` / `00000040` in the model - i.e. the booter builds a slightly
different descriptor set, which is the next thing to align.

### 10.1 Why "Ctrl+E hangs" is not a model bug

A gdbstub breakpoint on the booter's branch (`0x81108cc0`) shows the guard is
**unconditional**: `$s0` is zeroed two instructions earlier (`move s0,zero` in
the `jalr` delay slot), so `and s0,s0,s1` is always 0 and `bne s0,s1` is always
taken.  There is no link- or register-dependent escape from that path, so the
instruction that faults (`lb v1,-0x70ec($t6)`, see sec.9) executes on every
etherboot entry.

In the flashed image that instruction is `lw v1,-0x70ec($fp)`; our reloc loader
rewrites it because its "literal address word" rule matches words whose top 18
bits are `0x8FC00` (see sec.9a).  A board running the same decoded image would
fault identically, so the board must be running a decoded image that differs -
the fix is in the loader's relocation rule, not in the machine model.

## 11. Warm reset: re-enter the bootloader, not the DDR-init stage

Symptom: `system_reset` (HMP; note `reset` is not a command, the monitor
answers `unknown command: 'reset'`) restarts the CPU at `0xb9000000`, the
BootROM stage-1 runs, and then the guest never prints again.  The PC trace
shows the stage returning into `0x8900xxxx`, calling back to `b9000238` and
ending in a loop at `0x00000d10`; Breed's banner never returns and the guest
that answers afterwards is the *previous* instance still in RAM.

Cause: on a warm reset the stage-1 code sees the state the previous boot left
in its SRAM window and takes a different hand-over path than on a cold boot
(cold: `b9002148 -> 89001834`; warm: `b9002148 -> 890019d8 -> b9000238 ->
0xd10`).  The model has no DDR to train, so re-running the stage only replays
that stale hand-over.

Fix: `mt7621_reset()` counts resets and sets `s->warm_boot` for every reset
after the first; `loader.c` then takes the existing "step 2" path for uImage
images (`bl_off` present): load the bootloader from the image at its load
address and enter its entry point, exactly the state a cold boot hands over.
`MT7621_STAGE1_WARM=1` forces the old behaviour for A/B.

Verified with `breed-gapm7200-en.bin` (two monitor resets in a row: full
banner each time, shell answers `help` after the last one) and with the
byte-for-byte die image (Breed banner + `RouterBOOT booter 7.17` twice), plus
Breed's own `reset` command.

## 12. Probe-time RX interrupts: `ifconfig` right after `insmod raether.ko`

Symptom (manual module path, traffic on the wire):

```
insmod raether.ko; ifconfig eth0 192.168.1.100 up
CPU 0 Unable to handle kernel paging request ..., epc == 0, ra == 80286e44
Kernel panic - not syncing: Fatal exception in interrupt
```

`ra` is `napi_poll()`'s `n->poll(n, weight)` and the NAPI has `poll == NULL`:
the probe programs the rings and enables RX DMA with `INT_MASK` clear, frames
arrive, `RX_DONE` is latched, and the first unmask (in `ndo_open`, before
`netif_napi_add`) raises the line.  Waiting 25 s before `ifconfig` avoids it.

Reproduced deterministically with a 300 ms broadcast on the model's peer
(no netinstall needed).  Fix: `RX_DONE` is latched only while the driver has
`RX_DONE` enabled — the real board does not see those frames because the
switch does not forward to the CPU port until the port is up, and this model
has no switch to enforce that.  `TX_DONE` must keep latching regardless of
the mask: that is what fixes the separate `in_transmit:101` queue stall.
`MT7621_MASKED_STATUS=1` restores mask-gated latching for both.

Verified: immediate `ifconfig` with traffic -> `eth0: phy link up`, no panic;
`sleep 25` then `ifconfig` -> link up.

Still open in this area: `eth0..eth4: Caught tx_queue_len zero misconfig` and
`soft lockup ... [ww2]` come from per-port state the switch driver builds from
MT7530 registers that the model leaves at 0 (`mt7621_switch_defaults()` only
presets `CREV`/`HWTRAP`).

## 13. QDMA TX completion: publish to DRX (0x1b14), not only DTX (0x1b04)

Symptom (mainline `mtk_soc_eth`, OpenWrt 6.6):

```
mtk_soc_eth 1e100000.ethernet eth0: NETDEV WATCHDOG: CPU: 0: transmit queue 3 timed out 5620 ms
```

repeating every ~5 s, while frames do leave the port.  The driver reclaims
from the QDMA **DRX** pointer and requires the descriptor's owner bit:

```
	dma = mtk_r32(eth, reg_map->qdma.drx_ptr);      /* MTK_QTX_DRX_PTR 0x1b14 */
	while ((cpu != dma) && budget) {
		desc = ...desc->txd2...;
		if ((desc->txd3 & TX_DMA_OWNER_CPU) == 0) break;
	}
```

The model published DMA progress only to DTX (`0x1b04`, which is the DMA's
own index), so `drx_ptr` never moved and no buffer was reclaimed.  Publish to
both (DTX/DRX for context 0, `0x1b24`/`0x1b34` for the second context the
driver programs for GMAC2).  Verified: 7 timeouts in 65 s before, 0 after,
userspace reached; the die image still boots.

The second QDMA TX context (`0x1b20` CTX / `0x1b24` DTX, GMAC2) is now
modelled explicitly instead of being stored as an inert register.

## 14. The 1004Kc is 2 cores x 2 VPs - and four registers have to say so

Symptom: `VPE topology {2,2} total 4` was eventually printed, but the kernel
still brought up one CPU (`smp: Brought up 1 node, 1 CPU`), or - with the
topology fixed but the VPC mapping wrong - two (the boot core's VP0/VP1) while
both VPs of core 1 stayed offline.

The 1004Kc core implements **two VPEs with one TC each**, and Linux (`CONFIG_MIPS_MT_SMP=y`,
`CONFIG_MIPS_CPS=y`) builds both its CPU topology and its secondary bring-up
from that.  Four independent register values matter:

1. **`CPC_Cx_CONFIG` at `0xc010`** (the CM >= 3.5 path, not the earlier
   `GCR_Cx_CONFIG` at `GCR_CL_OTHER + 0`): `mips_cps_numvps()` returns
   `PVPE + 1`, and this kernel reads only the CPC alias.  Returning 0 there
   made a 2x2 part look like four single-VP cores (`{1,1,1,1}`).
   `GCR_CONFIG.PCORES` must be "cores minus one" (1), not "VPs minus one".

2. **`MVPConf0.PVPE`** (bits 14:10) is "VPEs per core minus one" and must be 1.
   QEMU's `mvp_init()` leaves 0 (`TODO: actually do 2 VPEs`), which
   * cps-vec's `mips_cps_get_bootcfg()` uses to mask `EBase.CPUNum` into a
     VP id (mask 0 -> every VP computes VP0), and
   * `mips_cps_core_init()` uses to decide whether to loop over the core's
     VPEs at all ("If there's only 1, we're done"), so the second VP was
     never bound (`mttc0 TCBIND`) or halted (`mttc0 TCHALT`), and
     `mips_cps_boot_vpes()` then skipped it ("Skip the VPE if its TC is not
     halted").

3. **`GCR_CL_ID` (GCR+0x2028) is the *core* id**, not the VPE/cpu index:
   `mips_cps_get_bootcfg()` indexes `mips_cps_core_bootcfg[core]` with it.
   Returning the VPE index made the second VP of a core (and both VPs of
   core 1) read past the end of that array.

4. **`GCR_CL_COHERENCE` (GCR+0x2008) is per core.**  With one shared cell,
   core 1 read the `0xff` the boot core had already written and skipped
   `mips_cps_core_init()` entirely.  The model now keeps one byte per core
   (`cm_coherent[]`), so a freshly released core starts non-coherent, runs
   the cache/coherence setup and initialises its VPEs.

Reset state and MT addressing complete the picture:

* VPE0 of each core is the master VPE (`VPEConf0.MVP`); the other VPs come
  out of reset halted and inactive (`TCHalt=1`, `TCStatus.A=0`, `VPA=0`).
  QEMU's own MT reset block is skipped because it runs before
  `mt7621_cpu_hw_config()` sets `Config3.MT`, so the model sets that state
  itself, on every CPU reset.

* `VPECONTROL.TARGTC` (bits 7:0) is a **TC number inside the processor
  (core)**, which is what cps-vec's `mttc0` sequences rely on.  QEMU's
  `mips_cpu_map_tc()` treated it as a global thread index
  (`vpe_idx = tc / nr_threads`), which only coincides on a single-core MT
  part: from cpu 2 it addressed cpu 0/1 instead of the other VP of core 1.
  The machine now sets `CPUState.nr_threads` to the VPs per core, and
  `mips_cpu_map_tc()` maps a core-local TC to `core * nr_threads + tc`
  (each VPE has one TC here, so the local TC index is 0).

* The translation-time gate in `gen_mftr()`/`gen_mttr()` rejected the whole
  sequence: it NOPs when `TARGTC > MVPConf0.PTC`, and on a 1004K `PTC` is 0
  (one TC per VPE) while `TARGTC` is 1 for the second VP of the core.  The
  gate now asks whether the TC exists in the processor (`TARGTC < VPs per
  core`) and whether the target is another VPE of this core, resolving the
  target VCPU the same way as the runtime mapping.  (The helpers still
  resolve `TARGTC` from the live register, so a translation that saw a stale
  value cannot misdirect the access.)

* `dvpe`/`evpe` are per processor: they must only affect the other VPEs of
  the executing core.  QEMU's helpers iterated over every CPU, so releasing
  core 1's VPs stopped the running boot core.

Result: `CPU0..CPU3 revision is: 0001992f (MIPS 1004Kc)`,
`VPE topology {2,2} total 4`, `smp: Brought up 1 node, 4 CPUs` - the
board's own topology.  Verified over repeated boots (no soft lockups, no
oops) and across a warm reset; `MT7621_VPS=1` restores the old flat
`{1,1,1,1}` view and `MT7621_NO_MT=1` the "two cores, one VP each" one.

## 15. CP0 compare: it is GIC local interrupt 2, not CPU interrupt pin 7

Symptom: with all four CPUs up, OpenWrt 6.6 printed `irq 7: nobody cared` on
the secondary CPUs (`handlers:` empty) and then disabled IRQ 7.  On the real
LG-7200 with the same image there is **no irq 7 activity at all**:
`/proc/interrupts` lists only GIC lines (`8: ... MIPS GIC Local 1 timer`, the
IPIs, and the peripherals), no `7:` entry, no "spurious interrupt", no
"nobody cared".  Both run the same `sync-r4k.c` path, so the compare is armed
on both (`Synchronize counters for CPU 1..3: done.`).

The difference is where the interrupt goes.  On a core covered by a
Coherence Manager the CP0 count/compare interrupt is the GIC's **VP-local
interrupt `GIC_LOCAL_INT_TIMER` (local 2)**, not the core's IP7 pin:

* `arch/mips/include/asm/mips-gic.h`: `GIC_LOCAL_INT_COMPARE` (1) is the
  GIC's own count/compare, `GIC_LOCAL_INT_TIMER` (2) is documented as the
  "CP0 count/compare interrupt".
* `gic_get_c0_compare_int()` maps the CP0 timer that way when the local
  interrupt is routable; a guest enables it by setting the VP mask bit and
  writing `GIC_VPE_TIMER_MAP` (`gic_all_vpes_irq_cpu_online()` writes
  `BIT(GIC_LOCAL_INT_TIMER)` to `GIC_VPE_SMASK` and the map register).
* On this image nothing ever enables local 2: `r4k_clockevent_init()` returns
  `-ENXIO` because `mips_hpt_frequency` is never set on ramips/MIPS_GIC
  (`arch/mips/ralink/timer-gic.c` only runs `of_clk_init()`/`timer_probe()`),
  so the GIC clockevent (local 1) is the only timer.  The compare armed by
  sync-r4k therefore latches in `GIC_VP_PEND` and is dropped by the GIC mask -
  exactly what the board shows.  IP7 stays free for the GIC's shared
  interrupts (the board DTB reserves it: `mti,reserved-cpu-vectors = <7>`).

The model raised `Cause.IP7` directly from `cpu_mips_timer_expire()`, which is
the pre-CM wiring, and `handle_percpu_irq()`'s ack/EOI pair (clear `IM7`,
then `unmask_mips_irq`) re-enabled it while `Cause.TI` stayed latched: 100k
deliveries later, `note_interrupt()` printed `irq 7: nobody cared`.

Fixed by moving the delivery to the GIC:

* `cpu_mips_timer_irq()` is a weak core hook carrying the old IP7 behaviour;
  the mt7621 CM module overrides it and calls `mips_gic_local_irq(...,
  GIC_LOCAL_INT_TIMER, level)`.
* The GIC model now implements local 2 (pending bit, `GIC_VP_MASK` bit,
  `GIC_VPE_TIMER_MAP`) and ORs it into the CPU pin evaluation together with
  shared interrupts and local 1.  Masked, nothing is delivered - the board's
  behaviour; enabled, it arrives on whichever pin the guest mapped.

Result: OpenWrt boots cleanly (`Brought up 1 node, 4 CPUs`, no irq 7, no
"Disabling IRQ"), `/proc/cpuinfo` matches the board (`core 0/0/1/1`,
`VPE 0/1/0/1`, `ASEs: mips16 dsp mt`), and `/proc/interrupts` matches it too
(only GIC lines, `Local 1 timer` per CPU).  Verified over repeated boots, a
70 s run and a warm reset.

The same probe explains the CP0 compare write race the model compensates
for: a guest arms Compare from a Count it has just read, and host call
latency can make a small delta (`read_c0_count() + 16`) arrive already behind
the count.  The wrap-around interpretation then schedules the compare 2^32
ticks (~4.9 s) later, which is what `c0_compare_int_usable()`'s 50-tick
window rejects.  `cpu_mips_store_compare()` now distinguishes a fresh arm
(`Cause.TI` clear) from an acknowledgment (`Cause.TI` set, e.g.
`c0_compare_interrupt()`'s re-write of the same value): a fresh arm at most
10 us behind expires immediately, an ack keeps the wrap semantics.
`MT7621_NO_CMP_CLAMP=1` restores the old behaviour for A/B.

## 16. The frame engine's owner changes at the hand-off (and GLO_CFG is not a plain register)

Symptom (netinstall agent, `/init.real` start):

```
[   15.07] CPU 0 Unable to handle kernel paging request at virtual address 00000000
           epc == 00000000, ra == 80286e44
           Kernel panic - not syncing: Fatal exception in interrupt
```

`ra` is inside `napi_poll()`, at the `work = n->poll(n, n->weight)` indirect
call, so a `napi_struct` had `->poll == NULL`.  Two model defects combined:

1. **`ETH_PDMA_GLO_CFG` was captured by an early store-and-return** in
   `mt7621_eth_write()`, before the handler that implements its semantics.
   That handler starts the engines on the `TX_EN` 0->1 edge (kicking every
   ring) and tracks `RX_EN`; with it unreachable, descriptors queued while
   the driver had the engine off never got their edge kick (per-ring CTX
   writes normally cover that, but not the first batch), and the RX side had
   no notion of which engine generation owned the ring.

2. **Frames were delivered into the bootloader's ring after the hand-off.**
   RouterBOOT leaves `GLO_CFG = 0x45` (RX_EN set) and a live ring at
   `0x0fddcd60`; the kernel reuses that RAM long before its driver probes the
   MAC.  From the moment the kernel starts, every broadcast the installer
   sends (PING/OFFR, every few seconds) was DMA-written into freed kernel
   memory for ~15 s.  When `/init.real` brought the interface up and the first
   RX interrupt ran `net_rx_action`, the corrupted `napi_struct` was called
   through a NULL `->poll`.  A real part does not do this: the bootloader's
   DMA configuration dies with the hand-off.

Fix:

* `PDMA_GLO_CFG` is handled only by the full handler (no early store).  Its
  `TX_EN` 0->1 edge kicks all four rings, which is what starts the first
  batch the driver queued with the engine off.
* The RX ring base the bootloader armed is remembered whenever non-kernel code
  programs it.  Ownership is then checked per delivered frame against who is
  running on the boot CPU, and the discriminator is the **kernel text window**
  (`0x80000000..0x801fffff`), not a "bootloader range": RouterBOOT runs both
  at `0xa0201000` and from a relocated KSEG0 copy around `0x80e00000`, while
  Breed (`0x8ff...`) and the stage-2 loader (`0x81000000`) are elsewhere.  A
  range test for bootloader addresses therefore refused RouterBOOT's own
  BOOTP/TFTP replies and the board never fetched the agent; while the
  bootloader is not in the kernel window its ring stays live, and once the
  kernel's text runs, frames for the bootloader's ring are refused until the
  driver programs its own base (`RX_2B_OFFSET` set).  A timer-polled
  "hand-off" flag was tried first and is not sufficient either: RouterOS's
  boot path never trips that poll.  The per-frame check needs no timing
  assumptions.

A third, independent defect produced the same crash *before any frame
arrives* (confirmed by the user: "immediate, before broadcast and offer
begins").  The bootloader leaves the engine's latched interrupt status set
(the model's `TX_DONE` latch is deliberately mask-independent, and a final
bootloader RX can remain pending), and `raether` enables `INT_MASK`
(`0x3000f`) *before* `netif_napi_add()` and before `request_irq` unmasks the
line.  A stale `RX_DONE0` therefore fires the instant the mask is enabled,
`napi_schedule` runs on a zeroed `napi_struct`, and `net_rx_action` calls a
NULL `->poll`.  The engine's status belongs to the firmware generation: the
first 0 -> non-zero `INT_MASK` write (and a MAC reset, `GLO_CFG` with both
engines off) discards it, logging

```
mt7621: eth: discarded firmware interrupt status 00010000 at driver unmask
```

Verified on the OpenWrt image with a socket netdev: the bootloader's 42-byte
ARP frames are delivered into its ring, the stale `RX_DONE0` is discarded at
the driver's first unmask, frames for the old ring are refused between the
hand-off and the driver probe, and delivery resumes into the kernel's own
ring (`base=02da4000 cnt=512`, tagged, `linux=1`); the guest still boots with
`{2,2}` / four CPUs, no irq 7 activity and survives a warm reset.

## 17. A reset resets every controller, not just the CPU

The machine used to reset a handful of fields inline (`nfi_regs`, the SPI
register file, the ethernet engine, systick) and rely on the rest being
harmless.  That is how the model accumulated "which latch did *this* boot
leave behind?" bugs, one per report.  A real reset clears the whole SoC, so
`mt7621_reset()` now calls one complete reset per block and nothing else:

| block | reset | clears |
|---|---|---|
| SYSCTL | `mt7621_sysctl_reset` | whole 0x1e000000 window, straps, `SYS_RSTSTAT` latch |
| MEMC | `mt7621_dramc_reset` | PLL/DDR defaults (the trained values the BootROM leaves) |
| WDT | `mt7621_wdt_reset` | armed bark `QEMUTimer`, timer block, cause flag |
| CM/CPC | `mt7621_cm_reset` | `GCR_*`/`CPC_*`, per-core coherence, core-selector latches |
| NFI | `mt7621_nfi_reset` | register file, FIFOs, address stream, pending read, program buffer/FDM |
| SPI | `mt7621_spi_reset` | controller registers, RX FIFO, command/address latch |
| eth | `mt7621_eth_dev_reset` | PDMA/QDMA/QDMA ring indices, MT7530 file, PHYs, GIC line |
| xHCI/IPPC | `mt7621_usb_reset` | whole MAC + port-power window |
| EIP-93 | `mt7621_crypto_reset` | register file |
| systick | `mt7621_systick_reset` | registers, both timers, counter anchor, GIC line |
| L1D | `mt7621_dcache_reset` | tags/line geometry from the new `Config1` |
| bring-up | `mt7621_loader_reset` | hand-off latch, kernel-text polls, PC history, budgets |

Deliberately *not* reset: DRAM and its aliases (a warm reset keeps memory,
which the bootloader's warm path depends on), the NAND medium and SPI NOR
contents (flash is not erased by a reset), the board's strapping properties,
and the debug sinks.  Order matters only where a default consumes a latched
value: SYSCTL runs first because it folds a watchdog cause into `SYS_RSTSTAT`
before `mt7621_wdt_reset` drops the flag, and systick runs last because its
reset releases the GIC line that the BootROM hand-over then re-arms.

`MT7621_NAND_SNAP=<file>` writes the medium at the first *real* reset (not the
one QEMU performs while starting the machine), and `MT7621_TAGLOG_BOOT=<p>`
keeps one NFI read trace per boot in `<p>.0`, `<p>.1`, …; a `pageread:` line
carries the resolved row, offset, first word and address latch, which is what
made the truncation below visible.

## 18. Warm reboot loads a truncated kernel: the guest's yaffs GC erases it

Symptom (RouterOS installed on the die image): the first boot is fine, a
`/system reboot` after a few minutes prints

```
[NB] hdr=0000086a size=0020aa20 obj=00010100 seq=001011ff
[NB] chunks=00000415          <- one chunk short of need=1046
[NB] anchor=00000870
jumping to kernel code
```
and then nothing at all - no `Initial ramdisk`, no kernel banner.

It is not a reset leak.  A byte-for-byte snapshot of the medium at the reset
(`MT7621_NAND_SNAP`) is identical to the boot dump, and the per-boot read
trace differs only where the *guest* had erased pages: during boot 0 the
running system's yaffs garbage collector erases block 22 (`0x580-0x5bf`) and
block 36 (`0x900-0x92d`), writes 108 of the 110 live pages into block 37+
(`0x940-0x9ab`), and drops two pages: `0x5a6` (a yaffs metadata page of a
different object, `obj=0x100` - correctly dropped) and `0x92d`, the page our
loader maps to the last chunk of the kernel object (`obj=0x10100`,
`chunk=1046`, `seq=0x101500`, the file's partial 544-byte tail).

The cave reads raw tags and takes the highest `seq` per chunk; yaffs takes its
own liveness decision and, for that page, decides it is dead.  When the page
is erased, the loader has no copy to fall back to, so the object it assembles
is short by one chunk - and because the missing chunk sits at the end of the
ELF (section header table), the loader hands the stub a truncated image and
the boot dies before the console exists.

### 18.1 Root cause: the page is an artifact of our offline image builder

`0x92d` does not exist on the board.  Its own dump
(`~/Downloads/backup-flash-dump-lg7200.bin`, the raw capture) has that row
**erased** (data `ff…`, spare `ff…`); the board's `kernel` object is 1045
chunks (2138356 B) and ends at `0x92c`.  Our boot image has 1046 chunks
(2140704 B) because the console-patched kernel was injected offline with
`nandboot/rb-nand-kernel.py put`, which for a chunk that has no page yet
allocates one and then writes

    sp = bytearray(template)      # template = chunk 1's page (0x870), 64 B: tag *and* ECC
    pack seq/obj/chunk            # only these three fields are updated
    -> the page claims chunk 1046 but carries chunk 1's ECC bytes

Measured: across all 65536 pages there is exactly one pair with different data
but the same 32-byte ECC field, `0x870`/`0x92d`; the tag fields differ only in
`seq` and `chunk`.  Hardware ECC over different data cannot be equal, so those
bytes were written by software - and the writer was us.

The emulator never writes that page (per-run `nand-wr:` trace: programs at
`2350+`, `9976+`, `1024/1025/1056/1057`; none at `2349`), so the defect is
entirely in the image we start from.  yaffs sees a tag whose protection bytes
belong to another page, treats the chunk as dead, and its GC drops it; on real
hardware the same page would have been rejected at read time by the ECC engine
before any of this.

`rb-nand-kernel.py put` now **refuses** to allocate pages unless
`--allow-emulator-alloc` is given, and says why.  The clean way to grow the
object is to write it through yaffs (install it, or copy the file inside the
filesystem) so the controller synthesises tag + ECC.  The model's own ECC
engine is a stub (§18.2), so pages written *in* the emulator get `0xff` ECC -
fine for emulator-only images, invalid on flash.

### 18.2 The model cannot validate ECC either

Under this model that path is not exercised: the emulated NFI's ECC block is a
stub (`0x914` falls into the "no corrected/uncorrectable bits" branch, `0x90c`
reports idle, `0x918` reports every sector done).  A real BCH encoder/decoder
over `512+8` bytes with 8 parity bytes per sector (strength 4, 13 parity bits)
would be needed before QEMU can reproduce "this page's tag does not decode";
until then the only ECC-faithful test of either the writer or the loader is
hardware.  What the emulator *can* show - and did - is the guard: a short load
prints `chunks=N/need`, the missing chunk, `REFUSING: ELF tail missing`, and
the stock ` kernel not found or data is corrupted`, instead of hanging.

### 18.3 Loader side

`nandboot/nand.c` now reads every page the way yaffs does: decoder on, per-sector
`DECENUM` checked (0xf = uncorrectable), `DECEL` positions applied to data *and*
FDM, and a page that fails is invisible to the header scan, the chunk map and the
copy. On failure it prints the stock ` kernel not found or data is corrupted` and
returns -1 instead of jumping.

Under this model that path is **not exercised**: the emulated NFI's ECC block is
a stub (`0x914` falls into the "no corrected/uncorrectable bits" branch, `0x90c`
reports idle, `0x918` reports every sector done). A real BCH encoder/decoder over
`512+8` bytes with 8 parity bytes per sector (strength 4, 13 parity bits) would
be needed before QEMU can reproduce "this page's tag does not decode". Until
then, the only ECC-faithful test of the loader is hardware; QEMU proves the
no-regression path (the cave still finds all 1046 chunks when the medium has
them).
