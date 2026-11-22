/*
 * MediaTek MT7621 (MIPS 1004Kc) bring-up machine
 *
 * This machine models the SoC, not the firmware: DDR at 0, the palmbus
 * peripherals at 0x1e000000, and a boot path that either
 *
 *   - takes a raw image (load-addr/entry properties), or
 *   - takes a uImage (the format the MT7621 BootROM loads from NAND),
 *     strips the 64-byte header and loads the payload at the header's
 *     load address.
 *
 * The first target is the Breed bootloader taken from a parallel-NAND
 * dump, followed by OpenWrt.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "mt7621/mt7621.h"
#include "qapi/visitor.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "hw/boards.h"
#include "hw/char/serial-mm.h"
#include "hw/char/serial.h"
#include "hw/intc/mips_gic.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "hw/mips/mips.h"
#include "accel/tcg/cpu-ldst.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "net/net.h"
#include "qemu/main-loop.h"
#include "exec/icount.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
#include "qemu/log.h"
#include "exec/icache-shadow.h"
#include "hw/timer/mips_gictimer.h"
#include "qemu/timer.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/block-backend.h"
#include "target/mips/cpu-qom.h"
#include "target/mips/internal.h"


/* SoC memory map */
#define MT7621_RAM_BASE         0x00000000
#define MT7621_PERIPH_BASE      0x1e000000
#define MT7621_PERIPH_SIZE      0x00100000

#define MT7621_SYSCTL_OFF       0x00000000
/*
 * SPI NOR is memory-mapped by the BootROM at 0x1fc00000.  The MPU decodes
 * a 4 MiB window there, which is all the boot chain needs: RouterBOOT's
 * second stage lives at flash offset 0x10000.
 */
#define MT7621_UART0_OFF        0x00000c00

/* MT7621 NAND flash interface (register map matches Linux mtk_nand) */
#define MT7621_DRAMC_OFF        0x00005000

/*
 * Interrupt wiring taken from the DTB appended to the flash kernel:
 * a MIPS GIC at 0x1fbc0000, UART0 on shared interrupt 26 and the
 * "ralink,mt7621-systick" 50 kHz counter on shared interrupt 5.
 */
/*
 * 64 shared interrupts, not an arbitrary 256.  RouterBOOT validates the
 * GIC before it will use it: it reads GIC_SH_CONFIG and requires the
 * NUMINTRS field (bits 23:16, which QEMU derives as num_irq / 8 - 1) to be
 * exactly 7 -- i.e. 64 interrupts.  Our own devices only use IRQ 5 and 26,
 * and Linux sizes the GIC from this same field, so 64 is both the hardware
 * truth and strictly safer.
 */
#define MT7621_GIC_NUM_IRQ      64
#define MT7621_UART0_IRQ        26

/*
 * ralink,cevt-systick: a 16-bit up-counter clocked at 50 kHz.  Linux
 * programs COMPARE with "COUNT + delta" and expects an interrupt when
 * the counter reaches it.
 */
/* Every KSEG0/KSEG1 physical address the CPU can generate, for aliasing. */
#define MT7621_KSEG1_WINDOW     0x20000000
/*
 * Frame engine / switch register block.  The DT says 64 KiB but the MDIO
 * control register lives at 0x10004, so map 128 KiB.
 */

/*
 * MIPS Coherence Manager (CM) / Cluster Power Controller (CPC).
 * CPC sits at 0x1fbf0000, the global control registers (GCR) at 0x1fbf8000.
 */

#define MT7621_UART_CLOCK       50000000
#define MT7621_UART_BAUD_BASE   (MT7621_UART_CLOCK / 16)
#define MT7621_UART1_IRQ        27

/* sysctl register offsets we care about */
#define SYS_CHIP_NAME0          0x00
#define SYS_CHIP_NAME1          0x04
#define SYS_CHIP_REV_ID         0x0c
#define SYS_SYSCFG0             0x10
#define SYS_CLKCFG0             0x2c
#define SYS_RSTCTL              0x34
#define SYS_RSTSTAT             0x38
#define WDT_RST_CAUSE           (1u << 1)

/*
 * Ralink timer/watchdog block in the system controller's second 256 byte
 * page (1e000100).  Linux's mt7621_wdt drives timer 1: TMR1LOAD holds the
 * reload value in prescaler ticks, TMR1CTL bit 7 enables counting and bits
 * 16..31 are the prescaler in microseconds.  Writing TMRSTAT bit 9 reloads
 * (that is the "ping"); letting it run out resets the SoC.
 */
#define SYS_CUR_CLK_STS         0x44
#define SYS_GPIO_MODE           0x60



bool mt7621_trace;

/*
 * Boolean env knob.  These ML-style knobs used to be tested with a bare
 * getenv(), which is true for *any* value -- including "0" -- so a launcher
 * that always exports MT7621_FOO=0 silently turned the feature on.  Treat
 * the usual false spellings as off.
 */
bool mt7621_flag(const char *name)
{
    const char *v = getenv(name);

    if (!v || !*v) {
        return false;
    }
    return strcmp(v, "0") && strcmp(v, "no") && strcmp(v, "off") &&
           strcmp(v, "false");
}


static void mt7621_rstctl_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    MT7621State *s = opaque;
    uint32_t old = s->sysctl_regs[SYS_RSTCTL / 4];

    s->sysctl_regs[SYS_RSTCTL / 4] = val;

    if ((val & 1) && !(old & 1)) {
        if (getenv("MT7621_RSTLOG") && s->cpu) {
            CPUMIPSState *env = &s->cpu->env;
            uint32_t sp = env->active_tc.gpr[29];
            uint32_t pc = env->active_tc.PC;
            uint32_t ra = env->active_tc.gpr[31];
            int i;

            fprintf(stderr, "restart: pc=%08x ra=%08x sp=%08x\n",
                    pc, ra, sp);
            for (i = 0; i < 64; i++) {
                uint32_t v;
                if (address_space_rw(&address_space_memory,
                                     sp + i * 4, MEMTXATTRS_UNSPECIFIED,
                                     (uint8_t *)&v, 4, false)) {
                    break;
                }
                if (v >= 0x80000000 && v < 0x82000000) {
                    fprintf(stderr, "  stack[%2d] %08x\n", i, v);
                }
            }
        }
        if (getenv("MT7621_NO_RESET")) {
            /* keep the machine frozen so the ring buffer can be read */
            fprintf(stderr, "mt7621: guest requested a reset, holding\n");
            return;
        }
        mt7621_eth_quiesce(s);
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static uint64_t mt7621_rstctl_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;

    return s->sysctl_regs[SYS_RSTCTL / 4];
}

static const MemoryRegionOps mt7621_rstctl_ops = {
    .read = mt7621_rstctl_read,
    .write = mt7621_rstctl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};


/*
 * The MT7621 uses the legacy MIPS GIC, and its reset mapping sends every
 * shared interrupt to VP0 pin 0, i.e. CPU interrupt line 2.  That is why
 * Breed only programs SMASK/POL and then enables IM2: it never writes the
 * mapping registers.  QEMU's GIC model resets everything unmapped, so put
 * the SoC default back after the device reset has run.
 */
static void mt7621_gic_default_map(MT7621State *s)
{
    MIPSGICState *gic;
    int i;

    if (!s->gic) {
        return;
    }

    gic = MIPS_GIC(s->gic);
    for (i = 0; i < gic->num_irq; i++) {
        gic->irq_state[i].map_pin = 0;
        gic->irq_state[i].map_vp = 0;
    }
}

static void mt7621_gic_reset(void *opaque)
{
    mt7621_gic_default_map(opaque);
}

