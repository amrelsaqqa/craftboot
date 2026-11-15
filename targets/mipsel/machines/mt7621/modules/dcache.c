/*
 * MT7621A (MIPS 1004Kc) data cache model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * QEMU has no cache model: KSEG0 (cached) and KSEG1 (uncached) both read and
 * write the same RAM.  On silicon they do not.  A store through KSEG1 (the
 * DMA aliases, 0xA0000000|p) goes straight to memory and does not update the
 * copy of that line that the cache holds from earlier KSEG0 traffic.
 *
 * That difference is observable in the firmware this machine runs.  Breed's
 * DMA wrapper allocates a buffer, rounds the pointer up to the cache line size
 * and clears n = size - 1 + 2*line bytes from the rounded pointer *through the
 * uncached alias*.  When the rounded pointer is not line aligned the clear
 * runs a few bytes past the allocation, into the next chunk's size field -
 * which the allocator wrote through KSEG0 moments earlier.  On hardware the
 * cache still returns the old size, so the next malloc finds a valid block and
 * the boot continues; without a cache model the zero is immediately visible,
 * the allocator sees a zero-sized block and gives up ("unable to allocate
 * fifos").
 *
 * Geometry is the 1004Kc's L1D (32 KiB, 4-way, line size from Config1.Dl).
 * The cache is write-through, because the firmware writes relocated code
 * through KSEG0 and executes it without ever issuing a cache or synci
 * instruction - stores have to reach memory for instruction fetch to work.
 * Reads come from the line once it is resident, which is what makes an
 * uncached write leave a stale copy behind, exactly as on the board.
 *
 * Cached pages are forced through the TCG slow path, where
 * mt7621_dcache_access() serves the data access from here instead of RAM;
 * uncached pages and device accesses never reach it.  `cache` instructions are
 * applied through mt7621_dcache_cache_op() and synci through
 * mt7621_dcache_cache_op(type=data, op=hit writeback invalidate).
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/core/cpu.h"
#include "exec/cpu-dcache.h"
#include "exec/mips-dcache.h"
#include "system/memory.h"
#include "mt7621.h"

/* 1004Kc L1 D-cache: 32 KiB, 4-way. */
#define MT7621_DCACHE_BYTES   (32 * KiB)
#define MT7621_DCACHE_WAYS    4
#define MT7621_DCACHE_MAXLINE 64

#define KSEG0_BASE 0x80000000u
#define KSEG2_BASE 0xc0000000u

/* implemented in target/mips; declared here so the machine can ask */
bool mips_mapping_is_cacheable(CPUMIPSState *env, target_ulong address,
                               MMUAccessType access_type, int mmu_idx);

static MT7621State *dcache_state;

static MT7621DLine *dcache_lookup(MT7621State *s, hwaddr pa, bool *hit)
{
    unsigned shift = s->dcache.line_shift;
    unsigned set = (pa >> shift) & (s->dcache.nsets - 1);
    MT7621DLine *line = &s->dcache.lines[set * MT7621_DCACHE_WAYS];
    uint64_t tag = pa >> shift;
    int i;

    for (i = 0; i < MT7621_DCACHE_WAYS; i++) {
        if (line[i].valid && line[i].tag == tag) {
            *hit = true;
            return &line[i];
        }
    }
    *hit = false;
    return line;
}

static void dcache_writeback(MT7621State *s, MT7621DLine *line)
{
    if (line->valid && line->dirty) {
        memcpy(s->dcache.ram + (line->tag << s->dcache.line_shift),
               line->data, s->dcache.line_size);
        line->dirty = false;
    }
}

static MT7621DLine *dcache_fill(MT7621State *s, hwaddr pa)
{
    unsigned shift = s->dcache.line_shift;
    unsigned set = (pa >> shift) & (s->dcache.nsets - 1);
    MT7621DLine *line = &s->dcache.lines[set * MT7621_DCACHE_WAYS];
    MT7621DLine *victim = &line[0];
    int i;

    for (i = 0; i < MT7621_DCACHE_WAYS; i++) {
        if (!line[i].valid) {
            victim = &line[i];
            break;
        }
        if (line[i].lru < victim->lru) {
            victim = &line[i];
        }
    }
    if (victim->valid) {
            dcache_writeback(s, victim);
    }
    victim->tag = pa >> shift;
    victim->valid = true;
    victim->dirty = false;
    memcpy(victim->data, s->dcache.ram + ((pa >> shift) << shift),
           s->dcache.line_size);
    return victim;
}

