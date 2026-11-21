/*
 * MediaTek MT7621 -- the NAND flash interface (NFI, 0x1e003000) and the
 * Samsung K9F1G08U0B die behind it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "mt7621.h"

#define MT7621_NFI_OFF          0x00003000

#define NFI_CNFG                0x00
#define NFI_PAGEFMT             0x04
#define NFI_CON                 0x08
#define NFI_ACCCON              0x0c
#define NFI_INTR_EN             0x10
#define NFI_INTR_STA            0x14
#define NFI_CMD                 0x20
#define NFI_ADDRNOB             0x30
#define NFI_COLADDR             0x34
#define NFI_ROWADDR             0x38
#define NFI_STRDATA             0x40
#define NFI_CNRNB               0x44
#define NFI_DATAW               0x50
#define NFI_DATAR               0x54
#define NFI_PIO_DIRDY           0x58
#define NFI_STA                 0x60
#define NFI_ADDRCNTR            0x70
#define NFI_CSEL                0x90

#define MT7621_NAND_PAGES_BLK   64

/*
 * The controller works on 512-byte sectors.  With AUTO_FMT_EN the data port
 * streams data sectors only and the spare bytes come out through the FDM
 * registers; without it the port streams the raw page, i.e. each sector is
 * followed by its own spare bytes.  The address space is therefore
 *
 *   raw:  [512 data][16 spare] x4     (2112 bytes)
 *   data: [512 data] x4              (2048 bytes, spare via FDML/FDMM)
 *
 * RouterBOOT's NAND driver (and U-Boot's mt7621_nand) read the bad-block mark
 * with a one-byte access at column `ecc.size + badblockpos` (512), which the
 * hardware resolves to the first spare byte of sector 0.  Treating every
 * column as plain page data made every block whose byte 512 was not 0xff look
 * bad, so the layout has to follow the controller, not the dump.
 */
#define MT7621_NFI_SECTOR_SIZE  512
#define MT7621_NFI_SECTORS      (MT7621_NAND_PAGE_SIZE / MT7621_NFI_SECTOR_SIZE)
#define MT7621_NFI_SPARE        (MT7621_NAND_OOB_SIZE / MT7621_NFI_SECTORS)


/*
 * MT7621_NAND_BADBLOCKS=17,18,... marks blocks bad the way the factory does:
 * a non-0xff byte in the first spare byte of the block's first pages.  The
 * dump carries no OOB, so the model synthesises 0xff there and every block
 * looks good -- unlike the real device, which reports nine bad blocks starting
 * at 0x220000 (block 17).  This knob reproduces the hardware map so the
 * yaffs/format failure can be studied without touching real NAND.
 */
static bool mt7621_nand_block_bad(MT7621State *s, uint64_t row)
{
    const char *list = getenv("MT7621_NAND_BADBLOCKS");
    uint64_t blk;

    if (!list) {
        return false;
    }
    blk = row / MT7621_NAND_PAGES_BLK;
    if (getenv("MT7621_NAND_BBLOG")) {
        fprintf(stderr, "nand-bb: query row=%" PRIu64 " (block %" PRIu64 ")\n",
                row, blk);
    }
    while (*list) {
        char *end;
        unsigned long v = strtoul(list, &end, 0);

        if (end == list) {
            break;
        }
        if ((uint64_t)v == blk) {
            return true;
        }
        list = end;
        while (*list == ',' || *list == ' ') {
            list++;
        }
    }
    return false;
}

/*
 * One spare byte of a page, as the controller presents it: the die image's
 * OOB when the dump carries one, the programmed shadow otherwise, and the
 * factory bad-block mark (spare[0] of the block's first two pages) on top.
 * Every view that exposes spare bytes - byte reads, FDM registers, page
 * assembly - has to go through this, otherwise a mark written by
 * MT7621_NAND_BADBLOCKS (or by the guest) stays invisible to whichever path
 * the driver happens to use.
 */
static uint8_t mt7621_nand_oob_byte(MT7621State *s, uint64_t row, unsigned oi)
{
    uint8_t v = 0xff;

    if (oi >= MT7621_NAND_OOB_SIZE) {
        return 0xff;
    }
    if (s->nand &&
        s->nand_stride == MT7621_NAND_PAGE_SIZE + MT7621_NAND_OOB_SIZE) {
        uint64_t off = row * s->nand_stride;

        if (off + MT7621_NAND_PAGE_SIZE + oi < s->nand_size) {
            v = s->nand[off + MT7621_NAND_PAGE_SIZE + oi];
        }
    } else if (s->nand_oob && row < s->nand_oob_pages) {
        v = s->nand_oob[row * MT7621_NAND_OOB_SIZE + oi];
    }
    if (oi == 0 && (row % MT7621_NAND_PAGES_BLK) < 2 &&
        mt7621_nand_block_bad(s, row)) {
        v = 0x00;
    }
    return v;
}

