/*
 * MediaTek MT7621 -- ralink,cevt-systick: a 16-bit up-counter at 50 kHz with a
 * compare register, on shared interrupt 5.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "mt7621.h"

#define MT7621_SYSTICK_OFF      0x00000500
#define MT7621_SYSTICK_IRQ      5

#define MT7621_SYSTICK_HZ       50000
#define MT7621_SYSTICK_PERIOD   50      /* 1 ms at 50 kHz */
#define SYSTICK_CONFIG          0x00
#define SYSTICK_COMPARE         0x04
#define SYSTICK_COUNT           0x08
#define SYSTICK_CFG_CNT_EN      (1u << 0)
#define SYSTICK_CFG_EXT_STK_EN  (1u << 1)

/*
 * ralink,cevt-systick: 16-bit up-counter at 50 kHz with a compare register.
 * Linux uses it as a oneshot clockevent, so the only event we need to model
 * is "counter reached COMPARE".
 */
static uint32_t mt7621_systick_count(MT7621State *s, int64_t now)
{
    int64_t elapsed;

    if (!(s->systick_regs[SYSTICK_CONFIG / 4] & SYSTICK_CFG_CNT_EN)) {
        return s->systick_base_count & 0xffff;
    }

    elapsed = now - s->systick_base_ns;
    if (elapsed < 0) {
        elapsed = 0;
    }
    return (s->systick_base_count +
            (uint32_t)(elapsed * MT7621_SYSTICK_HZ / NANOSECONDS_PER_SECOND))
           & 0xffff;
}

/*
 * systick reset: registers, counter anchor, both timers and the GIC line.
 * mt7621_systick_boot_handover() then re-creates the state the BootROM leaves
 * (counter running, compare at 0xffff, interrupt enabled).
 */
void mt7621_systick_reset(MT7621State *s)
{
    if (s->systick_timer) {
        timer_del(s->systick_timer);
    }
    if (s->systick_pulse_off) {
        timer_del(s->systick_pulse_off);
    }
    memset(s->systick_regs, 0, sizeof(s->systick_regs));
    s->systick_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->systick_base_count = 0;
    s->systick_pending = false;
    if (s->systick_irq) {
        qemu_irq_lower(s->systick_irq);
    }
}

void mt7621_systick_set_pending(MT7621State *s, bool pending)
{
    s->systick_pending = pending;
    if (getenv("MT7621_NO_SYSTICK")) {
        return;                     /* bring-up probe: no GIC input at all */
    }
    qemu_set_irq(s->systick_irq, pending);
}

void mt7621_systick_rearm(MT7621State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t count, compare, delta;

    timer_del(s->systick_timer);
    if (!(s->systick_regs[SYSTICK_CONFIG / 4] & SYSTICK_CFG_CNT_EN)) {
        return;
    }

    count = mt7621_systick_count(s, now);
    compare = s->systick_regs[SYSTICK_COMPARE / 4] & 0xffff;
    delta = (compare - count) & 0xffff;
    if (!delta) {
        delta = 0x10000;
    }

    /* Re-anchor so the counter keeps its current value. */
    s->systick_base_count = count;
    s->systick_base_ns = now;
    timer_mod(s->systick_timer,
              now + (int64_t)delta * NANOSECONDS_PER_SECOND / MT7621_SYSTICK_HZ);
}

/*
 * The 16-bit counter free-runs and matches COMPARE once per wrap, so the
 * device keeps ticking whether or not the guest re-arms it.  The interrupt
 * is asserted as a short pulse: the real one stays up until the handler
 * writes COMPARE, but a pulse keeps a guest that never acknowledges it
 * from wedging the CPU in the ISR.
 */
static void mt7621_systick_expire(void *opaque)
{
    MT7621State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->systick_base_count = s->systick_regs[SYSTICK_COMPARE / 4] & 0xffff;
    s->systick_base_ns = now;
    mt7621_systick_set_pending(s, true);
    timer_mod(s->systick_pulse_off, now + 20000);
    /* Keep a steady tick for firmware that arms COMPARE only once. */
    s->systick_regs[SYSTICK_COMPARE / 4] =
        (s->systick_base_count + MT7621_SYSTICK_PERIOD) & 0xffff;
    mt7621_systick_rearm(s);
}

static void mt7621_systick_pulse_release(void *opaque)
{
    MT7621State *s = opaque;

    mt7621_systick_set_pending(s, false);
}

static uint64_t mt7621_systick_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t val;

    switch (addr & 0xf) {
    case SYSTICK_COUNT:
        val = mt7621_systick_count(s, now);
        break;
    default:
        val = s->systick_regs[(addr & 0xf) >> 2];
        break;
    }

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: systick read %08" HWADDR_PRIx
                      " -> %08x\n", addr, val);
    }
    return val;
}

static void mt7621_systick_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    MT7621State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: systick write %08" HWADDR_PRIx
                      " <- %08" PRIx64 "\n", addr, val);
    }

    switch (addr & 0xf) {
    case SYSTICK_CONFIG:
        s->systick_base_count = mt7621_systick_count(s, now);
        s->systick_base_ns = now;
        s->systick_regs[SYSTICK_CONFIG / 4] = val;
        if (!(val & SYSTICK_CFG_CNT_EN)) {
            timer_del(s->systick_timer);
            mt7621_systick_set_pending(s, false);
        } else {
            mt7621_systick_rearm(s);
        }
        break;
    case SYSTICK_COMPARE:
        s->systick_regs[SYSTICK_COMPARE / 4] = val;
        /* The compare write both clears the match and arms the next one. */
        mt7621_systick_set_pending(s, false);
        mt7621_systick_rearm(s);
        break;
    default:
        s->systick_regs[(addr & 0xf) >> 2] = val;
        break;
    }
}

static const MemoryRegionOps mt7621_systick_ops = {
    .read = mt7621_systick_read,
    .write = mt7621_systick_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};


void mt7621_systick_init(MT7621State *s)
{
    memset(s->systick_regs, 0, sizeof(s->systick_regs));
    s->systick_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->systick_base_count = 0;
    s->systick_irq = qdev_get_gpio_in(s->gic, MT7621_SYSTICK_IRQ);
    s->systick_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    mt7621_systick_expire, s);
    s->systick_pulse_off = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        mt7621_systick_pulse_release, s);
    memory_region_init_io(&s->systick, NULL, &mt7621_systick_ops, s,
                          "mt7621.systick", 0x10);
    memory_region_add_subregion(&s->periph, MT7621_SYSTICK_OFF, &s->systick);
}

/*
 * The MT7621 BootROM hands over with the 50 kHz systick already running and
 * its compare interrupt enabled; Breed only hooks GIC IRQ 5 and relies on
 * that.  Reproduce the hand-over state.
 */
void mt7621_systick_boot_handover(MT7621State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!s->systick_timer) {
        return;
    }
    s->systick_regs[SYSTICK_CONFIG / 4] =
        SYSTICK_CFG_CNT_EN | SYSTICK_CFG_EXT_STK_EN;
    s->systick_regs[SYSTICK_COMPARE / 4] = 0xffff;
    s->systick_base_count = 0;
    s->systick_base_ns = now;
    mt7621_systick_set_pending(s, false);
    mt7621_systick_rearm(s);
}
