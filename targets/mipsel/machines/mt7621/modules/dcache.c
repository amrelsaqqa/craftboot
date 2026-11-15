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
 * What hardware needs to answer is *residency*: when an uncached store hits a
 * line, was that line in the cache?  The part is write-through, so a resident
 * line equals memory unless something wrote memory behind the cache's back, and
 * then the pre-store bytes are the only thing worth keeping.  That shapes the
 * whole model:
 *
 *   - residency is a tag array: 32 KiB, four ways, line size from Config1.Dl,
 *     LRU per set.  A line is *never* copied in from memory - a clean line is
 *     bit-identical to memory, so storing it would be work for nothing;
 *   - the only bytes kept are the ones an uncached store made stale: when such
 *     a store lands on a resident line, its pre-store bytes go into a small
 *     stale table and the page is marked so its loads come here;
 *   - stores are routed through the model on every cacheable page, because a
 *     store is what makes a line resident (write-allocate; measured - without
 *     it this firmware stops at "unable to allocate fifos");
 *   - loads are routed through it only for pages holding a stale line, and are
 *     served from that table.  Everywhere else a resident line equals memory,
 *     so QEMU's own load path is exactly what the hardware returns.
 *
 * `cache` instructions and `synci` drop a line (writeback is a no-op in a
 * write-through cache); index operations invalidate everything.  `ll`/`sc`
 * bypass the model - QEMU's atomics write memory directly - so `sc` is followed
 * by a line invalidate in gen_st_cond.
 *
 * Nothing ages and nothing is tuned to a guest: whether a read sees an old
 * value is decided by residency and invalidation, the two things the hardware
 * uses.
 */

#include "qemu/osdep.h"
#include "qemu/target-info.h"
#include <emmintrin.h>
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
static uint64_t dcache_hits, dcache_fills, dcache_stores, dcache_stale_made;
static uint64_t dcache_reads_stale, dcache_invals;

/* MT7621_DSTALE=1 reports reads served from a stale snapshot. */
static bool dcache_stale_on;
static int dcache_stale_budget = 200;

/* MT7621_DCNOOP=1 keeps the routing but lets the model do nothing. */
static bool dcache_noop;

/* Per physical page: how many of its lines have a stale snapshot. */
static uint16_t *dcache_page_stale;

/*
 * Stale snapshots, direct-mapped by line number.  Only lines an uncached store
 * found resident ever appear here, so a small table is enough; a collision
 * keeps the newer snapshot, which can only reduce what is served from stale
 * bytes - the safe direction.
 */
struct MT7621Stale {
    uint32_t line;              /* line number + 1, 0 = empty */
    uint8_t data[MT7621_DCACHE_MAXLINE];
};
static struct MT7621Stale *dcache_stale;
static unsigned dcache_stale_n;

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

static inline unsigned dcache_line(hwaddr pa)
{
    return (pa >> dcache_state->dcache.line_shift) &
           (dcache_state->dcache.nsets - 1);
}

static inline MT7621DSet *dcache_set(MT7621State *s, hwaddr pa)
{
    return &s->dcache.sets[dcache_line(pa)];
}

static inline struct MT7621Stale *dcache_stale_of(uint32_t line)
{
    return &dcache_stale[line & (dcache_stale_n - 1)];
}

/*
 * Which way of this set holds the line, or -1.  Four tags, one compare each;
 * on x86-64 they are compared as a single vector when SSE2 is available.
 */
static inline int dcache_way(MT7621DSet *set, uint32_t want)
{
#if defined(__SSE2__) && defined(__x86_64__)
    __m128i tags = _mm_loadu_si128((const __m128i *)set->tag);
    __m128i w = _mm_set1_epi32((int)want);
    unsigned mask = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(tags, w)));

    return mask ? ctz32(mask) : -1;
#else
    int i;

    for (i = 0; i < MT7621_DCACHE_WAYS; i++) {
        if (set->tag[i] == want) {
            return i;
        }
    }
    return -1;
#endif
}

/* Drop a way, and with it any stale snapshot and page accounting. */
static void dcache_drop_way(MT7621State *s, MT7621DSet *set, int way)
{
    uint32_t tag = set->tag[way];

    if (tag && (set->stale & (1u << way))) {
        uint32_t line = tag - 1;
        struct MT7621Stale *st = dcache_stale_of(line);
        unsigned page = (line << s->dcache.line_shift) >> 12;

        if (st->line == tag) {
            st->line = 0;
        }
        set->stale &= ~(1u << way);
        if (dcache_page_stale[page]) {
            dcache_page_stale[page]--;
        }
    }
    set->tag[way] = 0;
    set->lru[way] = 0;
}