/* Store spare bytes for a page; a data-only dump keeps them in a shadow. */
static void mt7621_nand_oob_store(MT7621State *s, uint64_t row, uint32_t off,
                                  const uint8_t *buf, uint32_t n)
{
    if (!s->nand_oob || row >= s->nand_oob_pages) {
        return;
    }
    if (off >= MT7621_NAND_OOB_SIZE) {
        return;
    }
    if (off + n > MT7621_NAND_OOB_SIZE) {
        n = MT7621_NAND_OOB_SIZE - off;
    }
    memcpy(s->nand_oob + row * MT7621_NAND_OOB_SIZE + off, buf, n);
}

static inline uint32_t mt7621_nfi_data_to_raw(uint32_t idx)
{
    uint32_t sector = idx / MT7621_NFI_SECTOR_SIZE;
    uint32_t off = idx % MT7621_NFI_SECTOR_SIZE;

    return sector * (MT7621_NFI_SECTOR_SIZE + MT7621_NFI_SPARE) + off;
}

/*
 * NAND flash interface.
 *
 * The controller is modelled at the level the firmware drives it: commands
 * go to NFI_CMD, addresses to COLADDR/ROWADDR, and data is pulled out of
 * NFI_DATAR while NFI_PIO_DIRDY reports that the next byte is ready.  Status
 * is always "idle" because the emulated flash answers instantly.
 *
 * The backing store is the same image passed with -kernel, which is the raw
 * NAND dump: page 2048 bytes, OOB 64 bytes, 64 pages per block.  The dump
 * holds only the main area, so the OOB is synthesised all-0xff, i.e. every
 * block good.
 */
static void mt7621_nfi_fifo_put(MT7621State *s, const void *data, size_t len)
{
    if (len > sizeof(s->nfi_fifo)) {
        len = sizeof(s->nfi_fifo);
    }
    memcpy(s->nfi_fifo, data, len);
    s->nfi_fifo_len = len;
    s->nfi_fifo_pos = 0;
}

/*
 * The Linux driver sends each address byte on its own (COLADDR followed by
 * ADDRNOB = 1); two column bytes and three row bytes make up a page
 * address.  Breed instead programs the count once, so fall back to the
 * COLADDR/ROWADDR fields when no byte stream was collected.
 */
static uint64_t mt7621_nfi_addr_col(MT7621State *s)
{
    if (s->nfi_addr_len >= 2) {
        s->nfi_col = s->nfi_addr[0] | (s->nfi_addr[1] << 8);
    }
    return s->nfi_col;
}

static uint64_t mt7621_nfi_addr_row(MT7621State *s)
{
    if (s->nfi_addr_len >= 3) {
        uint64_t row = s->nfi_addr[2];

        if (s->nfi_addr_len > 3) {
            row |= (uint64_t)s->nfi_addr[3] << 8;
        }
        if (s->nfi_addr_len > 4) {
            row |= (uint64_t)s->nfi_addr[4] << 16;
        }
        s->nfi_row = row;
    }
    return s->nfi_row;
}

static void mt7621_nfi_page_read(MT7621State *s)
{
    uint8_t buf[MT7621_NAND_PAGE_SIZE + MT7621_NAND_OOB_SIZE];
    uint64_t off;

    memset(buf, 0xff, sizeof(buf));

    mt7621_nfi_addr_col(s);
    mt7621_nfi_addr_row(s);

    off = (uint64_t)s->nfi_row * s->nand_stride;
    if (s->nand && off < s->nand_size) {
        uint64_t n = s->nand_size - off;

        if (n > s->nand_stride) {
            n = s->nand_stride;
        }
        memcpy(buf, s->nand + off, n);     /* data + spare when present */
    }

    /*
     * Build the raw page the controller exposes: every 512-byte sector is
     * followed by its 16 spare bytes.  Data-only reads (AUTO_FMT_EN) map
     * through mt7621_nfi_data_to_raw(), byte reads use this raw layout
     * directly, which is how a column of 512 reaches the first spare byte.
     */
    {
        unsigned sec;
        uint8_t *dst = s->nfi_fifo;

        for (sec = 0; sec < MT7621_NFI_SECTORS; sec++) {
            memcpy(dst + sec * (MT7621_NFI_SECTOR_SIZE + MT7621_NFI_SPARE),
                   buf + sec * MT7621_NFI_SECTOR_SIZE,
                   MT7621_NFI_SECTOR_SIZE);
            if (s->nand_stride == MT7621_NAND_PAGE_SIZE +
                                   MT7621_NAND_OOB_SIZE) {
                memcpy(dst + sec * (MT7621_NFI_SECTOR_SIZE +
                                    MT7621_NFI_SPARE) +
                       MT7621_NFI_SECTOR_SIZE,
                       buf + MT7621_NAND_PAGE_SIZE +
                       sec * MT7621_NFI_SPARE, MT7621_NFI_SPARE);
            } else {
                uint8_t *sp = dst + sec * (MT7621_NFI_SECTOR_SIZE +
                                           MT7621_NFI_SPARE) +
                              MT7621_NFI_SECTOR_SIZE;

                if (s->nand_oob &&
                    (uint64_t)s->nfi_row < s->nand_oob_pages) {
                    memcpy(sp, s->nand_oob +
                           (uint64_t)s->nfi_row * MT7621_NAND_OOB_SIZE +
                           sec * MT7621_NFI_SPARE, MT7621_NFI_SPARE);
                } else {
                    memset(sp, 0xff, MT7621_NFI_SPARE);
                }
            }
        }
    }

    if ((s->nfi_row % MT7621_NAND_PAGES_BLK) < 2 &&
        mt7621_nand_block_bad(s, s->nfi_row)) {
        /* spare[0] of sector 0: the bad-block marker the BBT reads at col 512 */
        s->nfi_fifo[MT7621_NFI_SECTOR_SIZE] = 0x00;
    }

    s->nfi_served = 0;
    s->nfi_fifo_len = MT7621_NAND_PAGE_SIZE + MT7621_NAND_OOB_SIZE;
    s->nfi_fifo_pos = 0;
}

