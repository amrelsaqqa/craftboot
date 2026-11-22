/*
 * Private register map and frame-descriptor bits for the MT7621 frame engine
 * (PDMA receive rings, QDMA transmit ring) and the MT7530 behind MDIO.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MIPS_MT7621_ETH_H
#define HW_MIPS_MT7621_ETH_H

#define MT7621_ETH_IAC          0x10004

#define ETH_IAC_ACCESS          (1u << 31)
#define ETH_IAC_READ            (1u << 19)
#define ETH_IAC_WRITE           (1u << 18)
#define ETH_IAC_START           (1u << 16)
#define ETH_IAC_ADDR_SHIFT      20
#define ETH_IAC_REG_SHIFT       25

/*
 * PDMA ring block at 0x1e100800 (MediaTek SDK layout, also what RouterBOOT
 * programs):
 *   0x0800 TX base ptr   0x0804 TX max cnt   0x0808 TX ctx idx  0x080c TX dtx idx
 *   0x0900 RX base ptr   0x0904 RX max cnt   0x0908 RX crx idx  0x090c RX drx idx
 *   0x0a04 PDMA global config (TX_EN bit0, RX_EN bit2)
 * Linux only uses the receive half here and transmits through QDMA (0x1a00),
 * but the bootloader queues its DHCP frames on this TX ring.
 */
#define ETH_PTX_BASE_PTR0       0x0800
#define ETH_PTX_MAX_CNT0        0x0804
#define ETH_PTX_CTX_IDX0        0x0808
#define ETH_PTX_DTX_IDX0        0x080c
#define ETH_PDMA_GLO_CFG        0x0a04

/* PDMA TX descriptor (16 bytes): {SDP0, len/LS0/DDONE, SDP1, fport/...} */
#define PTX_DDONE               (1u << 31)
#define PTX_LS0                 (1u << 30)
#define PTX_SDL0(w)             (((w) >> 16) & 0x3fff)
#define PTX_LS1                 (1u << 14)
#define PTX_SDL1(w)             ((w) & 0x3fff)
#define PTX_FPORT(w)            (((w) >> 25) & 0x7)

/* DMA engines (MT7621: PDMA receive rings, QDMA transmit) */
#define ETH_PRX_BASE_PTR0       0x0900
#define ETH_PRX_MAX_CNT0        0x0904
#define ETH_PRX_CRX_IDX0        0x0908
#define ETH_PRX_DRX_IDX0        0x090c
#define ETH_PDMA_INT_STATUS     0x0a20
#define ETH_PDMA_INT_MASK       0x0a28
#define ETH_QDMA_GLO_CFG        0x1a04
#define ETH_QTX_CTX_PTR         0x1b00
#define ETH_QTX_DTX_PTR         0x1b04
#define ETH_QTX_CTX_PTR1        0x1b20
#define ETH_QTX_DTX_PTR1        0x1b24
#define ETH_QTX_DRX_PTR         0x1b14
#define ETH_QDMA_INT_STATUS     0x1a18
#define ETH_QDMA_INT_MASK       0x1a1c

#define PDMA_RX_DONE            (1u << 16)
/*
 * The driver normally runs the frame engine with *delayed* RX interrupts:
 * PDMA_RX_DLY_INT_CFG (0x0a08) / TX_DLY_INT_CFG (0x0a0c) are programmed and
 * MTK_PDMA_INT_MASK is set to MTK_RX_DONE_DLY (bit 30) instead of
 * MTK_RX_DONE_INT0.  The hardware then coalesces completions and raises that
 * one latched bit; the driver's ISR tests the same bit in INT_STATUS.
 *
 * Modelling only bit 16 means a driver in delayed mode is never interrupted,
 * its rx NAPI never polls, and every frame we place in the ring stays there
 * with RX_DMA_DONE set - which looks exactly like "the tap never sent
 * anything".
 */
#define PDMA_RX_DLY_INT         (1u << 30)
/*
 * The Linux raeth driver masks 0x0003000f in PDMA_INT_MASK: TX done for the
 * four queues in bits 0..3 and RX done for the two RX rings in bits 16..17.
 * RouterBOOT only polls, so these used to be missing entirely - which left
 * Linux waiting for TX completions until NETDEV WATCHDOG fired.
 */
#define PDMA_TX_DONE0           (1u << 0)
/*
 * Delayed TX completion: a driver that programs PDMA_TX_DLY_INT_CFG (0x0a0c)
 * masks MTK_TX_DONE_DLY (bit 28) instead of the per-queue bits 0..3, and the
 * hardware latches that single bit once the coalescing counter fires.  The
 * model only ever raised the per-queue bits, so such a driver waited for a
 * completion that could never arrive: every queue filled to
 * max_tx_buffers_per_port, the stack reported "transmit queue 0 timed out"
 * and RouterOS restarted the switch in a loop.  Raised only when the guest
 * has actually masked it, so poll-only users (RouterBOOT) are unaffected.
 */
#define PDMA_TX_DLY_INT         (1u << 28)
/*
 * RX_DONE_INT0 is bit 16 of PDMA_INT_STATUS, and the driver does enable it
 * (its mask is 0x0003000f: TX done for four queues in bits 0..3, RX done for
 * the two RX rings in bits 16..17).  The old model raised bit 30 instead,
 * which is outside every mask the driver programs, so Linux was never told
 * that a frame had landed: it kept posting RX descriptors but never ran its
 * NAPI poll on its own and CRX_IDX0 stayed at the value written during probe
 * (0xff) while DRX_IDX0 walked the ring.  Raising it only when the mask bit
 * is set keeps it away from RouterBOOT, whose TFTP state machine polls
 * PDMA_INT_STATUS and wedges on unexpected bits.
 */
#define QDMA_TX_DONE            (1u << 28)
/*
 * TX descriptor ownership: the CPU fills a descriptor (owner bit clear) and
 * the DMA engine sets it again once the frame has left the chip.  The driver
 * only reclaims descriptors whose owner bit is set, so it must be written
 * back here.
 */
#define TX_DMA_OWNER_CPU        (1u << 31)
#define TX_DMA_FPORT_SHIFT      25
#define TX_DMA_TSO              (1u << 28)
#define TX_DMA_LS0              (1u << 30)   /* last buffer of this frame */
#define TX_DMA_CHKSUM           (0x7u << 29)
/* QDMA TX ring: 2048 descriptors of 16 bytes. */
#define TX_RING_BYTES           0x8000

/* RX descriptor bits. */
#define RX_DMA_FPORT_SHIFT      19
#define RX_DMA_SPECIAL_TAG      (1u << 22)
#define TX_DMA_PLEN0(x)         ((((x) & 0x3fff)) << 16)
#define RX_DMA_DONE             (1u << 31)
#define RX_DMA_PLEN0(x)         ((((x) & 0x3fff)) << 16)

#endif /* HW_MIPS_MT7621_ETH_H */
