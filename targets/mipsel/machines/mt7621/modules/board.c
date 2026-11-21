/*
 * MediaTek MT7621 -- the board layer: which preset this is, and the boot-mode
 * bit in SYSCFG0 that follows from it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "mt7621.h"

#define MT7621_CHIP_MODE        1       /* SYSCFG0[3:0]: parallel NAND */

static MT7621Board mt7621_probe_board(const char *filename)
{
    uint8_t hdr[16];
    int fd;
    ssize_t n;
    uint32_t w;

    fd = open(filename, O_RDONLY | O_BINARY);
    if (fd < 0) {
        return MT7621_BOARD_LG_GAPM7200;
    }
    n = pread(fd, hdr, sizeof(hdr), 0);
    close(fd);
    if (n < 8) {
        return MT7621_BOARD_LG_GAPM7200;
    }
    /* uImage: the parallel-NAND layout used by Breed / OpenWrt. */
    if (ldl_be_p(hdr) == 0x27051956) {
        return MT7621_BOARD_LG_GAPM7200;
    }
    /*
     * RouterBOOT starts with "b 0x28" (beq zero,zero,+9) followed by a
     * zero delay slot and its stage header.  That header is only used by
     * SPI NOR boards, so treat it as the MikroTik preset.
     */
    w = ldl_le_p(hdr);
    if ((w >> 26) == 0x04 && (w & 0x03ffffff) == 9 &&
        ldl_le_p(hdr + 4) == 0) {
        return MT7621_BOARD_RB750GR3;
    }
    return MT7621_BOARD_LG_GAPM7200;
}

void mt7621_board_apply(MT7621State *s, const char *filename)
{
    s->board = MT7621_BOARD_AUTO;
    if (s->board_name) {
        if (!strcmp(s->board_name, "lg-gapm7200") ||
            !strcmp(s->board_name, "gapm7200") ||
            !strcmp(s->board_name, "lg")) {
            s->board = MT7621_BOARD_LG_GAPM7200;
        } else if (!strcmp(s->board_name, "rb750gr3") ||
                   !strcmp(s->board_name, "mikrotik-rb750gr3") ||
                   !strcmp(s->board_name, "mikrotik")) {
            s->board = MT7621_BOARD_RB750GR3;
        } else {
            error_report("mt7621: unknown board '%s' (use lg-gapm7200 or "
                         "rb750gr3)", s->board_name);
            exit(1);
        }
    }
    if (s->board == MT7621_BOARD_AUTO) {
        s->board = mt7621_probe_board(filename);
    }

    /*
     * The die soldered on each board.  The B70 (lg-gapm7200) carries an ESMT
     * 128 MiB part (vendor 0xc8); its own heap dump names it "ESMT NAND
     * 128MiB 3.3V 8-bit".  The model used to answer with the Samsung ID
     * (0xec) for every board, which made firmware report a part that is not
     * there.  Boards can carry ESMT, Winbond, Macronix, ... so this is a
     * per-board default, and -M mt7621,nand-id=... overrides it.
     */
    if (!s->nand_id_set) {
        /*
         * The real GAPM7200 answers 0xc8 0xd1 ("ESMT NAND 128MiB 3,3V
         * 8-bit" in the vendor kernel log).  The preset used to carry
         * 0xc8 0xf1 -- an ESMT vendor byte with the wrong chip byte, so
         * the guest identified a part that is not soldered on the board
         * and any identity derived from the flash ID (RouterOS software-id
         * among them) came out different from the hardware.
         */
        static const uint8_t esmt_128mib[8] = { 0xc8, 0xd1, 0x00, 0x15,
                                                0x40, 0x00, 0x00, 0x00 };
        static const uint8_t samsung_128mib[8] = { 0xec, 0xf1, 0x00, 0x15,
                                                   0x40, 0x00, 0x00, 0x00 };
        const uint8_t *id = s->board == MT7621_BOARD_LG_GAPM7200
                            ? esmt_128mib : samsung_128mib;

        memcpy(s->nand_id, id, sizeof(s->nand_id));
    }

    if (s->board == MT7621_BOARD_RB750GR3) {
        s->chip_mode = 2;       /* SYSCFG0: boot from SPI 3-byte address */
    } else {
        s->chip_mode = MT7621_CHIP_MODE;    /* parallel NAND */
    }

    /*
     * Cache geometry lives in the 1004Kc model (cpu-defs.c.inc): 32 KiB,
     * 4-way, 32-byte lines.  Firmware and Linux both derive their flush
     * strategy from it, and Linux only installs r4k_blast_icache/dcache for
     * line sizes 16/32/64/128, so it must stay plausible.  MT7621_CFG1 can
     * still override the descriptor bits for experiments.
     */
}