/*
 * Optional write-through to a block backend, so a session's NAND changes
 * survive QEMU restarts without touching the pristine dump.  Usage:
 *   -drive file=nand.img,if=none,id=nand,format=raw
 * The image is seeded from the -kernel dump the first time it is used.
 */
int mt7621_pwrite_all(int fd, const void *buf, size_t len, off_t off)
{
    const uint8_t *p = buf;

    while (len) {
        ssize_t n = pwrite(fd, p, len, off);

        if (n <= 0) {
            return -1;
        }
        p += n;
        off += n;
        len -= n;
    }
    return 0;
}

static void mt7621_nand_persist(MT7621State *s, uint64_t off,
                                const void *data, uint64_t len)
{
    bool failed;

    if (off + len > s->nand_size) {
        return;
    }
    if (s->nand_fd >= 0) {
        failed = mt7621_pwrite_all(s->nand_fd, data, len, off) < 0;
    } else if (s->nand_blk) {
        failed = blk_pwrite(s->nand_blk, off, len, data, 0) < 0;
    } else {
        return;
    }
    if (failed && !s->nand_wr_error) {
        warn_report("mt7621: cannot write the NAND image, changes are "
                    "kept in RAM only");
        s->nand_wr_error = true;
    }
}

static void mt7621_nfi_cmd(MT7621State *s, uint32_t cmd)
{
    /*
     * Samsung K9F1G08U0B: 1 Gbit, 2 KiB pages, 64 B spare, 128 KiB blocks.
     * Both Breed and Linux know this part; Breed also derives the same
     * geometry from byte 3 when its table only matches on byte 1.
     * Byte 3 is the legacy geometry code: bits 1:0 scale the page size from
     * 1 KiB (so 1 = 2 KiB), bit 2 doubles the spare area (8 -> 16 bytes per
     * 512-byte sector, i.e. 64 bytes per page) and bits 5:4 scale the block
     * size from 64 KiB (so 1 = 128 KiB).
     */
    uint8_t status = 0xe0;

    if (getenv("MT7621_NFILOG")) {
        fprintf(stderr, "nfi: cmd %02x col %x row %x\n", cmd, s->nfi_col, s->nfi_row);
    }
    s->nfi_served = 0;
    s->nfi_last_cmd = cmd;
    /*
     * Only commands that begin a new operation restart the address byte
     * stream; program/erase confirm (0x10/0xd0) and read start (0x30)
     * must still see the address they were given.
     */
    switch (cmd) {
    case 0x00:
    case 0x60:
    case 0x80:
    case 0x90:
    case 0xff:
        s->nfi_addr_len = 0;
        break;
    default:
        break;
    }

    switch (cmd) {
    case 0x90:                          /* read id */
    {
        static const uint8_t no_id[8] = { 0xff, 0xff, 0xff, 0xff,
                                          0xff, 0xff, 0xff, 0xff };
        unsigned cs = s->nfi_regs[NFI_CSEL >> 2] & 0x3;

        /*
         * The die is on CS0 only.  Breed probes CS0 and CS1 during scan; the
         * model used to answer both, so the same NAND was reported twice,
         * which cost an extra page buffer and shifted every later allocation.
         */
        if (cs != 0) {
            if (getenv("MT7621_CSLOG")) {
                fprintf(stderr, "nfi: id probe on cs=%u -> no die\n", cs);
            }
            mt7621_nfi_fifo_put(s, no_id, sizeof(no_id));
        } else {
            if (getenv("MT7621_CSLOG")) {
                fprintf(stderr, "nfi: id probe on cs=%u\n", cs);
            }
            mt7621_nfi_fifo_put(s, s->nand_id, sizeof(s->nand_id));
        }
        break;
    }
    case 0x30:                          /* read: page data + spare */
    case 0xec:                          /* cache read (mtk-nandc variant) */
        /*
         * 0xEC was missing: the driver issues it (six times during the
         * bad-block scan) and then polls NFI_PIO_DIRDY, which stayed 0
         * because nothing had filled the FIFO.  Each miss cost the driver
         * its full PIO timeout (~6 s), enough to trip the RCU stall detector
         * and to drag the whole boot out.  It reads the page currently in
         * the cache, like 0x30.
         */
        mt7621_nfi_page_read(s);
        break;
    case 0x70:                          /* read status */
        mt7621_nfi_fifo_put(s, &status, 1);
        break;
    case 0x80:                          /* page program */
        s->nfi_wbuf_len = 0;
        s->nfi_wcol = s->nfi_col;       /* where this program starts */
        s->nfi_writing = true;
        break;
    case 0x10:                          /* program confirm */
        if (s->nfi_writing && s->nand) {
            uint64_t row = mt7621_nfi_addr_row(s);
            uint64_t off = row * s->nand_stride;
            uint32_t n = s->nfi_wbuf_len;
            uint32_t wcol = s->nfi_wcol;

            if (wcol >= MT7621_NAND_PAGE_SIZE) {
                /* OOB-only program (tags written after the data page) */
                mt7621_nand_oob_store(s, row,
                                      wcol - MT7621_NAND_PAGE_SIZE,
                                      s->nfi_wbuf, n);
            } else if (n >= MT7621_NAND_PAGE_SIZE + MT7621_NAND_OOB_SIZE) {
                /*
                 * Raw page stream: [512 data][16 spare] per sector.  Split it,
                 * because a main-area-only dump has nowhere to put the spare
                 * bytes and yaffs keeps its tags there.
                 */
                unsigned sec;

                for (sec = 0; sec < MT7621_NFI_SECTORS; sec++) {
                    const uint8_t *src = s->nfi_wbuf +
                        sec * (MT7621_NFI_SECTOR_SIZE + MT7621_NFI_SPARE);
                    uint64_t doff = off + sec * MT7621_NFI_SECTOR_SIZE;

                    if (doff + MT7621_NFI_SECTOR_SIZE <= s->nand_size) {
                        memcpy(s->nand + doff, src, MT7621_NFI_SECTOR_SIZE);
                        mt7621_nand_persist(s, doff, src,
                                            MT7621_NFI_SECTOR_SIZE);
                    }
                    mt7621_nand_oob_store(s, row,
                                          sec * MT7621_NFI_SPARE,
                                          src + MT7621_NFI_SECTOR_SIZE,
                                          MT7621_NFI_SPARE);
                }
            } else {
                if (n > s->nand_stride) {
                    n = s->nand_stride;
                }
                if (off + n <= s->nand_size) {
                    memcpy(s->nand + off, s->nfi_wbuf, n);
                    mt7621_nand_persist(s, off, s->nfi_wbuf, n);
                }
            }
        }
        s->nfi_writing = false;
        {
            uint8_t st = 0xe0;
            mt7621_nfi_fifo_put(s, &st, 1);
        }
        break;
    case 0xd0:                          /* erase confirm */
        if (s->nand) {
            uint64_t row = mt7621_nfi_addr_row(s);
            uint64_t blk_start = (row / MT7621_NAND_PAGES_BLK) *
                                 MT7621_NAND_PAGES_BLK * s->nand_stride;
            uint64_t blk_size = (uint64_t)MT7621_NAND_PAGES_BLK *
                                s->nand_stride;

            if (blk_start + blk_size <= s->nand_size) {
                memset(s->nand + blk_start, 0xff, blk_size);
                mt7621_nand_persist(s, blk_start, s->nand + blk_start,
                                    blk_size);
            }
            if (s->nand_oob) {
                uint64_t blk = row / MT7621_NAND_PAGES_BLK;

                if ((blk + 1) * MT7621_NAND_PAGES_BLK <= s->nand_oob_pages) {
                    memset(s->nand_oob + blk * MT7621_NAND_PAGES_BLK *
                           MT7621_NAND_OOB_SIZE, 0xff,
                           (uint64_t)MT7621_NAND_PAGES_BLK *
                           MT7621_NAND_OOB_SIZE);
                }
            }
        }
        {
            uint8_t st = 0xe0;
            mt7621_nfi_fifo_put(s, &st, 1);
        }
        break;
    case 0x00:                          /* read setup / reset */
    case 0xff:
        s->nfi_fifo_len = 0;
        s->nfi_fifo_pos = 0;
        s->nfi_served = 0;
        break;
    default:
        break;
    }
}

