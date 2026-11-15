/*
 * MT7621A (MIPS 1004Kc) data cache model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * QEMU has no cache model: KSEG0 (cached) and KSEG1 (uncached) both read and
 * write one copy of DRAM.  On silicon they do not.  A store through KSEG1 (the
 * DMA aliases, 0xA0000000|p) goes straight to memory and does not disturb the
 * copy of that line the cache holds from earlier KSEG0 traffic.
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
 * How it is modelled
 * ------------------
 * The 1004Kc's L1D is write-through, which is why the firmware can write code
 * through a cached mapping and execute it without a cache operation: memory
 * always holds the latest stores.  So there is exactly one DRAM here, and the
 * cache is a shadow of what an uncached write has made stale:
 *
 *   - uncached stores are intercepted (they are rare: DMA buffers); the page's
 *     current contents are saved in a shadow before the store reaches memory,
 *     and the page is marked shadowed;
 *   - cached accesses to a shadowed page are served from the shadow, so they
 *     keep seeing the pre-store value - the line the DMA clear left behind on
 *     hardware; cached stores are written through to both;
 *   - every other cached access goes through QEMU's normal fast path into
 *     memory, which is always up to date because the part is write-through;
 *   - hit invalidate / writeback-invalidate drop the shadowed copy (memory is
 *     authoritative again); index operations drop every shadow.  Writeback is
 *     a no-op: a write-through cache never holds data memory does not have.
 *
 * The slow path is paid only on pages an uncached write touched - a couple in
 * a boot, not every access.
 */

#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "exec/cpu-dcache.h"
#include "exec/mips-dcache.h"
#include "exec/cputlb.h"
#include "system/memory.h"
#include "mt7621.h"

#include "system/physmem.h"
#include "exec/translation-block.h"
#include "exec/guest-cycles.h"

#define MT7621_PAGE 4096
#define MT7621_DCACHE_LINE 32
#define MT7621_LINES_PER_PAGE (MT7621_PAGE / MT7621_DCACHE_LINE)

/*
 * How long a line stays stale.  A 32 KiB 4-way L1D turns over constantly, so a
 * stale line survives only until the cache needs its way again - a handful of
 * lines in practice.  Keeping a small window is what makes this faithful: the
 * firmware's DMA clear only needs the header line to stay stale across the few
 * instructions until the next malloc, while anything older reverts to memory.
 */
#define MT7621_STALE_WINDOW 64

/*
 * A stale line also ages out with guest progress: the L1D is turned over by
 * everything the CPU does, not just by more uncached stores.  Without this a
 * single stale line can outlive the object that owned it (which is how a slab
 * page ends up with a pointer from an earlier life).
 */
/*
 * How long a line stays stale, in retired instructions.  It has to cover the
 * firmware's DMA clear (a ~0x43f-byte store loop plus the driver path that
 * follows it) - measured: below ~100k Breed's own allocation fails and the eth
 * probe dereferences a NULL buffer - while still expiring the way a real L1D
 * does, so kernel and userspace never read a line from an earlier life.
 */
#define MT7621_STALE_MAX_AGE 200000

/* Per page: the stale copy of each line an uncached write touched. */
struct MT7621ShadowPage {
    uint8_t data[MT7621_PAGE];
    uint8_t line[MT7621_LINES_PER_PAGE];
    uint64_t gen[MT7621_LINES_PER_PAGE];
};

static uint64_t dcache_progress;
/* pages whose cached mappings must be re-translated, flushed outside helpers */
static hwaddr dcache_pending_flush[64];
static unsigned dcache_pending_flush_n;
static uint64_t dcache_stale_age = MT7621_STALE_MAX_AGE;

static void mt7621_dcache_progress(CPUState *cs, unsigned insns)
{
    dcache_progress += insns;
    if (dcache_pending_flush_n) {
        dcache_pending_flush_n = 0;
        tlb_flush(cs);
    }
}

