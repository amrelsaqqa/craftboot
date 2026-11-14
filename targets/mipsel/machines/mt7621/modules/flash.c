/*
 * MediaTek MT7621 -- the SPI NOR controller (0x1e000b00) and the W25Q128FV
 * die behind it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "mt7621.h"

/*
 * SPI NOR controller (0x1e000b00).  Register layout, per the Linux and
 * U-Boot drivers:
 *
 *   0x00 TRANS    bit 8 start, bit 16 busy, [3:0] command bytes,
 *                 bit 6 = capture a 4-byte RX word (RouterBOOT style)
 *   0x04 OPCODE   up to 4 command bytes, big-endian
 *   0x08 DATA0..  FIFO, little-endian bytes
 *   0x28 MASTER   clock select / more-buffer mode
 *   0x2c MOREBUF  command/MISO/MOSI bit counts (Linux style)
 *   0x38 POLAR    chip select (written 0 to deassert)
 *   0x3c SPACE    dummy clocks
 *
 * The flash itself is a Winbond W25Q128FV (EF 40 18), matching the ID in
 * the board's hard configuration.
 */
#define MT7621_SPI_OFF          0x00000b00
#define SPI_TRANS               0x00
#define SPI_OPCODE              0x04
#define SPI_DATA0               0x08
#define SPI_MASTER              0x28
#define SPI_MOREBUF             0x2c
#define SPI_POLAR               0x38
#define SPI_SPACE               0x3c

#define SPI_TRANS_START         (1u << 8)
#define SPI_TRANS_BUSY          (1u << 16)
#define SPI_TRANS_RXWORD        (1u << 6)

static const uint8_t mt7621_spi_id[3] = { 0xef, 0x40, 0x18 };

/* MT7621_SPI_JEDEC=0xef4018: override the RDID bytes (identity probing). */
static void mt7621_spi_jedec(uint8_t out[3])
{
    const char *p = getenv("MT7621_SPI_JEDEC");
    unsigned long v;

    memcpy(out, mt7621_spi_id, 3);
    if (!p) {
        return;
    }
    v = strtoul(p, NULL, 0);
    out[0] = (v >> 16) & 0xff;
    out[1] = (v >> 8) & 0xff;
    out[2] = v & 0xff;
}

static uint8_t mt7621_flash_read_byte(MT7621State *s, uint32_t addr)
{
    if (s->flash && addr < s->flash_size) {
        return s->flash[addr];
    }
    return 0xff;
}

static void mt7621_flash_write_bytes(MT7621State *s, uint32_t addr,
                                     const uint8_t *buf, int len)
{
    int i;

    if (!s->flash) {
        if (getenv("MT7621_WRLOG")) {
            fprintf(stderr, "spi: WR helper called with s->flash == NULL "
                    "(addr=%06x len=%d)\n", addr, len);
        }
        return;
    }
    if (getenv("MT7621_WRLOG")) {
        static int prog_budget = 16;
        int dirty = 0;
        uint8_t before = addr < s->flash_size ? s->flash[addr] : 0xee;

        if (addr < s->flash_size) {
            for (i = 0; i < len; i++) {
                if (s->flash[addr + i] != 0xff) {
                    dirty++;
                }
            }
        }
        if (dirty && prog_budget-- > 0) {
            fprintf(stderr, "spi: PROG-NONERASED addr=%06x len=%d dirty=%d "
                    "first=%02x\n", addr, len, dirty, before);
        } else if (prog_budget == 16) {
            fprintf(stderr, "spi: WR helper addr=%06x len=%d flash_size=%06llx "
                    "before=%02x\n", addr, len,
                    (unsigned long long)s->flash_size, before);
            prog_budget--;
        }
    }
    for (i = 0; i < len; i++) {
        if (addr + i < s->flash_size) {
            s->flash[addr + i] &= buf[i];   /* NOR can only clear bits */
        }
    }
}