static uint8_t mt7621_nfi_data_byte(MT7621State *s)
{
    uint32_t cnfg = s->nfi_regs[NFI_CNFG >> 2];
    uint32_t logical = s->nfi_col + s->nfi_served;
    uint32_t idx;

    /*
     * The legacy main-area dump exposes the spare area at columns 2048..2111;
     * keep that view working for the booters that use it.  This is also the
     * view nand_read_oob() takes, i.e. what UBI's bad-block probe and the
     * on-flash BBT see, so it has to report the programmed spare bytes *and*
     * the factory bad-block marks - reading straight from the dump here made
     * MT7621_NAND_BADBLOCKS invisible to every Linux guest.
     */
    if (s->nfi_col >= MT7621_NAND_PAGE_SIZE) {
        uint32_t oi = (s->nfi_col - MT7621_NAND_PAGE_SIZE) + s->nfi_served;

        return mt7621_nand_oob_byte(s, s->nfi_row, oi);
    }

    if (cnfg & (1u << 6)) {             /* BYTE_RW: raw page layout */
        idx = logical;
    } else if (cnfg & (1u << 9)) {      /* AUTO_FMT_EN: data sectors only */
        idx = mt7621_nfi_data_to_raw(logical);
    } else {                            /* raw page layout */
        idx = logical;
    }

    {
        uint8_t v = idx < s->nfi_fifo_len ? s->nfi_fifo[idx] : 0xff;

        if (getenv("MT7621_NAND_BBLOG") && s->nfi_col == 512) {
            static int bbbyte_budget = 12;

            if (bbbyte_budget-- > 0) {
                fprintf(stderr, "nand-bb: byte col=%u served=%u cnfg=%08x "
                        "idx=%u (fifo_len=%u) -> %02x\n",
                        (unsigned)s->nfi_col, (unsigned)s->nfi_served,
                        (unsigned)cnfg, (unsigned)idx,
                        (unsigned)s->nfi_fifo_len, v);
            }
        }
        return v;
    }
}

