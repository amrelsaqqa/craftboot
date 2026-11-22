/*
 * MediaTek MT7621 -- the Inside Secure EIP-93 crypto engine at 0x1e004000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "mt7621.h"

/*
 * Minimal Inside Secure EIP-93 crypto engine (crypto@1e004000, periph offset
 * 0x4000).  RouterOS probes PE_REVISION and panics with "EipNumber: N" unless
 * EIP_NO (bits 7:0 of 0x1fc) is the value it expects; the MT7621 pairs with an
 * EIP-93, i.e. EIP_NO = 93 decimal = 0x5d (NOT 0x93 - the field is the IP
 * number, not a hex literal).
 *
 * Register map follows drivers/crypto/inside-secure/eip93/eip93-regs.h.  This
 * models the probe/configuration half: the revision and option registers
 * describe the MT7621's algorithm set, the control/status registers report the
 * engine ready, and every other register is a plain store so the driver's
 * ring setup reads back what it wrote.  Command completion is not modelled -
 * no operation is ever reported done - which is enough for bring-up; real
 * crypto work would need the descriptor ring and an interrupt.
 */
#define MT7621_CRYPTO_OFF       0x00004000
#define MT7621_CRYPTO_SIZE      0x00000800

#define EIP93_REG_PE_CTRL_STAT  0x000
#define EIP93_REG_PE_LENGTH     0x01c
#define EIP93_REG_PE_CD_COUNT   0x090
#define EIP93_REG_PE_RD_COUNT   0x094
#define EIP93_REG_PE_RING_PNTR  0x098
#define EIP93_REG_PE_CONFIG     0x100
#define EIP93_REG_PE_STATUS     0x104
#define EIP93_REG_PE_OPTION_1   0x1f4
#define EIP93_REG_PE_REVISION   0x1fc
#define EIP93_REG_INT_MASK_STAT 0x204
#define EIP93_REG_INT_MASK      0x208

#define EIP93_REV_MAJ           1
#define EIP93_REV_MIN           0
#define EIP93_REV_PATCH         0
#define EIP93_EIP_NO            93      /* decimal, as the register encodes it */

/* AES (128/192/256), DES/TDES, MD5, SHA1, SHA224, SHA256 - MT7621's set. */
#define EIP93_OPTION_1_VAL      ((1u << 19) | (1u << 18) | (1u << 17) | \
                                 (1u << 16) | (1u << 15) | (1u << 14) | \
                                 (1u << 13) | (1u << 2) | (1u << 0))

void mt7621_crypto_reset(MT7621State *s)
{
    memset(s->crypto_regs, 0, sizeof(s->crypto_regs));
    s->crypto_regs[EIP93_REG_PE_REVISION / 4] =
        (EIP93_REV_MAJ << 24) | (EIP93_REV_MIN << 20) |
        (EIP93_REV_PATCH << 16) | EIP93_EIP_NO;
    /*
     * MT7621_CRYPTO_OPTS=0 advertises no algorithms: the driver then registers
     * none and every crypto request takes the kernel's software path.  That is
     * the honest choice while this model does not complete descriptors - a
     * registered-but-never-completing engine would hang real requests, and a
     * faked completion would hand back wrong data.
     */
    {
        const char *o = getenv("MT7621_CRYPTO_OPTS");

        /* Default: advertise nothing (see above), i.e. software crypto by
         * default.  MT7621_CRYPTO_OPTS=<bits> enables the algorithm set once
         * descriptor completion exists; EIP93_OPTION_1_VAL is the full MT7621
         * set. */
        s->crypto_regs[EIP93_REG_PE_OPTION_1 / 4] =
            o ? (strtoul(o, NULL, 0) & EIP93_OPTION_1_VAL) : 0;
    }
    /* PE_READY | HOST_READY: engine up, host may hand it work. */
    s->crypto_regs[EIP93_REG_PE_CTRL_STAT / 4] = 0x3;
}

static uint64_t mt7621_crypto_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & (MT7621_CRYPTO_SIZE - 1);
    uint32_t val;

    switch (off) {
    case EIP93_REG_PE_CTRL_STAT:
        val = 0x3;                      /* still ready */
        break;
    case EIP93_REG_PE_STATUS:
        val = 0;
        break;
    case EIP93_REG_PE_CD_COUNT:
    case EIP93_REG_PE_RD_COUNT:
    case EIP93_REG_PE_RING_PNTR:
        val = 0;                        /* no descriptors completed */
        break;
    case EIP93_REG_PE_REVISION:
        val = s->crypto_regs[off / 4];
        if (getenv("MT7621_CRYPTO_NUM")) {
            val = (val & ~0xffu) | (strtoul(getenv("MT7621_CRYPTO_NUM"),
                                            NULL, 0) & 0xff);
        }
        break;
    default:
        val = s->crypto_regs[off / 4];
        break;
    }
    if (getenv("MT7621_CRYPTLOG")) {
        static int budget = 64;

        if (budget-- > 0) {
            fprintf(stderr, "crypto: read  %04x size=%u -> %08x\n",
                    (unsigned)off, size, val);
        }
    }
    return val;
}

static void mt7621_crypto_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & (MT7621_CRYPTO_SIZE - 1);
    uint32_t val = (uint32_t)value;

    if (getenv("MT7621_CRYPTLOG")) {
        static int budget = 64;

        if (budget-- > 0) {
            fprintf(stderr, "crypto: write %04x <- %08x size=%u\n",
                    (unsigned)off, val, size);
        }
    }
    switch (off) {
    case EIP93_REG_PE_CONFIG:
        if (val & 0x1) {                /* RST_PE */
            mt7621_crypto_reset(s);
            return;
        }
        s->crypto_regs[off / 4] = val;
        return;
    case EIP93_REG_INT_MASK_STAT:
        /* write-one-to-clear status */
        s->crypto_regs[off / 4] &= ~val;
        return;
    case EIP93_REG_PE_CD_COUNT:
    case EIP93_REG_PE_RD_COUNT:
        /* writing the low byte kicks the engine; nothing completes here */
        return;
    case EIP93_REG_PE_REVISION:
    case EIP93_REG_PE_OPTION_1:
        return;                         /* read-only */
    default:
        s->crypto_regs[off / 4] = val;
        return;
    }
}

static const MemoryRegionOps mt7621_crypto_ops = {
    .read = mt7621_crypto_read,
    .write = mt7621_crypto_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};


void mt7621_crypto_init(MT7621State *s)
{
    mt7621_crypto_reset(s);
    memory_region_init_io(&s->crypto, NULL, &mt7621_crypto_ops, s,
                          "mt7621.crypto", MT7621_CRYPTO_SIZE);
    memory_region_add_subregion(&s->periph, MT7621_CRYPTO_OFF, &s->crypto);
}
