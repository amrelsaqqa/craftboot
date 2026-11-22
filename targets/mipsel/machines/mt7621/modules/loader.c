/*
 * MediaTek MT7621 -- the boot-image loader: uImage on NAND, raw flash dumps,
 * and ELF kernels loaded the way a bootloader would.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "mt7621.h"

/* KSEG1 window: 512 MiB on this SoC, upper half aliases the DRAM below */
#define MT7621_LOAD_WINDOW 0x20000000ULL

/*
 * Loader/bring-up reset: every latch the model uses to follow the boot is a
 * property of *one* boot.  Keeping any of them across a reset makes the
 * second boot take the first boot's path (the hand-off latch especially: a
 * bootloader frame would be refused because "the kernel already owns the
 * engine").
 */
void mt7621_loader_reset(MT7621State *s)
{
    s->seen_high_pc = false;
    s->seen_boot = false;
    s->handoff_done = false;
    s->kernel_hits = 0;
    s->bl_timer_armed = false;
    s->entry_logged = false;
    s->rom_logged = false;
    s->pc_hist_idx = 0;
    memset(s->pc_hist, 0, sizeof(s->pc_hist));
    s->halt_ticks = 0;
    s->halt_seen_running = false;
    s->ramwin_n = 0;
    s->cmdline_cursor = 0;
    s->cmdline_phys = 0;
    s->cmdline_ticks = 0;
    s->cmdline_kernel_seen = false;
    s->cmdline_patches = 0;
    s->cmdline_deadline_ns = 0;
}

