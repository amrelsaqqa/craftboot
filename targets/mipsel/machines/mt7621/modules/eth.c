/*
 * MediaTek MT7621 -- the Ethernet frame engine (PDMA receive rings, QDMA
 * transmit ring), the MDIO bus, the MT7530 switch behind it and the QEMU NIC
 * that bridges one switch port to a host backend.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "mt7621.h"
#include "eth.h"

/*
 * Ethernet frame engine + MDIO.  Only what firmware needs to identify the
 * PHY and bring the MAC up is modelled: the MDIO control register performs
 * the transaction immediately, and the switch/PHY at address 31 answers
 * with a plausible MT7530 identity.
 */
static void mt7621_eth_update_irq(MT7621State *s);
static void mt7621_eth_tx(MT7621State *s, uint32_t ctx);
/*
 * MT7621 switch special tag
 * ------------------------
 * The switch driver inserts four bytes after the source MAC before handing a
 * frame to the CPU port: 00 <port bitmap> 00 00.  The real switch strips them
 * on the wire; we have no switch, so strip them here.  Without this the
 * netinstall discovery broadcast (00 50 00 00 08 00 ...) reaches the tap with
 * 0x0050 where an EtherType belongs and every listener drops it.
 *
 * Returns true if the frame should be sent, false if it was addressed to a
 * switch port other than the one this backend represents.
 */
static bool mt7621_tx_strip_tag(MT7621State *s, uint8_t *f, size_t *plen)
{
    size_t len = *plen;
    unsigned bitmap;

    if (len < 18 || f[12] != 0x00 || f[14] != 0x00 || f[15] != 0x00) {
        return true;                        /* untagged conduit frame */
    }
    bitmap = f[13];
    if (!(f[0] & 1) && bitmap && !(bitmap & (1u << s->eth_port))) {
        return false;                       /* another port owns it */
    }
    memmove(f + 12, f + 16, len - 16);
    *plen = len - 4;
    return true;
}

static void mt7621_eth_pdma_tx(MT7621State *s, unsigned q);
static void mt7621_ni_dump(const char *dir, const uint8_t *f, size_t len);

/*
 * MT7530 behind the MDIO bus: Linux talks to it through PHY address 0x1f
 * with a page/lo/hi protocol (mt7530_mii_read/write):
 *   write(0x1f, 0x1f, page); lo = read(0x1f, r); hi = read(0x1f, 0x10)
 * where page = (reg >> 6) & 0x3ff and r = (reg >> 2) & 0xf.
 */
static uint32_t *mt7621_sw_word(MT7621State *s, unsigned page, unsigned r)
{
    return &s->sw_regs[((page & 0x3ff) << 4) | (r & 0xf)];
}

static uint16_t mt7621_eth_phy_read(MT7621State *s, unsigned phy, unsigned reg)
{
    bool up;

    if (phy == 0x1f) {
        unsigned r = reg & 0x1f;

        if (r == 0x1f) {
            return 0xffff;      /* page register: data ignored on read */
        }
        if (r == 0x10) {
            uint32_t w = *mt7621_sw_word(s, s->sw_page, s->sw_r);

            s->sw_r = 0;
            return (w >> 16) & 0xffff;
        }
        s->sw_r = r;
        /* HWTRAP (0x7800) must never read back zero: the probe polls it. */
        if ((((unsigned)s->sw_page << 6) | ((unsigned)r << 2)) == 0x7800 &&
            *mt7621_sw_word(s, s->sw_page, r) == 0) {
            *mt7621_sw_word(s, s->sw_page, r) = 1;
        }
        if (getenv("MT7621_ETHLOG")) {
            fprintf(stderr, "eth: sw rd page=%03x r=%x -> %08x\n",
                    s->sw_page, r, *mt7621_sw_word(s, s->sw_page, r));
        }
        return *mt7621_sw_word(s, s->sw_page, r) & 0xffff;
    }

    /*
     * Only the switch port that owns the net backend has a cable in it.
     * MT7530 PHY addresses are the switch port numbers: 0 = the WAN port,
     * 1..4 = the four LAN PHYs.  Which *label* a port carries is a
     * device-tree decision, not hardware: mainline OpenWrt maps lan1 = port 1
     * ... lan4 = port 4 (the real board's dmesg shows wan = mt7530-0:00,
     * lan1 = :01 ... lan4 = :04), while the vendor image used for bring-up
     * reverses them (its lan1 is mt7530-0:04 and its lan4 is :01).  So the
     * backend port also decides which label reports the cable.
     *
     * A real PHY reports link only after autonegotiation finished; claiming
     * it on the very first poll makes genphy_read_status() resolve the link
     * once, before phylink programmed the advertisement, and it then
     * short-circuits every later poll, leaving the port at a stale speed.
     */
    /*
     * RouterBOOT 7.x checks the cable on a PHY that does not necessarily
     * match the port we bridge, and spins on MTK_PHY_IAC until it sees
     * link.  MT7621_ETH_ANYLINK reports link on every PHY, which is what
     * the tap actually represents while we model a single backend.
     */
    if (getenv("MT7621_ETH_ANYLINK")) {
        /*
         * Debug/etherboot knob: claim link on every PHY (including the WAN
         * PHY at address 0) and without the autoneg delay, so booters that
         * probe the wrong PHY, or probe only once, still see a cable.
         */
        up = true;
    } else {
        up = phy && phy == s->eth_port;
    }
    if (up && !getenv("MT7621_ETH_ANYLINK")) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        if (!s->phy_link_ns) {
            /*
             * Short, not instant: Linux's phylink programs the
             * advertisement and resolves the link on its first status read,
             * but RouterBOOT checks "is there a cable" only ~2 s into the
             * boot, before its DHCP phase, so the delay has to be well
             * below that.
             */
            s->phy_link_ns = now + 300 * 1000 * 1000LL;
        }
        up = now >= s->phy_link_ns;
    }

    switch (reg & 0x1f) {
    case 1:                             /* BMSR: live status */
        /* No autoneg: the PHY is strapped to 1000BASE-T full duplex. */
        return up ? 0x7965 : 0x7941;
    case 10:                            /* MII_STAT1000: link partner */
        return up ? 0x3800 : 0x0000;
    case 15:                            /* MII_ESTATUS: capabilities */
        return 0xc000;                  /* 1000BASE-T full/half */
    default:
        return s->phy_regs[phy & 0x1f][reg & 0x1f];
    }
}

static uint64_t mt7621_eth_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & (MT7621_ETH_SIZE - 1);
    uint32_t val = s->eth_regs[off >> 2];

    if (getenv("MT7621_ETHLOG")) {
        fprintf(stderr, "eth: read %04x -> %08x (size %u)\n",
                (unsigned)off, val, size);
    }
    return val;
}

