/*
 * MediaTek MT7621 -- the shared register-file window (sysctl, dramc) and the
 * GPIO semantics that live inside it.
 *
 * Both windows use one MemoryRegionOps over a uint32_t array: the sysctl page
 * at 0x1e000000 and the DRAM controller page.  The GPIO block is part of the
 * sysctl page, so its read/write special cases belong here rather than in the
 * machine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "mt7621.h"

/*
 * The GPIO block lives inside the system controller.  Bits set in the
 * direction register drive the pin, every other bit is an input.  The board
 * wires its buttons active-low with pull-ups, so an *idle* input reads 1;
 * echoing the output latch back for input bits makes an unconnected pin look
 * permanently pressed (Breed then aborts autoboot with "button press").
 */
#define SYS_GPIO_CTRL           0x600
#define SYS_GPIO_DATA           0x620
#define SYS_GPIO_DSET           0x634
#define SYS_GPIO_DCLR           0x638
#define SYS_GPIO_MASK           0x00ffffffu

static uint32_t mt7621_gpio_inputs(uint32_t *regs)
{
    uint32_t dir = regs[SYS_GPIO_CTRL / 4] & SYS_GPIO_MASK;
    uint32_t out = regs[SYS_GPIO_DATA / 4] & dir;

    /* Input bits read their pulled-up idle level. */
    return out | (~dir & SYS_GPIO_MASK);
}

static uint64_t mt7621_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t *regs = opaque;
    uint32_t off = addr & 0xfff;
    uint32_t val = regs[off >> 2];

    if (off == SYS_GPIO_DATA) {
        val = mt7621_gpio_inputs(regs);
    }

    /*
     * The block is 32-bit oriented, but the SoC happily answers byte and
     * halfword loads (MikroTik's 3.3.5 kernel reads the chip revision with
     * an lhu).  Shifting here keeps those from turning into a bus error.
     */
    if (size < 4) {
        val >>= 8 * (off & 3);
    }

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: reg read  %08" HWADDR_PRIx
                      " -> %08x\n", addr, val);
    }
    return val;
}

static void mt7621_reg_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    uint32_t *regs = opaque;
    uint32_t off = addr & 0xfff;

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: reg write %08" HWADDR_PRIx
                      " <- %08" PRIx64 "\n", addr, val);
    }

    if (size < 4) {
        /* Merge the written lane into the current register value. */
        unsigned shift = 8 * (off & 3);
        uint32_t mask = (size == 2 ? 0xffffu : 0xffu) << shift;

        val = (regs[off >> 2] & ~mask) | (((uint32_t)val << shift) & mask);
        regs[off >> 2] = val;
        return;
    }

    switch (off) {
    case SYS_GPIO_DATA:
    {
        /* Only pins configured as outputs latch a value. */
        uint32_t dir = regs[SYS_GPIO_CTRL / 4] & SYS_GPIO_MASK;

        regs[SYS_GPIO_DATA / 4] =
            (regs[SYS_GPIO_DATA / 4] & ~dir) | ((uint32_t)val & dir);
        return;
    }
    case SYS_GPIO_DSET:
        regs[SYS_GPIO_DATA / 4] |= (uint32_t)val;
        return;
    case SYS_GPIO_DCLR:
        regs[SYS_GPIO_DATA / 4] &= ~(uint32_t)val;
        return;
    default:
        break;
    }

    regs[off >> 2] = val;
}


const MemoryRegionOps mt7621_reg_ops = {
    .read = mt7621_reg_read,
    .write = mt7621_reg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};