static void mt7621_sysctl_reset(MT7621State *s)
{
    memset(s->sysctl_regs, 0, sizeof(s->sysctl_regs));
    /*
     * "MT76" / "21  ".  Linux's ralink SoC probe compares these two words
     * and panics with "mt7621: unknown SoC" if they do not match.
     */
    s->sysctl_regs[SYS_CHIP_NAME0 / 4]   = 0x3637544d;
    s->sysctl_regs[SYS_CHIP_NAME1 / 4]   = 0x20203132;
    /*
     * SYS_CHIP_REV_ID, decoded the way U-Boot, Breed and Linux's ralink probe
     * do it: bit 17 = CPU_ID (second core present), bit 16 = PKG_ID,
     * bits 11:8 = version, bits 3:0 = eco.
     *
     * The real lg-gapm7200 boards report "SoC Type: MediaTek MT7621 ver:1
     * eco:3", so model exactly that; Breed prints the same fields as
     * "MT7621A ver 1, eco 3".  MT7621_ECO overrides the eco nibble for A/B
     * tests (some kernels gate errata workarounds on it).
     */
    {
        const char *eco = getenv("MT7621_ECO");
        unsigned e = eco ? (unsigned)atoi(eco) & 0xf : 3;

        s->sysctl_regs[SYS_CHIP_REV_ID / 4]  = 0x00030000 | (1 << 8) | e;
    }
    /*
     * SYSCFG0: CHIP_MODE[3:0] tells the firmware which boot flash is wired
     * up; 0 means "no NAND" and makes board_init skip the NFI entirely.
     * Bits above it: DRAM type and xtal mode.
     */
    /*
     * xtal mode lives in SYSCFG0[8:6]: Linux's clk-mt7621 maps 0-2 to
     * 20 MHz, 3-5 to 40 MHz and 6-7 to 25 MHz.  Real MT7621 boards (the
     * K2P log Breed prints, for instance) run a 40 MHz crystal, and Breed,
     * U-Boot and Linux all derive their delays from it -- 25 MHz made every
     * clock in the boot log wrong (DDR 2500 instead of 1200, Bus 218
     * instead of 293).
     */
    {
        const char *cfg0 = getenv("MT7621_SYSCFG0");
        /*
         * The board's own booter dump (real lg-gapm7200, etherboot working)
         * reads SYS_SYSCFG0 back as 0x000232c1 - CHIP_MODE, DDR type, the
         * 40 MHz xtal in bits 8:6 (= 3) and bits 12/13/17.  The generic
         * 0x100 | CHIP_MODE value only agreed on the xtal, and the booter
         * derives its Ethernet descriptor set from these straps, so the
         * board gets its real value here.
         */
        uint32_t board_cfg0 = s->board == MT7621_BOARD_LG_GAPM7200
                              ? 0x000232c1u
                              : (0x00000100u | s->chip_mode);

        s->sysctl_regs[SYS_SYSCFG0 / 4] = cfg0
            ? (uint32_t)strtoul(cfg0, NULL, 0)
            : board_cfg0;
    }
    /*
     * Offset 0x58: the same board dump reads 0x0a210a01 there and the
     * generic model left it zero.  MT7621_SYSCTL58 overrides for A/B.
     */
    {
        const char *v58 = getenv("MT7621_SYSCTL58");

        if (v58) {
            s->sysctl_regs[0x58 / 4] = (uint32_t)strtoul(v58, NULL, 0);
        } else if (s->board == MT7621_BOARD_LG_GAPM7200) {
            s->sysctl_regs[0x58 / 4] = 0x0a210a01u;
        }
    }
    /*
     * CPLL clock config.  RouterBOOT programs 0x40800000 on the RB750Gr3,
     * which is how Linux ends up reporting the 875 MHz CPU clock (and the
     * 1.75 GHz GIC counter derived from it); the cold-reset value 0x40000000
     * makes it report 25 MHz instead, which then disagrees with the emulated
     * timers by a factor of 70.
     */
    {
        const char *cfg = getenv("MT7621_CLKCFG");
        const char *cfg1 = getenv("MT7621_CLKCFG1");

        s->sysctl_regs[SYS_CLKCFG0 / 4] = cfg ? (uint32_t)strtoul(cfg, NULL, 0)
                                              : 0x40800000;
        if (cfg1) {
            s->sysctl_regs[0x30 / 4] = (uint32_t)strtoul(cfg1, NULL, 0);
        }
    }
    {
        const char *sts = getenv("MT7621_CLKSTS");

        s->sysctl_regs[SYS_CUR_CLK_STS / 4] = sts ? (uint32_t)strtoul(sts, NULL, 0)
                                                  : 0x00000101;
    }
    /*
     * 0x3fc is polled for bit 10 before Breed's DDR3 init: it reports the
     * DDR/PLL calibration as finished. Our DDR is always ready.
     */
    s->sysctl_regs[0x3fc / 4]            = 0x00000400;
    /*
     * SYSC_RSTSTAT reports the reset cause.  Keep the bit we recorded when
     * the watchdog barked (Breed overwrites this register itself once it
     * runs, but a real SoC latches the cause across a watchdog reset).
     */
    if (s->wdt_rst_cause) {
        s->sysctl_regs[SYS_RSTSTAT / 4] = WDT_RST_CAUSE;
        s->wdt_rst_cause = false;
    }
}

static void mt7621_dramc_reset(MT7621State *s)
{
    memset(s->dramc_regs, 0, sizeof(s->dramc_regs));
    {
        const char *v604 = getenv("MT7621_MEMC604");
        const char *v618 = getenv("MT7621_MEMC618");

        /*
         * DDR rate decode: 0x604 bit 2 halves it and 0x618 carries the
         * multiplier ((v >> 4) * 4 with a 40 MHz reference).  0x4 / 0xf0 is
         * the pair that prints DDR 1200 on a 40 MHz crystal, the rate a real
         * B70-class board reports.
         */
        s->dramc_regs[0x604 / 4] = v604 ? (uint32_t)strtoul(v604, NULL, 0)
                                        : 0x00000004;
        s->dramc_regs[0x618 / 4] = v618 ? (uint32_t)strtoul(v618, NULL, 0)
                                        : 0x000000f0;
    }
    /*
     * MEMC CPU PLL (0x648), decoded by Linux's clk-mt7621 as
     *   cpu = ((FBDIV + 1) * xtal) >> prediv_tbl[PREDIV], then / ffiv * ffrac
     * with FBDIV = bits 10:4 and PREDIV = bits 13:12.  The old value
     * 0x03800000 has both fields zero, so the kernel computed
     * (0 + 1) * 25 MHz = 25 MHz for the CPU and for the GIC clocksource:
     * every udelay() came out 35x too long (the NAND bad-block scan took a
     * minute and tripped the RCU stall detector) and the clock reported in
     * dmesg was nonsense.
     *
     * FBDIV 21 / PREDIV 0 with the board's 40 MHz crystal gives
     * (21 + 1) * 40 MHz = 880 MHz, the CPU clock a real board reports
     * ("Clocks: CPU: 880MHz ... Ref: 40MHz") and the rate the GIC counter is
     * modelled at (mips_gictimer_set_freq).  The 69 / 1 pair above belonged
     * to the old 25 MHz-crystal model, where it produced 875 MHz.
     */
    {
        const char *pll = getenv("MT7621_CPU_PLL");

        /*
         * The value the real BootROM's DDR-init stage leaves in the MEMC PLL
         * (traced: 0xc2005ab2).  It is what the bootloader reads to report
         * "CPU frequency: 880 MHz"; the previous model default (FBDIV 21,
         * PREDIV 0 = 0x150) is decoded by the booter as 440 MHz.  A cold boot
         * was masked by the stage-1 write, but a warm reset skips stage 1 (the
         * DRAM is already trained at that PLL) and then reported half speed.
         */
        s->dramc_regs[0x648 / 4] = pll ? (uint32_t)strtoul(pll, NULL, 0)
                                       : 0xc2005ab2u;      /* 880MHz */
    }
    {
        const char *ddr = getenv("MT7621_DDR_PLL");

        s->dramc_regs[0x64c / 4] = ddr ? (uint32_t)strtoul(ddr, NULL, 0)
                                       : (29 << 4);
    }
    {
        const char *v410 = getenv("MT7621_MEMC410");
        const char *v80c = getenv("MT7621_MEMC80C");

        if (v410) {
            s->dramc_regs[0x410 / 4] = (uint32_t)strtoul(v410, NULL, 0);
        }
        if (v80c) {
            s->dramc_regs[0x80c / 4] = (uint32_t)strtoul(v80c, NULL, 0);
        }
    }
    s->dramc_regs[0x3fc / 4] = 0x00000400;
}

/*
 * `poweroff` and `halt` on this SoC both end in machine_hang(): interrupts
 * are disabled, all of them are masked and the CPU parks in WAIT.  Nothing
 * is written to the system controller, so the only way to notice is that a
 * CPU which can no longer be woken stays halted.
 */
/* CP0 Count, for the per-CPU skew fix in the CM and the diagnostic below. */
uint32_t cpu_mips_get_count(CPUMIPSState *env);
void cpu_mips_store_count(CPUMIPSState *env, uint32_t value);

