/*
 * MediaTek MT7621 -- the system-controller watchdog (timer 1 of the Ralink
 * timer block at 0x1e000100).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "mt7621.h"

#define MT7621_WDT_OFF          0x100
#define MT7621_WDT_SIZE         0x100
#define WDT_TMRSTAT             0x00
#define WDT_TMR1CTL             0x20
#define WDT_TMR1LOAD            0x24
#define WDT_TMRSTAT_RESTART     (1u << 9)
#define WDT_TMR1CTL_ENABLE      (1u << 7)

/*
 * Watchdog reset: the timer block is cleared and any armed bark is dropped,
 * so a reboot cannot inherit the previous boot's countdown.  SYS_RSTSTAT is
 * handled by mt7621_sysctl_defaults(), which runs before this and latches the
 * recorded cause.
 */
void mt7621_wdt_reset(MT7621State *s)
{
    if (s->wdt_timer) {
        timer_del(s->wdt_timer);
    }
    memset(&s->sysctl_regs[MT7621_WDT_OFF / 4], 0, MT7621_WDT_SIZE);
    s->wdt_rst_cause = false;
}

static void mt7621_wdt_arm(MT7621State *s)
{
    uint32_t ctl = s->sysctl_regs[(MT7621_WDT_OFF + WDT_TMR1CTL) / 4];
    uint32_t load = s->sysctl_regs[(MT7621_WDT_OFF + WDT_TMR1LOAD) / 4];
    uint32_t prescale = (ctl >> 16) & 0xffff;
    int64_t ns;

    if (!(ctl & WDT_TMR1CTL_ENABLE) || load == 0) {
        timer_del(s->wdt_timer);
        return;
    }
    if (prescale == 0) {
        prescale = 1;
    }
    ns = (int64_t)load * prescale * 1000;      /* ticks of prescale usec */
    /* test hook: force a short bark to exercise the reset path */
    if (getenv("MT7621_WDT_TEST_MS")) {
        ns = strtoull(getenv("MT7621_WDT_TEST_MS"), NULL, 0) * 1000000LL;
    }
    timer_mod(s->wdt_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
    if (getenv("MT7621_WDTLOG")) {
        fprintf(stderr, "wdt: arm t=%lldms ctl=%08x load=%08x prescale=%u "
                "ns=%lld\n",
                (long long)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000),
                ctl, load, prescale, (long long)ns);
    }
}

static void mt7621_wdt_expire(void *opaque)
{
    MT7621State *s = opaque;

    /*
     * The watchdog resets the SoC when it is enabled and the guest stops
     * petting it - that is the hardware contract, and RouterOS relies on it:
     * /system reboot stops petting and waits for the board to reset.  The
     * installed system pets every 2 s with a 15 s timeout (MT7621_WDTLOG), so
     * a healthy guest never trips it.
     *
     * MT7621_NO_WDT_RESET=1 (or MT7621_NO_RESET=1) turns the reset into a
     * warning for A/B; it used to be the default, which made /system reboot
     * hang forever instead of resetting.
     */
    warn_report("mt7621: watchdog expired at t=%lldms%s",
                (long long)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000),
                (!mt7621_flag("MT7621_NO_WDT_RESET") &&
                 !mt7621_flag("MT7621_NO_RESET")) ? ", resetting" : "");
    if (mt7621_flag("MT7621_NO_WDT_RESET") || mt7621_flag("MT7621_NO_RESET")) {
        return;
    }
    s->wdt_rst_cause = true;
    {
        mt7621_eth_quiesce(s);
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static uint64_t mt7621_wdt_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    uint32_t val = s->sysctl_regs[(MT7621_WDT_OFF +
                                   (addr & (MT7621_WDT_SIZE - 1))) / 4];

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: wdt read  %03x -> %08x\n",
                      (unsigned)(addr & (MT7621_WDT_SIZE - 1)), val);
    }
    return val;
}

static void mt7621_wdt_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    MT7621State *s = opaque;
    hwaddr off = addr & (MT7621_WDT_SIZE - 1);

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: wdt write %03x <- %08x\n",
                      (unsigned)off, (uint32_t)val);
    }
    if (getenv("MT7621_WDTLOG")) {
        fprintf(stderr, "wdt: write %03x <- %08x (pc=%08x)\n",
                (unsigned)off, (uint32_t)val,
                s->cpu ? (uint32_t)s->cpu->env.active_tc.PC : 0);
    }
    s->sysctl_regs[(MT7621_WDT_OFF + off) / 4] = val;

    switch (off) {
    case WDT_TMRSTAT:
        if (val & WDT_TMRSTAT_RESTART) {
            mt7621_wdt_arm(s);
        }
        break;
    case WDT_TMR1CTL:
    case WDT_TMR1LOAD:
        mt7621_wdt_arm(s);
        break;
    }
}

static const MemoryRegionOps mt7621_wdt_ops = {
    .read = mt7621_wdt_read,
    .write = mt7621_wdt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};


void mt7621_wdt_init(MT7621State *s)
{
    s->wdt_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mt7621_wdt_expire, s);
    memory_region_init_io(&s->wdt, NULL, &mt7621_wdt_ops, s,
                          "mt7621.wdt", MT7621_WDT_SIZE);
    memory_region_add_subregion(&s->sysctl, MT7621_WDT_OFF, &s->wdt);
}
