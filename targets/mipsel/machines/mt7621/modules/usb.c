/*
 * MediaTek MT7621 -- the SSUSB block: the xHCI MAC at 0x1e1c0000 and the
 * IPPC port power/clock controller at 0x1e1d0700.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "mt7621.h"

/*
 * Two register blocks, one device.  xhci-mtk powers the ports through the
 * IPPC block, waits for the PLL/reset status bits, and only then initialises
 * the xHCI MAC, resets it and starts it; modelling one without the other only
 * moves the probe failure one step further:
 *
 *   - IPPC only: the "clocks are not stable" poll passes and the probe then
 *     fails in xhci_init, because HCSPARAMS/HCCPARAMS read back as zero.
 *   - MAC only: the probe never gets that far, because ip_pw_sts1 reads zero
 *     and the 20 ms timeout expires first.
 *
 * So the model covers both, to the point xhci-mtk and xhci-hcd need:
 *
 *   - IPPC: the control registers (plain storage), ip_pw_sts1 reporting the
 *     PLL stable and the four reset deasserted bits, and ip_xhci_cap telling
 *     the driver how many USB2/USB3 ports to power.
 *   - xHCI: a capability register file (CAPLENGTH, version, port/slot counts,
 *     HCCPARAMS with the extended-capability pointer), the operational
 *     registers the driver resets and starts (USBCMD/USBSTS with the
 *     HCHalted bit tracking the Run/Stop bit, PAGESIZE, CRCR, DCBAAP,
 *     CONFIG), three PORTSC registers, the interrupter set (plain storage),
 *     and a Supported Protocol extended-capability pair describing the two
 *     USB 2.0 ports and the one USB 3.0 port.
 *
 * No USB device is attached to this machine: every port reports powered with
 * no connection (CCS clear), which is what the board's unconnected USB pins
 * look like, so the roothubs come up empty and nothing is ever enumerated.
 * Commands, events, doorbells and transfers are therefore not modelled - the
 * controller is idle with an empty command ring.
 *
 * Port counts come from the SoC: MT7621A's SSUSB implements two USB 2.0 ports
 * and one USB 3.0 port, and RouterOS's device tree leaves the counts to these
 * registers (IPPC ip_xhci_cap), which is why they are part of the model rather
 * than a compile-time guess in the driver.
 */

/* --- IPPC ----------------------------------------------------------------- */

#define MT7621_IPPC_OFF         0x001d0700
#define MT7621_IPPC_SIZE        0x00000100

#define IPPC_CTR0               0x00
#define IPPC_CTR1               0x04
#define IPPC_CTR2               0x08
#define IPPC_CTR3               0x0c
#define IPPC_STS1               0x10
#define IPPC_STS2               0x14
#define IPPC_XHCI_CAP           0x24
#define IPPC_U3_CTRL_P          0x30    /* 4 x 8 bytes */
#define IPPC_U2_CTRL_P          0x50    /* 5 x 8 bytes */
#define IPPC_U2_PHY_PLL         0x7c

/* ip_pw_sts1: what the driver compares against (drivers/usb/host/xhci-mtk.c) */
#define STS1_SYSPLL_STABLE      (1u << 0)
#define STS1_REF_RST            (1u << 8)
#define STS1_SYS125_RST         (1u << 10)
#define STS1_XHCI_RST           (1u << 11)
#define STS1_U3_MAC_RST         (1u << 16)
#define STS1_IP_SLEEP_STS       (1u << 30)

/*
 * ip_xhci_cap: the driver reads the port counts from it (bits 7:0 = USB3
 * ports, bits 15:8 = USB2 ports) and powers exactly that many ports.
 */
#define XHCI_CAP_U3_PORTS       1
#define XHCI_CAP_U2_PORTS       2

/* --- xHCI MAC ------------------------------------------------------------- */

#define MT7621_USB_OFF          0x001c0000
#define MT7621_USB_SIZE         0x00001000

#define XHCI_CAPLENGTH          0x00        /* byte 0; HCIVERSION at 0x02 */
#define XHCI_HCSPARAMS1         0x04
#define XHCI_HCSPARAMS2         0x08
#define XHCI_HCSPARAMS3         0x0c
#define XHCI_HCCPARAMS1         0x10
#define XHCI_DBOFF              0x14
#define XHCI_RTSOFF             0x18
#define XHCI_HCCPARAMS2         0x1c