static void mt7621_flash_erase(MT7621State *s, uint32_t addr, uint32_t len)
{
    uint32_t start = addr & ~(len - 1);

    if (getenv("MT7621_WRLOG")) {
        fprintf(stderr, "spi: ER helper addr=%06x len=%06x -> start=%06x "
                "flash=%p size=%06llx inrange=%d\n", addr, len, start,
                (void *)s->flash, (unsigned long long)s->flash_size,
                !!(s->flash && start + len <= s->flash_size));
    }
    if (s->flash && start + len <= s->flash_size) {
        memset(s->flash + start, 0xff, len);
    }
}

/*
 * Clock the given MOSI bytes through the flash and fill miso_out with what
 * the chip returns.  bytes_out is advanced only for the RX phase (bytes
 * after cmd_len), which is what the controller captures into the FIFO.
 */
/* Diagnostics: log every flash *modifying* SPI transaction. */
static int mt7621_wrlog_budget = 40000;

static void mt7621_spi_flash_run(MT7621State *s, const uint8_t *mosi,
                                 int n, int cmd_len, uint8_t *miso, int rx_len)
{
    int i;
    int pos = 0;
    int rx_pos = cmd_len;
    uint32_t args[8];
    int nargs = 0;

    memset(miso, 0xff, rx_len);

    if (getenv("MT7621_SPILOG")) {
        fprintf(stderr, "spi: op %02x n=%d cmd=%d rx=%d bytes=",
                n > 0 ? mosi[0] : -1, n, cmd_len, rx_len);
        for (i = 0; i < n; i++) {
            fprintf(stderr, "%02x", mosi[i]);
        }
        fprintf(stderr, "\n");
    }

    if (n <= 0) {
        /* A read-only transaction continuing a READ command. */
        if (s->spi_read_mode) {
            for (i = 0; i < rx_len; i++) {
                miso[i] = mt7621_flash_read_byte(s, s->spi_addr++);
            }
        }
        return;
    }

    /* The opcode is the first byte; the rest is address / data. */
    for (i = 1; i < n && nargs < (int)ARRAY_SIZE(args); i++) {
        args[nargs++] = mosi[i];
    }

    if (getenv("MT7621_WRLOG")) {
        /* Everything that is not a plain read/ID/status query is interesting:
         * those are the paths that modify the device.  Only *logged* ops
         * consume the budget, otherwise the boot's reads eat it. */
        switch (mosi[0]) {
        case 0x03: case 0x0b: case 0xeb: case 0x05: case 0x35:
        case 0x9f: case 0x4b: case 0x90: case 0xab:
            break;
        default:
            if (mt7621_wrlog_budget-- > 0) {
                fprintf(stderr, "spi: OP op=%02x n=%d cmd=%d rx=%d bytes=",
                        mosi[0], n, cmd_len, rx_len);
                for (i = 0; i < n; i++) {
                    fprintf(stderr, "%02x", mosi[i]);
                }
                fprintf(stderr, "\n");
            }
            break;
        }
    }

    switch (mosi[0]) {
    case 0x9f:                          /* RDID */
    {
        uint8_t id[3];

        mt7621_spi_jedec(id);
        s->spi_read_mode = false;
        /*
         * RouterBOOT reads the JEDEC ID as a 32-bit word and compares the
         * whole thing against the table entry 0x001840ef (ef 40 18), so the
         * fourth byte has to be zero - not 0xff - or the booter decides the
         * chip is unknown and disables flash programming.
         */
        for (i = 0; i < (int)rx_len; i++) {
            miso[i] = i < 3 ? id[i] : 0x00;
        }
        break;
    }
    case 0x4b:                          /* RDUID (Winbond): 64-bit UID */
        /*
         * flash_get_uid() reads nine bytes - one dummy followed by the ID -
         * and prints them as a little-endian u64, so emit the dummy and then
         * the ID least-significant byte first.
         */
        s->spi_read_mode = false;
        for (i = 0; i < rx_len; i++) {
            if (i == 0) {
                miso[i] = 0xff;
            } else if (i <= 8) {
                miso[i] = (uint8_t)(s->spi_uid >> (8 * (i - 1)));
            } else {
                miso[i] = 0xff;
            }
        }
        break;
    case 0x90:                          /* manufacturer + device ID */
        s->spi_read_mode = false;
        for (i = 0; i < rx_len; i++) {
            miso[i] = i == 0 ? 0xef : (i == 1 ? 0x17 : 0x00);
        }
        break;
    case 0x05:                          /* RDSR */
    case 0x35:                          /* RDSR2 */
        s->spi_read_mode = false;
        for (i = 0; i < rx_len; i++) {
            miso[i] = 0x00;             /* not busy, no protection */
        }
        break;
    case 0x03:                          /* READ */
    case 0x0b: {                        /* FAST READ */
        static int rdlog_budget = 8;
        uint32_t addr = 0;

        for (i = 0; i < 3 && i < nargs; i++) {
            addr = (addr << 8) | args[i];
        }
        s->spi_addr = addr;
        s->spi_read_mode = true;
        rx_pos = mosi[0] == 0x0b ? 5 : 4;
        for (i = 0; i < rx_len; i++) {
            miso[i] = mt7621_flash_read_byte(s, s->spi_addr++);
        }
        if (getenv("MT7621_SPILOG") && rdlog_budget-- > 0) {
            fprintf(stderr, "spi: read op=%02x addr=%06x len=%d -> %02x %02x "
                    "%02x %02x %02x %02x\n", mosi[0], addr, rx_len,
                    miso[0], miso[1], miso[2], miso[3], miso[4], miso[5]);
        }
        if (getenv("MT7621_RDLOG")) {
            /* Semantic read trace: what the guest asked the flash for. Keeps
             * the mount/verify sequence visible without SPILOG's register
             * firehose. */
            static int rdlog2 = 200000;
            if (rdlog2-- > 0) {
                fprintf(stderr, "spi: RD addr=%06x len=%d %02x%02x%02x%02x\n",
                        addr, rx_len, miso[0], miso[1], miso[2], miso[3]);
            }
        }
        break;
    }
    case 0x02:                          /* page program */
    case 0x01:                          /* write status */
    case 0x06:                          /* write enable */
    case 0x04:                          /* write disable */
    case 0x20:                          /* sector erase 4K */
    case 0x52:                          /* block erase 32K */
    case 0xd8:                          /* block erase 64K */
    case 0x60:                          /* chip erase */
    case 0xc7: {
        uint32_t addr = 0;

        for (i = 0; i < 3 && i < nargs; i++) {
            addr = (addr << 8) | args[i];
        }
        switch (mosi[0]) {
        case 0x02:
            mt7621_flash_write_bytes(s, addr, mosi + 4, n - 4);
            break;
        case 0x20:
            mt7621_flash_erase(s, addr, 4096);
            break;
        case 0x52:
            mt7621_flash_erase(s, addr, 32768);
            break;
        case 0xd8:
            mt7621_flash_erase(s, addr, 65536);
            break;
        case 0x60:
        case 0xc7:
            if (s->flash) {
                memset(s->flash, 0xff, s->flash_size);
            }
            break;
        default:
            break;
        }
        s->spi_read_mode = false;
        break;
    }
    default:
        s->spi_read_mode = false;
        break;
    }

    /* RX bytes were already produced above for the read commands; for the
     * others, RX captures 0xff. */
    (void)pos;
    (void)rx_pos;
}