static void mt7621_eth_write(void *opaque, hwaddr addr, uint64_t val64,
                             unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & (MT7621_ETH_SIZE - 1);
    uint32_t val = (uint32_t)val64;

    if (getenv("MT7621_ETHLOG")) {
        fprintf(stderr, "eth: write %04x <- %08x (size %u)\n",
                (unsigned)off, val, size);
    }
    if (getenv("MT7621_QDMALOG") && off >= 0x1a00 && off < 0x1b80) {
        fprintf(stderr, "qdma: write %04x <- %08x (size %u)\n",
                (unsigned)off, val, size);
    }

    if (off == ETH_QDMA_GLO_CFG) {
        s->qtx_en = !!(val & 1);
        s->eth_regs[off >> 2] = val;
        return;
    }
    if (off == ETH_PDMA_GLO_CFG) {
        s->eth_regs[off >> 2] = val;
        return;
    }
    /*
     * Four PDMA transmit rings, one per Linux TX queue: BASE_PTR/MAX_CNT/
     * CTX_IDX/DTX_IDX at 0x0800 + q*0x10.  Ring 0 is the only one RouterBOOT
     * uses, but the Linux driver puts its queues on all four and waits for a
     * TX_DONE bit per queue, so an unmodelled ring stalls that queue forever
     * (NETDEV WATCHDOG: "transmit queue N timed out").
     */
    if (off >= ETH_PTX_BASE_PTR0 && off < ETH_PTX_BASE_PTR0 + 4 * 0x10) {
        unsigned q = (off - ETH_PTX_BASE_PTR0) >> 4;

        s->eth_regs[off >> 2] = val;
        if ((off & 0xf) == 0) {                 /* BASE_PTR: ring reload */
            s->ptx_idx[q] = 0;
        }
        mt7621_eth_pdma_tx(s, q);
        return;
    }
    if (off == ETH_QTX_DTX_PTR) {
        /*
         * The driver points this at the start of a freshly allocated ring
         * before enabling the DMA, so it is both the forward pointer and
         * the ring base.
         */
        s->qtx_dtx = val;
        s->qtx_base = val;
        s->eth_regs[off >> 2] = val;
        return;
    }
    if (off == ETH_QTX_DRX_PTR) {
        s->eth_regs[off >> 2] = val;
        return;
    }
    if (off == ETH_QTX_CTX_PTR) {
        s->eth_regs[off >> 2] = val;
        /* Kicks that arrive while the engine is off (ring setup) are
         * ignored, exactly like the hardware does. */
        if (s->qtx_en) {
            mt7621_eth_tx(s, val);
        }
        return;
    }
    if (off == ETH_PDMA_INT_STATUS) {
        s->eth_regs[off >> 2] &= ~val;     /* write 1 to clear */
        mt7621_eth_update_irq(s);
        return;
    }
    if (off == ETH_QDMA_INT_STATUS) {
        s->eth_regs[off >> 2] &= ~val;
        mt7621_eth_update_irq(s);
        return;
    }
    if (off == ETH_PRX_BASE_PTR0) {
        /*
         * Only the base pointer changes here.  RouterBOOT rewrites it for
         * every descriptor it hands back, so resetting the DMA index would
         * make the engine restart at descriptor 0 each time and drop frames
         * into descriptors the bootloader still owns.
         */
        s->eth_regs[off >> 2] = val;
        return;
    }
    if (off == ETH_PDMA_GLO_CFG) {
        uint32_t old = s->eth_regs[off >> 2];

        s->eth_regs[off >> 2] = val;
        /* RX DMA (re)start: the engine begins at descriptor 0 again */
        if ((val & 0x4) && !(old & 0x4)) {
            s->rx_drx_idx = 0;
            s->eth_regs[ETH_PRX_DRX_IDX0 / 4] = 0;
        }
        return;
    }
    if (off == 0x0a08) {                /* PDMA_RST_IDX */
        s->eth_regs[off >> 2] = val;
        if (val & (1u << 16)) {         /* RST_DRX_IDX0 */
            s->rx_drx_idx = 0;
            s->eth_regs[ETH_PRX_DRX_IDX0 / 4] = 0;
        }
        return;
    }
    if (off == ETH_PRX_MAX_CNT0) {
        s->eth_regs[off >> 2] = val;
        if (s->rx_drx_idx >= val) {
            s->rx_drx_idx = 0;
        }
        return;
    }
    if (off == ETH_QDMA_INT_MASK || off == ETH_PDMA_INT_MASK) {
        /*
         * A level-triggered line must drop as soon as the driver masks the
         * source, otherwise the CPU stays in the interrupt handler forever.
         */
        s->eth_regs[off >> 2] = val;
        mt7621_eth_update_irq(s);
        return;
    }

    if (off == MT7621_ETH_IAC &&
        (val & ETH_IAC_ACCESS) && (val & ETH_IAC_START)) {
        unsigned phy = (val >> ETH_IAC_ADDR_SHIFT) & 0x1f;
        unsigned reg = (val >> ETH_IAC_REG_SHIFT) & 0x1f;

        if (val & ETH_IAC_WRITE) {
            unsigned data = val & 0xffff;

            if (phy == 0x1f) {
                if (reg == 0x1f) {
                    s->sw_page = data & 0x3ff;
                } else if (reg == 0x10) {
                    uint32_t *w = mt7621_sw_word(s, s->sw_page, s->sw_r);

                    *w = (*w & 0xffff) | (data << 16);
                    /*
                     * MT7530_ATC (0x90): the driver starts an address-table
                     * operation and polls ATC_BUSY (bit 31) until it clears.
                     * Complete it immediately.
                     */
                    {
                        unsigned regno = ((unsigned)s->sw_page << 6) |
                                         ((unsigned)s->sw_r << 2);

                        if (regno == 0x80) {
                            *w &= ~0x00008000u;  /* MT7530_ATC: ATC_BUSY */
                        } else if (regno == 0x90) {
                            *w &= ~0x80000000u;
                        }
                    }
                } else {
                    uint32_t *w;

                    s->sw_r = reg;
                    w = mt7621_sw_word(s, s->sw_page, reg);
                    *w = (*w & 0xffff0000) | data;
                }
            }
            val &= ~ETH_IAC_ACCESS;
        } else {
            val = (val & ~(ETH_IAC_ACCESS | 0xffff)) |
                  mt7621_eth_phy_read(s, phy, reg);
        }
    }

    s->eth_regs[off >> 2] = val;
}

/*
 * PDMA transmit ring (0x1e100800).  RouterBOOT drives this one, Linux uses
 * QDMA instead, so both paths have to work.  Descriptors are handed to the
 * engine with DDONE clear and a length in SDL0; the engine clears it and
 * sets DDONE when the frame is gone.
 */