static void mt7621_poweroff_poll(void *opaque)
{
    MT7621State *s = opaque;
    CPUState *cs = CPU(s->cpu);
    CPUMIPSState *env = &s->cpu->env;
    bool parked = cs->halted && !(env->CP0_Status & (1 << CP0St_IE));

    /*
     * Bring-up: report the CP0/TLB state the guest starts the kernel with.
     * The netboot path hands over from a bootloader that has been running for
     * seconds, so the state it leaves behind matters.
     */
    /*
     * MT7621_COUNTLOG: per-CPU CP0 Count against the virtual clock.  The
     * guest derives sched_clock from Count and timekeeping from the GIC
     * counter, so a per-CPU baseline that differs shows up here as CPUs
     * that are not in step.
     */
    if (getenv("MT7621_COUNTLOG") && s->cpu) {
        static int64_t countlog_last;
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        if (now - countlog_last >= 500 * 1000000LL) {
            int ci;

            countlog_last = now;
            fprintf(stderr, "countlog: t=%" PRIi64 "ms", now / 1000000);
            for (ci = 0; ci < (int)s->ncpus; ci++) {
                fprintf(stderr, " c%d=%" PRIu32, ci,
                        cpu_mips_get_count(&s->cpus[ci]->env));
            }
            fprintf(stderr, "\n");
        }
    }

    if (getenv("MT7621_ENTRYLOG") && s->cpu) {
        uint32_t hpc = env->active_tc.PC;

        s->pc_hist[s->pc_hist_idx++ % PC_HIST] = hpc;
        if (!s->rom_logged && hpc >= 0xbfc00000 && hpc < 0xbfc01000) {
            int k;

            s->rom_logged = true;
            fprintf(stderr, "rom reached: pc=%08x status=%08x cause=%08x "
                    "badvaddr=%08x epc=%08x errpc=%08x\n",
                    hpc, env->CP0_Status, env->CP0_Cause,
                    (uint32_t)env->CP0_BadVAddr, (uint32_t)env->CP0_EPC,
                    (uint32_t)env->CP0_ErrorEPC);
            fprintf(stderr, "pcs:");
            for (k = 0; k < PC_HIST; k++) {
                uint32_t v = s->pc_hist[(s->pc_hist_idx + k) % PC_HIST];

                if (v) {
                    fprintf(stderr, " %08x", v);
                }
            }
            fprintf(stderr, "\n");
        }
    }
    /*
     * Diagnostic: register dump at one PC (MT7621_PCHIT=0x...), a few times
     * per boot.  A bootloader that takes a different branch after a warm
     * reset is a register/memory difference, not a code difference.
     */
    if (getenv("MT7621_PCHIT") && s->cpu && s->pchit_budget > 0) {
        uint32_t hit = (uint32_t)strtoul(getenv("MT7621_PCHIT"), NULL, 0);

        if ((uint32_t)env->active_tc.PC == hit) {
            int r;

            s->pchit_budget--;
            fprintf(stderr, "pchit pc=%08x", hit);
            for (r = 1; r <= 31; r++) {
                fprintf(stderr, " r%d=%08x", r,
                        (uint32_t)env->active_tc.gpr[r]);
            }
            fprintf(stderr, "\n");
        }
    }
    /*
     * Bootloader -> kernel hand-off.  RouterBOOT and Breed use CP0
     * Count/Compare as a delay timer and jump to the kernel with the last
     * Compare still armed (Breed: 0x00432380).  A kernel whose clockevent is
     * the GIC compare never claims IP7, so the stale compare expires on the
     * next Count wrap and every expiry is an unhandled "irq 7: nobody
     * cared", which the kernel then disables - the real board does not show
     * this.  Track the bootloader's arming state instead of a one-shot: the
     * bootloader may briefly run in the kernel window during its own boot,
     * then arm its delay timer again before the real hand-off.
     *
     * MT7621_NO_HANDOFF_TIMER_CLR=1 disables it for A/B.
     */
    if (s->cpu) {
        uint32_t pc = (uint32_t)env->active_tc.PC;
        bool boot_range = (pc >= 0xa0000000u) ||
                          (pc >= 0x8f000000u && pc < 0x90000000u);

        if (boot_range) {
            s->seen_boot = true;
            s->kernel_hits = 0;
            if (env->CP0_Compare) {
                s->bl_timer_armed = true;
            }
        } else if (!s->handoff_done) {
            if (pc >= 0x80000000u && pc < 0x80200000u) {
                s->kernel_hits++;
            } else {
                s->kernel_hits = 0;
            }
        }
        if (!s->handoff_done && s->seen_boot && s->kernel_hits >= 2) {
            /*
             * The Linux kernel's text is the only thing that runs in
             * 0x80000000..0x801fffff on this boot path (Breed is at 0x8ff...,
             * the stage-2 loader at 0x81000000, RouterBOOT at 0xa0201000 and
             * in its relocated KSEG0 copy at 0x80e00000), so the first kernel
             * PC after bootloader code is the hand-off.  Latching this does
             * not depend on the bootloader having armed CP0 Compare - relying
             * on that (or on sampling the live PC per frame) missed the
             * hand-off entirely on some images and then refused the kernel
             * driver's own RX ring, which looked like "no RX and no TX".
             */
            s->bl_timer_armed = false;
            s->handoff_done = true;
            fprintf(stderr, "mt7621: kernel hand-off at pc=%08x: frame "
                    "engine ownership changes (boot RX ring %08x dormant)\n",
                    pc, s->bl_rx_base);
            if (!getenv("MT7621_NO_HANDOFF_TIMER_CLR")) {
                env->CP0_Count = 0;
                env->CP0_Compare = 0;
                env->CP0_Cause &= ~(1 << 9);     /* CP0Ca_TI */
                if (getenv("MT7621_BOOTLOG")) {
                    fprintf(stderr, "mt7621: hand-off at pc=%08x: CP0 timer "
                            "cleared\n", pc);
                }
            }
        }
    }
    /* Diagnostic: the first PCs the guest executes after reset. */
    if (getenv("MT7621_BOOTLOG") && s->cpu) {
        if (s->bootlog_budget-- > 0) {
            fprintf(stderr, "bootpc: pc=%08x entry=%08x status=%08x\n",
                    (uint32_t)env->active_tc.PC, (uint32_t)s->entry,
                    env->CP0_Status);
        }
    }

    /*
     * Diagnostic: TLB-miss exceptions.  EXCLOG below deliberately filters
     * these out (codes 1/2/3/8 are normal user-space traffic), but a guest
     * that loops in its TLB-refill handler shows nothing else: the handler is
     * entered, cannot map the address and returns into the same instruction.
     * Log the first few with BadVAddr and the faulting words, which is what
     * names the pointer that went out of range.
     */
    if (getenv("MT7621_TLBLOG") && s->cpu) {
        static int tlb_budget = 48;
        uint32_t cause = env->CP0_Cause;
        unsigned code = (cause >> 2) & 0x1f;

        /*
         * User-space misses would eat the budget long before a booter fault,
         * so MT7621_TLBLOG_MIN=<pc> restricts the log to faults at or above
         * that address (e.g. 0x81000000 keeps only the booter's own).
         */
        const char *tlb_min = getenv("MT7621_TLBLOG_MIN");

        if (tlb_min &&
            (uint32_t)env->active_tc.PC < (uint32_t)strtoul(tlb_min, NULL, 0)) {
            return;
        }

        if ((code == 1 || code == 2 || code == 3 || code == 8) &&
            tlb_budget-- > 0) {
            uint32_t epc = (uint32_t)env->CP0_EPC;
            uint32_t prev = cpu_ldl_code(env, epc - 4);
            uint32_t insn = cpu_ldl_code(env, epc);

            fprintf(stderr, "tlbmiss: code=%u pc=%08x epc=%08x "
                    "badvaddr=%08x cause=%08x prev=%08x insn=%08x\n",
                    code, (uint32_t)env->active_tc.PC, epc,
                    (uint32_t)env->CP0_BadVAddr, cause, prev, insn);
        }
    }

    /* Diagnostic: report every architectural exception the guest takes. */
    if (getenv("MT7621_EXCLOG") && s->cpu) {
        static int exc_budget = 4096;
        uint32_t cause = env->CP0_Cause;
        unsigned code = (cause >> 2) & 0x1f;

        /* TLB misses (1,2,3,8) are normal user-space traffic and would eat
         * the budget long before the interesting fault; only report the
         * codes that mean something went wrong. */
        bool interesting = (code == 4 || code == 5 || code == 10 ||
                            code == 11 || code == 12 || code == 13 ||
                            code == 26);
        if (code && interesting && exc_budget-- > 0) {
            fprintf(stderr, "exc: code=%u pc=%08x epc=%08x badvaddr=%08x "
                    "status=%08x cause=%08x\n", code,
                    (uint32_t)env->active_tc.PC, (uint32_t)env->CP0_EPC,
                    (uint32_t)env->CP0_BadVAddr, env->CP0_Status, cause);
            /*
             * Reserved Instruction and DSP: show the word the guest choked on
             * and the preceding few, which names the instruction and its delay
             * slot context without needing a guest-side disassembler.  A DSP
             * exception on instruction stream the kernel never emitted means
             * control flow went into data, and these words say where.
             */
            if (code == 11 || code == 26) {
                uint32_t epc = (uint32_t)env->CP0_EPC;
                uint32_t insn[4];
                int k;

                for (k = 0; k < 4; k++) {
                    insn[k] = cpu_ldl_code(env, epc - 8 + 4 * k);
                }
                fprintf(stderr, "RI: %08x: %08x %08x [%08x] %08x\n", epc - 8,
                        insn[0], insn[1], insn[2], insn[3]);
            }
        }
    }

    if (getenv("MT7621_ENTRYLOG") && !s->entry_logged && s->cpu) {
        uint32_t pc = env->active_tc.PC;

        if (pc >= 0x80600000 && pc < 0x81000000) {
            int i;

            s->entry_logged = true;
            if (getenv("MT7621_ENTRY_FIX")) {
                int t;

                env->CP0_Status &= ~(1 << CP0St_ERL);
                env->CP0_Config0 = (env->CP0_Config0 & ~7u) | 2;
                for (t = 0; t < MIPS_TLB_MAX; t++) {
                    env->tlb->mmu.r4k.tlb[t].VPN = 0;
                    env->tlb->mmu.r4k.tlb[t].PageMask = 0;
                    env->tlb->mmu.r4k.tlb[t].PFN[0] = 0;
                    env->tlb->mmu.r4k.tlb[t].PFN[1] = 0;
                }
                fprintf(stderr, "entry: normalised status=%08x config0=%08x\n",
                        env->CP0_Status, (uint32_t)env->CP0_Config0);
            }
            fprintf(stderr, "entry: pc=%08x status=%08x cause=%08x wired=%u "
                    "entryhi=%08x index=%u config0=%08x config1=%08x\n",
                    pc, env->CP0_Status, env->CP0_Cause, env->CP0_Wired,
                    (uint32_t)env->CP0_EntryHi, (uint32_t)env->CP0_Index,
                    (uint32_t)env->CP0_Config0, (uint32_t)env->CP0_Config1);
            for (i = 0; i < 8; i++) {
                fprintf(stderr, "  tlb[%d] vpn=%08x mask=%08x asid=%02x "
                        "g=%d pfn0=%08x pfn1=%08x\n", i,
                        env->tlb->mmu.r4k.tlb[i].VPN,
                        env->tlb->mmu.r4k.tlb[i].PageMask,
                        env->tlb->mmu.r4k.tlb[i].ASID,
                        env->tlb->mmu.r4k.tlb[i].G,
                        (uint32_t)env->tlb->mmu.r4k.tlb[i].PFN[0],
                        (uint32_t)env->tlb->mmu.r4k.tlb[i].PFN[1]);
            }
        }
    }
    if (!s->halt_seen_running) {
        if (!cs->halted) {
            s->halt_seen_running = true;
        }
    } else if (parked) {
        if (++s->halt_ticks >= 4) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
    } else {
        s->halt_ticks = 0;
    }

    {
        extern void mips_gic_level_report(void);
        mips_gic_level_report();
    }
    timer_mod(s->poweroff_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              ((!s->handoff_done ||
                getenv("MT7621_ENTRYLOG") || getenv("MT7621_EXCLOG") ||
                getenv("MT7621_TLBLOG") || getenv("MT7621_BOOTLOG"))
               ? 1 : 250));
}