#define XHCI_CAP_BASE           0x40        /* CAPLENGTH: operational regs here */
#define XHCI_USBCMD             0x40
#define XHCI_USBSTS             0x44
#define XHCI_PAGESIZE           0x48
#define XHCI_DNCTRL             0x4c
#define XHCI_CRCR               0x50
#define XHCI_DCBAAP             0x58
#define XHCI_CONFIG             0x60
#define XHCI_MFINDEX            0x64
#define XHCI_PORTSC(n)          (0x80 + 0x10 * ((n) - 1))

#define USB_CMD_RS              (1u << 0)
#define USB_CMD_HCRST           (1u << 1)

#define USB_STS_HCH             (1u << 0)
#define USB_STS_EINT            (1u << 3)
#define USB_STS_PCD             (1u << 4)
#define USB_STS_CNR             (1u << 11)

#define PORTSC_CCS              (1u << 0)
#define PORTSC_PED              (1u << 1)
#define PORTSC_PP               (1u << 9)
#define PORTSC_WRITE_MASK       0x0e00c0e0u   /* the RW1C/control half */

#define XHCI_MAX_PORTS          (XHCI_CAP_U2_PORTS + XHCI_CAP_U3_PORTS)
#define XHCI_MAX_SLOTS          8
#define XHCI_MAX_INTRS          1

/* extended capabilities: two Supported Protocol entries */
#define XHCI_XECP_OFFSET        0x140
#define XHCI_XECP_DWORDS        (XHCI_XECP_OFFSET / 4)

#define EXT_CAP_PROTOCOL        0x02
#define EXT_CAP_ID(x)           ((x) & 0xff)
#define EXT_CAP_NEXT(x)         (((x) >> 8) & 0xff)
#define EXT_CAP_REV(x)          (((x) >> 16) & 0xffff)
#define EXT_CAP_PORT_OFF(x)     ((x) & 0xff)
#define EXT_CAP_PORT_COUNT(x)   (((x) >> 8) & 0xff)

void mt7621_usb_reset(MT7621State *s)
{
    memset(s->usb_regs, 0, sizeof(s->usb_regs));
    memset(s->ippc_regs, 0, sizeof(s->ippc_regs));

    s->usb_run = false;

    /* Capability block.  Version 1.00, three ports, one interrupter. */
    s->usb_regs[XHCI_CAPLENGTH / 4] = XHCI_CAP_BASE | (0x0100 << 16);
    s->usb_regs[XHCI_HCSPARAMS1 / 4] = XHCI_MAX_PORTS |
                                       ((uint32_t)XHCI_MAX_INTRS << 8) |
                                       ((uint32_t)XHCI_MAX_SLOTS << 16);
    s->usb_regs[XHCI_HCSPARAMS2 / 4] = 0;
    s->usb_regs[XHCI_HCSPARAMS3 / 4] = 0;
    /*
     * AC64 (bit 0) because the SoC is 32-bit but the controller masters
     * 64-bit addresses (the resource size is the 4 KiB MAC window; the rings
     * live in DRAM), PPC (bit 3) as the port power bit is modelled, and XECP
     * pointing at the Supported Protocol entries.
     */
    s->usb_regs[XHCI_HCCPARAMS1 / 4] = 0x1 | 0x8 | (XHCI_XECP_DWORDS << 16);
    s->usb_regs[XHCI_DBOFF / 4] = 0x0800;
    s->usb_regs[XHCI_RTSOFF / 4] = 0x0900;
    s->usb_regs[XHCI_HCCPARAMS2 / 4] = 0;

    s->usb_regs[XHCI_PAGESIZE / 4] = 1;         /* 4 KiB */

    /*
     * Halted until the driver sets Run/Stop, and Controller Not Ready clear:
     * xhci_reset() requires STS_HALT before it will reset the controller and
     * then waits for STS_CNR to drop.
     */
    s->usb_regs[XHCI_USBSTS / 4] = USB_STS_HCH;

    /*
     * Ports: powered, nothing plugged in.  CCS stays clear, so the roothub
     * reports an empty bus; PP is what the driver checks after powering up.
     */
    for (unsigned i = 1; i <= XHCI_MAX_PORTS; i++) {
        s->usb_regs[XHCI_PORTSC(i) / 4] = PORTSC_PP;
    }

    /* USB 2.0: ports 1..2, USB 3.0: port 3.  Last entry terminates the list. */
    s->usb_regs[(XHCI_XECP_OFFSET + 0x00) / 4] =
        EXT_CAP_PROTOCOL | (0x04 << 8) | (0x00 << 16) | (0x02 << 24);
    s->usb_regs[(XHCI_XECP_OFFSET + 0x04) / 4] = 0x20425355;   /* "USB " */
    s->usb_regs[(XHCI_XECP_OFFSET + 0x08) / 4] =
        1 | ((uint32_t)XHCI_CAP_U2_PORTS << 8);
    s->usb_regs[(XHCI_XECP_OFFSET + 0x0c) / 4] = 0;

    s->usb_regs[(XHCI_XECP_OFFSET + 0x10) / 4] =
        EXT_CAP_PROTOCOL | (0x00 << 8) | (0x00 << 16) | (0x03 << 24);
    s->usb_regs[(XHCI_XECP_OFFSET + 0x14) / 4] = 0x20505353;   /* "SSP " */
    s->usb_regs[(XHCI_XECP_OFFSET + 0x18) / 4] =
        (1 + XHCI_CAP_U2_PORTS) | ((uint32_t)XHCI_CAP_U3_PORTS << 8);
    s->usb_regs[(XHCI_XECP_OFFSET + 0x1c) / 4] = 0;

    /* IPPC: the PLL is up and the four domains are out of reset. */
    s->ippc_regs[IPPC_STS1 / 4] = STS1_SYSPLL_STABLE | STS1_REF_RST |
                                  STS1_SYS125_RST | STS1_XHCI_RST |
                                  STS1_U3_MAC_RST;
    s->ippc_regs[IPPC_STS2 / 4] = 0;
    s->ippc_regs[IPPC_XHCI_CAP / 4] = XHCI_CAP_U3_PORTS |
                                      ((uint32_t)XHCI_CAP_U2_PORTS << 8);
}

