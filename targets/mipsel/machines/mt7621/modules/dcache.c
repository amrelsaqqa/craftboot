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
 * The model
 * ---------
 * A real 32 KiB 4-way L1D: tags, per-set LRU, line size from Config1.Dl, and a
 * resident copy of every line it holds.  It is write-through, so a resident
 * line equals memory *until something writes memory behind the cache's back* -
 * an uncached store or a device DMA.  That is the only situation in which a
 * cached read can differ from a read of memory, and it is therefore the only
 * thing the machine has to pay for:
 *
 *   - stores are seen on every cacheable page, because a store is what makes a
 *     line resident (write-allocate; measured: without it this firmware fails
 *     at "unable to allocate fifos").  A store updates the line and QEMU still
 *     performs it, so memory stays current;
 *   - an uncached store is seen too.  If it lands on a line the model holds,
 *     the line's pre-store bytes are kept and the line is marked stale; if the
 *     model holds nothing for that line, nothing is kept - the hardware would
 *     have read memory as well;
 *   - a load is only routed to the model for pages that hold a stale line.
 *     Everywhere else the line content equals memory, so QEMU's own load path
 *     is exactly what the hardware would return and stays at full speed;
 *   - `cache` instructions and `synci` drop lines (writeback is a no-op in a
 *     write-through cache), and index operations invalidate the set;
 *   - `ll`/`sc` bypass the model (QEMU's atomics write memory directly), so
 *     `sc` is followed by a line invalidate in gen_st_cond.
 *
 * Residency is tracked for lines the CPU *stored*, which is the case the
 * firmware depends on and the only case a stale copy can matter for here: a
 * line that was resident only because it was read is treated as absent, so the
 * model can under-report staleness (the guest then sees memory, which is
 * newer) but can never invent an old value for a line the cache did not hold.
 *
 * No ageing, no staleness window, nothing tuned to a guest.
 */

#include "qemu/osdep.h"
#include "qemu/target-info.h"
#include "qemu/units.h"
#include "hw/core/cpu.h"
#include "exec/cpu-dcache.h"
#include "exec/mips-dcache.h"
#include "exec/cputlb.h"
#include "system/memory.h"
#include "mt7621.h"

/* 1004Kc L1 D-cache: 32 KiB, 4-way. */
#define MT7621_DCACHE_BYTES   (32 * KiB)
#define MT7621_DCACHE_WAYS    4
#define MT7621_DCACHE_MAXLINE 64

#define KSEG0_BASE 0x80000000u
#define KSEG2_BASE 0xc0000000u

static MT7621State *dcache_state;

/* Counters, reported with MT7621_STALETRACE=1 (diagnostics only). */
static bool dcache_trace;
static uint64_t dcache_hits, dcache_misses, dcache_fills, dcache_invals;
static uint64_t dcache_stores, dcache_stale_made, dcache_reads_stale;

/*
 * MT7621_DSTALE=1 reports reads served from a stale line whose bytes no longer
 * match memory: exactly the reads the guest would not make without a cache.
 */
static bool dcache_stale_on;
static int dcache_stale_budget = 200;

/*
 * MT7621_DCNOOP=1: keep the routing but let the model do nothing, to measure
 * what the forced slow path costs on its own.
 */
static bool dcache_noop;

/* Diagnostic: no store tracking, and residency assumed (see tlb_fill). */
static bool dcache_no_store;

/* Per physical page: how many of its lines are stale. */
static uint16_t *dcache_stale_lines;

static inline uint8_t *dcache_ram(MT7621State *s, hwaddr pa)
{
    return (uint8_t *)s->dcache.ram + pa;
}

/* Wrap an aliased DRAM address onto the RAM it mirrors. */
static inline bool dcache_wrap(MT7621State *s, hwaddr *pa)
{
    if (*pa >= s->dcache.size) {
        if (*pa >= s->dcache.alias_limit) {
            return false;
        }
        *pa %= s->dcache.size;
    }
    return true;
}

static inline unsigned dcache_page(hwaddr pa)
{
    return pa >> 12;
}

static inline unsigned dcache_line(hwaddr pa)
{
    return (pa >> dcache_state->dcache.line_shift) &
           (dcache_state->dcache.nsets - 1);
}

static inline MT7621DLine *dcache_set(MT7621State *s, hwaddr pa)
{
    return &s->dcache.lines[dcache_line(pa) * MT7621_DCACHE_WAYS];
}

/* The resident line for this physical address, or the set's first way. */
static MT7621DLine *dcache_lookup(MT7621State *s, hwaddr pa, bool *hit)
{
    unsigned shift = s->dcache.line_shift;
    MT7621DLine *line = dcache_set(s, pa);
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

/* Drop a line; if it was stale, its page may stop needing the slow path. */
static void dcache_drop(MT7621State *s, MT7621DLine *line)
{
    if (line->valid && line->stale) {
        unsigned page = dcache_page(line->tag << s->dcache.line_shift);

        if (dcache_stale_lines[page]) {
            dcache_stale_lines[page]--;
        }
    }
    line->valid = false;
    line->stale = false;
    line->lru = 0;
}

/* Refill a line from memory, replacing the least recently used way. */
static MT7621DLine *dcache_fill(MT7621State *s, hwaddr pa)
{
    MT7621DLine *line = dcache_set(s, pa);
    MT7621DLine *victim = &line[0];
    hwaddr base = pa & ~(hwaddr)(s->dcache.line_size - 1);
    unsigned n = s->dcache.line_size;
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
    dcache_drop(s, victim);
    if (base + n > s->dcache.size) {
        n = s->dcache.size - base;
    }
    memcpy(victim->data, dcache_ram(s, base), n);
    if (n < s->dcache.line_size) {
        memset(victim->data + n, 0, s->dcache.line_size - n);
    }
    victim->tag = pa >> s->dcache.line_shift;
    victim->valid = true;
    dcache_fills++;
    return victim;
}

/*
 * An uncached store: if it lands on a line the cache holds, keep the bytes as
 * they were.  That copy is what a later cached read must see.  If the cache
 * holds nothing for the line, this store is the first thing to touch it as far
 * as the CPU is concerned, so memory is the truth and nothing is kept.
 */
static void dcache_uncached_store(CPUState *cs, MT7621State *s, hwaddr pa,
                                  unsigned size)
{
    unsigned done = 0;

    while (done < size) {
        hwaddr p = pa + done;
        unsigned off = p & (s->dcache.line_size - 1);
        unsigned n = MIN(size - done, s->dcache.line_size - off);
        bool hit;
        MT7621DLine *line = dcache_lookup(s, p, &hit);

        if (!hit && dcache_no_store) {
            hit = true;         /* diagnostic: assume the line is resident */
            line = dcache_fill(s, p);
        }
        if (hit && !line->stale) {
            /*
             * The line is resident and its copy still matches memory: take
             * the bytes before the store overwrites them and mark it stale.
             * Loads on this page must now come through the model, so the TLB
             * entries for it have to go.
             */
            hwaddr base = p & ~(hwaddr)(s->dcache.line_size - 1);
            unsigned page = dcache_page(base);

            memcpy(line->data, dcache_ram(s, base), s->dcache.line_size);
            line->stale = true;
            dcache_stale_made++;
            if (dcache_stale_lines[page]++ == 0) {
                /* the page's loads must now come through the model */
                tlb_flush(cs);
            }
        }
        done += n;
    }
}

/*
 * Slow-path data access.  The caller has translated the access and only sends
 * what the model asked to see, so this is a tag compare and a copy.
 */
static bool mt7621_dcache_access(CPUState *cs, vaddr addr, hwaddr pa,
                                 unsigned size, bool is_write, bool cached,
                                 uint64_t *value)
{
    MT7621State *s = dcache_state;
    unsigned off;
    bool hit;
    MT7621DLine *line;
    uint64_t v;

    if (!s || !s->dcache.enabled || dcache_noop || size == 0 || size > 8) {
        return false;
    }
    if (!dcache_wrap(s, &pa)) {
        return false;
    }
    if (!cached) {
        if (is_write) {
            dcache_uncached_store(cs, s, pa, size);
        }
        return false;           /* QEMU performs the store / the load */
    }

    off = pa & (s->dcache.line_size - 1);
    if (off + size > s->dcache.line_size) {
        return false;           /* would straddle a line: QEMU's business */
    }

    line = dcache_lookup(s, pa, &hit);
    if (!hit) {
        if (!is_write) {
            /*
             * A load of a line that is not resident: memory is the truth, and
             * QEMU's own path returns it.  Only stale lines need the model.
             */
            return false;
        }
        line = dcache_fill(s, pa);      /* write-allocate */
        dcache_misses++;
    } else {
        dcache_hits++;
    }
    line->lru = ++s->dcache.clock;

    if (is_write) {
        /*
         * A clean line's copy is never read: the model serves memory for it,
         * and a stale snapshot re-reads memory at the moment it is taken.  So
         * the bytes only have to be written into a line that is *already*
         * stale, where the cache copy is what a later load must see.
         */
        if (line->stale) {
            v = value ? *value : 0;
            if (target_big_endian()) {
                unsigned k;

                for (k = 0; k < size; k++) {
                    line->data[off + k] = (uint8_t)(v >> (8 * k));
                }
            } else {
                memcpy(line->data + off, &v, size);
            }
        }
        /*
         * Write-through: the line is updated and QEMU performs the store, so
         * memory stays current.  Writing memory here instead would skip
         * QEMU's dirty and translated-block bookkeeping, and invalidating a TB
         * from inside a store helper makes QEMU re-execute the instruction.
         */
        dcache_stores++;
        return false;
    }

    if (!line->stale) {
        return false;           /* identical to memory: QEMU's path is right */
    }
    if (dcache_stale_on) {
        if (memcmp(line->data + off, dcache_ram(s, pa), size) != 0 &&
            dcache_stale_budget > 0) {
            dcache_stale_budget--;
            fprintf(stderr, "dstale: pa=%08x va=%08x pc=%08x "
                    "cache=%02x%02x%02x%02x mem=%02x%02x%02x%02x\n",
                    (unsigned)pa, (unsigned)addr,
                    (unsigned)cpu_env(cs)->active_tc.PC,
                    line->data[off + 3], line->data[off + 2],
                    line->data[off + 1], line->data[off],
                    dcache_ram(s, pa + 3)[0], dcache_ram(s, pa + 2)[0],
                    dcache_ram(s, pa + 1)[0], dcache_ram(s, pa)[0]);
        }
    }
    if (target_big_endian()) {
        unsigned k;

        v = 0;
        for (k = 0; k < size; k++) {
            v |= (uint64_t)line->data[off + k] << (8 * k);
        }
    } else {
        memcpy(&v, line->data + off, size);
    }
    *value = v;
    dcache_reads_stale++;
    return true;
}

/* Does this page hold a line a load must be served from the model? */
static bool mt7621_dcache_page_slow(CPUState *cs, hwaddr pa)
{
    MT7621State *s = dcache_state;

    if (!s || !s->dcache.enabled || dcache_noop) {
        return false;
    }
    if (!dcache_wrap(s, &pa)) {
        return false;
    }
    return dcache_stale_lines[dcache_page(pa)] != 0;
}

/*
 * Translate a cache-op address.  Only `cache`/`synci` come through here, so a
 * lookup is fine; data accesses get their physical address from the slow path.
 */
static hwaddr dcache_pa(CPUState *cs, vaddr addr)
{
    if (addr >= KSEG0_BASE && addr < KSEG2_BASE) {
        return addr & 0x1fffffff;       /* kseg0/kseg1 are unmapped */
    }
    return cpu_get_phys_page_debug(cs, addr);
}

/*
 * Cache operations.  A write-through cache never has dirty data to write back,
 * so writeback is a no-op; invalidate means "drop the line, memory is
 * authoritative again".
 */
void mt7621_dcache_cache_op(CPUState *cs, vaddr addr, unsigned type,
                            unsigned op)
{
    MT7621State *s = dcache_state;
    hwaddr pa;

    if (!s || !s->dcache.enabled || type == 0) {
        return;                 /* the icache shadow owns the I side */
    }
    if (op <= 3) {
        unsigned i, n = s->dcache.nsets * MT7621_DCACHE_WAYS;

        if (op == 1) {
            return;             /* index load tag: nothing reads tags back */
        }
        for (i = 0; i < n; i++) {
            dcache_drop(s, &s->dcache.lines[i]);
        }
        return;
    }
    if (op == 6) {
        return;                 /* hit writeback: nothing is ever dirty */
    }
    if (op != 4 && op != 5) {
        return;
    }
    pa = dcache_pa(cs, addr);
    if (pa == (hwaddr)-1 || !dcache_wrap(s, &pa)) {
        return;
    }
    {
        bool hit;
        MT7621DLine *line = dcache_lookup(s, pa, &hit);

        if (hit) {
            dcache_drop(s, line);
            dcache_invals++;
        }
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
    unsigned dl, line, i, n;

    if (!s->dcache.enabled || !s->dcache.lines || !cs) {
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
    n = s->dcache.nsets * MT7621_DCACHE_WAYS;
    for (i = 0; i < n; i++) {
        s->dcache.lines[i].valid = false;
        s->dcache.lines[i].stale = false;
    }
    memset(dcache_stale_lines, 0, s->dcache.size / 4096 *
           sizeof(*dcache_stale_lines));
}

static void dcache_report(void)
{
    fprintf(stderr, "dcache: hits=%" PRIu64 " misses=%" PRIu64 " fills=%"
            PRIu64 " stores=%" PRIu64 " stale_made=%" PRIu64
            " stale_reads=%" PRIu64 " invalidates=%" PRIu64 "\n",
            dcache_hits, dcache_misses, dcache_fills, dcache_stores,
            dcache_stale_made, dcache_reads_stale, dcache_invals);
}

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
    dcache_noop = getenv("MT7621_DCNOOP") != NULL;
    dcache_no_store = getenv("MT7621_DCACHENOSTORE") != NULL;

    s->dcache.ram = memory_region_get_ram_ptr(&s->ram);
    s->dcache.size = ram_size;
    if (!s->dcache.alias_limit) {
        s->dcache.alias_limit = ram_size;
    }
    /* enough lines for the smallest line size the encoding allows (4 B) */
    s->dcache.lines = g_new0(MT7621DLine, MT7621_DCACHE_BYTES / 4);
    dcache_stale_lines = g_new0(uint16_t, ram_size / 4096);
    s->dcache.enabled = true;
    /* Defaults until the CPU's Config1 is available; reset refines them. */
    s->dcache.line_size = 32;
    s->dcache.line_shift = 5;
    s->dcache.nsets = MT7621_DCACHE_BYTES / (32 * MT7621_DCACHE_WAYS);

    cpu_dcache_access_handler = mt7621_dcache_access;
    cpu_dcache_page_slow = mt7621_dcache_page_slow;
    mips_dcache_op = mt7621_dcache_cache_op;
    if (dcache_trace) {
        s->dcache.timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, dcache_tick, s);
        timer_mod(s->dcache.timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 15000);
    }
    fprintf(stderr, "mt7621: L1 data cache model enabled (32 KiB, 4-way)\n");
}
