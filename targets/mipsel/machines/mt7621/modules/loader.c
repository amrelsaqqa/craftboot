/*
 * MediaTek MT7621 -- the boot-image loader: uImage on NAND, raw flash dumps,
 * and ELF kernels loaded the way a bootloader would.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "mt7621.h"

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
        if (bl_off && !getenv("MT7621_STAGE1")) {
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
        if ((load & 0x1fffffff) + size > s->ram.size) {
            error_report("mt7621: image 0x%" PRIx64 "+0x%" PRIx64
                         " does not fit in RAM", load, size);
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
    close(fd);

    s->entry = entry;
}