static uint64_t mt7621_usb_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & (MT7621_USB_SIZE - 1);

    if (off == XHCI_USBSTS) {
        uint32_t val = s->usb_regs[off / 4];

        /*
         * HCHalted is not a latch: it follows the Run/Stop bit, which is the
         * only thing xhci_start() waits for after writing it.
         */
        val = s->usb_run ? (val & ~USB_STS_HCH) : (val | USB_STS_HCH);
        if (getenv("MT7621_USBLOG")) {
            fprintf(stderr, "usb: read  %04x -> %08x\n", (unsigned)off, val);
        }
        return val;
    }
    if (off == XHCI_USBCMD || off == XHCI_MFINDEX) {
        uint32_t val = s->usb_regs[off / 4];

        if (getenv("MT7621_USBLOG")) {
            fprintf(stderr, "usb: read  %04x -> %08x\n", (unsigned)off, val);
        }
        return val;
    }
    if (off >= XHCI_PORTSC(1) && off <= XHCI_PORTSC(XHCI_MAX_PORTS) &&
        ((off - XHCI_PORTSC(1)) % 0x10) == 0) {
        unsigned n = (off - XHCI_PORTSC(1)) / 0x10 + 1;
        uint32_t val = s->usb_regs[off / 4];

        /* A port's CCS/PED follow the modelled connection, i.e. none. */
        val &= ~(PORTSC_CCS | PORTSC_PED);
        if (getenv("MT7621_USBLOG")) {
            fprintf(stderr, "usb: read  port%u -> %08x\n", n, val);
        }
        return val;
    }

    if (getenv("MT7621_USBLOG")) {
        static unsigned budget = 96;

        if (budget--) {
            fprintf(stderr, "usb: read  %04x -> %08x\n", (unsigned)off,
                    s->usb_regs[off / 4]);
        }
    }
    return s->usb_regs[off / 4];
}