static uint64_t mt7621_nfi_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & 0xfff;
    static int budget = 200;

    if (getenv("MT7621_NFIREGS") && budget-- > 0) {
        fprintf(stderr, "nfi rd %03x\n", (unsigned)off);
    }
    uint32_t word = s->nfi_regs[off >> 2];
    uint64_t val;

    /*
     * ECC block (0x800+): the emulated flash is error free, so report every
     * engine as idle with zero corrected/uncorrectable bits.  Writing the
     * control registers has no effect that the firmware can observe.
     */
    if (off >= 0x800) {
        /*
         * 0x80c / 0x90c are the encoder and decoder status words; bit 0
         * reports "engine idle", so it has to read set.  Everything else
         * (corrected / uncorrectable counts) is zero for clean data.
         */
        if (off == 0x80c || off == 0x90c) {
            return 1;               /* encoder / decoder idle */
        }
        if (off == 0x918) {
            return 0xffff;          /* decoder done, one bit per sector */
        }
        return 0;                   /* no corrected / uncorrectable bits */
    }

    switch (off) {
    case NFI_DATAR:
    {
        uint32_t v = 0;
        unsigned i;

        /*
         * CNFG_BYTE_RW selects byte mode: the data port then hands out one
         * byte per access, even for word-wide reads.  Page transfers clear
         * it and move `size` bytes per access.
         */
        if (s->nfi_regs[NFI_CNFG >> 2] & (1u << 6)) {
            val = mt7621_nfi_data_byte(s);
            s->nfi_served++;
            break;
        }

        for (i = 0; i < size; i++) {
            v |= (uint32_t)mt7621_nfi_data_byte(s) << (8 * i);
            s->nfi_served++;
        }
        val = v;
        break;
    }
    case NFI_PIO_DIRDY:
        /*
         * The controller is never busy: data appears in the FIFO the moment a
         * command asks for it.  Bit 0 means "PIO direction ready", i.e. the
         * interface can take or hand over a byte -- not "a byte is pending",
         * which is what the FIFO drain below reports.  Breed 1.2 polls it
         * between operations and treats 0 as "PIO mode not ready".
         */
        val = 1;
        if (getenv("MT7621_NFILOG") && !val) {
            fprintf(stderr, "pio not ready: last cmd %02x col %x row %x addrlen %u\n",
                    s->nfi_last_cmd, s->nfi_col, s->nfi_row, s->nfi_addr_len);
        }
        break;
    case NFI_INTR_STA:
        /* Report everything the firmware enabled as already pending. */
        val = s->nfi_regs[NFI_INTR_EN >> 2];
        break;
    case NFI_STA:
        val = 0;
        break;
    case NFI_ADDRCNTR:
        /* SEC_CNTR is bits 15:12; report all sectors transferred so the
         * driver's program-completion poll succeeds immediately. */
        val = 0x0000f000;
        break;
    case 0xa0 ... 0xbf:
    {
        /*
         * NFI_FDML/FDMM: the spare bytes the controller extracted from the
         * last page.  These used to be hard-wired to 0xffffffff ("the dump
         * has no OOB"), which meant a guest that reads the spare through the
         * FDM registers - RouterOS's mt7621-nand driver does, in AUTO_FMT
         * mode - could never see a bad-block mark or any programmed OOB.
         * Serve the real spare bytes, four per register, wrapping over the
         * 64-byte area.
         */
        unsigned w = (off - 0xa0) >> 2;
        unsigned base = (w * 4) & (MT7621_NAND_OOB_SIZE - 1);

        val = (uint32_t)mt7621_nand_oob_byte(s, s->nfi_row, base) |
              ((uint32_t)mt7621_nand_oob_byte(s, s->nfi_row, base + 1) << 8) |
              ((uint32_t)mt7621_nand_oob_byte(s, s->nfi_row, base + 2) << 16) |
              ((uint32_t)mt7621_nand_oob_byte(s, s->nfi_row, base + 3) << 24);
        break;
    }
    default:
        switch (size) {
        case 1:
            val = (word >> ((off & 3) * 8)) & 0xff;
            break;
        case 2:
            val = (word >> ((off & 2) * 8)) & 0xffff;
            break;
        default:
            val = word;
            break;
        }
        break;
    }

    if (getenv("MT7621_NFISPIN")) {
        /*
         * Diagnostic: a driver stuck in a poll loop reads the same register
         * with the same value thousands of times.  Keep one bucket per
         * offset so timer reads interleaved with the poll do not reset the
         * count, and report the first bucket that reaches 5000.
         */
        typedef struct {
            uint32_t off;
            uint32_t val;
            unsigned count;
            bool reported;
        } SpinBucket;
        static SpinBucket buckets[16];
        unsigned i;

        for (i = 0; i < ARRAY_SIZE(buckets); i++) {
            if (buckets[i].off == (uint32_t)off &&
                buckets[i].val == (uint32_t)val) {
                if (!buckets[i].count) {
                    buckets[i].off = off;
                    buckets[i].val = val;
                }
                if (++buckets[i].count == 5000 && !buckets[i].reported) {
                    buckets[i].reported = true;
                    fprintf(stderr, "nfi spin: off=%03x val=%08x\n",
                            (unsigned)off, (uint32_t)val);
                }
                break;
            }
        }
        if (i == ARRAY_SIZE(buckets)) {
            /* find a free or least-used bucket */
            unsigned victim = 0;

            for (i = 0; i < ARRAY_SIZE(buckets); i++) {
                if (!buckets[i].count) {
                    victim = i;
                    break;
                }
                if (buckets[i].count < buckets[victim].count) {
                    victim = i;
                }
            }
            buckets[victim].off = off;
            buckets[victim].val = (uint32_t)val;
            buckets[victim].count = 1;
            buckets[victim].reported = false;
        }
    }
    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: nfi read  %03x (size %u) -> %0*"
                      PRIx64 "\n", (unsigned)off, size, size * 2, val);
    }
    return val;
}