static uint8_t dcache_read_byte(MT7621State *s, hwaddr pa)
{
    bool hit;
    MT7621DLine *line = dcache_lookup(s, pa, &hit);

    if (!hit) {
        line = dcache_fill(s, pa);
    }
    line->lru = ++s->dcache.clock;
    return line->data[pa & (s->dcache.line_size - 1)];
}

/*
 * Write-through: the caller also sends the store to the next level.  This is
 * what the guest depends on - it writes relocated code through KSEG0 and
 * executes it without ever issuing a cache or synci instruction, so stores
 * must reach memory.  Reads still come from the line, which is why an
 * uncached write (the DMA clear) leaves the cached copy stale.
 */
static void dcache_store(MT7621State *s, hwaddr pa, uint8_t v)
{
    bool hit;
    MT7621DLine *line = dcache_lookup(s, pa, &hit);

    if (!hit) {
        line = dcache_fill(s, pa);
    }
    line->data[pa & (s->dcache.line_size - 1)] = v;
    line->dirty = false;
}

/*
 * Slow-path data access hook.  Only called for data accesses to pages the
 * guest mapped as cacheable; the access may cross a line or a page, so it is
 * served byte by byte through the cache.
 */
static bool mt7621_dcache_access(CPUState *cs, vaddr addr, unsigned size,
                                 bool is_write, uint64_t *value)
{
    MT7621State *s = dcache_state;
    CPUMIPSState *env;
    hwaddr pa, p;
    uint64_t w = value ? *value : 0;
    uint64_t r = 0;
    unsigned done = 0, left = size;

    if (!s || !s->dcache.enabled || size > 8) {
        return false;
    }
    env = cpu_env(cs);
    if (!mips_mapping_is_cacheable(env, addr,
                                   is_write ? MMU_DATA_STORE : MMU_DATA_LOAD,
                                   0)) {
        return false;
    }
    if (addr >= KSEG0_BASE && addr < KSEG2_BASE) {
        /* kseg0/kseg1 are unmapped: the physical address is a mask */
        pa = addr & 0x1fffffff;
    } else {
        pa = cpu_get_phys_page_debug(cs, addr);
        if (pa == (hwaddr)-1) {
            return false;
        }
    }

    /* Accesses may span a line; MIPS is little endian. */
    p = pa;
    while (left) {
        unsigned line_off = p & (s->dcache.line_size - 1);
        unsigned n = MIN(left, s->dcache.line_size - line_off);
        bool hit;
        MT7621DLine *line = dcache_lookup(s, p, &hit);

        if (!hit) {
            line = dcache_fill(s, p);
        }
        line->lru = ++s->dcache.clock;
        if (is_write) {
            memcpy(line->data + line_off, (uint8_t *)&w + done, n);
        } else {
            memcpy((uint8_t *)&r + done, line->data + line_off, n);
        }
        p += n;
        left -= n;
        done += n;
    }
    /*
     * Write-through: the store also reaches the next level.  The guest writes
     * relocated code through KSEG0 and executes it without a cache or synci
     * instruction, so stores must be visible to instruction fetch.  Reads come
     * from the line, which is why the uncached DMA clear leaves it stale.
     */
    if (is_write) {
        memcpy(s->dcache.ram + pa, &w, done);
    } else {
        *value = r;
    }
    return true;
}

void mt7621_dcache_invalidate_line(MT7621State *s, hwaddr pa)
{
    bool hit;
    MT7621DLine *line = dcache_lookup(s, pa, &hit);

    if (hit) {
        line->valid = false;
        line->dirty = false;
    }
}

void mt7621_dcache_writeback_line(MT7621State *s, hwaddr pa)
{
    bool hit;
    MT7621DLine *line = dcache_lookup(s, pa, &hit);

    if (hit) {
        dcache_writeback(s, line);
    }
}