/*
 * Kernel command line injection for bring-up.
 *
 * RouterBOOT assembles the kernel command line in a scratch buffer inside
 * its own RAM image and hands a pointer to the kernel, so there is nothing
 * to patch in the flash image.  The buffer only exists for the instant
 * between "loading kernel... OK" and the jump, and its physical address
 * moves with the bootloader build, so sweep the bootloader's RAM window and
 * append the extra options in place.
 *
 * The anchor also appears inside the printk ring buffer (as part of the
 * "Kernel command line: ..." line), so a NUL immediately before it is
 * required to stay on RouterBOOT's copy.
 */
#define MT7621_CMDLINE_BASE         0x01100000
#define MT7621_CMDLINE_SIZE         0x00400000
#define MT7621_CMDLINE_PAGE         0x1000
#define MT7621_CMDLINE_PAGES_TICK   16
#define MT7621_CMDLINE_LIFETIME_NS  (60 * 1000000000LL)

static bool mt7621_cmdline_patch(MT7621State *s, uint32_t phys)
{
    char buf[1024];
    char tmp[1024];
    size_t len, extra = strlen(s->cmdline_extra);
    size_t i;

    if (address_space_read(&address_space_memory, phys,
                           MEMTXATTRS_UNSPECIFIED, buf, sizeof(buf))) {
        return false;
    }
    buf[sizeof(buf) - 1] = 0;
    len = strnlen(buf, sizeof(buf) - 1);
    if (strstr(buf, s->cmdline_extra)) {
        return true;
    }
    if (len + extra + 3 > sizeof(buf)) {
        return false;
    }
    /* Only extend into padding; the buffer may be followed by live data. */
    for (i = len; i <= len + extra + 1; i++) {
        if (buf[i] != 0) {
            return false;
        }
    }
    snprintf(tmp, sizeof(tmp), "%s %s", buf, s->cmdline_extra);
    if (address_space_write(&address_space_memory, phys,
                            MEMTXATTRS_UNSPECIFIED, tmp, strlen(tmp) + 1)) {
        return false;
    }
    s->cmdline_patches++;
    if (s->cmdline_patches <= 10) {
        fprintf(stderr, "mt7621: cmdline #%u at tick %u: '%s'\n",
                s->cmdline_patches, s->cmdline_ticks, tmp);
    }
    return true;
}

static void mt7621_cmdline_poll(void *opaque)
{
    MT7621State *s = opaque;
    static const char anchor[] = "root=/dev/ram0 bootimage=1";
    uint8_t page[MT7621_CMDLINE_PAGE + 64];
    int n;

    s->cmdline_ticks++;

    if (s->cpu && !s->cmdline_kernel_seen) {
        uint32_t pc = s->cpu->env.active_tc.PC;

        if (pc >= 0x80100000 && pc < 0x81000000) {
            s->cmdline_kernel_seen = true;
            if (getenv("MT7621_TRACE")) {
                fprintf(stderr, "mt7621: kernel PC %08x at tick %u "
                        "(patches %u)\n", pc, s->cmdline_ticks,
                        s->cmdline_patches);
            }
        }
    }

    if (getenv("MT7621_TRACE") && !(s->cmdline_ticks % 10000)) {
        fprintf(stderr, "mt7621: cmdline poll #%u phys=%08x patches=%u\n",
                s->cmdline_ticks, s->cmdline_phys, s->cmdline_patches);
    }

    if (s->cmdline_phys) {
        /*
         * Once the buffer address is known, watch it closely: RouterBOOT
         * writes the string and jumps within a millisecond or so, and the
         * kernel parses it almost immediately after entry.
         */
        int r = address_space_read(&address_space_memory, s->cmdline_phys,
                                   MEMTXATTRS_UNSPECIFIED, page, 1024);
        if (getenv("MT7621_TRACE") && page[0] &&
            !(s->cmdline_ticks % 20000)) {
            fprintf(stderr, "mt7621: cmdline peek #%u r=%d '%c%c%c%c%c%c'\n",
                    s->cmdline_ticks, r, page[0], page[1], page[2],
                    page[3], page[4], page[5]);
        }
        if (!memcmp(page, anchor, sizeof(anchor) - 1)) {
            mt7621_cmdline_patch(s, s->cmdline_phys);
        }
    } else {
        /* Address unknown: sweep the bootloader's RAM window. */
        for (n = 0; n < MT7621_CMDLINE_PAGES_TICK; n++) {
            uint32_t off = s->cmdline_cursor;
            uint32_t phys = MT7621_CMDLINE_BASE + off;
            uint8_t *hit;

            if (address_space_read(&address_space_memory, phys,
                                   MEMTXATTRS_UNSPECIFIED, page,
                                   sizeof(page))) {
                break;
            }
            s->cmdline_cursor += MT7621_CMDLINE_PAGE;
            if (s->cmdline_cursor + sizeof(page) >= MT7621_CMDLINE_SIZE) {
                s->cmdline_cursor = 0;
            }

            hit = memmem(page, MT7621_CMDLINE_PAGE, anchor,
                         sizeof(anchor) - 1);
            if (!hit || hit == page || hit[-1] != 0) {
                continue;
            }
            s->cmdline_phys = phys + (hit - page);
            if (getenv("MT7621_TRACE")) {
                fprintf(stderr, "mt7621: cmdline buffer at phys %08x\n",
                        s->cmdline_phys);
            }
            mt7621_cmdline_patch(s, s->cmdline_phys);
            break;
        }
    }

    if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->cmdline_deadline_ns) {
        timer_mod(s->cmdline_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (s->cmdline_phys ? 100000 : 2000000));
    }
}

/*
 * CPU configuration registers for the MT7621's 1004Kc.
 *
 * These must be applied *after* cpu_reset(), which reloads every CP0 config
 * register from the CPU model.  mt7621_reset() (the machine reset handler)
 * runs before the per-CPU handlers, so anything it writes to Config1/2/3/7
 * is overwritten - that is how Config3.MT kept vanishing and why the SMP
 * topology could never be made to stick.  Every CPU applies this from its own
 * reset handler, so cores and re-released VPs all see the same hardware.
 */