static void mt7621_nfi_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    MT7621State *s = opaque;
    static int budget = 200;

    if (getenv("MT7621_NFIREGS") && budget-- > 0) {
        fprintf(stderr, "nfi wr %03x = %08x\n", (unsigned)(addr & 0xfff),
                (unsigned)val);
    }
    uint32_t off = addr & 0xfff;
    uint32_t shift = (off & 3) * 8;
    uint32_t mask;

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: nfi write %03x (size %u) <- %0*"
                      PRIx64 "\n", (unsigned)off, size, size * 2, val);
    }

    if (off >= 0x800) {
        return;                 /* ECC control: no visible effect */
    }

    switch (off) {
    case NFI_DATAW:
        if (s->nfi_writing) {
            unsigned i;

            for (i = 0; i < size && s->nfi_wbuf_len < sizeof(s->nfi_wbuf); i++) {
                s->nfi_wbuf[s->nfi_wbuf_len++] =
                    ((uint32_t)val >> (8 * i)) & 0xff;
            }
        }
        return;
    case NFI_CMD:
        s->nfi_regs[off >> 2] = (uint32_t)val;
        mt7621_nfi_cmd(s, (uint32_t)val);
        return;
    case NFI_CON:
        s->nfi_regs[off >> 2] = (uint32_t)val;
        if (val & 0x2) {        /* NFI_RST */
            s->nfi_fifo_len = 0;
            s->nfi_fifo_pos = 0;
            s->nfi_served = 0;
            s->nfi_col = 0;
            s->nfi_row = 0;
        }
        return;
    case NFI_ADDRNOB:
        /*
         * Two conventions: Breed programs a cycle count once (e.g. 0x22 =
         * two column + two row cycles) with the values in COLADDR/ROWADDR,
         * while the Linux driver sends every address byte separately as
         * COLADDR followed by ADDRNOB = 1.  Accumulate the byte stream.
         */
        s->nfi_regs[off >> 2] = (uint32_t)val;
        if (val == 1) {
            if (s->nfi_addr_len < ARRAY_SIZE(s->nfi_addr)) {
                s->nfi_addr[s->nfi_addr_len++] = s->nfi_addr_pending & 0xff;
            }
        } else {
            s->nfi_addr_len = 0;
        }
        return;
    case NFI_COLADDR:
        s->nfi_addr_pending = (uint32_t)val;
        s->nfi_col = (uint32_t)val;
        s->nfi_regs[off >> 2] = (uint32_t)val;
        return;
    case NFI_ROWADDR:
        if (getenv("MT7621_NFILOG")) {
            fprintf(stderr, "rowaddr <- %x (size %u)\n", (unsigned)val, size);
        }
        s->nfi_row = (uint32_t)val;
        s->nfi_regs[off >> 2] = (uint32_t)val;
        return;
    default:
        break;
    }

    switch (size) {
    case 1:
        mask = 0xff;
        break;
    case 2:
        shift = (off & 2) * 8;
        mask = 0xffff;
        break;
    default:
        shift = 0;
        mask = 0xffffffff;
        break;
    }
    s->nfi_regs[off >> 2] =
        (s->nfi_regs[off >> 2] & ~(mask << shift)) |
        ((((uint32_t)val) & mask) << shift);
}

