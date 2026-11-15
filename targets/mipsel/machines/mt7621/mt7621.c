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

static void mt7621_sysctl_defaults(MT7621State *s)
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

        s->sysctl_regs[SYS_SYSCFG0 / 4] = cfg0
            ? (uint32_t)strtoul(cfg0, NULL, 0)
            : (0x00000100 | s->chip_mode);
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

static void mt7621_dramc_defaults(MT7621State *s)
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
     * FBDIV 69 / PREDIV 1 gives (69 + 1) * 25 MHz / 2 = 875 MHz, the clock
     * the GIC counter is modelled at (mips_gictimer_set_freq) and the value
     * RouterBOOT reports for the RB750Gr3.
     */
    {
        const char *pll = getenv("MT7621_CPU_PLL");

        s->dramc_regs[0x648 / 4] = pll ? (uint32_t)strtoul(pll, NULL, 0)
                                       : ((21 << 4) | (0 << 12)); /* 880MHz */
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
    /* Diagnostic: report every architectural exception the guest takes. */
    if (getenv("MT7621_EXCLOG") && s->cpu) {
        static int exc_budget = 4096;
        uint32_t cause = env->CP0_Cause;
        unsigned code = (cause >> 2) & 0x1f;

        /* TLB misses (1,2,3,8) are normal user-space traffic and would eat
         * the budget long before the interesting fault; only report the
         * codes that mean something went wrong. */
        bool interesting = (code == 4 || code == 5 || code == 10 ||
                            code == 11 || code == 12 || code == 13);
        if (code && interesting && exc_budget-- > 0) {
            fprintf(stderr, "exc: code=%u pc=%08x epc=%08x badvaddr=%08x "
                    "status=%08x cause=%08x\n", code,
                    (uint32_t)env->active_tc.PC, (uint32_t)env->CP0_EPC,
                    (uint32_t)env->CP0_BadVAddr, env->CP0_Status, cause);
            /* Reserved Instruction: show the word the guest choked on and
             * the preceding few, which names the instruction and its delay
             * slot context without needing a guest-side disassembler. */
            if (code == 11) {
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

    timer_mod(s->poweroff_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              (getenv("MT7621_ENTRYLOG") || getenv("MT7621_EXCLOG") ? 1 : 250));
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

static void mt7621_reset(void *opaque)
{
    MT7621State *s = opaque;
    CPUMIPSState *env = &s->cpu->env;

    if (getenv("MT7621_RSTLOG")) {
        fprintf(stderr, "mt7621: machine reset, cpu pc=%08x ra=%08x\n",
                env->active_tc.PC, env->active_tc.gpr[31]);
    }

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
    }
    if (getenv("MT7621_CFG3")) {
        env->CP0_Config3 = strtoul(getenv("MT7621_CFG3"), NULL, 0);
    }

    if (s->wdt_timer) {
        timer_del(s->wdt_timer);
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
     * The board boots this firmware at Dl=4, so its allocation of that buffer
     * is 32-byte aligned; this model's lands at 0x8fddcd58 (8 bytes into the
     * line).  Until the difference in the pre-Ethernet allocation sequence is
     * measured against the board, lg-gapm7200 reports Dl=2.  MT7621_DL
     * overrides it for experiments (MT7621_DL=4 reproduces the hardware value
     * and the failure).
     */
    if (s->board == MT7621_BOARD_LG_GAPM7200) {
        env->CP0_Config1 = (env->CP0_Config1 & ~(7u << 10)) | (2u << 10);
    }
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

    mt7621_dcache_reset(s);

    /* Everything the guest programmed is gone after a real reset. */
    mt7621_eth_dev_reset(s);
    memset(s->nfi_regs, 0, sizeof(s->nfi_regs));
    memset(s->spi_regs, 0, sizeof(s->spi_regs));
    s->spi_read_mode = false;
    s->spi_addr = 0;
    s->nfi_fifo_len = 0;
    s->nfi_fifo_pos = 0;
    s->nfi_col = 0;
    s->nfi_row = 0;
    s->nfi_served = 0;
    s->nfi_last_cmd = 0;
    s->nfi_addr_len = 0;
    s->nfi_addr_pending = 0;
    s->nfi_wbuf_len = 0;
    s->nfi_writing = false;
    mt7621_sysctl_defaults(s);
    mt7621_dramc_defaults(s);
    mt7621_cm_defaults(s);
    memset(s->systick_regs, 0, sizeof(s->systick_regs));

    /*
     * Linux scribbled over the RAM the bootloader lived in, so the BootROM
     * has to load it again before the CPU is released.
     */
    if (s->image_file && (!s->spi_nor || s->elf_kernel)) {
        mt7621_load_image(s, s->image_file);
    }

    /* Malta does the same when a -kernel image is supplied. */
    env->CP0_Status &= ~(1 << 2); /* CP0St_ERL */
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

static void mt7621_machine_init(MachineState *machine)
{
    MT7621State *s = MT7621_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    qemu_irq uart_irq;
    uint64_t ram_size = machine->ram_size;
    int i;

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
         * Run the GIC shared counter at the real device's CPU clock (875 MHz,
         * the rate RouterBOOT reports and OpenWrt programs its clockevents
         * against) for the whole machine, instead of the 25 MHz reference the
         * old hardcoded constant used.
         */
        mips_gictimer_set_freq(875 * 1000 * 1000);
    }


    mt7621_dump_init(s);

    s->poweroff_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                     mt7621_poweroff_poll, s);
    timer_mod(s->poweroff_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              (getenv("MT7621_ENTRYLOG") || getenv("MT7621_EXCLOG") ? 1 : 250));

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

    /* L1 D-cache model over RAM and its aliases (MT7621_DCACHE=1) */
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

    mt7621_sysctl_defaults(s);
    memory_region_init_io(&s->sysctl, NULL, &mt7621_reg_ops, s->sysctl_regs,
                          "mt7621.sysctl", 0x1000);
    memory_region_init_io(&s->rstctl, NULL, &mt7621_rstctl_ops, s,
                          "mt7621.rstctl", 4);
    memory_region_add_subregion(&s->sysctl, SYS_RSTCTL, &s->rstctl);
    mt7621_wdt_init(s);
    mt7621_spi_init(s);
    memory_region_add_subregion(&s->periph, MT7621_SYSCTL_OFF, &s->sysctl);

    mt7621_systick_init(s);

    mt7621_dramc_defaults(s);
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