static void mt7621_eth_pdma_tx(MT7621State *s, unsigned q)
{
    uint32_t base = s->eth_regs[(ETH_PTX_BASE_PTR0 + q * 0x10) / 4];
    uint32_t cnt = s->eth_regs[(ETH_PTX_MAX_CNT0 + q * 0x10) / 4];
    uint32_t ctx = s->eth_regs[(ETH_PTX_CTX_IDX0 + q * 0x10) / 4];
    uint32_t idx = s->ptx_idx[q];
    bool processed = false;
    uint8_t frame[2048];
    size_t flen = 0;
    unsigned fport = 0;
    int guard = 0;

    if (!base || !cnt || cnt > 0x1000) {
        s->ptx_idx[q] = ctx;
        return;
    }
    if (idx >= cnt) {
        idx = 0;
    }
    if (ctx >= cnt) {
        ctx = 0;
    }

    while (idx != ctx && guard++ <= (int)cnt) {
        uint32_t p = base + idx * 16;
        uint32_t addr0 = address_space_ldl_le(&address_space_memory, p,
                                              MEMTXATTRS_UNSPECIFIED, NULL);
        uint32_t info2 = address_space_ldl_le(&address_space_memory, p + 4,
                                              MEMTXATTRS_UNSPECIFIED, NULL);
        uint32_t addr1 = address_space_ldl_le(&address_space_memory, p + 8,
                                              MEMTXATTRS_UNSPECIFIED, NULL);
        uint32_t info4 = address_space_ldl_le(&address_space_memory, p + 12,
                                              MEMTXATTRS_UNSPECIFIED, NULL);
        unsigned len0 = PTX_SDL0(info2);
        unsigned len1 = (info2 & PTX_LS1) ? PTX_SDL1(info2) : 0;
        bool last = !!(info2 & PTX_LS0);

        if (info2 & PTX_DDONE) {
            /* descriptor not handed to the DMA: nothing else to do */
            break;
        }
        if (flen == 0) {
            fport = PTX_FPORT(info4);
        }
        if (addr0 && len0 && flen + len0 <= sizeof(frame)) {
            address_space_read(&address_space_memory, addr0,
                               MEMTXATTRS_UNSPECIFIED, frame + flen, len0);
            flen += len0;
        }
        if (addr1 && len1 && flen + len1 <= sizeof(frame)) {
            address_space_read(&address_space_memory, addr1,
                               MEMTXATTRS_UNSPECIFIED, frame + flen, len1);
            flen += len1;
        }

        if (last && flen >= 14) {
            bool forward = true;

            forward = mt7621_tx_strip_tag(s, frame, &flen);
            if (getenv("MT7621_ETHLOG")) {
                fprintf(stderr, "eth: ptx idx=%u fport=%u tag=%02x%02x "
                        "len=%zu dst=%02x:%02x:%02x:%02x:%02x:%02x fwd=%d",
                        idx, fport, frame[12], frame[13], flen, frame[0],
                        frame[1], frame[2], frame[3], frame[4], frame[5],
                        forward);
                if (flen >= 34 && frame[12] == 0x08 && frame[13] == 0x00) {
                    fprintf(stderr, " sport=%u dport=%u proto=%u",
                            (frame[34] << 8) | frame[35],
                            (frame[36] << 8) | frame[37], frame[23]);
                }
                fprintf(stderr, "\n");
            }
            if (getenv("MT7621_ETHLOG") && flen >= 300 &&
                frame[12] == 0x08 && frame[13] == 0x00 &&
                frame[23] == 0x11 &&
                (((frame[34] << 8) | frame[35]) == 68 ||
                 ((frame[36] << 8) | frame[37]) == 67 ||
                 ((frame[34] << 8) | frame[35]) == 67 ||
                 ((frame[36] << 8) | frame[37]) == 68)) {
                unsigned i;
                fprintf(stderr, "eth: bootp len=%zu:", flen);
                for (i = 42; i < flen && i < 420; i++) {
                    fprintf(stderr, " %02x", frame[i]);
                }
                fprintf(stderr, "\n");
            }
            {
                static unsigned dump_budget = 40;
                if (getenv("MT7621_ETHLOG") && dump_budget && flen >= 60) {
                    unsigned i, n = flen < 96 ? (unsigned)flen : 96;
                    dump_budget--;
                    fprintf(stderr, "eth: dump len=%zu:", flen);
                    for (i = 0; i < n; i++) {
                        fprintf(stderr, " %02x", frame[i]);
                    }
                    fprintf(stderr, "\n");
                }
            }
            if (getenv("MT7621_NILOG")) {
                fprintf(stderr, "eth: txn len=%zu fport=%u b12=%02x%02x%02x%02x "
                        "dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
                        flen, fport, frame[12], frame[13], frame[14], frame[15],
                        frame[0], frame[1], frame[2], frame[3], frame[4], frame[5]);
            }
            if (forward) {
                mt7621_ni_dump("tx", frame, flen);
            }
            if (forward && s->nic) {
                qemu_send_packet(qemu_get_queue(s->nic), frame, flen);
            }
            flen = 0;
        }

        address_space_stl_le(&address_space_memory, p + 4,
                             info2 | PTX_DDONE, MEMTXATTRS_UNSPECIFIED, NULL);
        processed = true;
        idx = (idx + 1) % cnt;
    }

    s->ptx_idx[q] = idx;
    s->eth_regs[(ETH_PTX_DTX_IDX0 + q * 0x10) / 4] = idx;
    /*
     * Tell the driver that the descriptors it handed us are free again.
     * Linux masks TX_DONE_INT0 and reclaims in its interrupt handler; with
     * no interrupt every queue fills up to max_tx_buffers_per_port and the
     * stack times out, restarts the switch and retries forever.
     */
    /*
     * Only report completions once the guest has enabled PDMA interrupts.
     * RouterBOOT polls PDMA_INT_STATUS without ever setting the mask, and an
     * unexpected status bit wedges its TFTP state machine (the transfer
     * crawls through 1400 retries instead of completing).
     */
    if (processed && !getenv("MT7621_NOPDMA_TXDONE") &&
        (s->eth_regs[ETH_PDMA_INT_MASK / 4] & (PDMA_TX_DONE0 << q))) {
        s->eth_regs[ETH_PDMA_INT_STATUS / 4] |= PDMA_TX_DONE0 << q;
        mt7621_eth_update_irq(s);
    }
    if (getenv("MT7621_ETHLOG")) {
        fprintf(stderr, "eth: ptx done idx=%u ctx=%u\n", idx, ctx);
    }
}

static void mt7621_eth_update_irq(MT7621State *s)
{
    uint32_t pdma = s->eth_regs[ETH_PDMA_INT_STATUS / 4] &
                    s->eth_regs[ETH_PDMA_INT_MASK / 4];
    uint32_t qdma = s->eth_regs[ETH_QDMA_INT_STATUS / 4] &
                    s->eth_regs[ETH_QDMA_INT_MASK / 4];

    qemu_set_irq(s->eth_irq, (pdma | qdma) != 0);
}

/* Move one received frame into the next PDMA descriptor. */

/*
 * Netinstall discovery chatter (UDP 5000 / 59813) is worth seeing in full:
 * the handshake state machine is byte-exact, so dump whole payloads.
 */
static void mt7621_ni_dump(const char *dir, const uint8_t *f, size_t len)
{
    unsigned sport, dport;
    size_t i;

    if (!getenv("MT7621_NILOG") || len < 42) {
        return;
    }
    if (f[12] != 0x08 || f[13] != 0x00 || f[23] != 0x11) {
        return;                                   /* IPv4 UDP only */
    }
    if ((f[14] & 0x0f) != 5) {
        return;
    }
    sport = (f[34] << 8) | f[35];
    dport = (f[36] << 8) | f[37];
    if (sport != 5000 && dport != 5000 && sport != 59813 && dport != 59813) {
        return;
    }
    fprintf(stderr, "eth: ni-%s len=%zu dst=%02x:%02x:%02x:%02x:%02x:%02x "
            "src=%02x:%02x:%02x:%02x:%02x:%02x ip=%u.%u.%u.%u->%u.%u.%u.%u "
            "sport=%u dport=%u:", dir, len,
            f[0], f[1], f[2], f[3], f[4], f[5],
            f[6], f[7], f[8], f[9], f[10], f[11],
            f[26], f[27], f[28], f[29], f[30], f[31], f[32], f[33],
            sport, dport);
    for (i = 42; i < len && i < 242; i++) {
        fprintf(stderr, " %02x", f[i]);
    }
    fprintf(stderr, " |");
    for (i = 42; i < len && i < 242; i++) {
        fputc(g_ascii_isprint(f[i]) ? f[i] : '.', stderr);
    }
    fprintf(stderr, "|\n");
}