static void mt7621_usb_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & (MT7621_USB_SIZE - 1);
    uint32_t val = (uint32_t)value;

    if (getenv("MT7621_USBLOG")) {
        static unsigned budget = 96;

        if (budget--) {
            fprintf(stderr, "usb: write %04x <- %08x\n", (unsigned)off, val);
        }
    }

    switch (off) {
    case XHCI_USBCMD:
        /*
         * Run/Stop and Host Controller Reset are the only command bits with
         * visible state.  HCRST clears itself (the model performs the reset
         * instantly) and leaves the controller halted.
         */
        if (val & USB_CMD_HCRST) {
            mt7621_usb_reset(s);
            return;
        }
        s->usb_run = !!(val & USB_CMD_RS);
        s->usb_regs[off / 4] = val;
        return;
    case XHCI_USBSTS:
        /* Write-one-to-clear status bits; HCH and CNR are not writable. */
        s->usb_regs[off / 4] &= ~(val & (USB_STS_EINT | USB_STS_PCD));
        return;
    case XHCI_MFINDEX:
    case XHCI_DBOFF:
    case XHCI_RTSOFF:
    case XHCI_CAPLENGTH:
    case XHCI_HCSPARAMS1:
    case XHCI_HCSPARAMS2:
    case XHCI_HCSPARAMS3:
    case XHCI_HCCPARAMS1:
    case XHCI_HCCPARAMS2:
        return;                             /* read-only */
    default:
        break;
    }

    if (off >= XHCI_PORTSC(1) && off <= XHCI_PORTSC(XHCI_MAX_PORTS) &&
        ((off - XHCI_PORTSC(1)) % 0x10) == 0) {
        /*
         * PORTSC: keep the read-only state (PP) and apply the writable bits
         * (the RW1C change bits, which we simply don't latch, and the port
         * control bits the driver may set).  No connection is ever reported,
         * so a port reset cannot complete and the driver leaves it alone.
         */
        s->usb_regs[off / 4] =
            (s->usb_regs[off / 4] & ~PORTSC_WRITE_MASK) |
            (val & PORTSC_WRITE_MASK);
        return;
    }

    s->usb_regs[off / 4] = val;
}

static uint64_t mt7621_ippc_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & (MT7621_IPPC_SIZE - 1);

    if (getenv("MT7621_USBLOG")) {
        static unsigned budget = 96;

        if (budget--) {
            fprintf(stderr, "ippc: read  %04x -> %08x\n", (unsigned)off,
                    s->ippc_regs[off / 4]);
        }
    }
    return s->ippc_regs[off / 4];
}

static void mt7621_ippc_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & (MT7621_IPPC_SIZE - 1);
    uint32_t val = (uint32_t)value;

    if (getenv("MT7621_USBLOG")) {
        static unsigned budget = 96;

        if (budget--) {
            fprintf(stderr, "ippc: write %04x <- %08x\n", (unsigned)off, val);
        }
    }

    switch (off) {
    case IPPC_STS1:
        /*
         * Status only.  IP_SLEEP_STS is the one bit that is a live state: the
         * driver sets it by powering the block down, but nothing in this
         * model sleeps, so it stays clear.
         */
        s->ippc_regs[off / 4] &= ~STS1_IP_SLEEP_STS;
        return;
    case IPPC_STS2:
    case IPPC_XHCI_CAP:
        return;                             /* read-only */
    default:
        s->ippc_regs[off / 4] = val;
        return;
    }
}

static const MemoryRegionOps mt7621_usb_ops = {
    .read = mt7621_usb_read,
    .write = mt7621_usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static const MemoryRegionOps mt7621_ippc_ops = {
    .read = mt7621_ippc_read,
    .write = mt7621_ippc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void mt7621_usb_reset_hook(void *opaque)
{
    mt7621_usb_reset(opaque);
}

void mt7621_usb_init(MT7621State *s)
{
    mt7621_usb_reset(s);

    memory_region_init_io(&s->usb, NULL, &mt7621_usb_ops, s,
                          "mt7621.usb", MT7621_USB_SIZE);
    memory_region_add_subregion(&s->periph, MT7621_USB_OFF, &s->usb);

    memory_region_init_io(&s->ippc, NULL, &mt7621_ippc_ops, s,
                          "mt7621.ippc", MT7621_IPPC_SIZE);
    memory_region_add_subregion(&s->periph, MT7621_IPPC_OFF, &s->ippc);

    qemu_register_reset(mt7621_usb_reset_hook, s);
}