void mt7621_dcache_invalidate_set(MT7621State *s, unsigned set)
{
    MT7621DLine *line = &s->dcache.lines[set * MT7621_DCACHE_WAYS];
    int i;

    for (i = 0; i < MT7621_DCACHE_WAYS; i++) {
        line[i].valid = false;
        line[i].dirty = false;
    }
}

void mt7621_dcache_flush_all(MT7621State *s, bool invalidate)
{
    unsigned n = s->dcache.nsets * MT7621_DCACHE_WAYS;
    unsigned i;

    for (i = 0; i < n; i++) {
        dcache_writeback(s, &s->dcache.lines[i]);
        if (invalidate) {
            s->dcache.lines[i].valid = false;
        }
    }
}

/*
 * Apply a `cache` instruction to the data side.  Called from target/mips.
 * Index operations select a set by address; hit operations take a virtual
 * address, translated here to the physical line the cache is tagged with.
 */
void mt7621_dcache_cache_op(CPUState *cs, vaddr addr, unsigned type,
                            unsigned op)
{
    MT7621State *s = dcache_state;

    if (!s || !s->dcache.enabled) {
        return;
    }
    if (type == 0) {
        /* primary instruction cache: the icache shadow owns that side */
        return;
    }
    if (op <= 3) {
        /* index invalidate / index store tag initialise the cache */
        if (op == 1) {
            return;             /* index load tag: nothing reads tags back */
        }
        mt7621_dcache_invalidate_set(s, (addr >> s->dcache.line_shift) %
                                        s->dcache.nsets);
        return;
    }

    hwaddr pa = cpu_get_phys_page_debug(cs, addr);

    if (pa == (hwaddr)-1) {
        return;
    }
    switch (op) {
    case 4:                     /* hit invalidate */
        mt7621_dcache_invalidate_line(s, pa);
        break;
    case 5:                     /* hit writeback invalidate */
        mt7621_dcache_writeback_line(s, pa);
        mt7621_dcache_invalidate_line(s, pa);
        break;
    case 6:                     /* hit writeback */
        mt7621_dcache_writeback_line(s, pa);
        break;
    default:
        break;
    }
}

/*
 * (Re)configure the cache from the CPU's Config1, so the geometry follows the
 * Dl the machine presents: Dl=2 -> 8-byte lines, Dl=4 -> 32 like the board.
 */
void mt7621_dcache_reset(MT7621State *s)
{
    CPUState *cs = qemu_get_cpu(0);
    unsigned dl, line;

    if (!s->dcache.enabled || !cs) {
        return;
    }
    dl = (cpu_env(cs)->CP0_Config1 >> 10) & 7;
    line = 1u << (dl + 1);      /* Breed's encoding: Dl=2 -> 8, Dl=4 -> 32 */
    if (line < 4 || line > MT7621_DCACHE_MAXLINE) {
        line = 32;
    }
    s->dcache.line_size = line;
    s->dcache.line_shift = ctz32(line);
    s->dcache.nsets = MT7621_DCACHE_BYTES / (line * MT7621_DCACHE_WAYS);
    mt7621_dcache_flush_all(s, true);
}

void mt7621_dcache_init(MT7621State *s)
{
    if (!getenv("MT7621_DCACHE")) {
        return;
    }
    dcache_state = s;
    s->dcache.lines = g_new0(MT7621DLine, MT7621_DCACHE_BYTES / 4);
    s->dcache.enabled = true;
    s->dcache.line_size = 32;
    s->dcache.line_shift = 5;
    s->dcache.nsets = MT7621_DCACHE_BYTES / (32 * MT7621_DCACHE_WAYS);

    /* DRAM as its own address space: fills and writebacks bypass the CPU. */
    s->dcache.ram = memory_region_get_ram_ptr(&s->ram);
    cpu_dcache_access_handler = mt7621_dcache_access;
    mips_dcache_op = mt7621_dcache_cache_op;
    fprintf(stderr, "mt7621: L1 data cache model enabled\n");
}