static const MemoryRegionOps mt7621_nfi_ops = {
    .read = mt7621_nfi_read,
    .write = mt7621_nfi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

void mt7621_nand_load(MT7621State *s, const char *filename)
{
    int fd;
    off_t sz;

    if (!filename) {
        return;
    }

    fd = open(filename, O_RDONLY | O_BINARY);
    if (fd < 0) {
        return;
    }

    sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (sz > 0) {
        s->nand = g_malloc(sz);
        if (pread(fd, s->nand, sz, 0) == sz) {
            s->nand_size = sz;
        } else {
            g_free(s->nand);
            s->nand = NULL;
        }
    }
    close(fd);

    /*
     * Two dump layouts are common:
     *   main area only : pages of 2048 bytes  (e.g. 128 MiB for 128 MiB
     *                    of flash, 65536 pages)
     *   full die dump  : pages of 2048 + 64 spare (132 MiB for the same
     *                    flash); "mtdall" style dumps look like this
     * Pick the one that gives a power-of-two page count, which real NAND
     * always has.
     */
    if (s->nand_size) {
        uint64_t pages = s->nand_size / (MT7621_NAND_PAGE_SIZE +
                                         MT7621_NAND_OOB_SIZE);
        const char *lay = getenv("MT7621_NAND_LAYOUT");
        const char *src = getenv("MT7621_NAND_SOURCE");
        bool src_die = false, src_data = false, forced = false;

        /*
         * The guest always sees a die: a NAND part has a spare area, and the
         * controller hands it out through NFI_FDML/FDMM (and the legacy
         * 2048+ view) whether or not the image file carries it.
         *
         * What the knob selects is the *file format*, i.e. where the model
         * reads pages from:
         *
         *   MT7621_NAND_SOURCE=die   file pages are 2112 bytes (2048 data +
         *                            64 spare, interleaved) - a full-die /
         *                            "mtdall" dump
         *   MT7621_NAND_SOURCE=data  file pages are 2048 bytes, main area
         *                            only; the spare is synthesised (erased,
         *                            plus any MT7621_NAND_BADBLOCKS marks)
         *   unset                    auto: die when the size divides into a
         *                            power-of-two number of 2112-byte pages,
         *                            data otherwise
         *
         * MT7621_NAND_LAYOUT=die|data is accepted as a legacy alias for the
         * same thing.  Forcing "data" is what makes a truncated main-area
         * image work: its page count is not a nice power of two, and the
         * model must not reinterpret 2048-byte pages as interleaved ones.
         */
        if (!src || !*src) {
            src = lay;
        }
        if (src && (!strcmp(src, "die") || !strcmp(src, "full") ||
                    !strcmp(src, "oob"))) {
            src_die = true;
            forced = true;
        } else if (src && (!strcmp(src, "data") || !strcmp(src, "flat") ||
                           !strcmp(src, "main"))) {
            src_data = true;
            forced = true;
        }

        if (src_die) {
            s->nand_stride = MT7621_NAND_PAGE_SIZE + MT7621_NAND_OOB_SIZE;
            if (s->nand_size % s->nand_stride) {
                warn_report("mt7621: %s is not a whole number of %u-byte "
                            "pages; ignoring the last %u bytes",
                            filename, s->nand_stride,
                            (unsigned)(s->nand_size % s->nand_stride));
            }
        } else if (src_data) {
            s->nand_stride = MT7621_NAND_PAGE_SIZE;
        } else if (s->nand_size % (MT7621_NAND_PAGE_SIZE +
                                   MT7621_NAND_OOB_SIZE)
                   == 0 && pages && !(pages & (pages - 1))) {
            s->nand_stride = MT7621_NAND_PAGE_SIZE + MT7621_NAND_OOB_SIZE;
        } else {
            s->nand_stride = MT7621_NAND_PAGE_SIZE;
        }
        s->nand_pages = s->nand_size / s->nand_stride;
        if (s->nand_stride == MT7621_NAND_PAGE_SIZE && s->nand_pages) {
            s->nand_oob_pages = s->nand_pages;
            s->nand_oob = g_malloc0(s->nand_pages * MT7621_NAND_OOB_SIZE);
            memset(s->nand_oob, 0xff,
                   (size_t)s->nand_pages * MT7621_NAND_OOB_SIZE);
        }
        if (s->nand_stride != MT7621_NAND_PAGE_SIZE) {
            printf("mt7621: NAND dump includes the spare area "
                   "(%u-byte pages, %u pages%s)\n",
                   s->nand_stride, s->nand_pages,
                   forced ? ", forced by MT7621_NAND_SOURCE" : "");
        } else {
            printf("mt7621: NAND dump is main-area only, spare synthesised "
                   "(%u-byte pages, %u pages%s)\n",
                   s->nand_stride, s->nand_pages,
                   forced ? ", forced by MT7621_NAND_SOURCE" : "");
        }
    }
}


void mt7621_nfi_init(MT7621State *s)
{
    memset(s->nfi_regs, 0, sizeof(s->nfi_regs));
    memory_region_init_io(&s->nfi, NULL, &mt7621_nfi_ops, s,
                          "mt7621.nfi", 0x1000);
    memory_region_add_subregion(&s->periph, MT7621_NFI_OFF, &s->nfi);
}

/*
 * Attach an optional writable NAND image passed as
 *   -drive file=nand.img,if=none,id=nand,format=raw
 * A fresh/empty image is seeded from the dump; an existing one is used as
 * is, so writes survive QEMU restarts while the dump stays pristine.
 */
void mt7621_nand_attach(MT7621State *s)
{
    const char *path = getenv("MT7621_NAND_IMAGE");
    BlockBackend *blk;
    int64_t len;
    int fd;
    off_t sz;

    s->nand_fd = -1;

    /*
     * Preferred: a plain image file we create and seed ourselves, so the
     * user does not have to know the flash size:
     *   MT7621_NAND_IMAGE=~/nand-state.img qemu-system-mipsel ...
     */
    if (path) {
        if (!s->nand) {
            warn_report("mt7621: MT7621_NAND_IMAGE needs a readable boot dump");
            return;
        }
        fd = open(path, O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            warn_report("mt7621: cannot open NAND image %s: %s, changes will "
                        "be kept in RAM only", path, strerror(errno));
            return;
        }
        sz = lseek(fd, 0, SEEK_END);
        if (sz < 0) {
            warn_report("mt7621: cannot size %s", path);
            close(fd);
            return;
        }
        if ((uint64_t)sz < s->nand_size) {
            if (mt7621_pwrite_all(fd, s->nand, s->nand_size, 0) < 0) {
                warn_report("mt7621: cannot seed %s", path);
                close(fd);
                return;
            }
            printf("mt7621: NAND image %s seeded from the boot dump "
                   "(%" PRIu64 " bytes)\n", path, s->nand_size);
        } else {
            if (pread(fd, s->nand, s->nand_size, 0) != (ssize_t)s->nand_size) {
                warn_report("mt7621: cannot read %s", path);
                close(fd);
                return;
            }
            printf("mt7621: using persistent NAND image %s\n", path);
        }
        s->nand_fd = fd;
        return;
    }

    /* Alternative, QEMU style (the file has to exist):
     *   -drive file=nand.img,if=none,id=nand,format=raw
     */
    blk = blk_by_name("nand");
    if (!blk) {
        return;
    }
    if (!s->nand) {
        warn_report("mt7621: -drive id=nand needs a readable boot dump");
        return;
    }

    len = blk_getlength(blk);
    if (len < 0) {
        warn_report("mt7621: cannot size the NAND image, ignoring it");
        return;
    }
    if (len < s->nand_size) {
        if (blk_pwrite(blk, 0, s->nand_size, s->nand, 0) < 0) {
            warn_report("mt7621: cannot seed the NAND image, changes will "
                        "be kept in RAM only");
            return;
        }
        printf("mt7621: NAND image seeded from the boot dump (%" PRIu64
               " bytes)\n", s->nand_size);
    } else {
        if (blk_pread(blk, 0, s->nand_size, s->nand, 0) < 0) {
            warn_report("mt7621: cannot read the NAND image, changes will "
                        "be kept in RAM only");
            return;
        }
        printf("mt7621: using the persistent NAND image (%" PRIu64
               " bytes)\n", s->nand_size);
    }
    s->nand_blk = blk;
}