/* Is this line still stale, or has the cache turned over since? */
static inline bool dcache_line_stale(struct MT7621ShadowPage *sp, unsigned line)
{
    if (!sp->line[line]) {
        return false;
    }
    if (dcache_progress - sp->gen[line] > dcache_stale_age) {
        sp->line[line] = 0;
        return false;
    }
    return true;
}

/* implemented in target/mips; declared here so the machine can ask */
bool mips_mapping_is_cacheable(CPUMIPSState *env, target_ulong address,
                               MMUAccessType access_type, int mmu_idx);

static MT7621State *dcache_state;
static unsigned dcache_pages;

typedef struct MT7621StaleFifo {
    uint32_t key[MT7621_STALE_WINDOW];  /* physical line address >> 5 */
    unsigned head;
    unsigned count;
    unsigned limit;                     /* MT7621_STALE_WINDOW override */
} MT7621StaleFifo;

static inline unsigned dcache_line(hwaddr pa)
{
    return (pa % MT7621_PAGE) / MT7621_DCACHE_LINE;
}

static MT7621StaleFifo stale_fifo;

/* Evict the oldest stale line: it is out of the cache, memory is the truth. */
static void dcache_stale_push(MT7621State *s, hwaddr pa)
{
    uint32_t key = (uint32_t)(pa >> 5);

    if (!stale_fifo.limit) {
        return;                 /* staleness disabled: pure write-through */
    }
    stale_fifo.key[stale_fifo.head] = key;
    stale_fifo.head = (stale_fifo.head + 1) % MT7621_STALE_WINDOW;
    if (stale_fifo.count < stale_fifo.limit) {
        stale_fifo.count++;
        return;
    }
    {
        uint32_t old = stale_fifo.key[stale_fifo.head];
        hwaddr p = (hwaddr)old << 5;
        struct MT7621ShadowPage *sp =
            (struct MT7621ShadowPage *)s->dcache.shadow[p / MT7621_PAGE];

        if (sp) {
            sp->line[dcache_line(p)] = 0;
        }
    }
}

static inline uint8_t *dcache_ram(MT7621State *s, hwaddr pa)
{
    return (uint8_t *)s->dcache.ram + pa;
}


/*
 * Has the CPU written this page?  QEMU marks a RAM page dirty on the first CPU
 * store after it was clean, which is exactly the hint needed here: a line can
 * only be in the cache if the CPU touched the page, and only such lines may
 * become stale when a store bypasses the cache.
 */
static inline bool dcache_page_cpu_written(hwaddr pa)
{
    return physical_memory_get_dirty_flag(pa, DIRTY_MEMORY_MIGRATION);
}

static bool dcache_pa(CPUState *cs, vaddr addr, hwaddr *pa)
{
    if (addr >= 0x80000000u && addr < 0xc0000000u) {
        *pa = addr & 0x1fffffff;
        return true;
    }
    *pa = cpu_get_phys_page_debug(cs, addr);
    return *pa != (hwaddr)-1;
}

bool mt7621_dcache_page_slow(CPUState *cs, hwaddr pa)
{
    MT7621State *s = dcache_state;

    if (getenv("MT7621_DCACHE_NOOP")) {
        return false;
    }
    return s && s->dcache.enabled && pa < s->dcache.size &&
           (uint8_t *)s->dcache.shadow[pa / MT7621_PAGE] != NULL;
}

/*
 * An uncached store: for every line it touches on a page the CPU has written,
 * keep what the cache holds (the line as it was) and mark it stale.  Lines the
 * CPU never touched are left alone - memory is the cache's content for them.
 */