/* Make this line resident, replacing the least recently used way. */
static int dcache_fill(MT7621State *s, hwaddr pa)
{
    MT7621DSet *set = dcache_set(s, pa);
    uint32_t want = (pa >> s->dcache.line_shift) + 1;
    int victim = 0, i;

    for (i = 0; i < MT7621_DCACHE_WAYS; i++) {
        if (!set->tag[i]) {
            victim = i;
            break;
        }
        if (set->lru[i] < set->lru[victim]) {
            victim = i;
        }
    }
    dcache_drop_way(s, set, victim);
    set->tag[victim] = want;
    dcache_fills++;
    return victim;
}

/*
 * An uncached store.  Where it lands on a resident line, keep the bytes as they
 * were: that copy is what a later cached read must see.  Where it lands on a
 * line the cache does not hold, memory is the truth and nothing is kept.
 */
static void dcache_uncached_store(CPUState *cs, MT7621State *s, hwaddr pa,
                                  unsigned size)
{
    unsigned done = 0;

    while (done < size) {
        hwaddr p = pa + done;
        unsigned off = p & (s->dcache.line_size - 1);
        unsigned n = MIN(size - done, s->dcache.line_size - off);
        MT7621DSet *set = dcache_set(s, p);
        uint32_t want = (p >> s->dcache.line_shift) + 1;
        int way = dcache_way(set, want);

        if (way >= 0 && !(set->stale & (1u << way))) {
            uint32_t line = want - 1;
            struct MT7621Stale *st = dcache_stale_of(line);
            hwaddr base = p & ~(hwaddr)(s->dcache.line_size - 1);
            unsigned page = base >> 12;

            memcpy(st->data, dcache_ram(s, base), s->dcache.line_size);
            st->line = want;
            set->stale |= 1u << way;
            dcache_stale_made++;
            if (dcache_page_stale[page]++ == 0) {
                /* this page's loads must now come through the model */
                tlb_flush(cs);
            }
        }
        done += n;
    }
}

/*
 * Slow-path data access.  The caller has translated the access and only sends
 * what the model asked to see: every store on a cacheable page, and loads on
 * pages holding a stale snapshot.
 */
