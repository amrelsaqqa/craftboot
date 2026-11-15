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
Every modelled input matches:

| field | hardware | model |
|---|---|---|
| Clocks | CPU 880, DDR 1200, Bus 220, Ref 40 | same |
| DRAM | 256MB, `80000000-8fffffff` | same |
| Kernel | 248KB, `8ffc0000-8fffe25f` | same |
| Boot parameters | 256KB, `8fd7be3c-8fdbbe3b` | same |
| Managed memory | 2MB, `8fdbbf3c-8ffbbf3b` | same |
| Cached DMA line size (`0x8fffc624`) | 32 | 32 |

Only "Managed memory used" differs: 860.87KB on the device vs 410.89KB in the
model.  A heap dump at the shell shows why that says nothing about boot time:
by then the device's heap holds a DHCP exchange (`192.168.206.91 ->
192.168.1.1`) and the telnet/console buffers, which overwrite the region the
model still uses for its boot-time descriptor chain.  The device has no UART
header, so a serial-console `sysinfo` (no network traffic) is not available.

That leaves dynamic allocation *ordering* around the ring allocation as the
only unmeasured variable; every static input the model can expose is identical.

### Measured, 2026-11-15

A gdb probe of both Breed images on this machine (`full.bin`, r1286, and
mc-g500, r1280) shows the same allocator arithmetic:

| Dl | line Breed caches | first `malloc(0x400)` | second identical call |
|---|---|---|---|
| 2 | 8 | `n = 0x40f` -> `0x8fddd6f8` | `0x8fdddb10` (ok) |
| 4 | 32 | `n = 0x43f` -> `0x8fddd6f8` | `0` (NULL) |

`n = size - 1 + 2 * line` is what the wrapper requests; `malloc` is a
dlmalloc-style allocator that rounds chunks to 8 bytes and never returns a
pointer aligned to more than that. The clear is
`memset(round_up(p, line), 0, n)`: at line = 8 the pointer is already aligned
and the clear ends one byte short of the next chunk, at line = 32 it starts 8
bytes into the allocation and ends inside the next chunk's header, so the
second allocation fails. `tools/mipsel/mt7621/breed-dl-probe.sh` reproduces the
table.

**Open question.** The same dump boots at Dl = 4 on the real board, so the
hardware must hand Breed a DMA allocation that starts on a cache-line boundary
(or an allocator whose chunk layout absorbs the overrun). What the model
controls there is the heap base and nothing else; until that is measured on
hardware (the pointer returned by the first 0x400 DMA allocation, and
`Config1` as the device reports it), `board=lg-gapm7200` keeps reporting
`Dl = 2` and `MT7621_DL` overrides it for experiments. U-Boot and RouterBOOT
are unaffected: both boot this model at `Dl = 4`.

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
first 1 KiB buffer still lands at 0x8fddd6f8), so the ~0x9dc offset between the
model's and the device's arena pointer comes from another allocation, not the
SPI probe.

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