void mt7621_cpu_hw_config(MT7621State *s, CPUMIPSState *env)
{
    /*
     * Config0.M (bit 31): "more configuration registers present".  The
     * kernel walks Config1 -> Config2 -> Config3 only while each previous
     * register has this bit set (decode_configs()).  The 1004Kc has it set;
     * without it the guest never reads Config2/Config3, so neither the L2/M
     * bits nor MIPS_ASE_MIPSMT (Config3.MT) are ever seen - which is why
     * mips_cps_numvps() kept returning 1 even with CONFIG_MIPS_MT_SMP=y.
     */
    env->CP0_Config0 |= (1u << 31);
    /*
     * The real 1004Kc advertises ULRI + CMGCR, but with them visible this
     * model's OpenWrt guests take the MIPS CPS path and die before printing
     * anything (openwrt 5.15: zero bytes), while RouterOS 3.3.5 is unaffected.
     * Default to the pre-CPS view and let MT7621_CMGCR=1 (or MT7621_CFG3) opt
     * in - MikroTik's 5.6.3 netinstall kernel needs it to find the CM/GIC.
     */
    /*
     * ULRI + CMGCR are part of this part's Config3 and are now on by default:
     * the netinstall kernel needs the CM to find the GIC, and the LG board
     * boots identically either way (its earlier "dies as soon as CPS is
     * visible" was fixed with the SMP model).  MT7621_NO_CMGCR=1 restores the
     * old pre-CPS view for A/B testing.
     */
    if (mt7621_flag("MT7621_NO_CMGCR") && !getenv("MT7621_CFG3")) {
        env->CP0_Config3 &= ~((1u << 13) | (1u << 29));
    }
    if (getenv("MT7621_CFG2")) {
        env->CP0_Config2 = strtoul(getenv("MT7621_CFG2"), NULL, 0);
    } else if (!getenv("MT7621_NO_L2")) {
        /*
         * The board's own CPU reports a 256 KiB, 8-way, 32-byte-line L2
         * (OpenWrt on hardware: "MIPS secondary cache 256kB, 8-way,
         * linesize 32 bytes"); Config2.SL=0 hides it and the kernel boots
         * without any secondary cache.  Fields: SA(3:0)=ways-1=7,
         * SL(7:4)=line/2-1=3?  measured below, SS(11:8) selects the size.
         * Measured against the guest's own decode: 0x447 prints exactly
         * 256kB/8-way/32B.  MT7621_NO_L2=1 hides it again for A/B.
         */
        /*
         * The M bit (31) is not decoration: decode_configs() reads
         * Config3/4/5 only while the previous register has it set.  With
         * Config2.M clear the kernel never read Config3 at all - no CM
         * (CMGCR): "MIPS CPS SMP unable to proceed without a CM", and no MT.
         */
        env->CP0_Config2 = 0x80000447;
    }
    if (getenv("MT7621_CFG3")) {
        env->CP0_Config3 = strtoul(getenv("MT7621_CFG3"), NULL, 0);
    }

    if (s->wdt_timer) {
        timer_del(s->wdt_timer);
    }

    /*
     * Config7.AR: the MT7621's 1004K reports its D-cache as effectively
     * physically indexed ("address recognition"), which is why OpenWrt on
     * hardware prints "Primary data cache 32kB, 4-way, PIPT, no aliases"
     * while this model printed VIPT/aliases (c-r4k.c only sets
     * MIPS_CACHE_PINDEX when Config7.AR is readable).  With AR clear the
     * guest also believes it has virtual aliases and (RouterOS) drops to a
     * single core.  MT7621_NO_AR=1 restores the old view for A/B.
     */
    /*
     * The 1004K core is multithreaded: Config3.MT describes 2 VPs (hardware
     * threads) per core, which is what the board's kernel reports as
     * "VPE topology {2,2}".  Without it mips_cps_numvps() returns 1 and the
     * CM's PVPE field is ignored.  MT7621_NO_MT=1 restores the old view.
     */
    if (!getenv("MT7621_NO_MT")) {
        env->CP0_Config3 |= (1u << 2);      /* CP0C3_MT */
    }
    if (s->cm_vps > 1 && !getenv("MT7621_NO_MT")) {
        /*
         * The MT half of the same hardware description.  The 1004Kc core
         * implements two VPEs with one TC each, so:
         *
         *  - MVPConf0.PVPE is "VPEs minus one" (1).  cps-vec reads it to
         *    size the VPE loop and to derive the EBase.CPUNum mask, and
         *    QEMU's default of 0 hides the second VPE from both.
         *  - VPE0 of each core is the master VPE (MVP); a core reset leaves
         *    the other VPEs halted and inactive until the boot VPE starts
         *    them (cps-vec's mips_cps_core_init()/mips_cps_boot_vpes()).
         *
         * Both are core properties, applied on every CPU reset.
         */
        CPUState *cs = env_cpu(env);
        unsigned vp = cs->cpu_index % s->cm_vps;

        cs->nr_threads = s->cm_vps;
        env->mvp->CP0_MVPConf0 &= ~(0x1fu << CP0MVPC0_PVPE);
        env->mvp->CP0_MVPConf0 |= (s->cm_vps - 1) << CP0MVPC0_PVPE;
        if (vp == 0) {
            env->CP0_VPEConf0 |= (1u << CP0VPEC0_MVP);
        } else {
            env->CP0_VPEConf0 &= ~(1u << CP0VPEC0_VPA);
            env->active_tc.CP0_TCStatus = 0;
            env->active_tc.CP0_TCHalt = 1;
        }
    }
    if (!getenv("MT7621_NO_AR")) {
        env->CP0_Config7 |= (1u << 16);      /* MIPS_CONF7_AR */
    }
    if (s->cfg1_cache_fields) {
        env->CP0_Config1 = (env->CP0_Config1 & ~0xffffu) |
                           s->cfg1_cache_fields;
    }
    /*
     * Breed (r1286, r1280) sizes its DMA buffers as
     *
     *     n = size - 1 + 2 * line;  p = malloc(n);
     *     memset(round_up(p, line), 0, n);
     *
     * dlmalloc only guarantees 8-byte alignment, so with line = 32 the clear
     * starts 0, 8, 16 or 24 bytes after p.  For the first 1 KiB buffer
     * (n = 0x43f, 1088 usable bytes) only the 0 offset fits: 8 or more runs
     * past the chunk and overwrites the next chunk's size field, so the
     * following malloc returns NULL ("unable to allocate fifos").
     *
     * The board boots this firmware at Dl=4, and a real L1D is what makes it
     * survive that clear, so with the cache model enabled the machine reports
     * the hardware value and the guest sizes its cache maintenance for a
     * 32 KiB / 32-byte-line cache, as the model does.
     *
     * Dl is now always the hardware value: an 8-byte line is not something a
     * Linux guest can live with.  r4k_blast_dcache_setup() only installs a
     * blast routine for 16/32/64/128-byte lines, so at Dl=2
     * r4k_blast_dcache/icache stay NULL and the first cache flush (typically
     * an IRQ-time DMA sync) jumps to address 0 - the netinstall kernel on
     * lg-gapm7200 died exactly that way (epc=0, BadVA=0, ExcCode 2, plus a
     * "smp_call_function_many from IRQ" warning on the way down).  Breed's
     * allocator needs the modelled cache at this line size, which is why the
     * L1D model now defaults on for the whole machine (see
     * mt7621_dcache_init); MT7621_DCACHE=0 restores the bare-TCG baseline.
     * MT7621_DL=2 still forces the old 8-byte baseline for experiments, and
     * MT7621_DCACHE=0 turns the model off.
     */
    /*
     * Debug knob only (the real part is Dl=4, i.e. 32-byte lines): override
     * Config1.Dl (bits 12:10) to A/B the d-cache line size without rebuilding.
     */
    {
        const char *dl = getenv("MT7621_DL");

        if (dl) {
            env->CP0_Config1 = (env->CP0_Config1 & ~(7u << 10)) |
                               ((strtoul(dl, NULL, 0) & 7) << 10);
        }
    }
    if (getenv("MT7621_RSTLOG")) {
        fprintf(stderr, "cpu hw cfg: cfg1=%08x cfg2=%08x cfg3=%08x cfg7=%08x\n",
                (uint32_t)env->CP0_Config1, (uint32_t)env->CP0_Config2,
                (uint32_t)env->CP0_Config3, (uint32_t)env->CP0_Config7);
    }
}
static void mt7621_reset(void *opaque)
{
    MT7621State *s = opaque;
    CPUMIPSState *env = &s->cpu->env;

    /*
     * Cold boot runs the BootROM's DDR-init stage; a warm reset does not need
     * it (DRAM is trained), and re-running it on this model ends in a stale
     * hand-over: the stage returns into state left by the previous boot and
     * the CPU ends up executing at 0xd10, so the bootloader never restarts
     * (no banner, no output).  The real BootROM detects the trained DRAM and
     * skips the training work, so entering the bootloader directly is the
     * same end state; MT7621_STAGE1_WARM=1 forces the stage anyway.
     */
    s->reset_count++;
    s->warm_boot = s->reset_count > 1 && !getenv("MT7621_STAGE1_WARM");
    s->pchit_budget = 4;
    {
        const char *n = getenv("MT7621_BOOTLOG_N");

        s->bootlog_budget = n ? atoi(n) : 40;
    }
    if (getenv("MT7621_RSTLOG")) {
        fprintf(stderr, "mt7621: machine reset, cpu pc=%08x ra=%08x "
                "cfg3=%08x cfg7=%08x\n",
                env->active_tc.PC, env->active_tc.gpr[31],
                (uint32_t)env->CP0_Config3, (uint32_t)env->CP0_Config7);
    }

    /*
     * MT7621_CFGLOG=1 prints the line size the guest will actually see, so a
     * test that means to run at a given Dl can assert it instead of assuming
     * an environment variable took effect.  Breed derives the size as
     * 1 << (Dl + 1): Dl=2 -> 8, Dl=4 -> 32.
     */
    if (getenv("MT7621_CFGLOG")) {
        unsigned dl = ((uint32_t)env->CP0_Config1 >> 10) & 7;

        fprintf(stderr, "cpu: Config1=%08x Dl=%u line=%u\n",
                (uint32_t)env->CP0_Config1, dl, 1u << (dl + 1));
    }

    /*
     * MT7621_NAND_SNAP: keep a copy of the medium as the guest left it, the
     * first time the machine resets.  Whatever the running system programmed
     * or erased is what the next boot's loader has to cope with.
     */
    mt7621_nand_taglog_boot_open();
    {
        const char *snap = getenv("MT7621_NAND_SNAP");

        /*
         * reset_count == 1 is the reset QEMU performs while starting the
         * machine (before the guest has run a single instruction); the medium
         * at that point is the boot dump and says nothing about what the
         * guest changed.  Only capture from the second reset on.
         */
        if (snap && s->reset_count > 1 && access(snap, F_OK) != 0) {
            mt7621_nand_snapshot(s, snap);
        }
    }

    /*
     * A real reset clears every controller on the SoC, so a reboot never
     * inherits state from the system it replaced.  Each block resets itself
     * completely - register file, FIFOs, latches, pending interrupts - and
     * the order below is the only place that knows about dependencies:
     *
     *   sysctl  first, because it latches a recorded watchdog reset cause
     *           into SYS_RSTSTAT before wdt_reset() drops the cause flag;
     *   CM      next: the core/VPE geometry it reports comes from ncpus and
     *           the MT7621_VPS knob, not from anything the guest wrote;
     *   systick last: its reset releases the GIC line, and the BootROM
     *           hand-over at the end of this function re-arms the counter.
     *
     * Deliberately not touched: DRAM and its aliases (contents survive a warm
     * reset, which is what the bootloader's warm path relies on), the NAND
     * medium and the SPI NOR contents (flash is not erased by a reset), the
     * board's strapping properties, and the debug sinks.
     */
    mt7621_sysctl_reset(s);      /* SYSCTL window + straps + RSTSTAT */
    mt7621_dramc_reset(s);       /* MEMC/PLL defaults */
    mt7621_wdt_reset(s);         /* timer block, armed bark, cause flag */
    mt7621_cm_reset(s);          /* GCR/CPC + core-selector latches */
    mt7621_nfi_reset(s);         /* NFI: regs, FIFOs, address/read latches */
    mt7621_spi_reset(s);         /* SPI controller; NOR contents kept */
    mt7621_eth_dev_reset(s);     /* frame engine, switch, PHYs, GIC line */
    mt7621_usb_reset(s);         /* xHCI MAC + IPPC */
    mt7621_crypto_reset(s);      /* EIP-93 register file */
    mt7621_systick_reset(s);     /* regs, both timers, GIC line */
    mt7621_dcache_reset(s);      /* L1D tags/line geometry */
    mt7621_loader_reset(s);      /* per-boot bring-up latches */

    /*
     * Linux scribbled over the RAM the bootloader lived in, so the BootROM
     * has to load it again before the CPU is released.
     */
    if (s->image_file && (!s->spi_nor || s->elf_kernel)) {
        mt7621_load_image(s, s->image_file);
    }

    /* Malta does the same when a -kernel image is supplied. */
    env->CP0_Status &= ~(1 << 2); /* CP0St_ERL */
    /*
     * The hand-over must not inherit the bootloader's CP0 timer.  RouterBOOT
     * and Breed both use Count/Compare as a delay timer (Breed's last write
     * is e.g. Compare=0x00432380), and a kernel whose clockevent is the GIC
     * compare never claims IP7: the stale Compare then expires on the next
     * Count wrap and every expiry is an unhandled "irq 7: nobody cared",
     * which the kernel eventually disables.  A real reset clears the timer;
     * clear it here so the guest starts from the same state.
     */
    env->CP0_Count = 0;
    env->CP0_Compare = 0;
    env->CP0_Cause &= ~(1 << 9);         /* CP0Ca_TI */
    env->active_tc.PC = s->entry;
    if (getenv("MT7621_ENTRY_DIRTY") && s->elf_kernel) {
        /* reproduce the CP0/TLB state RouterBOOT leaves at the hand-over */
        int t;

        env->CP0_Status |= (1 << CP0St_ERL) | (1 << 10);
        env->CP0_Config0 = 0x80000485;
        env->CP0_EntryHi = 0x800fc000;
        for (t = 0; t < 8; t++) {
            env->tlb->mmu.r4k.tlb[t].VPN = 0x800fc000 - t * 0x4000;
            env->tlb->mmu.r4k.tlb[t].PageMask = 0;
            env->tlb->mmu.r4k.tlb[t].PFN[0] = 0;
            env->tlb->mmu.r4k.tlb[t].PFN[1] = 0;
        }
    }
    if (s->elf_kernel) {
        uint32_t a0 = s->elf_argc, a1 = s->elf_argv, a2 = 0, a3 = 0;
        const char *ov;

        /*
         * Some bootloaders (MikroTik's, for the RouterOS kernel) use the
         * VxWorks-style convention where a0 == -2 marks a1 as the boot
         * argument block; the kernel only records that pointer in that case.
         */
        if ((ov = getenv("MT7621_ELF_A0")) != NULL) {
            a0 = strtoul(ov, NULL, 0);
        }
        if ((ov = getenv("MT7621_ELF_A1")) != NULL) {
            a1 = strtoul(ov, NULL, 0);
        }
        if ((ov = getenv("MT7621_ELF_A2")) != NULL) {
            a2 = strtoul(ov, NULL, 0);
        }
        if ((ov = getenv("MT7621_ELF_A3")) != NULL) {
            a3 = strtoul(ov, NULL, 0);
        }
        env->active_tc.gpr[4] = a0;
        env->active_tc.gpr[5] = a1;
        env->active_tc.gpr[6] = a2;
        env->active_tc.gpr[7] = a3;
    }

    /*
     * The MT7621 BootROM hands over with the 50 kHz systick already running
     * and its compare interrupt enabled; Breed only hooks GIC IRQ 5 and
     * relies on that.  Reproduce the hand-over state.
     */
    mt7621_systick_boot_handover(s);

}