/*
 * RX descriptor word 3 names the port a frame arrived on, but the two stacks
 * that run on this SoC put the field at different bits and disagree on what
 * it means:
 *
 *   Linux mtk_eth_soc.h: RX_DMA_FPORT_SHIFT 19, and the driver does
 *   mac = fport - 1 before indexing eth->netdev[], which has MTK_MAC_COUNT
 *   (2) entries.  So bit 19 must carry 1 for GMAC0 / 2 for GMAC1.
 *
 *   RouterOS raeth extracts ext(rxd4, 18, 3) and, in switch mode, only
 *   accepts 5 (the switch CPU port); anything else is thrown away with
 *   "switch0: rx too long".
 *
 * Either encoding is "an out-of-range MAC index" to the other stack, and the
 * Linux driver then releases the descriptor before any counter moves: the
 * ring looks untouched and every interface reports zero RX, which is easy to
 * mistake for the backend never sending anything.
 *
 * Only the Linux driver enables the QDMA transmit engine (GLO_CFG bit 0);
 * RouterOS transmits through the PDMA rings, so that bit is the run-time
 * tell.  MT7621_RX_FPORT forces the port number (Linux layout) for tests.
 */
static uint32_t mt7621_rx_fport_field(MT7621State *s, unsigned *fport)
{
    const char *p = getenv("MT7621_RX_FPORT");
    unsigned port = p ? ((unsigned)atoi(p) & 0x7) : (s->qtx_en ? 1 : 5);

    if (fport) {
        *fport = port;
    }
    if (p || s->qtx_en) {
        return (uint32_t)port << 19;        /* Linux: bits 19..21 */
    }
    return 5u << 18;                        /* RouterOS: bits 18..20 */
}

