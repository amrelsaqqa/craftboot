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

#define MT7621_PAGE 4096
#define MT7621_DCACHE_LINE 32

/* implemented in target/mips; declared here so the machine can ask */
bool mips_mapping_is_cacheable(CPUMIPSState *env, target_ulong address,
                               MMUAccessType access_type, int mmu_idx);

static MT7621State *dcache_state;
static unsigned dcache_pages;

static inline uint8_t *dcache_ram(MT7621State *s, hwaddr pa)
{
    return (uint8_t *)s->dcache.ram + pa;
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

    return s && s->dcache.enabled && pa < s->dcache.size &&
           s->dcache.shadow[pa / MT7621_PAGE] != NULL;
}

/* Save a page's current contents and put it on the slow path. */
static bool dcache_shadow_page(CPUState *cs, hwaddr pa)
{
    MT7621State *s = dcache_state;
    unsigned page = pa / MT7621_PAGE;
    uint8_t *shadow;

    if (page >= dcache_pages) {
        return false;
    }
    if (s->dcache.shadow[page]) {
        return true;            /* already shadowed */
    }
    shadow = g_malloc(MT7621_PAGE);
    memcpy(shadow, dcache_ram(s, page * MT7621_PAGE), MT7621_PAGE);
    s->dcache.shadow[page] = shadow;
    /* cached mappings of this page must now come through the model */
    tlb_flush(cs);
    return true;
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
    uint8_t *shadow, buf[8];
    uint64_t v = value ? *value : 0;
    unsigned i;
    bool cached;

    if (!s || !s->dcache.enabled) {
        return false;
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
        if (!dcache_shadow_page(cs, pa)) {
            return false;
        }
        /* the store bypasses the cache: memory only */
        for (i = 0; i < size; i++) {
            dcache_ram(s, pa + i)[0] = (uint8_t)(v >> (i * 8));
        }
        return true;
    }

    shadow = s->dcache.shadow[pa / MT7621_PAGE];
    if (!shadow) {
        return false;           /* not shadowed: fast path is right */
    }
    shadow += pa % MT7621_PAGE;
    if (is_write) {
        for (i = 0; i < size; i++) {
            shadow[i] = (uint8_t)(v >> (i * 8));
            dcache_ram(s, pa + i)[0] = shadow[i];   /* write-through */
        }
    } else {
        memcpy(buf, shadow, size);
        v = 0;
        for (i = 0; i < size; i++) {
            v |= (uint64_t)buf[i] << (i * 8);
        }
        *value = v;
    }
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
        tlb_flush(cs);
        return;
    }
    if (op == 6) {              /* hit writeback: memory is already current */
        return;
    }
    if ((op == 4 || op == 5) && dcache_pa(cs, addr, &pa) &&
        pa < s->dcache.size) {
        uint8_t *shadow = s->dcache.shadow[pa / MT7621_PAGE];

        if (shadow) {
            memcpy(shadow + (pa % MT7621_PAGE),
                   dcache_ram(s, pa), MT7621_DCACHE_LINE);
        }
    }
}

void mt7621_dcache_init(MT7621State *s, uint64_t ram_size)
{
    if (!getenv("MT7621_DCACHE")) {
        return;
    }
    dcache_state = s;
    dcache_pages = ram_size / MT7621_PAGE;
    s->dcache.ram = memory_region_get_ram_ptr(&s->ram);
    s->dcache.shadow = g_new0(uint8_t *, dcache_pages);
    s->dcache.size = ram_size;
    s->dcache.enabled = true;

    cpu_dcache_page_slow = mt7621_dcache_page_slow;
    cpu_dcache_access_handler = mt7621_dcache_access;
    mips_dcache_op = mt7621_dcache_cache_op;
    fprintf(stderr, "mt7621: write-through data cache model enabled\n");
}