void mt7621_load_image(MT7621State *s, const char *filename)
{
    uint8_t hdr[64];
    uint8_t *buf;
    uint64_t load, entry, size;
    off_t off = 0, total;
    int fd;

    fd = open(filename, O_RDONLY | O_BINARY);
    if (fd < 0) {
        error_report("mt7621: cannot open %s: %s", filename, strerror(errno));
        exit(1);
    }

    total = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (s->nand_stride && s->nand_stride != MT7621_NAND_PAGE_SIZE) {
        total = (total / s->nand_stride) * MT7621_NAND_PAGE_SIZE;
    }

    if (pread(fd, hdr, sizeof(hdr), 0) != sizeof(hdr)) {
        error_report("mt7621: %s: short read", filename);
        exit(1);
    }

    if (ldl_be_p(hdr) == 0x7f454c46) {
        /*
         * ELF image: exactly what RouterBOOT hands over on a netboot.  Load
         * each PT_LOAD segment at its physical address and start at e_entry,
         * which is how the bootloader transfers control.
         */
        uint8_t eh[52];
        uint32_t phoff, phentsize, phnum, elf_entry;
        uint32_t i;

        s->elf_kernel = true;

        if (pread(fd, eh, sizeof(eh), 16) != sizeof(eh)) {
            error_report("mt7621: %s: short ELF header", filename);
            exit(1);
        }
        elf_entry = ldl_le_p(eh + 8);
        phoff     = ldl_le_p(eh + 12);
        phentsize = lduw_le_p(eh + 26);
        phnum     = lduw_le_p(eh + 28);

        if (!s->entry) {
            s->entry = elf_entry;
        }
        for (i = 0; i < phnum; i++) {
            uint8_t ph[32];
            uint32_t type, p_off, paddr, filesz, memsz;
            uint8_t *seg;

            if (pread(fd, ph, sizeof(ph), phoff + i * phentsize) != sizeof(ph)) {
                break;
            }
            type   = ldl_le_p(ph);
            p_off  = ldl_le_p(ph + 4);
            paddr  = ldl_le_p(ph + 12);
            filesz = ldl_le_p(ph + 16);
            memsz  = ldl_le_p(ph + 20);
            if (type != 1) {            /* PT_LOAD */
                continue;
            }
            if ((paddr & 0x1fffffff) + memsz > s->ram.size) {
                error_report("mt7621: %s: segment 0x%x+0x%x does not fit",
                             filename, paddr, memsz);
                exit(1);
            }
            seg = g_malloc0(filesz ? filesz : 1);
            if (filesz && pread(fd, seg, filesz, p_off) != filesz) {
                error_report("mt7621: %s: short segment read", filename);
                exit(1);
            }
            address_space_write(&address_space_memory,
                                paddr & 0x1fffffff, MEMTXATTRS_UNSPECIFIED,
                                seg, filesz);
            if (memsz > filesz) {
                uint8_t *z = g_malloc0(memsz - filesz);

                if (getenv("MT7621_ELF_DIRTYBSS")) {
                    /* mimic a bootloader that does not zero memsz-filesz */
                    memset(z, 0xa5, memsz - filesz);
                }
                address_space_write(&address_space_memory,
                                    (paddr & 0x1fffffff) + filesz,
                                    MEMTXATTRS_UNSPECIFIED, z,
                                    memsz - filesz);
                g_free(z);
            }
            g_free(seg);
            printf("mt7621: ELF segment 0x%08x-0x%08x loaded at 0x%08x\n",
                   p_off, p_off + filesz, paddr);
        }
        /*
         * Bring-up hook: reproduce the argc/argv hand-over RouterBOOT uses,
         * so the netboot path can be compared against the direct one.
         */
        if (getenv("MT7621_ELF_ARGS")) {
            gchar **tok = g_strsplit(getenv("MT7621_ELF_ARGS"), " ", 0);
            uint32_t strp = 0x01800000, argvp = 0x01801000;
            uint32_t ptrs[64];
            int n = 0, ti;

            for (ti = 0; tok[ti] && n < 64; ti++) {
                size_t l = strlen(tok[ti]) + 1;
                address_space_write(&address_space_memory, strp,
                                    MEMTXATTRS_UNSPECIFIED, tok[ti], l);
                ptrs[n++] = 0xa0000000 | strp;
                strp += (l + 3) & ~3;
            }
            address_space_write(&address_space_memory, argvp,
                                MEMTXATTRS_UNSPECIFIED, ptrs, n * 4);
            s->elf_argc = n;
            s->elf_argv = 0xa0000000 | argvp;
            printf("mt7621: ELF args argc=%d argv=%08x\n", n, s->elf_argv);
            g_strfreev(tok);
        }
        printf("mt7621: ELF entry 0x%08x\n", s->entry);
        close(fd);
        return;
    }

    if (s->uimage || ldl_be_p(hdr) == 0x27051956) {
        if (ldl_be_p(hdr) != 0x27051956) {
            error_report("mt7621: %s: not a uImage", filename);
            exit(1);
        }
        uint32_t img_size = (uint32_t)ldl_be_p(hdr + 12);
        /*
         * MT7621 BootROM NAND header: the uImage name field (32 bytes) is
         * overlaid by nand_header_t, so ih_bootloader_offset lives at +52.
         * When non-zero the BootROM first runs the stage-1 DDR init image
         * and then loads the bootloader from that file offset.
         */
        uint32_t bl_off = ldl_be_p(hdr + 52);

        size  = img_size;
        load  = (uint32_t)ldl_be_p(hdr + 16);
        entry = (uint32_t)ldl_be_p(hdr + 20);
        off   = 64;
        if (s->load_addr) {
            load = s->load_addr;
        }
        if (s->entry) {
            entry = s->entry;
        }
        if (bl_off && !getenv("MT7621_NO_ROM_STAGE1") && !s->warm_boot) {
            /*
             * Hardware path: with a non-zero ih_bootloader_offset the BootROM
             * runs the DDR-init stage first.  It copies the image data that
             * precedes the bootloader (bl_off - 64 bytes) into the BootROM's
             * stage-1 window at 0xb9000000 and enters it there; stage 1 trains
             * DRAM and then loads and jumps to the bootloader at ih_load.
             *
             * The emulated DRAM is already up, so stage 1 only has to make
             * its own way to the bootloader -- but running it is what the
             * silicon does, and the bootloader may rely on state it leaves
             * (exception vectors, TLB, caches).  Measured: the Breed die dump
             * boots only through this path; the old "step 2" shortcut (jump
             * straight to the bootloader) prints nothing at all with it.
             * MT7621_NO_ROM_STAGE1=1 restores the shortcut for A/B.
             */
            load  = 0xb9000000;
            entry = 0xb9000000;
            off   = 64;
            size  = (uint64_t)bl_off - 64;
            printf("mt7621: BootROM step 1: DDR-init stage 0x%" PRIx64
                   " bytes @0x%08x -> 0x%08x, then bootloader @0x%x\n",
                   size, (unsigned)off, (unsigned)load, bl_off);
            /* the bootloader still has to be in place for the hand-over */
            {
                uint32_t bl_load = (uint32_t)ldl_be_p(hdr + 16);
                uint32_t bl_size = 64 + (uint64_t)img_size - bl_off;

                if (bl_load & 0x80000000) {
                    uint8_t *tmp = g_malloc(bl_size);
                    if (mt7621_flash_pread(s, fd, bl_off, tmp, bl_size) !=
                        (ssize_t)bl_size) {
                        error_report("mt7621: %s: short read at 0x%x",
                                     filename, bl_off);
                        exit(1);
                    }
                    address_space_write(&address_space_memory,
                                        bl_load & 0x1fffffff,
                                        MEMTXATTRS_UNSPECIFIED, tmp, bl_size);
                    g_free(tmp);
                }
            }
        } else if (bl_off && !getenv("MT7621_STAGE1")) {
            if (bl_off >= 64 + (uint64_t)img_size) {
                error_report("mt7621: bootloader offset 0x%x out of range",
                             bl_off);
                exit(1);
            }
            /*
             * BootROM step 2: the bootloader is loaded to ih_load (a KSEG1
             * address, called out in the MT7621 NAND image header) and
             * entered there.  Stage-1 has already trained DDR by now.
             */
            load  = (uint32_t)ldl_be_p(hdr + 16);
            entry = (uint32_t)ldl_be_p(hdr + 20);
            off   = bl_off;
            size  = 64 + (uint64_t)img_size - bl_off;
            printf("mt7621: BootROM boot step 2: bootloader 0x%" PRIx64
                   " bytes from file 0x%x -> 0x%" PRIx64 "\n",
                   size, bl_off, load);
        }
        /*
         * The KSEG1 window is twice the DRAM size on this SoC (the second
         * 256 MiB aliases the first), and the BootROM runs stage 1 out of the
         * 0xbfc00000 window, which lands in that alias.  Check the window,
         * not the RAM region, or a legitimate stage-1 load is rejected.
         */
        if ((load & 0x1fffffff) + size > MT7621_LOAD_WINDOW) {
            error_report("mt7621: image 0x%" PRIx64 "+0x%" PRIx64
                         " does not fit in the KSEG1 window", load, size);
            exit(1);
        }
        printf("mt7621: uImage payload 0x%" PRIx64 " bytes -> 0x%" PRIx64
               ", entry 0x%" PRIx64 "\n", size, load, entry);
    } else {
        load  = s->load_addr;
        entry = s->entry;
        size  = total;
        printf("mt7621: raw image 0x%" PRIx64 " bytes -> 0x%" PRIx64
               ", entry 0x%" PRIx64 "\n", size, load, entry);
    }

    if (size > total - off) {
        error_report("mt7621: %s: truncated image", filename);
        exit(1);
    }

    buf = g_malloc(MIN(size, 1 * MiB));
    while (size) {
        size_t chunk = MIN(size, 1 * MiB);
        ssize_t n = mt7621_flash_pread(s, fd, off, buf, chunk);

        if (n != chunk) {
            error_report("mt7621: %s: short read at 0x%" PRIx64,
                         filename, (uint64_t)off);
            exit(1);
        }
        {
            hwaddr paddr = load;

            /* KSEG0/KSEG1: the image header uses the uncached alias. */
            if (paddr >= 0x80000000 && paddr < 0xc0000000) {
                paddr &= 0x1fffffff;
            }
            address_space_write(&address_space_memory, paddr,
                                MEMTXATTRS_UNSPECIFIED, buf, chunk);
        }
        load += chunk;
        off  += chunk;
        size -= chunk;
    }
    g_free(buf);

    /*
     * Mirror the RouterBOOT area to 0x80e00000 before the booter runs.
     *
     * The 256 KiB "RouterBoot" partition is not only a config store: its
     * 0x28-byte header is the board's *flash descriptor*.  Word 0 is
     * 0x10000009, then the geometry the booter parses at 0x811018b4:
     *
     *   +0x14  config TLV offset ("Hard")
     *   +0x18, +0x1c, +0x20, +0x24  geometry windows
     *
     * and the TLVs themselves follow.  The booter reads all of that from RAM
     * at 0x80e00000, not from flash: the boot chain leaves the area there
     * before the booter's first line of C runs (ROUTERBOOT-NOTES.md sec.3,
     * sec.15, sec.18 - the reason the cave loader had to wget rb-area.bin
     * there).  Without it the booter writes only its magic word, parses zeros
     * for the geometry and dies on a NULL-based pointer the moment etherboot
     * needs a descriptor (observed: TLB load fault at 0xffff8f14 from
     * `lb v1,-0x70ec(t6)` with $t6 = 0).
     *
     * This board's own device tree puts the area at flash 0x1e0000:
     * "RouterBoot" 0x1e0000-0x220000.  MT7621_RB_AREA=<offset> overrides it,
     * MT7621_RB_AREA_MIRROR=0 disables the mirror for A/B.
     */
    if (s->board == MT7621_BOARD_LG_GAPM7200 &&
        (!getenv("MT7621_RB_AREA_MIRROR") ||
         atoi(getenv("MT7621_RB_AREA_MIRROR")))) {
        const char *area_env = getenv("MT7621_RB_AREA");
        uint64_t area_off = area_env ? strtoull(area_env, NULL, 0) : 0x1e0000;

        if (total >= area_off + 0x40000) {
            uint8_t *area = g_malloc(0x40000);

            if (mt7621_flash_pread(s, fd, area_off, area, 0x40000) ==
                0x40000) {
                address_space_write(&address_space_memory, 0xe00000,
                                    MEMTXATTRS_UNSPECIFIED, area, 0x40000);
                printf("mt7621: RouterBoot area 0x40000 from flash 0x%"
                       PRIx64 " -> 0x80e00000 (board descriptors)"
                       " [hdr=%08x geom=%08x,%08x,%08x,%08x hard=%08x]\n",
                       area_off,
                       ldl_le_p(area + 0x00), ldl_le_p(area + 0x14),
                       ldl_le_p(area + 0x18), ldl_le_p(area + 0x1c),
                       ldl_le_p(area + 0x20), ldl_le_p(area + 0xf000));
            }
            g_free(area);
        }
    }
    close(fd);

    s->entry = entry;
}