static void dcache_uncached_store(CPUState *cs, hwaddr pa, unsigned size,
                                  uint64_t value)
{
    MT7621State *s = dcache_state;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr p = pa + i;
        unsigned page = p / MT7621_PAGE;
        unsigned line = dcache_line(p);
        struct MT7621ShadowPage *sp;

        if (page >= dcache_pages) {
            continue;
        }
        sp = (struct MT7621ShadowPage *)s->dcache.shadow[page];
        if (!sp && (getenv("MT7621_DCACHE_ALWAYS_SHADOW") ||
                    dcache_page_cpu_written(page * MT7621_PAGE))) {
            sp = g_new0(struct MT7621ShadowPage, 1);
            s->dcache.shadow[page] = (uint8_t *)sp;
            /* cached mappings of this page must now come through the model */
            tlb_flush(cs);
        }
        if (sp && !sp->line[line]) {
            memcpy(sp->data + line * MT7621_DCACHE_LINE,
                   dcache_ram(s, page * MT7621_PAGE + line * MT7621_DCACHE_LINE),
                   MT7621_DCACHE_LINE);
            sp->line[line] = 1;
            sp->gen[line] = dcache_progress;
            if (getenv("MT7621_DCACHEDBG")) {
                static int budget = 400;
                if (budget-- > 0) {
                    fprintf(stderr, "dcache: stale line pa=%08x\n",
                            (unsigned)p);
                }
            }
            dcache_stale_push(s, p);
        }
        dcache_ram(s, p)[0] = (uint8_t)(value >> (i * 8));
    }
}

/*
 * The slow path.  Uncached stores create shadows; cached accesses to a
 * shadowed page are served from the shadow (and still written through to
 * memory, because the part is write-through).
 */
bool mt7621_dcache_access(CPUState *cs, vaddr addr, unsigned size,
                          bool is_write, uint64_t *value)
{
    MT7621State *s = dcache_state;
    CPUMIPSState *env;
    hwaddr pa;
    struct MT7621ShadowPage *sp;
    uint64_t v = value ? *value : 0;
    uint64_t r = 0;
    unsigned i;
    bool cached;

    if (!s || !s->dcache.enabled || getenv("MT7621_DCACHE_NOOP")) {
        return false;
    }
    if (getenv("MT7621_DCACHEHEAP")) {
        static int armed, budget;
        if (!armed && addr >= 0x8fddd180 && addr < 0x8fddd200) {
            armed = 1;
            budget = 25;
            fprintf(stderr, "dcache heap: == clear starts\n");
        }
        if (armed && budget > 0) {
            budget--;
            fprintf(stderr, "dcache heap: seq %c addr=%08x size=%u\n",
                    is_write ? 'w' : 'r', (unsigned)addr, size);
        }
    }
    if (getenv("MT7621_DCACHECALL")) {
        static int budget = 12;
        if (budget-- > 0) {
            fprintf(stderr, "dcache call: %c addr=%08x size=%u\n",
                    is_write ? 'w' : 'r', (unsigned)addr, size);
        }
    }
    env = cpu_env(cs);
    if (!dcache_pa(cs, addr, &pa) || pa + size > s->dcache.size) {
        return false;
    }
    cached = mips_mapping_is_cacheable(env, addr,
                                       is_write ? MMU_DATA_STORE
                                                : MMU_DATA_LOAD, 0);

    if (!cached) {
        if (!is_write) {
            return false;       /* uncached read: nothing to model */
        }
        dcache_uncached_store(cs, pa, size, v);
        /*
         * The store was written here, not by QEMU, so tell QEMU about it:
         * mark the page dirty and drop any translated block covering it.  A
         * guest that writes code or data through the uncached alias (or a
         * device that shares the page) would otherwise execute a stale
         * translation.
         */
        physical_memory_set_dirty_range(pa, size, DIRTY_CLIENTS_NOCODE);
        tb_invalidate_phys_range(cs, pa, pa + size);
        /*
         * Uncached stores target DMA buffers, never code, so performing them
         * here is safe: nothing needs a translated-block invalidation for
         * them.  Cached stores (below) do have to go back through QEMU, since
         * that is how the guest's relocated code becomes executable.
         */
        return true;
    }

    sp = (struct MT7621ShadowPage *)s->dcache.shadow[pa / MT7621_PAGE];
    if (!sp) {
        return false;           /* not shadowed: fast path is right */
    }
    if (is_write) {
        for (i = 0; i < size; i++) {
            hwaddr p = pa + i;
            unsigned line = dcache_line(p);

            if (dcache_line_stale(sp, line)) {
                sp->data[p % MT7621_PAGE] = (uint8_t)(v >> (i * 8));
            }
        }
        return false;           /* memory is written by the normal path */
    }
    for (i = 0; i < size; i++) {
        hwaddr p = pa + i;

        if (dcache_line_stale(sp, dcache_line(p))) {
            if (getenv("MT7621_DCACHEDBG")) {
                static int budget = 400;
                if (budget-- > 0) {
                    fprintf(stderr, "dcache: stale read pa=%08x\n",
                            (unsigned)p);
                }
            }
            r |= (uint64_t)sp->data[p % MT7621_PAGE] << (i * 8);
        } else {
            r |= (uint64_t)dcache_ram(s, p)[0] << (i * 8);
        }
    }
    *value = r;
    return true;
}