/*
 * Opcodes the model actually implements (plus the common JEDEC ones), used to
 * disambiguate the MOREBUF command byte order.
 */
static bool mt7621_spi_opcode_known(unsigned op)
{
    switch (op) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
    case 0x0b: case 0x13: case 0x20: case 0x35: case 0x3b: case 0x4b:
    case 0x52: case 0x60: case 0x90: case 0x9f: case 0xab: case 0xc7:
    case 0xd8: case 0xeb:
        return true;
    default:
        return false;
    }
}

static void mt7621_spi_exec(MT7621State *s)
{
    uint32_t trans = s->spi_regs[SPI_TRANS / 4];
    uint32_t morebuf = s->spi_regs[SPI_MOREBUF / 4];
    uint32_t opcode = s->spi_regs[SPI_OPCODE / 4];
    uint8_t mosi[80];
    uint8_t miso[32];
    unsigned cmd_len, mosi_len, rx_len;
    uint32_t cmd_bits = (morebuf >> 24) & 0x3f;
    uint32_t miso_bits = (morebuf >> 12) & 0x1ff;
    uint32_t mosi_bits = morebuf & 0x1ff;
    int i;

    if (morebuf) {
        cmd_len = cmd_bits / 8;
        mosi_len = mosi_bits / 8;
        rx_len = miso_bits / 8;
    } else {
        /*
         * Legacy (non MOREBUF) mode: TRANS holds the MOSI byte count in
         * bits 3:0 and the MISO byte count in bits 7:4.  RouterBOOT drives
         * the block this way and packs the command word as
         *
         *      opcode            in bits  7:0
         *      address byte 23:16 in bits 31:24
         *      address byte 15:8  in bits 23:16
         *      address byte  7:0  in bits 15:8
         *
         * so the wire order is opcode first, then the address MSB first.
         */
        /*
         * TRANS[3:0] is the *total* number of bytes shifted out: the
         * command word (up to 4 bytes) plus any data bytes that follow it
         * from the FIFO.  Page program issues 8 (02 + 3 address bytes +
         * the 4 payload bytes stashed in DATA0), while one-byte commands
         * such as WREN use 1 and plain reads use 4.
         */
        cmd_len = trans & 0xf;
        mosi_len = cmd_len > 4 ? cmd_len - 4 : 0;
        cmd_len = cmd_len > 4 ? 4 : cmd_len;
        rx_len = (trans >> 4) & 0xf;
    }
    if (getenv("MT7621_SPISTAT")) {
        static unsigned long long tx;
        static uint32_t low, high;
        uint32_t a = ((opcode >> 24) << 16) | ((opcode >> 8) & 0xffff);

        tx++;
        if (cmd_len >= 4 && (opcode & 0xff) == 0x03) {
            if (!low || a < low) {
                low = a;
            }
            if (a > high) {
                high = a;
            }
        }
        if (!(tx % 50000)) {
            fprintf(stderr, "spi: %llu tx, last op=%08x addr=%06x, "
                    "read window %06x..%06x\n",
                    tx, opcode, a, low, high);
        }
    }
    if (cmd_len + mosi_len > sizeof(mosi)) {
        mosi_len = sizeof(mosi) - cmd_len;
    }
    if (rx_len > sizeof(s->spi_rx)) {
        rx_len = sizeof(s->spi_rx);
    }

    if (getenv("MT7621_SPILOG")) {
        fprintf(stderr, "spi: exec trans=%08x more=%08x op=%08x "
                "cmd_len=%u mosi_len=%u rx_len=%u\n",
                trans, morebuf, opcode, cmd_len, mosi_len, rx_len);
    }
    if (getenv("MT7621_SPIENC")) {
        fprintf(stderr, "spienc: trans=%08x more=%08x op=%08x cmd=%u mosi=%u rx=%u\n",
                trans, morebuf, opcode, cmd_len, mosi_len, rx_len);
    }

    if (morebuf) {
        /*
         * MOREBUF command bytes come from the OPCODE register, but the two
         * firmware generations pack it differently:
         *
         *   RouterOS 7.18 agent : OPCODE=0x03000014 -> 03 00 00 14 (read @0x14)
         *   RouterOS 6.36       : OPCODE=0x00000005 -> 05 00 00 00 (RDSR)
         *
         * i.e. one puts the opcode in the high byte and the other in the low
         * byte with everything else zero.  Decide from whichever byte holds
         * a real flash command; default to MSB-first (the hardware layout).
         */
        if (!mt7621_spi_opcode_known((opcode >> 24) & 0xff) &&
            mt7621_spi_opcode_known(opcode & 0xff)) {
            for (i = 0; i < (int)cmd_len; i++) {
                mosi[i] = (opcode >> (8 * i)) & 0xff;
            }
        } else {
            for (i = 0; i < (int)cmd_len; i++) {
                mosi[i] = (opcode >> (8 * (cmd_len - 1 - i))) & 0xff;
            }
        }
    } else {
        static const int rot[4] = { 0, 24, 16, 8 };
        for (i = 0; i < (int)cmd_len; i++) {
            mosi[i] = (opcode >> rot[i]) & 0xff;
        }
    }
    for (i = 0; i < (int)mosi_len; i++) {
        mosi[cmd_len + i] = ((const uint8_t *)&s->spi_regs[SPI_DATA0 / 4])[i];
    }

    if (mt7621_trace) {
        QEMU_BUILD_BUG_ON(0);
    }

    memset(miso, 0xff, sizeof(miso));
    if (cmd_len == 0 && rx_len == 0 && mosi_len == 0) {
        /* nothing to do */
    } else if (cmd_len == 0) {
        if (s->spi_read_mode) {
            for (i = 0; i < (int)rx_len; i++) {
                miso[i] = mt7621_flash_read_byte(s, s->spi_addr++);
            }
        }
    } else {
        mt7621_spi_flash_run(s, mosi, cmd_len + mosi_len, cmd_len,
                             miso, rx_len);
    }

    for (i = 0; i < (int)rx_len; i++) {
        ((uint8_t *)&s->spi_regs[SPI_DATA0 / 4])[i] = miso[i];
    }

    trans &= ~(SPI_TRANS_START | SPI_TRANS_BUSY);
    s->spi_regs[SPI_TRANS / 4] = trans;
}