/*
 * MT7621_PERIPHSPY: name the peripheral-window offsets that no block answers.
 *
 * The peripheral container covers 1 MiB from 0x1e000000.  A guest access with
 * no subregion behind it reads as zero and writes vanish, which is silent by
 * design - and useless when a device is modelled at the wrong address.  This
 * catch-all is registered before the real blocks (so it is always overridden
 * where a model exists) and reports what falls through it.
 *
 * MT7621_PERIPHSPY_LO / _HI narrow the report to an absolute address range,
 * because early boot touches a lot of unused space.
 */
static bool mt7621_spy_wanted(hwaddr pa)
{
    const char *lo = getenv("MT7621_PERIPHSPY_LO");
    const char *hi = getenv("MT7621_PERIPHSPY_HI");

    if (lo && pa < strtoull(lo, NULL, 0)) {
        return false;
    }
    if (hi && pa >= strtoull(hi, NULL, 0)) {
        return false;
    }
    return true;
}

static uint64_t mt7621_spy_read(void *opaque, hwaddr addr, unsigned size)
{
    static unsigned budget = 6000;
    hwaddr pa = MT7621_PERIPH_BASE + addr;

    if (mt7621_spy_wanted(pa) && budget--) {
        fprintf(stderr, "periph: unassigned read  %08x size=%u\n",
                (unsigned)pa, size);
    }
    return 0;
}

