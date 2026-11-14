/*
 * MediaTek MT7621 -- the second UART (0x1e000e00), TX-only.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "mt7621.h"

#define MT7621_UART1_OFF        0x00000e00

/*
 * TX-only mirror of the second UART.  Enough of a 16550 for a polled console:
 * LSR always reports THRE|TEMT, everything else reads as zero, and writes to
 * THR end up in the host log.  RouterOS uses this port when it moves the
 * console off uartlite@c00, and without a sink the kernel's printk vanishes.
 */
static uint64_t mt7621_uart1_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    unsigned reg = (addr & 0xff) >> 2;

    switch (reg) {
    case 5:                             /* LSR */
        (void)s;
        return 0x60;                    /* THRE | TEMT */
    case 6:                             /* MSR */
        return 0xb0;                    /* CTS | DSR | DCD */
    default:
        return 0;
    }
}

static void mt7621_uart1_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MT7621State *s = opaque;
    unsigned reg = (addr & 0xff) >> 2;
    static bool at_line_start = true;

    switch (reg) {
    case 0:                             /* THR (ignoring DLAB) */
        if (at_line_start) {
            fputs("[uart1] ", stderr);
            at_line_start = false;
        }
        fputc((int)(val & 0xff), stderr);
        if ((val & 0xff) == '\n') {
            at_line_start = true;
        }
        fflush(stderr);
        break;
    case 1:                             /* IER */
    case 2:                             /* FCR */
    case 3:                             /* LCR */
    case 4:                             /* MCR */
        break;
    default:
        break;
    }
    (void)s;
}

static const MemoryRegionOps mt7621_uart1_ops = {
    .read = mt7621_uart1_read,
    .write = mt7621_uart1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * RouterOS can move its console to 0x1e000e00; give that port a sink.
 * MT7621_UART1=0 disables it.
 */
void mt7621_uart1_init(MT7621State *s)
{
    memory_region_init_io(&s->uart1, NULL, &mt7621_uart1_ops, s,
                          "mt7621.uart1", 0x100);
    memory_region_add_subregion(&s->periph, MT7621_UART1_OFF, &s->uart1);
}