static uint64_t mt7621_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & 0xff;
    uint32_t val;

    if (off >= 0x04 && off < 0x24) {
        /* FIFO: expose the individual bytes written by the driver. */
        val = ((const uint32_t *)s->spi_regs)[off / 4];
    } else {
        val = s->spi_regs[off / 4];
    }
    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: spi read  %03x -> %08x\n",
                      (unsigned)off, val);
    }
    return val;
}

static void mt7621_spi_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & 0xff;

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: spi write %03x <- %08x\n",
                      (unsigned)off, (uint32_t)val);
    }
    if (getenv("MT7621_SPILOG") &&
        (off <= SPI_DATA0 + 0x18 || off == SPI_MASTER ||
         off == SPI_MOREBUF || off == SPI_POLAR)) {
        fprintf(stderr, "spi: reg %03x <- %08x\n",
                (unsigned)off, (uint32_t)val);
    }
    if (getenv("MT7621_SPIREG")) {
        static int spireg_budget = 200000;
        if (spireg_budget-- > 0) {
            fprintf(stderr, "spi: REG %03x <- %08x (pc=%08x)\n",
                    (unsigned)off, (uint32_t)val,
                    (unsigned)s->pc_hist[(s->pc_hist_idx - 1) % PC_HIST]);
        }
    }
    if (off < 0x04) {
        s->spi_regs[off / 4] = val;
        if (val & SPI_TRANS_START) {
            mt7621_spi_exec(s);
        }
        return;
    }
    if (off >= 0x04 && off < 0x24) {
        ((uint32_t *)s->spi_regs)[off / 4] = val;
        return;
    }
    s->spi_regs[off / 4] = val;
    if (off == SPI_POLAR && val == 0) {
        /* Chip select deasserted: the next read starts a new command. */
        s->spi_read_mode = false;
    }
}

static const MemoryRegionOps mt7621_spi_ops = {
    .read = mt7621_spi_read,
    .write = mt7621_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};


void mt7621_spi_init(MT7621State *s)
{
    memory_region_init_io(&s->spi, NULL, &mt7621_spi_ops, s, "mt7621.spi", 0x100);
    memory_region_add_subregion(&s->sysctl, MT7621_SPI_OFF, &s->spi);
}