static void mt7621_spy_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    static unsigned budget = 6000;
    hwaddr pa = MT7621_PERIPH_BASE + addr;

    if (mt7621_spy_wanted(pa) && budget--) {
        fprintf(stderr, "periph: unassigned write %08x <- %08x size=%u\n",
                (unsigned)pa, (uint32_t)val, size);
    }
}

static const MemoryRegionOps mt7621_spy_ops = {
    .read = mt7621_spy_read,
    .write = mt7621_spy_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void mt7621_machine_init(MachineState *machine)
{
    MT7621State *s = MT7621_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    qemu_irq uart_irq;
    uint64_t ram_size = machine->ram_size;
    int i;

    /*
     * MT7621_LOG=<file>: send every model diagnostic there, independently of
     * how the shell captures stderr.  The bring-up traces (hand-off, RX/TX,
     * GIC sources) are the only way to see what the model did while the
     * guest's serial console is busy with its own output, and redirections in
     * a launcher script have repeatedly lost them.
     */
    {
        const char *logp = getenv("MT7621_LOG");

        if (logp && *logp) {
            FILE *lf = freopen(logp, "w", stderr);

            if (!lf) {
                fprintf(stderr, "mt7621: cannot open %s\n", logp);
            } else {
                setvbuf(stderr, NULL, _IOLBF, 0);
            }
        }
    }
    mt7621_trace = getenv("MT7621_TRACE") != NULL;
    s->board = MT7621_BOARD_LG_GAPM7200;
    /* resolves the preset (and its chip_mode) from -M board= or the image */
    mt7621_board_apply(s, machine->kernel_filename);

    if (ram_size > 512 * MiB) {
        error_report("mt7621: at most 512 MiB of RAM is supported");
        exit(1);
    }

    /* CPU */
    s->cpuclk = clock_new(OBJECT(s), "cpu-refclk");
    clock_set_hz(s->cpuclk, 880 * 1000 * 1000);

    s->ncpus = machine->smp.cpus;
    if (s->ncpus < 1) {
        s->ncpus = 1;
    }
    if (s->ncpus > MT7621_MAX_CPUS) {
        s->ncpus = MT7621_MAX_CPUS;
    }
    for (i = 0; i < (int)s->ncpus; i++) {
        MIPSCPU *cpu = mips_cpu_create_with_clock(machine->cpu_type,
                                                  s->cpuclk, false);

        s->cpus[i] = cpu;
        cpu_mips_irq_init_cpu(cpu);
        cpu_mips_clock_init(cpu);
        qemu_register_reset(mt7621_cpu_reset, cpu);
        if (i) {
            CPU(cpu)->halted = 1;
        }
    }
    s->cpu = s->cpus[0];

    if (getenv("MT7621_CFG1")) {
        /*
         * Only the cache descriptor bits (Ds/Dl/Da at 15:7).  Masking the
         * whole low half would also clobber the feature bits, in particular
         * Config1.FP, and hide the FPU from the guest.
         */
        s->cfg1_cache_fields = strtoul(getenv("MT7621_CFG1"), NULL, 0) & 0xff80;
    }
    if (s->cfg1_cache_fields) {
        s->cpu->env.CP0_Config1 = (s->cpu->env.CP0_Config1 & ~0xff80u) |
                                  s->cfg1_cache_fields;
    }
    qemu_register_reset(mt7621_reset, s);

    /*
     * The GIC shared counter runs off the CPU clock on this SoC.  RouterBOOT
     * reports the real rate (875 MHz on the RB750Gr3) and OpenWrt programs its
     * clock events against it, so the model has to tick at the same rate or
     * every guest timeout is off by the ratio.
     */
    /*
     * The compressed-kernel loader sits at its load address and is then
     * overwritten by its own decompression output; on real MIPS it survives
     * only because its instructions are still in the I-cache (the kernel even
     * says "FIXME: should we flush cache here?").  Shadow the megabyte the
     * RB750Gr3 netboot images load into (0x80b81000 / 0x80b91000) so that
     * instruction fetch keeps seeing the loader, and leave every other address
     * - including the whole NAND/RouterOS boot - on plain TCG semantics.
     */
    if (!getenv("MT7621_NO_ICACHE") && s->board == MT7621_BOARD_RB750GR3) {
        uint64_t lo = 0x80b00000ULL, hi = 0x80c00000ULL;
        const char *rng = getenv("MT7621_ICACHE_RANGE");

        if (rng) {
            unsigned long long rlo = 0, rsize = 0;

            if (sscanf(rng, "%lli+%lli", &rlo, &rsize) == 2 && rsize) {
                lo = rlo;
                hi = rlo + rsize;
            }
        }
        icache_shadow_enable(lo, hi);
    }

    if (getenv("MT7621_GIC_FREQ")) {
        mips_gictimer_set_freq(strtoul(getenv("MT7621_GIC_FREQ"), NULL, 0));
    } else {
        /*
         * The GIC shared counter is clocked from the CPU PLL: a real board
         * reports "Clocks: CPU: 880MHz ... Ref: 40MHz" and Linux derives its
         * clocksource/sched_clock rate from the same value, so the emulated
         * counter must run at exactly the CPU rate the guest decodes from
         * MEMC[0x648] (FBDIV 21 / PREDIV 0 on a 40 MHz crystal = 880 MHz).
         *
         * This used to be 875 MHz, the number the old 25 MHz-crystal model
         * produced from FBDIV 69 / PREDIV 1.  Once the crystal and the PLL
         * moved to the real board's 40 MHz / 880 MHz, the counter was left
         * behind: every guest timebase below the timer (clockevents,
         * timekeeping, sched_clock) then ran 0.57% slow while its udelay()
         * and BogoMIPS calibration - which come from CP0 Count - stayed
         * right.  Keep the two in step.
         */
        mips_gictimer_set_freq(880 * 1000 * 1000);
    }


    mt7621_dump_init(s);

    s->poweroff_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                     mt7621_poweroff_poll, s);
    timer_mod(s->poweroff_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              (getenv("MT7621_ENTRYLOG") || getenv("MT7621_EXCLOG") ||
               getenv("MT7621_TLBLOG") || getenv("MT7621_BOOTLOG") ? 1 : 250));

    if (getenv("MT7621_CMDLINE")) {
        s->cmdline_extra = g_strdup(getenv("MT7621_CMDLINE"));
        /*
         * RouterBOOT keeps the assembled command line in a scratch buffer
         * inside its RAM image.  Its address is build specific; the RB750Gr3
         * booter 3.35 uses this one.  Without a seed the poller falls back
         * to sweeping the whole bootloader window.
         */
        if (getenv("MT7621_CMDLINE_PHYS")) {
            s->cmdline_phys = strtoul(getenv("MT7621_CMDLINE_PHYS"), NULL, 0);
        } else if (s->board == MT7621_BOARD_RB750GR3) {
            s->cmdline_phys = 0x01200c80;
        }
        s->cmdline_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        mt7621_cmdline_poll, s);
        s->cmdline_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                 MT7621_CMDLINE_LIFETIME_NS;
        timer_mod(s->cmdline_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000000LL);
    }

    /* DDR */
    memory_region_init_ram(&s->ram, NULL, "mt7621.ddr", ram_size,
                           &error_fatal);
    memory_region_add_subregion(sysmem, MT7621_RAM_BASE, &s->ram);

    /*
     * The MT7621 DRAM controller only decodes the address bits that the
     * populated chips need, so accesses above the installed size wrap around
     * and alias onto low memory.  Breed detects the DDR size exactly this
     * way (write a pattern at 0, write 0 at 1<<n, read back 0), so the
     * aliases have to exist for it to measure less than 512 MiB.
     */
    if (ram_size & (ram_size - 1)) {
        error_report("mt7621: RAM size must be a power of two");
        exit(1);
    }
    s->ram_alias_count = MIN(MT7621_KSEG1_WINDOW / ram_size - 1,
                             ARRAY_SIZE(s->ram_alias));
    for (i = 0; i < s->ram_alias_count; i++) {
        memory_region_init_alias(&s->ram_alias[i], NULL, "mt7621.ddr-alias",
                                 &s->ram, 0, ram_size);
        memory_region_add_subregion(sysmem, (i + 1) * ram_size,
                                    &s->ram_alias[i]);
    }

    /*
     * L1 D-cache model over RAM and its aliases (MT7621_DCACHE=1).  The
     * aliases only exist below the peripheral window; above it the address
     * space belongs to devices and the boot flash, which are never cached in
     * DRAM even when the guest reaches them through a KSEG0 mapping.
     */
    s->dcache.alias_limit = MIN((hwaddr)(s->ram_alias_count + 1) * ram_size,
                                (hwaddr)MT7621_PERIPH_BASE);
    mt7621_dcache_init(s, ram_size);

    /*
     * The SoC interrupt controller is a MIPS GIC at 0x1fbc0000 (not the
     * RT305x-style INTC of older Ralink parts).  Directing a shared
     * interrupt to CPU line 0 gives Cause IP2, which is the line both
     * Breed and Linux expect for it.
     *
     * Map it after the DDR aliases: those cover 1 GiB..2 GiB, which
     * contains 0x1fbc0000, and a later subregion wins.
     */
    s->gic = qdev_new(TYPE_MIPS_GIC);
    qdev_prop_set_uint32(s->gic, "num-vp", s->ncpus);
    qdev_prop_set_uint32(s->gic, "num-irq", MT7621_GIC_NUM_IRQ);
    /*
     * The SoC's GIC has a 64-bit shared counter: Linux on the real board
     * reports "clocksource: GIC: mask 0xffffffffffffffff", which it derives
     * from GIC_SH_CONFIG.COUNTERBITS.  Keep the generic model's 32-bit
     * default for everything else.
     */
    {
        /*
         * Hardware is 64-bit on the MT7621; the knob exists to A/B other
         * guests (RouterOS) that may not cope with the wide counter.
         */
        const char *cb = getenv("MT7621_GIC_COUNTER_BITS");

        qdev_prop_set_uint32(s->gic, "counter-bits",
                             cb ? (uint32_t)atoi(cb) : 8);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->gic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gic), 0, MT7621_GIC_BASE);

    /*
     * Reset notifiers run in registration order, and the GIC registers its
     * own reset when the device is created, so register ours afterwards to
     * keep the SoC default mapping.
     */
    qemu_register_reset(mt7621_gic_reset, s);

    /*
     * Ethernet.  The frame engine raises GIC shared interrupt 3 and its
     * DMA rings move frames between guest memory and a QEMU net backend,
     * so a tap device on the host is bridged at layer 2.
     */
    mt7621_eth_init(s);

    /* palmbus */
    memory_region_init(&s->periph, NULL, "mt7621.periph",
                       MT7621_PERIPH_SIZE);

    /*
     * Bring-up knob: a catch-all over the whole peripheral window, added
     * first so every real block wins, that names the offsets a guest touches
     * but no model answers.  This is how a missing controller is found
     * without guessing (e.g. "which address is the SSUSB IPPC block on this
     * board's device tree?").
     */
    if (getenv("MT7621_PERIPHSPY")) {
        memory_region_init_io(&s->spy, NULL, &mt7621_spy_ops, s,
                              "mt7621.periphspy", MT7621_PERIPH_SIZE);
        memory_region_add_subregion(&s->periph, 0, &s->spy);
    }

    mt7621_sysctl_reset(s);
    memory_region_init_io(&s->sysctl, NULL, &mt7621_reg_ops, s->sysctl_regs,
                          "mt7621.sysctl", 0x1000);
    memory_region_init_io(&s->rstctl, NULL, &mt7621_rstctl_ops, s,
                          "mt7621.rstctl", 4);
    memory_region_add_subregion(&s->sysctl, SYS_RSTCTL, &s->rstctl);
    mt7621_wdt_init(s);
    mt7621_spi_init(s);
    memory_region_add_subregion(&s->periph, MT7621_SYSCTL_OFF, &s->sysctl);

    mt7621_systick_init(s);

    mt7621_dramc_reset(s);
    memory_region_init_io(&s->dramc, NULL, &mt7621_reg_ops, s->dramc_regs,
                          "mt7621.dramc", 0x1000);
    memory_region_add_subregion(&s->periph, MT7621_DRAMC_OFF, &s->dramc);

    mt7621_nfi_init(s);



    mt7621_cm_init(s);

    uart_irq = qdev_get_gpio_in(s->gic, MT7621_UART0_IRQ);
    serial_mm_init(&s->periph, MT7621_UART0_OFF, 2, uart_irq,
                   MT7621_UART_BAUD_BASE, serial_hd(0),
                   DEVICE_LITTLE_ENDIAN);
    /*
     * The second UART gets a TX-only stand-in rather than a second chardev
     * frontend (qemu_chr_fe_init refuses to share 'ser0').  RouterOS can move
     * its console to 0x1e000e00, and when that output has nowhere to go the
     * kernel looks silent.  Bytes written to its THR are mirrored into the
     * host log with a "[uart1] " prefix.  MT7621_UART1=0 disables the mirror.
     */
    if (mt7621_flag("MT7621_UART1")) {
        mt7621_uart1_init(s);
    }

    mt7621_crypto_init(s);
    mt7621_usb_init(s);

    memory_region_add_subregion(sysmem, MT7621_PERIPH_BASE, &s->periph);

    {
        const char *e = getenv("MT7621_ENTRY");
        const char *l = getenv("MT7621_LOAD");

        if (e) {
            s->entry = strtoul(e, NULL, 0);
        }
        if (l) {
            s->load_addr = strtoul(l, NULL, 0);
        }
    }

    /* Boot image */
    if (machine->kernel_filename) {
        uint8_t magic[4] = { 0 };
        int mfd = open(machine->kernel_filename, O_RDONLY | O_BINARY);

        s->image_file = g_strdup(machine->kernel_filename);
        if (mfd >= 0) {
            if (read(mfd, magic, 4) != 4) {
                magic[0] = 0;
            }
            close(mfd);
        }
        /*
         * A raw ELF here is a kernel (e.g. an OpenWrt initramfs image), not a
         * flash dump: load it like a bootloader would and run it.  Handy for
         * separating machine-model problems from bootloader ones.
         */
        if (ldl_be_p(magic) == 0x7f454c46) {
            mt7621_load_image(s, machine->kernel_filename);
        } else if (s->board == MT7621_BOARD_RB750GR3) {
            mt7621_nor_load(s, machine->kernel_filename);
        } else {
            mt7621_nand_load(s, machine->kernel_filename);
            mt7621_nand_attach(s);
            mt7621_load_image(s, machine->kernel_filename);
        }
    } else {
        error_report("mt7621: no boot image (-kernel) supplied");
        exit(1);
    }
}