/*
 * Cache operations.  A write-through cache never has dirty data to write
 * back, so writeback is a no-op; invalidate means "drop the shadowed copy,
 * memory is authoritative again".
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
        unsigned page;

        if (op == 1) {
            return;             /* index load tag */
        }
        /* whole-cache operation: memory is authoritative again */
        for (page = 0; page < dcache_pages; page++) {
            g_free(s->dcache.shadow[page]);
            s->dcache.shadow[page] = NULL;
        }
        return;
    }
    if (op == 6) {              /* hit writeback: memory is already current */
        return;
    }
    if ((op == 4 || op == 5) && dcache_pa(cs, addr, &pa) &&
        pa < s->dcache.size) {
        struct MT7621ShadowPage *sp =
            (struct MT7621ShadowPage *)s->dcache.shadow[pa / MT7621_PAGE];

        if (sp) {
            sp->line[dcache_line(pa)] = 0;
        }
    }
}


/*
 * A guest reset invalidates the cache: nothing survives it.  Called from the
 * machine's reset, which is what a guest reboot ends up triggering.
 */
void mt7621_dcache_reset(MT7621State *s)
{
    unsigned page;

    if (!s->dcache.enabled || !s->dcache.shadow) {
        return;
    }
    for (page = 0; page < dcache_pages; page++) {
        g_free(s->dcache.shadow[page]);
        s->dcache.shadow[page] = NULL;
    }
    memset(&stale_fifo, 0, sizeof(stale_fifo));
    stale_fifo.limit = getenv("MT7621_STALE_WINDOW")
                       ? strtoul(getenv("MT7621_STALE_WINDOW"), NULL, 0)
                       : MT7621_STALE_WINDOW;
    dcache_pending_flush_n = 0;
}

void mt7621_dcache_init(MT7621State *s, uint64_t ram_size)
{
    if (!getenv("MT7621_DCACHE")) {
        return;
    }
    dcache_state = s;
    if (getenv("MT7621_STALE_AGE")) {
        dcache_stale_age = strtoul(getenv("MT7621_STALE_AGE"), NULL, 0);
    }
    stale_fifo.limit = getenv("MT7621_STALE_WINDOW")
                       ? strtoul(getenv("MT7621_STALE_WINDOW"), NULL, 0)
                       : MT7621_STALE_WINDOW;
    if (stale_fifo.limit > MT7621_STALE_WINDOW) {
        stale_fifo.limit = MT7621_STALE_WINDOW;
    }
    dcache_pages = ram_size / MT7621_PAGE;
    s->dcache.ram = memory_region_get_ram_ptr(&s->ram);
    s->dcache.shadow = (uint8_t **)g_new0(struct MT7621ShadowPage *,
                                          dcache_pages);
    s->dcache.size = ram_size;
    s->dcache.enabled = true;

    guest_cycles_hook = mt7621_dcache_progress;
    cpu_dcache_page_slow = mt7621_dcache_page_slow;
    cpu_dcache_access_handler = mt7621_dcache_access;
    mips_dcache_op = mt7621_dcache_cache_op;
    fprintf(stderr, "mt7621: write-through data cache model enabled\n");
}
