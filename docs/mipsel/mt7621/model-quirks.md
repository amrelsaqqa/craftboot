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

The model therefore reports `Dl = 2` for this board only; the true value is
used everywhere else.  `MT7621_DL` overrides it for experiments.  This is a
guest bug, not a cache-coherency gap: no `cache` instruction executes, the
line size is only a number to the bootloader.

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

### What the board's own heap says (2026-11-15, after the dumps)

The board's chain at `Dl = 4` is identical to the model's: first DMA chunk
`0x8fddcd50` size `0x449`, DMA pointer `afddcd60`, then `0x8fddd198` (second
buffer), `0x8fddd5e0` (0x100 table), `0x8fddd6e8` (ring, size `0x18c69`).  So
the layout and the overrun are the same on both sides; there is no 24-byte
difference to find - the earlier theory was wrong.

What differs is the allocator's state.  Breed's allocator (TLSF, control block
at `0x8fff0000`, `sbrk` bookkeeping at `0x8fffcd30`) ends up with:

| | board | model |
|---|---|---|
| heap base / limit | `8fdbbf3c` / `8ffbbf3c` | same |
| `sbrk` cursor after boot | `8fe51000` | `8fdde000` |

and at the moment of the first DMA allocation the model's free block after the
buffer is `0x8fddd198` with size `0xe68` (it ends exactly at the cursor).  The
wrapper then clears `n = 0x43f` bytes from `round_up(p,32) = 0x8fddcd60`, i.e.
through `0x8fddd19e` - three bytes of the next chunk's size field.  A split
header `0x00000e69` becomes `0`: the allocator sees a zero-sized last block,
falls into its "extend the pool" path, and the extension (at the cursor, page
aligned, 0x1000) is not adjacent to that block, so it gives up and returns
NULL.  Verified by breakpoint: `extend` -> `sbrk(0x1000)` at `0x8fdde000` ->
`0xffffffff` -> `malloc` returns 0.  Writing `0xe69` back into
`0x8fddd19c` at that point makes the next `malloc` return `0x8fddd1a0`, the
board's exact second-buffer address.

So the remaining question is why the board's own clear does not cost it that
block.  Three bytes are always zeroed at `0x8fddd19c` when `n = 0x43f`, and a
chunk size below 16 MiB cannot survive that, so on the board that word must
not be a live chunk header at that moment - i.e. the board has already been
through an earlier Ethernet init attempt (its cursor is `0x73000` further
along than the model's) and re-enters with different heap state.  What is
missing to confirm and reproduce this is the board's boot log at `Dl = 4`
(the `Error:` lines and how many appear before it succeeds), not more dumps.

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
