/*
 * MT7621A (MIPS 1004Kc) L1 data cache model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * QEMU has no cache model: KSEG0 (cached) and KSEG1 (uncached) both read and
 * write the same RAM.  On silicon they do not.  A store through KSEG1 (the DMA
 * aliases, 0xA0000000|p) goes straight to memory and leaves whatever the cache
 * holds for that line untouched.
 *
 * That difference is observable in the firmware this machine runs.  Breed's DMA
 * wrapper allocates a buffer, rounds the pointer up to the cache line size and
 * clears n = size - 1 + 2*line bytes from the rounded pointer *through the
 * uncached alias*.  When the rounded pointer is not line aligned the clear runs
 * a few bytes past the allocation, into the next chunk's size field - which the
 * allocator wrote through KSEG0 moments earlier.  On hardware the cache still
 * holds the old size, so the next malloc finds a valid block and the boot
 * continues; without a cache the zero is immediately visible, the allocator
 * sees a zero-sized block and gives up ("unable to allocate fifos").
 *
 * This is therefore a real L1D, not an approximation of one:
 *
 *   - 32 KiB, 4-way, line size from Config1.Dl (Dl=4 -> 32 bytes, as the board
 *     reports), true LRU within a set;
 *   - every access the guest makes through a cacheable mapping goes through it
 *     (target/mips forces those pages onto the slow path), so hits, misses,
 *     fills and evictions are the ones the hardware would make;
 *   - a store is write-through: the line is updated here and the store itself
 *     is handed back to QEMU, which writes memory as usual.  The firmware
 *     writes relocated code through KSEG0 and executes it without a cache or
 *     synci instruction, so stores have to reach memory; performing that write
 *     inside the helper instead would skip QEMU's dirty and translated-block
 *     bookkeeping, and invalidating a TB from here makes QEMU re-execute the
 *     instruction, so the store would land twice;
 *   - both KSEG1 and device accesses bypass the cache entirely: they reach
 *     memory and never disturb a line, which is exactly how an uncached store
 *     leaves a stale copy behind;
 *   - `cache` instructions are applied for real: hit invalidate and hit
 *     writeback-invalidate drop the line (writeback is a no-op in a
 *     write-through cache), index operations invalidate the addressed set.
 *     synci arrives here as a hit writeback-invalidate.
 *
 * Nothing here is tuned to a guest.  There is no staleness window, no ageing
 * and no "page was written" heuristic: whether a read sees an old value is
 * decided by the same rules the silicon uses - is the line resident, and has
 * anything invalidated it since.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/core/cpu.h"
#include "exec/cpu-dcache.h"
#include "exec/mips-dcache.h"
#include "system/physmem.h"
#include "exec/translation-block.h"
#include "mt7621.h"

/* 1004Kc L1 D-cache: 32 KiB, 4-way. */
#define MT7621_DCACHE_BYTES   (32 * KiB)
#define MT7621_DCACHE_WAYS    4
#define MT7621_DCACHE_MAXLINE 64

/* mips_mapping_is_cacheable() lives in target/mips/system/physaddr.c */

static MT7621State *dcache_state;

/* Counters, reported with MT7621_STALETRACE=1 (diagnostics only). */
static bool dcache_trace;
static uint64_t dcache_hits, dcache_misses, dcache_fills, dcache_invals;
static uint64_t dcache_stores;

/*
 * MT7621_DSTALE=1 reports reads that were served from a resident line whose
 * bytes no longer match memory, i.e. exactly the reads the guest would not
 * make without a cache.  It is how the model's staleness is audited.
 */
static bool dcache_stale_on;
static int dcache_stale_budget = 200;

static inline uint8_t *dcache_ram(MT7621State *s, hwaddr pa)
{
    return (uint8_t *)s->dcache.ram + pa;
}