static void mt7621_set_board(Object *obj, const char *value,
                             Error **errp)
{
    MT7621State *s = MT7621_MACHINE(obj);

    g_free(s->board_name);
    s->board_name = g_strdup(value);
}

/*
 * Default RDUID value: a fixed, plausible Winbond unique ID.  Boards that
 * care about matching a real chip's identity override it with
 * -M mt7621,spi-uid=0x...
 */
#define MT7621_SPI_UID_DEFAULT 0xe4c1a75b3d9206f8ULL

static void mt7621_machine_instance_init(Object *obj)
{
    MT7621State *s = MT7621_MACHINE(obj);
    static const uint8_t default_id[8] = { 0xec, 0xf1, 0x00, 0x15,
                                           0x40, 0x00, 0x00, 0x00 };

    memcpy(s->nand_id, default_id, sizeof(default_id));
    s->spi_uid = MT7621_SPI_UID_DEFAULT;
}

/*
 * -M mt7621,nand-id=ecf1001540000000  -- the die's ID bytes, so a board can
 * carry an ESMT/Winbond/Macronix part instead of the default Samsung.
 */
static void mt7621_set_nand_id(Object *obj, const char *value, Error **errp)
{
    MT7621State *s = MT7621_MACHINE(obj);
    const char *p = value;
    int i;

    for (i = 0; i < 8; i++) {
        char byte[3];
        int n = 0;

        while (*p && !g_ascii_isxdigit(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        byte[n++] = *p++;
        if (*p && g_ascii_isxdigit(*p)) {
            byte[n++] = *p++;
        }
        byte[n] = 0;
        s->nand_id[i] = (uint8_t)strtoul(byte, NULL, 16);
    }
    s->nand_id_set = true;
}

static void mt7621_set_spi_uid(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    MT7621State *s = MT7621_MACHINE(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    s->spi_uid = value;
}

static void mt7621_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "board", NULL, mt7621_set_board);
    object_class_property_add_str(oc, "nand-id", NULL, mt7621_set_nand_id);
    object_class_property_set_description(oc, "board",
        "board preset: lg-gapm7200 (parallel NAND) or rb750gr3 (SPI NOR)");

    object_class_property_add(oc, "spi-uid", "uint64", NULL,
                              mt7621_set_spi_uid, NULL, NULL);
    object_class_property_set_description(oc, "spi-uid",
        "Winbond SPI NOR 64-bit unique ID reported by command 0x4b");

    mc->desc = "MediaTek MT7621 (MIPS 1004Kc)";
    mc->init = mt7621_machine_init;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("1004Kc");
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "mt7621.ram";
    mc->max_cpus = MT7621_MAX_CPUS;
    mc->default_cpus = 4;
}

static const TypeInfo mt7621_machine_type = {
    .name = TYPE_MT7621_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(MT7621State),
    .instance_init = mt7621_machine_instance_init,
    .class_init = mt7621_machine_class_init,
};

static void mt7621_machine_register_types(void)
{
    type_register_static(&mt7621_machine_type);
}

type_init(mt7621_machine_register_types);