static bool mt7621_dcache_access(CPUState *cs, vaddr addr, hwaddr pa,
                                 unsigned size, bool is_write, bool cached,
                                 uint64_t *value)
{
    MT7621State *s = dcache_state;
    MT7621DSet *set;
    uint32_t want;
    int way;
    unsigned off;

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
    set = dcache_set(s, pa);
    want = (pa >> s->dcache.line_shift) + 1;
    way = dcache_way(set, want);
    if (way < 0) {
        if (!is_write) {
            return false;       /* not resident, nothing stale: memory is right */
        }
        way = dcache_fill(s, pa);       /* write-allocate */
    } else {
        dcache_hits++;
    }
    set->lru[way] = ++s->dcache.clock;

    if (is_write) {
        /*
         * Write-through: the cached copy takes the store too, so a later read
         * of these bytes sees the new value while the rest of the line keeps
         * the old one.  QEMU still performs the store; doing it here would skip
         * its dirty/TB bookkeeping, and invalidating a TB from a store helper
         * makes QEMU re-execute the instruction.
         */
        if (set->stale & (1u << way)) {
            uint64_t v = value ? *value : 0;
            struct MT7621Stale *st = dcache_stale_of(want - 1);

            if (st->line == want) {
                if (target_big_endian()) {
                    unsigned k;

                    for (k = 0; k < size; k++) {
                        st->data[off + k] = (uint8_t)(v >> (8 * k));
                    }
                } else {
                    memcpy(st->data + off, &v, size);
                }
            }
        }
        dcache_stores++;
        return false;
    }

    if (!(set->stale & (1u << way))) {
        return false;           /* identical to memory: QEMU's path is right */
    }
    {
        struct MT7621Stale *st = dcache_stale_of(want - 1);
        uint64_t v;

        if (st->line != want) {
            return false;
        }
        if (dcache_stale_on && dcache_stale_budget > 0 &&
            memcmp(st->data + off, dcache_ram(s, pa), size) != 0) {
            dcache_stale_budget--;
            fprintf(stderr, "dstale: pa=%08x va=%08x pc=%08x "
                    "cache=%02x%02x%02x%02x mem=%02x%02x%02x%02x\n",
                    (unsigned)pa, (unsigned)addr,
                    (unsigned)cpu_env(cs)->active_tc.PC,
                    st->data[off + 3], st->data[off + 2],
                    st->data[off + 1], st->data[off],
                    dcache_ram(s, pa + 3)[0], dcache_ram(s, pa + 2)[0],
                    dcache_ram(s, pa + 1)[0], dcache_ram(s, pa)[0]);
        }
        if (target_big_endian()) {
            unsigned k;

            v = 0;
            for (k = 0; k < size; k++) {
                v |= (uint64_t)st->data[off + k] << (8 * k);
            }
        } else {
            memcpy(&v, st->data + off, size);
        }
        *value = v;
        dcache_reads_stale++;
        return true;
    }
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
    return dcache_page_stale[pa >> 12] != 0;
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

void mt7621_dcache_cache_op(CPUState *cs, vaddr addr, unsigned type,
                            unsigned op)
{
    MT7621State *s = dcache_state;
    hwaddr pa;

    if (!s || !s->dcache.enabled || type == 0) {
        return;                 /* the icache shadow owns the I side */
    }
    if (op <= 3) {
        unsigned i, n = s->dcache.nsets;

        if (op == 1) {
            return;             /* index load tag: nothing reads tags back */
        }
        /* whole-cache operation: nothing is resident or stale any more */
        for (i = 0; i < n; i++) {
            MT7621DSet *set = &s->dcache.sets[i];
            int w;

            for (w = 0; w < MT7621_DCACHE_WAYS; w++) {
                set->tag[w] = 0;
            }
            set->stale = 0;
        }
        if (dcache_stale_n) {
            memset(dcache_stale, 0,
                   dcache_stale_n * sizeof(*dcache_stale));
            memset(dcache_page_stale, 0, (s->dcache.size >> 12) *
                   sizeof(*dcache_page_stale));
        }
        dcache_invals++;
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
        MT7621DSet *set = dcache_set(s, pa);
        int way = dcache_way(set, (pa >> s->dcache.line_shift) + 1);

        if (way >= 0) {
            dcache_drop_way(s, set, way);
            dcache_invals++;
        }
    }
}

/*
 * Geometry follows the Dl the machine presents and a reset leaves the cache
 * empty.
 */
void mt7621_dcache_reset(MT7621State *s)
{
    CPUState *cs = qemu_get_cpu(0);
    unsigned dl, line, sets, i;

    if (!s->dcache.enabled || !cs) {
        return;
    }
    dl = (cpu_env(cs)->CP0_Config1 >> 10) & 7;
    line = 1u << (dl + 1);
    if (line < 4 || line > MT7621_DCACHE_MAXLINE) {
        line = 32;
    }
    s->dcache.line_size = line;
    s->dcache.line_shift = ctz32(line);
    sets = MT7621_DCACHE_BYTES / (line * MT7621_DCACHE_WAYS);
    s->dcache.nsets = sets;

    for (i = 0; i < sets; i++) {
        memset(s->dcache.sets[i].tag, 0, sizeof(s->dcache.sets[i].tag));
        s->dcache.sets[i].stale = 0;
    }
    if (dcache_stale_n) {
        memset(dcache_stale, 0, dcache_stale_n * sizeof(*dcache_stale));
        memset(dcache_page_stale, 0, (s->dcache.size >> 12) *
               sizeof(*dcache_page_stale));
    }
}

static void dcache_report(void)
{
    fprintf(stderr, "dcache: hits=%" PRIu64 " fills=%" PRIu64 " stores=%"
            PRIu64 " stale_made=%" PRIu64 " stale_reads=%" PRIu64
            " invalidates=%" PRIu64 "\n", dcache_hits, dcache_fills,
            dcache_stores, dcache_stale_made, dcache_reads_stale,
            dcache_invals);
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
    unsigned sets;

    if (!getenv("MT7621_DCACHE")) {
        return;
    }
    dcache_state = s;
    dcache_trace = getenv("MT7621_STALETRACE") != NULL;
    dcache_stale_on = getenv("MT7621_DSTALE") != NULL;
    dcache_noop = getenv("MT7621_DCNOOP") != NULL;

    s->dcache.ram = memory_region_get_ram_ptr(&s->ram);
    s->dcache.size = ram_size;
    if (!s->dcache.alias_limit) {
        s->dcache.alias_limit = ram_size;
    }

    /* enough sets for the smallest line size the encoding allows (4 B) */
    sets = MT7621_DCACHE_BYTES / (4 * MT7621_DCACHE_WAYS);
    s->dcache.sets = g_new0(MT7621DSet, sets);
    s->dcache.nsets = sets;
    s->dcache.line_size = 32;
    s->dcache.line_shift = 5;

    dcache_page_stale = g_new0(uint16_t, ram_size >> 12);
    dcache_stale_n = sets;
    dcache_stale = g_new0(struct MT7621Stale, dcache_stale_n);

    s->dcache.enabled = true;
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