/* The resident line for this physical address, or the set's first way. */
static MT7621DLine *dcache_lookup(MT7621State *s, hwaddr pa, bool *hit)
{
    unsigned shift = s->dcache.line_shift;
    MT7621DLine *line = &s->dcache.lines[((pa >> shift) % s->dcache.nsets) *
                                          MT7621_DCACHE_WAYS];
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

/* Refill a line from memory, replacing the least recently used way. */
static MT7621DLine *dcache_fill(MT7621State *s, hwaddr pa)
{
    unsigned shift = s->dcache.line_shift;
    unsigned size = s->dcache.line_size;
    MT7621DLine *line = &s->dcache.lines[((pa >> shift) % s->dcache.nsets) *
                                          MT7621_DCACHE_WAYS];
    MT7621DLine *victim = &line[0];
    hwaddr base = (pa >> shift) << shift;
    unsigned n = size;
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
    if (base + size > s->dcache.size) {
        n = s->dcache.size - base;      /* the last partial line */
    }
    if (n) {
        memcpy(victim->data, dcache_ram(s, base), n);
    }
    if (n < size) {
        memset(victim->data + n, 0, size - n);
    }
    victim->tag = pa >> shift;
    victim->valid = true;
    dcache_fills++;
    return victim;
}

/* Translate a data address to a physical one, or -1. */
static hwaddr dcache_pa(CPUState *cs, vaddr addr)
{
    if (addr >= KSEG0_BASE && addr < KSEG2_BASE) {
        return addr & 0x1fffffff;       /* kseg0/kseg1 are unmapped */
    }
    return cpu_get_phys_page_debug(cs, addr);
}

/*
 * Slow-path data access.  Only cacheable mappings reach this (uncached pages
 * and device memory keep QEMU's own path), and the access may cross a line, so
 * it is served a line at a time.
 */
static bool mt7621_dcache_access(CPUState *cs, vaddr addr, unsigned size,
                                 bool is_write, uint64_t *value)
{
    MT7621State *s = dcache_state;
    hwaddr pa;
    uint64_t w = value ? *value : 0;
    uint64_t r = 0;
    unsigned done = 0;

    if (!s || !s->dcache.enabled || size == 0 || size > 8) {
        return false;
    }
    /*
     * This hook also runs on an ordinary TLB miss, not only for pages the
     * target marked force-slow, so the mapping's cacheability has to be
     * checked here: an uncached access (KSEG1, a device mapping, an uncached
     * TLB page) must go to memory and never touch a line.
     */
    if (!mips_mapping_is_cacheable(cpu_env(cs), addr,
                                   is_write ? MMU_DATA_STORE : MMU_DATA_LOAD,
                                   0)) {
        return false;
    }
    pa = dcache_pa(cs, addr);
    if (pa == (hwaddr)-1) {
        return false;
    }
    /*
     * The DRAM controller only decodes the bits the populated chips need, so
     * the machine maps the same RAM again above the installed size (that is
     * how Breed measures the size: write through an alias, read it back).  An
     * alias is the same physical line, so index the cache with the wrapped
     * address - the cache is physically indexed, and the guest's mirror probe
     * only works if both addresses land on one line.
     */
    if (pa >= s->dcache.size) {
        if (pa >= s->dcache.alias_limit) {
            return false;
        }
        pa %= s->dcache.size;
    }
    if (pa + size > s->dcache.size) {
        return false;           /* straddles the top of DRAM: let QEMU store */
    }

    while (done < size) {
        unsigned off = (pa + done) & (s->dcache.line_size - 1);
        unsigned n = MIN(size - done, s->dcache.line_size - off);
        bool hit;
        MT7621DLine *line = dcache_lookup(s, pa + done, &hit);
        unsigned k;

        if (!hit) {
            line = dcache_fill(s, pa + done);
            dcache_misses++;
        } else {
            dcache_hits++;
            if (dcache_stale_on && !is_write) {
                unsigned j;
                bool differs = false;

                for (j = 0; j < n; j++) {
                    if (line->data[off + j] != dcache_ram(s, pa + done + j)[0]) {
                        differs = true;
                        break;
                    }
                }
                if (differs && dcache_stale_budget-- > 0) {
                    fprintf(stderr, "dstale: pa=%08x va=%08x pc=%08x "
                            "segctl1=%08x cache=%02x%02x%02x%02x "
                            "mem=%02x%02x%02x%02x\n",
                            (unsigned)(pa + done), (unsigned)addr,
                            (unsigned)cpu_env(cs)->active_tc.PC,
                            (uint32_t)cpu_env(cs)->CP0_SegCtl1,
                            line->data[off + 3], line->data[off + 2],
                            line->data[off + 1], line->data[off],
                            dcache_ram(s, pa + done + 3)[0],
                            dcache_ram(s, pa + done + 2)[0],
                            dcache_ram(s, pa + done + 1)[0],
                            dcache_ram(s, pa + done)[0]);
                }
            }
        }
        line->lru = ++s->dcache.clock;
        for (k = 0; k < n; k++) {
            if (is_write) {
                line->data[off + k] = (uint8_t)(w >> ((done + k) * 8));
            } else {
                r |= (uint64_t)line->data[off + k] << ((done + k) * 8);
            }
        }
        done += n;
    }

    if (is_write) {
        /*
         * Write-through: the line above is updated, and QEMU performs the
         * store to memory exactly as it would without a cache model.  Doing
         * that write here instead would skip QEMU's dirty tracking and its
         * translated-block bookkeeping for code pages - and invalidating a TB
         * from inside a store helper makes QEMU re-execute the instruction, so
         * the store would happen twice.
         */
        dcache_stores++;
        return false;
    }
    *value = r;
    return true;
}

/* Drop a line: memory is authoritative again. */
static void dcache_invalidate_line(MT7621State *s, hwaddr pa)
{
    bool hit;
    MT7621DLine *line = dcache_lookup(s, pa, &hit);

    if (hit) {
        line->valid = false;
        dcache_invals++;
    }
}

/* Invalidate every way of the set this address indexes. */
static void dcache_invalidate_set(MT7621State *s, hwaddr pa)
{
    MT7621DLine *line = &s->dcache.lines[((pa >> s->dcache.line_shift) %
                                          s->dcache.nsets) * MT7621_DCACHE_WAYS];
    int i;

    for (i = 0; i < MT7621_DCACHE_WAYS; i++) {
        if (line[i].valid) {
            line[i].valid = false;
            dcache_invals++;
        }
    }
}

static void dcache_flush_all(MT7621State *s)
{
    unsigned n = s->dcache.nsets * MT7621_DCACHE_WAYS;
    unsigned i;

    for (i = 0; i < n; i++) {
        s->dcache.lines[i].valid = false;
    }
}

/*
 * Apply a `cache` instruction to the data side.  Index operations select a set
 * by address; hit operations name the line, so the address is translated here.
 */
void mt7621_dcache_cache_op(CPUState *cs, vaddr addr, unsigned type,
                            unsigned op)
{
    MT7621State *s = dcache_state;
    hwaddr pa;

    if (!s || !s->dcache.enabled) {
        return;
    }
    if (type == 0) {
        /* primary instruction cache: the icache shadow owns that side */
        return;
    }
    if (op <= 3) {
        if (op == 1) {
            return;             /* index load tag: nothing reads tags back */
        }
        dcache_invalidate_set(s, addr);
        return;
    }
    if (op == 6) {
        return;                 /* hit writeback: nothing is ever dirty */
    }
    if (op != 4 && op != 5) {
        return;
    }
    pa = dcache_pa(cs, addr);
    if (pa != (hwaddr)-1) {
        dcache_invalidate_line(s, pa);
    }
}

/*
 * Geometry follows the Dl the machine presents (Breed derives the line size as
 * 1 << (Dl + 1): Dl=2 -> 8, Dl=4 -> 32, which is what the board reports), and a
 * reset leaves the cache empty.
 */
void mt7621_dcache_reset(MT7621State *s)
{
    CPUState *cs = qemu_get_cpu(0);
    unsigned dl, line;

    if (!s->dcache.enabled || !s->dcache.lines) {
        return;
    }
    if (!cs) {
        return;
    }
    dl = (cpu_env(cs)->CP0_Config1 >> 10) & 7;
    line = 1u << (dl + 1);
    if (line < 4 || line > MT7621_DCACHE_MAXLINE) {
        line = 32;
    }
    s->dcache.line_size = line;
    s->dcache.line_shift = ctz32(line);
    s->dcache.nsets = MT7621_DCACHE_BYTES / (line * MT7621_DCACHE_WAYS);
    dcache_flush_all(s);
}

static void dcache_report(void)
{
    fprintf(stderr, "dcache: hits=%" PRIu64 " misses=%" PRIu64 " fills=%"
            PRIu64 " stores=%" PRIu64 " invalidates=%" PRIu64 " lines=%u/%u\n",
            dcache_hits, dcache_misses, dcache_fills, dcache_stores,
            dcache_invals, dcache_state->dcache.nsets * MT7621_DCACHE_WAYS,
            dcache_state->dcache.nsets * MT7621_DCACHE_WAYS);
}

/* MT7621_STALETRACE=1: report the counters as the guest runs. */
static void dcache_tick(void *opaque)
{
    MT7621State *s = opaque;

    dcache_report();
    timer_mod(s->dcache.timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 15000);
}

void mt7621_dcache_init(MT7621State *s, uint64_t ram_size)
{
    if (!getenv("MT7621_DCACHE")) {
        return;
    }
    dcache_state = s;
    dcache_trace = getenv("MT7621_STALETRACE") != NULL;
    dcache_stale_on = getenv("MT7621_DSTALE") != NULL;
    s->dcache.ram = memory_region_get_ram_ptr(&s->ram);
    s->dcache.size = ram_size;
    if (!s->dcache.alias_limit) {
        s->dcache.alias_limit = ram_size;
    }
    /* enough lines for the smallest line size the encoding allows (4 B) */
    s->dcache.lines = g_new0(MT7621DLine, MT7621_DCACHE_BYTES / 4);
    s->dcache.enabled = true;
    /* Defaults until the CPU's Config1 is available; reset refines them. */
    s->dcache.line_size = 32;
    s->dcache.line_shift = 5;
    s->dcache.nsets = MT7621_DCACHE_BYTES / (32 * MT7621_DCACHE_WAYS);

    cpu_dcache_access_handler = mt7621_dcache_access;
    mips_dcache_op = mt7621_dcache_cache_op;
    if (dcache_trace) {
        s->dcache.timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, dcache_tick, s);
        timer_mod(s->dcache.timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 15000);
    }
    fprintf(stderr, "mt7621: L1 data cache model enabled (32 KiB, 4-way)\n");
}