static void mt7621_eth_rx(MT7621State *s, const uint8_t *buf, size_t len)
{
    /*
     * MT7621_NIMACFIX=1: netinstall-cli's protocol frames carry a zero server
     * MAC in the payload (bytes 0..5 after the UDP header).  7.15's client
     * printed "Found server at 00:00:00:00:00:00" and carried on; 7.24.6's
     * client silently ignores such offers and waits forever.  Fill the field
     * with the frame's real Ethernet source MAC to test that theory.
     */
    uint8_t nimacfix_buf[1600];
    if (mt7621_flag("MT7621_NIMACFIX") && len >= 48 && len <= sizeof(nimacfix_buf)) {
        unsigned sp, dp, i;
        bool zero = true;

        memcpy(nimacfix_buf, buf, len);
        if (nimacfix_buf[12] == 0x08 && nimacfix_buf[13] == 0x00 &&
            nimacfix_buf[23] == 0x11) {
            sp = (nimacfix_buf[34] << 8) | nimacfix_buf[35];
            dp = (nimacfix_buf[36] << 8) | nimacfix_buf[37];
            if (sp == 5000 || dp == 5000 || sp == 59813 || dp == 59813) {
                for (i = 42; i < 48; i++) {
                    if (nimacfix_buf[i]) {
                        zero = false;
                    }
                }
                if (zero) {
                    memcpy(nimacfix_buf + 42, nimacfix_buf + 6, 6);
                    if (getenv("MT7621_NILOG")) {
                        fprintf(stderr, "nimacfix: server MAC %02x:%02x:%02x:"
                                "%02x:%02x:%02x filled in\n",
                                nimacfix_buf[6], nimacfix_buf[7],
                                nimacfix_buf[8], nimacfix_buf[9],
                                nimacfix_buf[10], nimacfix_buf[11]);
                    }
                }
            }
        }
        buf = nimacfix_buf;
    }

    mt7621_ni_dump("rx", buf, len);

    uint32_t base = s->eth_regs[ETH_PRX_BASE_PTR0 / 4];
    uint32_t cnt = s->eth_regs[ETH_PRX_MAX_CNT0 / 4];
    uint32_t idx = s->eth_regs[ETH_PRX_DRX_IDX0 / 4];
    uint32_t desc, rxd1, rxd2, cap, crx;
    uint8_t tagged[1600];
    size_t flen;
    unsigned rx_off;
    bool tagged_mode;
    unsigned fport = 0;
    uint32_t fport_bits = 0;
    bool took_bql = !bql_locked();
    static int dbg;

    if (!base || !cnt || len < 14) {
        if (getenv("MT7621_ETHLOG")) {
            fprintf(stderr, "eth: rx drop no-ring base=%08x cnt=%u len=%zu\n",
                    base, cnt, len);
        }
        return;
    }
    if (idx >= cnt) {
        idx = 0;
    }

    /*
     * The engine writes the descriptor DRX_IDX points at and then advances
     * it.  A descriptor the software has not consumed yet still carries the
     * DDONE flag, which is the only reliable "ring full" test: RouterBOOT
     * keeps CRX_IDX at the index it just freed, so comparing the two
     * indices (as the Linux driver's convention suggests) drops every frame
     * after the first one.
     */
    crx = s->eth_regs[ETH_PRX_CRX_IDX0 / 4];
    {
        uint32_t busy = address_space_ldl_le(&address_space_memory,
                                             base + idx * 16 + 4,
                                             MEMTXATTRS_UNSPECIFIED, NULL);
        if (busy & RX_DMA_DONE) {
            if (getenv("MT7621_ETHLOG")) {
                fprintf(stderr, "eth: rx drop ring-full idx=%u crx=%u\n",
                        idx, crx);
            }
            return;
        }
    }

    if (base && cnt && len >= 14 && getenv("MT7621_NILOG")) {
        const char *probe = NULL;
        size_t i;

        for (i = 12; i + 4 <= len; i++) {
            if (!memcmp(buf + i, "PING", 4) || !memcmp(buf + i, "OFFR", 4)) {
                probe = (const char *)buf + i;
                break;
            }
        }
        if (probe) {
            fprintf(stderr, "eth: probe %c%c%c%c len=%zu idx=%u crx=%u "
                    "drx=%u base=%08x cnt=%u tail=%zu off=%zu\n",
                    probe[0], probe[1], probe[2], probe[3], len, idx, crx,
                    s->eth_regs[ETH_PRX_DRX_IDX0 / 4], base, cnt,
                    (size_t)((const uint8_t *)probe - buf), len);
        }
    }

    if (took_bql) {
        bql_lock();
    }

    desc = base + idx * 16;
    rxd1 = address_space_ldl_le(&address_space_memory, desc,
                                MEMTXATTRS_UNSPECIFIED, NULL);
    rxd2 = address_space_ldl_le(&address_space_memory, desc + 4,
                                MEMTXATTRS_UNSPECIFIED, NULL);
    /* Words 16:29 of a clean descriptor hold the buffer capacity. */
    cap = (rxd2 >> 16) & 0x3fff;
    if (!cap) {
        cap = 1536;
    }
    if (!rxd1) {
        if (getenv("MT7621_ETHLOG")) {
            fprintf(stderr, "eth: rx drop no-buffer idx=%u desc=%08x\n",
                    idx, desc);
        }
        goto out;
    }

    /*
     * Linux drives the port as a DSA master: the switch inserts its special
     * tag after the source MAC and the engine starts the frame two bytes
     * into the buffer (RX_2B_OFFSET).  RouterBOOT configures the switch
     * without the tag and with RX_2B_OFFSET clear, so honour that bit
     * instead of always building a tagged frame.
     */
    tagged_mode = !!(s->eth_regs[ETH_PDMA_GLO_CFG / 4] & (1u << 31));
    if (tagged_mode) {
        if (len + 4 > sizeof(tagged)) {
            len = sizeof(tagged) - 4;
        }
        memcpy(tagged, buf, 12);
        tagged[12] = 0x00;                /* untagged special tag */
        /*
         * Byte 13 of the special tag is the source port number in its low
         * three bits: switch.ko masks it with 0x7 before indexing its port
         * table, so a bitmap value of 1<<4 lands on port 0 (eth0) and the
         * agent's per-interface sockets never see their own frames.
         */
        tagged[13] = s->eth_port & 0x7;  /* source port */
        tagged[14] = 0x00;
        tagged[15] = 0x00;
        memcpy(tagged + 16, buf + 12, len - 12);
        flen = len + 4;
        rx_off = 2;
    } else {
        flen = len;
        if (flen > sizeof(tagged)) {
            flen = sizeof(tagged);
        }
        memcpy(tagged, buf, flen);
        rx_off = 0;
    }

    if (flen > cap) {
        flen = cap;                        /* stay inside the driver buffer */
    }
    /*
     * PDMA_GLO_CFG has RX_2B_OFFSET set, so the engine starts the frame two
     * bytes into the descriptor buffer; the driver's skb_reserve() accounts
     * for that offset.
     */
    address_space_write(&address_space_memory, rxd1 + rx_off,
                        MEMTXATTRS_UNSPECIFIED, tagged, flen);

    /* rxd2's DONE bit is the release flag, so write it last. */
    address_space_stl_le(&address_space_memory, desc + 8, 0,
                         MEMTXATTRS_UNSPECIFIED, NULL);
    /*
     * Word 3 of a filled RX descriptor carries the forward port.  The
     * RouterOS raeth driver extracts it with ext(.,18,3) and, in the tagged
     * (switch) mode, only accepts frames whose value is 5 - the CPU port -
     * otherwise it prints "switch0: rx too long: %d+" and throws the frame
     * away.  RouterBOOT never looks at the field, so keep its old value in
     * the untagged mode.
     */
    fport_bits = mt7621_rx_fport_field(s, &fport);
    if (getenv("MT7621_ETHLOG")) {
        static int fillbudget = 24;

        if (fillbudget-- > 0) {
            fprintf(stderr, "eth: fill base=%08x cnt=%u idx=%u desc=%08x "
                    "buf=%08x off=%u len=%zu rxd2=%08x fport=%u "
                    "mask=%08x status=%08x\n",
                    base, cnt, idx, desc, rxd1, rx_off, flen,
                    RX_DMA_DONE | RX_DMA_PLEN0((unsigned)flen), fport,
                    s->eth_regs[ETH_PDMA_INT_MASK / 4],
                    s->eth_regs[ETH_PDMA_INT_STATUS / 4]);
        }
    }
    address_space_stl_le(&address_space_memory, desc + 12,
                         tagged_mode ? fport_bits :
                         (1u << RX_DMA_FPORT_SHIFT),
                         MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(&address_space_memory, desc + 4,
                         RX_DMA_DONE | RX_DMA_PLEN0(flen) |
                         (mt7621_flag("MT7621_NO_RX_LSO") ? 0 : (1u << 30)),
                         MEMTXATTRS_UNSPECIFIED, NULL);

    s->rx_drx_idx = (idx + 1) % cnt;
    s->eth_regs[ETH_PRX_DRX_IDX0 / 4] = s->rx_drx_idx;
    {
        uint32_t rxint = s->eth_regs[ETH_PDMA_INT_MASK / 4] &
                         (PDMA_RX_DONE | PDMA_RX_DLY_INT);

        if (rxint) {
            s->eth_regs[ETH_PDMA_INT_STATUS / 4] |= rxint;
        }
    }

    {
        static unsigned big_budget = 6;
        if (getenv("MT7621_ETHLOG") && big_budget && len >= 1000) {
            unsigned i, n = flen < 52 ? (unsigned)flen : 52;
            big_budget--;
            fprintf(stderr, "eth: rxdump len=%zu:", flen);
            for (i = 0; i < n; i++) {
                fprintf(stderr, " %02x", tagged[i]);
            }
            fprintf(stderr, "\n");
        }
    }
    dbg++;
    if (getenv("MT7621_RXDBG")) {
        static int deliv;
        unsigned et = tagged_mode ? 16 : 12;

        if (deliv++ < 200) {
            fprintf(stderr, "rxdeliver[%d]: idx=%u len=%zu tagged=%d fport=%u "
                    "type=%02x%02x "
                    "dst=%02x:%02x:%02x:%02x:%02x:%02x\n", deliv, idx, flen,
                    tagged_mode, fport, flen > et + 1 ? tagged[et] : 0,
                    flen > et + 1 ? tagged[et + 1] : 0,
                    tagged[0], tagged[1], tagged[2], tagged[3], tagged[4],
                    tagged[5]);
        }
    }
    if (getenv("MT7621_ETHLOG") && dbg < 16) {
        uint32_t prev = desc - 16;
        uint32_t prev_rxd2 = address_space_ldl_le(&address_space_memory,
                                                  prev + 4,
                                                  MEMTXATTRS_UNSPECIFIED, NULL);

        fprintf(stderr, "eth: rx[%d] desc=%08x len=%zu/%u idx=%u crx=%u "
                "proto=%02x%02x dst=%02x:%02x:%02x:%02x:%02x:%02x "
                "prev_done=%d\n",
                dbg, desc, flen, cap, idx, crx, tagged[12], tagged[13],
                tagged[0], tagged[1], tagged[2], tagged[3], tagged[4],
                tagged[5], !!(prev_rxd2 & RX_DMA_DONE));
    }

    mt7621_eth_update_irq(s);

    /*
     * The NIC always has room, so tell the net core to retry anything the
     * backend queued while the core was busy.
     */
    qemu_flush_queued_packets(qemu_get_queue(s->nic));

out:
    if (took_bql) {
        bql_unlock();
    }
}

static bool mt7621_eth_can_receive(NetClientState *nc)
{
    return true;
}

static ssize_t mt7621_eth_receive(NetClientState *nc, const uint8_t *buf,
                                  size_t size)
{
    MT7621State *s = qemu_get_nic_opaque(nc);

    mt7621_eth_rx(s, buf, size);
    return size;
}

/*
 * MT7621_NITEST: inject synthetic discovery-style datagrams into the RX ring
 * so that "does this frame reach the agent's socket?" can be answered without
 * netinstall in the loop.  One probe per interesting source/destination pair.
 */
static void mt7621_nitest_send(MT7621State *s, uint32_t sip, uint32_t dip,
                               bool bcast, const char *tag)
{
    uint8_t f[128];
    uint8_t *ip = f + 14;
    uint8_t *udp = ip + 20;
    uint8_t *pl = udp + 8;
    unsigned plen = 20 + 5;
    unsigned i;
    uint32_t sum = 0;

    memcpy(f, bcast ? "\xff\xff\xff\xff\xff\xff" : "\x64\xd1\x54\x09\xb8\x25", 6);
    memcpy(f + 6, "\x02\x00\x00\x00\x00\x01", 6);
    f[12] = 0x08;
    f[13] = 0x00;

    memset(ip, 0, 20);
    ip[0] = 0x45;
    ip[2] = (20 + 8 + plen) >> 8;
    ip[3] = (20 + 8 + plen) & 0xff;
    ip[4] = 0x12;
    ip[5] = 0x34;
    ip[8] = 64;
    ip[9] = 17;
    ip[12] = sip & 0xff; ip[13] = (sip >> 8) & 0xff;
    ip[14] = (sip >> 16) & 0xff; ip[15] = (sip >> 24) & 0xff;
    ip[16] = dip & 0xff; ip[17] = (dip >> 8) & 0xff;
    ip[18] = (dip >> 16) & 0xff; ip[19] = (dip >> 24) & 0xff;
    for (i = 0; i < 20; i += 2) {
        sum += (ip[i] << 8) | ip[i + 1];
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    sum = ~sum & 0xffff;
    ip[10] = sum >> 8;
    ip[11] = sum & 0xff;

    udp[0] = 0x13; udp[1] = 0x88;         /* 5000 */
    udp[2] = 0x13; udp[3] = 0x88;
    udp[4] = (8 + plen) >> 8;
    udp[5] = (8 + plen) & 0xff;
    udp[6] = udp[7] = 0;                  /* no UDP checksum */

    memset(pl, 0, 20);
    memcpy(pl, "\x64\xd1\x54\x09\xb8\x25", 6);   /* peer MAC, non-zero */
    pl[14] = plen;
    pl[16] = 1;
    memcpy(pl + 20, tag, 5);

    fprintf(stderr, "nitest: inject %s sip=%08x dip=%08x bcast=%d "
            "base=%08x cnt=%u drx=%u crx=%u\n",
            tag, sip, dip, bcast,
            s->eth_regs[ETH_PRX_BASE_PTR0 / 4], s->eth_regs[ETH_PRX_MAX_CNT0 / 4],
            s->eth_regs[ETH_PRX_DRX_IDX0 / 4], s->eth_regs[ETH_PRX_CRX_IDX0 / 4]);
    mt7621_eth_rx(s, f, 14 + 20 + 8 + plen);
}

static void mt7621_nitest_tick(void *opaque)
{
    MT7621State *s = opaque;
    static unsigned n;

    n++;
    mt7621_nitest_send(s, 0x00000000, 0xffffffff, true, "PINGA");
    mt7621_nitest_send(s, 0xc0a80102, 0xffffffff, true, "PINGB");
    mt7621_nitest_send(s, 0xc0a80102, 0xc0a80103, false, "PINGC");
    if (n < 30) {
        timer_mod(s->nitest_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2000);
    }
}

static void mt7621_phy_defaults(MT7621State *s)
{
    unsigned p, r;

    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    for (p = 0; p < 32; p++) {
        s->phy_regs[p][0]  = 0x0140;    /* BMCR: 1000BASE-T full duplex
                                         * (bit 6 = speed MSB, no autoneg) */
        s->phy_regs[p][2]  = 0x0362;    /* PHY ID1 */
        s->phy_regs[p][3]  = 0x5c30;    /* PHY ID2 */
        s->phy_regs[p][4]  = 0x01e1;    /* 10/100 advertise */
        s->phy_regs[p][9]  = 0x0300;    /* MII_CTRL1000: 1000BASE-T f/h */
    }
    s->phy_link_ns = 0;
    (void)r;
}

static void mt7621_switch_defaults(MT7621State *s)
{
    /*
     * MT7530 identity: CREV (0x7ffc) >> 16 must be 0x7530 and HWTRAP
     * (0x7800) must read non-zero, otherwise mt7530-mdio refuses the chip
     * and DSA never creates the lan/wan ports.
     */
    memset(s->sw_regs, 0, sizeof(s->sw_regs));
    s->sw_page = 0;
    s->sw_r = 0;
    *mt7621_sw_word(s, (0x7ffc >> 6) & 0x3ff, (0x7ffc >> 2) & 0xf) = 0x75300000;
    *mt7621_sw_word(s, (0x7800 >> 6) & 0x3ff, (0x7800 >> 2) & 0xf) = 0x00000001;
}

static void mt7621_eth_cleanup(NetClientState *nc)
{
}

static NetClientInfo mt7621_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = mt7621_eth_can_receive,
    .receive = mt7621_eth_receive,
    .cleanup = mt7621_eth_cleanup,
};

/*
 * Transmit: the Linux driver enqueues descriptors and then writes the
 * physical address of the newest one to MTK_QTX_CTX_PTR.  Walk the chain
 * from the last completed descriptor, hand each buffer to the net backend
 * and publish the new done pointer.
 */
/*
 * The driver leaves TCP/UDP checksums to the MAC (NETIF_F_HW_CSUM): the
 * stack fills the checksum field with the pseudo-header sum and sets
 * TX_DMA_CHKSUM in txd4.  Finish the job here, otherwise every TCP segment
 * leaves with a bogus checksum and the peer silently drops it.
 */
static void mt7621_eth_tx_csum(uint8_t *frame, size_t len)
{
    size_t l3 = 14, l4 = 0, l4_len = 0, csum_off;
    unsigned eth, proto, ihl, tot;

    if (len < 34) {
        return;
    }
    eth = (frame[12] << 8) | frame[13];
    if (eth == 0x8100 || eth == 0x88a8) {
        if (len < 38) {
            return;
        }
        eth = (frame[16] << 8) | frame[17];
        l3 = 18;
    }

    if (eth == 0x0800) {                    /* IPv4 */
        if (len < l3 + 20 || (frame[l3] >> 4) != 4) {
            return;
        }
        ihl = (frame[l3] & 0xf) * 4;
        if (ihl < 20 || len < l3 + ihl) {
            return;
        }
        if (((frame[l3 + 6] << 8) | frame[l3 + 7]) & 0x1fff) {
            return;                         /* fragment: checksum is final */
        }
        tot = (frame[l3 + 2] << 8) | frame[l3 + 3];
        if (tot < ihl) {
            return;
        }
        proto = frame[l3 + 9];
        l4 = l3 + ihl;
        l4_len = tot - ihl;
    } else if (eth == 0x86dd) {             /* IPv6 */
        if (len < l3 + 40 || (frame[l3] >> 4) != 6) {
            return;
        }
        proto = frame[l3 + 6];
        l4 = l3 + 40;
        l4_len = (frame[l3 + 4] << 8) | frame[l3 + 5];
    } else {
        return;
    }

    switch (proto) {
    case 6:                                 /* TCP */
    case 17:                                /* UDP */
        csum_off = (proto == 6) ? 16 : 6;
        break;
    case 1:                                 /* ICMP */
    case 58:                                /* ICMPv6 */
        csum_off = 2;
        break;
    default:
        return;                             /* header with extensions, etc. */
    }

    if (len < l4 + csum_off + 2) {
        return;
    }
    if (l4_len > len - l4) {
        l4_len = len - l4;                  /* truncated: sum what we have */
    }

    {
        uint32_t sum = 0;
        size_t i;
        uint16_t c;

        for (i = 0; i + 1 < l4_len; i += 2) {
            sum += (frame[l4 + i] << 8) | frame[l4 + i + 1];
        }
        if (l4_len & 1) {
            sum += frame[l4 + l4_len - 1] << 8;
        }
        while (sum >> 16) {
            sum = (sum & 0xffff) + (sum >> 16);
        }
        c = ~sum;
        if (!c) {
            c = 0xffff;
        }
        frame[l4 + csum_off] = c >> 8;
        frame[l4 + csum_off + 1] = c & 0xff;
    }
}

/*
 * TSO: the driver hands us one oversized TCP segment and puts the MSS in the
 * checksum field (mtk_start_xmit does tcp_hdr(skb)->check = gso_size).  Split
 * it into MTU-sized frames, fixing up the IP length/id, the sequence number
 * and both checksums.
 */
static void mt7621_eth_tx_tso(MT7621State *s, const uint8_t *frame, size_t len)
{
    uint8_t out[1600];
    size_t l3 = 14;
    unsigned eth, proto, ihl, doff, hdr, payload_len, mss, off, room;

    if (len < 54) {
        return;
    }
    eth = (frame[12] << 8) | frame[13];
    if (eth == 0x8100 || eth == 0x88a8) {
        if (len < 58) {
            return;
        }
        eth = (frame[16] << 8) | frame[17];
        l3 = 18;
    }
    if (eth != 0x0800 && eth != 0x86dd) {
        return;
    }
    if (eth == 0x0800) {
        ihl = (frame[l3] & 0xf) * 4;
        proto = frame[l3 + 9];
        payload_len = ((frame[l3 + 2] << 8) | frame[l3 + 3]) - ihl;
    } else {
        ihl = 40;
        proto = frame[l3 + 6];
        payload_len = (frame[l3 + 4] << 8) | frame[l3 + 5];
    }
    if (proto != 6 || ihl < 20 || len < l3 + ihl + 20) {
        return;
    }
    doff = ((frame[l3 + ihl + 12] >> 4) & 0xf) * 4;
    if (doff < 20 || l3 + ihl + doff > len) {
        return;
    }
    if (payload_len > len - l3 - ihl - doff) {
        payload_len = len - l3 - ihl - doff;
    }

    mss = (frame[l3 + ihl + 16] << 8) | frame[l3 + ihl + 17];
    room = (eth == 0x0800) ? (1514 - l3 - ihl - doff)
                           : (1514 - l3 - ihl - doff);
    if (mss == 0 || mss > room) {
        mss = room;
    }
    if ((int)room <= 0) {
        return;
    }
    hdr = l3 + ihl + doff;

    for (off = 0; off < payload_len; off += mss) {
        unsigned chunk = payload_len - off;
        bool last;
        uint32_t sum = 0;
        size_t i;
        uint16_t c;

        if (chunk > mss) {
            chunk = mss;
        }
        last = (off + chunk >= payload_len);

        memcpy(out, frame, hdr);
        memcpy(out + hdr, frame + hdr + off, chunk);

        /* IPv4 header: total length, id and checksum */
        if (eth == 0x0800) {
            out[l3 + 2] = (ihl + doff + chunk) >> 8;
            out[l3 + 3] = (ihl + doff + chunk) & 0xff;
            sum = 0;
            for (i = 0; i < ihl; i += 2) {
                sum += (out[l3 + i] << 8) | out[l3 + i + 1];
            }
            while (sum >> 16) {
                sum = (sum & 0xffff) + (sum >> 16);
            }
            c = ~sum;
            out[l3 + 10] = c >> 8;
            out[l3 + 11] = c & 0xff;
        }
        /* TCP sequence number advances with the payload */
        {
            uint32_t seq = ((uint32_t)out[l3 + ihl + 4] << 24) |
                           (out[l3 + ihl + 5] << 16) |
                           (out[l3 + ihl + 6] << 8) | out[l3 + ihl + 7];

            seq += off;
            out[l3 + ihl + 4] = seq >> 24;
            out[l3 + ihl + 5] = seq >> 16;
            out[l3 + ihl + 6] = seq >> 8;
            out[l3 + ihl + 7] = seq;
        }
        /* PSH only on the final segment */
        out[l3 + ihl + 12] = (out[l3 + ihl + 12] & 0xf0) | (doff / 4);
        if (last) {
            out[l3 + ihl + 13] |= 0x08;
        } else {
            out[l3 + ihl + 13] &= ~0x08;
        }
        /* TCP checksum over pseudo header + segment */
        out[l3 + ihl + 16] = 0;
        out[l3 + ihl + 17] = 0;
        sum = 0;
        if (eth == 0x0800) {
            sum += (out[l3 + 12] << 8) | out[l3 + 13];
            sum += (out[l3 + 14] << 8) | out[l3 + 15];
            sum += (out[l3 + 16] << 8) | out[l3 + 17];
            sum += (out[l3 + 18] << 8) | out[l3 + 19];
        } else {
            for (i = 8; i < 40; i += 2) {
                sum += (out[l3 + i] << 8) | out[l3 + i + 1];
            }
        }
        sum += 6;
        sum += doff + chunk;
        for (i = 0; i < doff + chunk; i += 2) {
            unsigned hi = out[l3 + ihl + i];
            unsigned lo = (i + 1 < doff + chunk) ?
                          out[l3 + ihl + i + 1] : 0;

            sum += (hi << 8) | lo;
        }
        while (sum >> 16) {
            sum = (sum & 0xffff) + (sum >> 16);
        }
        c = ~sum;
        if (!c) {
            c = 0xffff;
        }
        out[l3 + ihl + 16] = c >> 8;
        out[l3 + ihl + 17] = c & 0xff;

        if (s->nic) {
            qemu_send_packet(qemu_get_queue(s->nic), out, hdr + chunk);
        }
    }
}

static void mt7621_eth_tx(MT7621State *s, uint32_t ctx)
{
    uint32_t p = s->qtx_dtx;
    uint32_t base = s->qtx_base;
    int guard = 0;
    unsigned sent = 0;
    /*
     * One frame can span several descriptors (skb head + page fragments);
     * the engine concatenates them until it meets TX_DMA_LS0.  Keep the
     * partially assembled frame across calls.
     */
    static uint8_t frame[0x10000];
    static size_t flen;
    static bool frame_started;
    static unsigned frame_fport;
    static bool frame_csum;
    static bool frame_tso;

    if (!base || p < base || p >= base + TX_RING_BYTES ||
        ctx < base || ctx >= base + TX_RING_BYTES) {
        /* the ring was re-allocated: resynchronise and send nothing */
        flen = 0;
        frame_started = false;
        s->qtx_dtx = ctx;
        s->qtx_base = ctx;
        return;
    }

    if (getenv("MT7621_ETHLOG")) {
        fprintf(stderr, "eth: tx ctx=%08x start=%08x\n", ctx, p);
    }

    while (p != ctx && guard++ < TX_RING_BYTES / 16) {
        uint32_t txd1 = address_space_ldl_le(&address_space_memory, p,
                                             MEMTXATTRS_UNSPECIFIED, NULL);
        uint32_t txd2 = address_space_ldl_le(&address_space_memory, p + 4,
                                             MEMTXATTRS_UNSPECIFIED, NULL);
        uint32_t txd3 = address_space_ldl_le(&address_space_memory, p + 8,
                                             MEMTXATTRS_UNSPECIFIED, NULL);
        uint32_t txd4 = address_space_ldl_le(&address_space_memory, p + 12,
                                             MEMTXATTRS_UNSPECIFIED, NULL);
        unsigned len = (txd3 >> 16) & 0x3fff;
        bool filled = !(txd3 & TX_DMA_OWNER_CPU);
        bool last = !!(txd3 & TX_DMA_LS0);

        if (getenv("MT7621_TXDBG")) {
            static int budget = 24;

            if (budget-- > 0) {
                fprintf(stderr, "txd @%08x: %08x %08x %08x %08x len=%u "
                        "filled=%d last=%d\n", p, txd1, txd2, txd3, txd4,
                        len, filled, last);
            }
        }
        if (filled && txd1 && len) {
            if (!frame_started) {
                frame_started = true;
                frame_fport = (txd4 >> TX_DMA_FPORT_SHIFT) & 0x7;
                frame_csum = !!(txd4 & TX_DMA_CHKSUM);
                frame_tso = !!(txd4 & TX_DMA_TSO);
            }
            if (len > sizeof(frame) - flen) {
                len = sizeof(frame) - flen;
            }
            address_space_read(&address_space_memory, txd1,
                               MEMTXATTRS_UNSPECIFIED, frame + flen, len);
            flen += len;

            if (last) {
                bool forward = false;

                /*
                 * Frames sent through GMAC1 (the DSA master) carry the MTK
                 * special tag at offset 12: byte 12 is the tag type and
                 * byte 13 the destination port bitmap.  Only frames the
                 * switch would put on our port may leave through this
                 * backend; GMAC2 (the wan MAC) is not behind the switch.
                 */
                if (frame_fport == 1) {
                    forward = mt7621_tx_strip_tag(s, frame, &flen);
                }

                if (forward && frame_tso) {
                    mt7621_eth_tx_tso(s, frame, flen);
                    forward = false;
                } else if (forward && frame_csum) {
                    mt7621_eth_tx_csum(frame, flen);
                }

                if (!forward && getenv("MT7621_ETHLOG") && flen > 18) {
                    fprintf(stderr, "eth: tx drop fport=%u tag=%02x%02x "
                            "len=%zu dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
                            frame_fport, frame[12], frame[13], flen,
                            frame[0], frame[1], frame[2], frame[3],
                            frame[4], frame[5]);
                }
                if (forward && getenv("MT7621_ETHLOG") && sent < 8) {
                    fprintf(stderr, "eth: txf dst=%02x:%02x:%02x:%02x:%02x:%02x "
                            "src=%02x:%02x:%02x:%02x:%02x:%02x type=%02x%02x "
                            "len=%zu\n",
                            frame[0], frame[1], frame[2], frame[3], frame[4],
                            frame[5], frame[6], frame[7], frame[8], frame[9],
                            frame[10], frame[11], frame[12], frame[13], flen);
                }
                if (forward) {
                    mt7621_ni_dump("tx", frame, flen);
                }
                if (forward && s->nic) {
                    qemu_send_packet(qemu_get_queue(s->nic), frame, flen);
                    sent++;
                }
                flen = 0;
                frame_started = false;
            }
        }

        /* Hand the descriptor back to the CPU (TX_DMA_DONE). */
        if (filled) {
            address_space_stl_le(&address_space_memory, p + 8,
                                 txd3 | TX_DMA_OWNER_CPU,
                                 MEMTXATTRS_UNSPECIFIED, NULL);
        }
        p = txd2;
    }

    if (getenv("MT7621_ETHLOG")) {
        fprintf(stderr, "eth: tx done p=%08x ctx=%08x sent=%u guard=%d\n",
                p, ctx, sent, guard);
    }

    s->qtx_dtx = p;
    s->eth_regs[ETH_QTX_DRX_PTR / 4] = p;
    if (s->nic) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
    if (p == ctx) {
        s->eth_regs[ETH_QDMA_INT_STATUS / 4] |= QDMA_TX_DONE;
        mt7621_eth_update_irq(s);
    }
}

static const MemoryRegionOps mt7621_eth_ops = {
    .read = mt7621_eth_read,
    .write = mt7621_eth_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * SYS_RSTCTL (0x34): Linux's ralink restart handler ends with
 * rt_sysc_w32(RSTCTL_RESET_SYSTEM, SYSC_REG_RESET_CTRL), i.e. a 32-bit
 * write of bit 0.  Anything else (peripheral reset bits, the PCI reset
 * mask the same handler does first) is just latched.
 */
/*
 * Ethernet.  The frame engine raises GIC shared interrupt 3 and its DMA rings
 * move frames between guest memory and a QEMU net backend, so a tap device on
 * the host is bridged at layer 2.
 */
void mt7621_eth_init(MT7621State *s)
{
    /*
     * The tap is a cable in one switch port.  Model the hardware: the LAN
     * PHYs are ports 1..4, so the cable sits in port 1 (mainline's lan1).
     * MT7621_ETH_PORT moves it; use 4 to make the vendor image's own "lan1"
     * the live port, which is what the bring-up logs used.  RouterOS numbers
     * ether1..5 as ports 0..4 and the tap has always been ether5 there, so
     * that board keeps 4.
     */
    const char *p = getenv("MT7621_ETH_PORT");
    unsigned def_port = s->board == MT7621_BOARD_RB750GR3 ? 4 : 1;

    s->eth_port = p ? (unsigned)atoi(p) : def_port;
    s->eth_irq = qdev_get_gpio_in(s->gic, 3);
    memset(s->eth_regs, 0, sizeof(s->eth_regs));
    mt7621_switch_defaults(s);
    mt7621_phy_defaults(s);

    /*
     * The frame engine's NIC is part of the SoC, so attach it to the user's
     * netdev (documented as id=net0) directly instead of going through the
     * pluggable-device matching.
     */
    {
        NetClientState *peer = qemu_find_netdev("net0");

        if (peer) {
            /* The NIC keeps a pointer to this, so it must outlive init. */
            memset(&s->nic_conf, 0, sizeof(s->nic_conf));
            qemu_macaddr_default_if_unset(&s->nic_conf.macaddr);
            s->nic_conf.peers.ncs[0] = peer;
            s->nic = qemu_new_nic(&mt7621_net_info, &s->nic_conf,
                                  "mt7621-eth", "mt7621-eth",
                                  &s->nic_guard, s);
            qemu_set_info_str(qemu_get_queue(s->nic), "mt7621-eth");
        }
    }

    /*
     * Ethernet/sysctl blocks the firmware probes: reads must not return
     * 0xffffffff, which makes ring-size computations overflow.
     */
    memory_region_init_io(&s->eth, NULL, &mt7621_eth_ops, s, "mt7621.eth",
                          MT7621_ETH_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x1e100000, &s->eth);

    /*
     * The frame engine contains 24 KiB of SRAM at 0x1e108000.  The MT7621
     * BootROM and the pre-DDR boot stages use it as stack and scratch
     * space; RouterBOOT's DDR init even keeps its function table there.
     * It must be real memory, not part of the register file.
     */
    memory_region_init_ram(&s->fe_sram, NULL, "mt7621.fe-sram",
                           0x00008000, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0x1e108000, &s->fe_sram);
}

/* reset state: called from the machine's reset handler */
void mt7621_eth_dev_reset(MT7621State *s)
{
    memset(s->eth_regs, 0, sizeof(s->eth_regs));
    s->qtx_dtx = 0;
    s->qtx_base = 0;
    s->qtx_en = false;
    s->rx_drx_idx = 0;
    mt7621_switch_defaults(s);
    mt7621_phy_defaults(s);
}
