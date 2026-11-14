/*
 * MediaTek MT7621 (MIPS 1004Kc) bring-up machine
 *
 * This machine models the SoC, not the firmware: DDR at 0, the palmbus
 * peripherals at 0x1e000000, and a boot path that either
 *
 *   - takes a raw image (load-addr/entry properties), or
 *   - takes a uImage (the format the MT7621 BootROM loads from NAND),
 *     strips the 64-byte header and loads the payload at the header's
 *     load address.
 *
 * The first target is the Breed bootloader taken from a parallel-NAND
 * dump, followed by OpenWrt.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "mt7621/mt7621.h"
#include "qapi/visitor.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "hw/boards.h"
#include "hw/char/serial-mm.h"
#include "hw/char/serial.h"
#include "hw/intc/mips_gic.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "hw/mips/mips.h"
#include "accel/tcg/cpu-ldst.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "net/net.h"
#include "qemu/main-loop.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
#include "qemu/log.h"
#include "exec/icache-shadow.h"
#include "hw/timer/mips_gictimer.h"
#include "qemu/timer.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/block-backend.h"
#include "target/mips/cpu-qom.h"
#include "target/mips/internal.h"


/* SoC memory map */
#define MT7621_RAM_BASE         0x00000000
#define MT7621_PERIPH_BASE      0x1e000000
#define MT7621_PERIPH_SIZE      0x00100000

#define MT7621_SYSCTL_OFF       0x00000000
/*
 * SPI NOR is memory-mapped by the BootROM at 0x1fc00000.  The MPU decodes
 * a 4 MiB window there, which is all the boot chain needs: RouterBOOT's
 * second stage lives at flash offset 0x10000.
 */
#define MT7621_SPI_FLASH_BASE   0x1fc00000
#define MT7621_SPI_WINDOW       0x00400000
#define MT7621_UART0_OFF        0x00000c00

/* MT7621 NAND flash interface (register map matches Linux mtk_nand) */
#define MT7621_DRAMC_OFF        0x00005000

/*
 * Interrupt wiring taken from the DTB appended to the flash kernel:
 * a MIPS GIC at 0x1fbc0000, UART0 on shared interrupt 26 and the
 * "ralink,mt7621-systick" 50 kHz counter on shared interrupt 5.
 */
/*
 * 64 shared interrupts, not an arbitrary 256.  RouterBOOT validates the
 * GIC before it will use it: it reads GIC_SH_CONFIG and requires the
 * NUMINTRS field (bits 23:16, which QEMU derives as num_irq / 8 - 1) to be
 * exactly 7 -- i.e. 64 interrupts.  Our own devices only use IRQ 5 and 26,
 * and Linux sizes the GIC from this same field, so 64 is both the hardware
 * truth and strictly safer.
 */
#define MT7621_GIC_NUM_IRQ      64
#define MT7621_UART0_IRQ        26

/*
 * ralink,cevt-systick: a 16-bit up-counter clocked at 50 kHz.  Linux
 * programs COMPARE with "COUNT + delta" and expects an interrupt when
 * the counter reaches it.
 */
/* Every KSEG0/KSEG1 physical address the CPU can generate, for aliasing. */
#define MT7621_KSEG1_WINDOW     0x20000000
/*
 * Frame engine / switch register block.  The DT says 64 KiB but the MDIO
 * control register lives at 0x10004, so map 128 KiB.
 */

/*
 * MIPS Coherence Manager (CM) / Cluster Power Controller (CPC).
 * CPC sits at 0x1fbf0000, the global control registers (GCR) at 0x1fbf8000.
 */

#define MT7621_UART_CLOCK       50000000
#define MT7621_UART_BAUD_BASE   (MT7621_UART_CLOCK / 16)
#define MT7621_UART1_IRQ        27

/* sysctl register offsets we care about */
#define SYS_CHIP_NAME0          0x00
#define SYS_CHIP_NAME1          0x04
#define SYS_CHIP_REV_ID         0x0c
#define SYS_SYSCFG0             0x10
#define SYS_CLKCFG0             0x2c
#define SYS_RSTCTL              0x34
#define SYS_RSTSTAT             0x38
#define WDT_RST_CAUSE           (1u << 1)

/*
 * Ralink timer/watchdog block in the system controller's second 256 byte
 * page (1e000100).  Linux's mt7621_wdt drives timer 1: TMR1LOAD holds the
 * reload value in prescaler ticks, TMR1CTL bit 7 enables counting and bits
 * 16..31 are the prescaler in microseconds.  Writing TMRSTAT bit 9 reloads
 * (that is the "ping"); letting it run out resets the SoC.
 */
#define SYS_CUR_CLK_STS         0x44
#define SYS_GPIO_MODE           0x60



bool mt7621_trace;

/*
 * Boolean env knob.  These ML-style knobs used to be tested with a bare
 * getenv(), which is true for *any* value -- including "0" -- so a launcher
 * that always exports MT7621_FOO=0 silently turned the feature on.  Treat
 * the usual false spellings as off.
 */
bool mt7621_flag(const char *name)
{
    const char *v = getenv(name);

    if (!v || !*v) {
        return false;
    }
    return strcmp(v, "0") && strcmp(v, "no") && strcmp(v, "off") &&
           strcmp(v, "false");
}


/*
 * MT7621_CP_SIG / MT7621_CP_PATCH: debug-only guest code patcher.
 *
 * Finds a hex signature in guest RAM (the agent's /init is unpacked into a
 * ramfs, so its text is an ordinary page-cache page) and overwrites words at
 * fixed deltas from the match.  Used to neutralise one branch at a time in
 * the netinstall discovery responder and see which condition produces the
 * NACK it answers with.
 */
static int mt7621_hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void mt7621_cp_apply(MT7621State *s, hwaddr at)
{
    const char *p = getenv("MT7621_CP_PATCH");
    char buf[256];

    if (!p || strlen(p) >= sizeof(buf)) {
        return;
    }
    strcpy(buf, p);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long delta, word;

        if (sscanf(tok, "%li:%li", &delta, &word) != 2) {
            continue;
        }
        address_space_stl_le(&address_space_memory, at + delta, (uint32_t)word,
                             MEMTXATTRS_UNSPECIFIED, NULL);
        fprintf(stderr, "cp: patched %08" HWADDR_PRIx " <- %08lx\n",
                at + delta, word);
    }
}

static void mt7621_cp_tick(void *opaque)
{
    MT7621State *s = opaque;
    const char *sig_hex = getenv("MT7621_CP_SIG");
    uint8_t sig[16];
    unsigned siglen = 0, i;
    static uint8_t chunk[4 << 20];
    hwaddr base;

    if (s->cp_done || !sig_hex) {
        return;
    }
    for (i = 0; sig_hex[i] && sig_hex[i + 1] && siglen < sizeof(sig); i += 2) {
        int hi = mt7621_hexval(sig_hex[i]);
        int lo = mt7621_hexval(sig_hex[i + 1]);

        if (hi < 0 || lo < 0) {
            break;
        }
        sig[siglen++] = (hi << 4) | lo;
    }
    if (siglen < 4) {
        s->cp_done = true;
        return;
    }
    for (base = 0; base + sizeof(chunk) <= 0x10000000;
         base += sizeof(chunk) - sizeof(sig)) {
        uint8_t *hit;

        address_space_read(&address_space_memory, base,
                           MEMTXATTRS_UNSPECIFIED, chunk, sizeof(chunk));
        hit = memmem(chunk, sizeof(chunk), sig, siglen);
        if (hit) {
            hwaddr at = base + (hit - chunk);

            fprintf(stderr, "cp: signature at %08" HWADDR_PRIx "\n", at);
            mt7621_cp_apply(s, at);
            s->cp_done = true;
            return;
        }
    }
    timer_mod(s->cp_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2000);
}

/*
 * MT7621_RAMDUMP=<ms>:<path> - one-shot dump of guest RAM, so runtime state
 * can be inspected without the monitor (its unix socket does not survive).
 */
static void mt7621_ramdump(void *opaque)
{
    const char *spec = getenv("MT7621_RAMDUMP");
    char *copy, *colon, *path;
    static uint8_t buf[1 << 20];
    hwaddr a;
    FILE *f;

    if (!spec) {
        return;
    }
    copy = g_strdup(spec);
    colon = strchr(copy, ':');
    if (!colon) {
        g_free(copy);
        return;
    }
    *colon = 0;
    path = colon + 1;
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "ramdump: cannot open %s\n", path);
        g_free(copy);
        return;
    }
    for (a = 0; a < 0x10000000; a += sizeof(buf)) {
        address_space_read(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                           buf, sizeof(buf));
        fwrite(buf, 1, sizeof(buf), f);
    }
    fclose(f);
    fprintf(stderr, "ramdump: wrote %s\n", path);
    g_free(copy);
}

/*
 * MT7621_RAMWIN=first_ms:step_ms:count:start_hex:len_hex:prefix - dump a
 * window of guest RAM a few times in a row (prefix.N), for state that only
 * exists briefly during startup.
 */
static void mt7621_ramwin(void *opaque)
{
    MT7621State *s = opaque;
    const char *spec = getenv("MT7621_RAMWIN");
    char *copy, *tok, *save = NULL;
    long first, step, count, start, len;
    char *prefix;
    char path[256];
    static uint8_t buf[1 << 20];
    FILE *f;
    hwaddr a;

    if (!spec || s->ramwin_n >= 1000) {
        return;
    }
    copy = g_strdup(spec);
    tok = strtok_r(copy, ":", &save);
    first = tok ? strtol(tok, NULL, 0) : 0;
    (void)first;
    tok = strtok_r(NULL, ":", &save); step = tok ? strtol(tok, NULL, 0) : 1000;
    tok = strtok_r(NULL, ":", &save); count = tok ? strtol(tok, NULL, 0) : 0;
    tok = strtok_r(NULL, ":", &save); start = tok ? strtol(tok, NULL, 16) : 0;
    tok = strtok_r(NULL, ":", &save); len = tok ? strtol(tok, NULL, 16) : 0;
    tok = strtok_r(NULL, ":", &save); prefix = tok;
    if (!prefix || len <= 0 || len > (long)sizeof(buf) || count <= 0) {
        g_free(copy);
        return;
    }
    snprintf(path, sizeof(path), "%s.%u", prefix, s->ramwin_n);
    f = fopen(path, "wb");
    if (f) {
        for (a = start; a < (hwaddr)(start + len); a += 4096) {
            address_space_read(&address_space_memory, a,
                               MEMTXATTRS_UNSPECIFIED, buf, 4096);
            fwrite(buf, 1, 4096, f);
        }
        fclose(f);
    }
    s->ramwin_n++;
    timer_mod(s->ramwin_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + step);
    if (s->ramwin_n >= (unsigned)count) {
        fprintf(stderr, "ramwin: %u windows written\n", s->ramwin_n);
    }
    g_free(copy);
}

static void mt7621_rstctl_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    MT7621State *s = opaque;
    uint32_t old = s->sysctl_regs[SYS_RSTCTL / 4];

    s->sysctl_regs[SYS_RSTCTL / 4] = val;

    if ((val & 1) && !(old & 1)) {
        if (getenv("MT7621_RSTLOG") && s->cpu) {
            CPUMIPSState *env = &s->cpu->env;
            uint32_t sp = env->active_tc.gpr[29];
            uint32_t pc = env->active_tc.PC;
            uint32_t ra = env->active_tc.gpr[31];
            int i;

            fprintf(stderr, "restart: pc=%08x ra=%08x sp=%08x\n",
                    pc, ra, sp);
            for (i = 0; i < 64; i++) {
                uint32_t v;
                if (address_space_rw(&address_space_memory,
                                     sp + i * 4, MEMTXATTRS_UNSPECIFIED,
                                     (uint8_t *)&v, 4, false)) {
                    break;
                }
                if (v >= 0x80000000 && v < 0x82000000) {
                    fprintf(stderr, "  stack[%2d] %08x\n", i, v);
                }
            }
        }
        if (getenv("MT7621_NO_RESET")) {
            /* keep the machine frozen so the ring buffer can be read */
            fprintf(stderr, "mt7621: guest requested a reset, holding\n");
            return;
        }
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static uint64_t mt7621_rstctl_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;

    return s->sysctl_regs[SYS_RSTCTL / 4];
}

static const MemoryRegionOps mt7621_rstctl_ops = {
    .read = mt7621_rstctl_read,
    .write = mt7621_rstctl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};


/*
 * Attach an optional writable NAND image passed as
 *   -drive file=nand.img,if=none,id=nand,format=raw
 * A fresh/empty image is seeded from the dump; an existing one is used as
 * is, so writes survive QEMU restarts while the dump stays pristine.
 */
static void mt7621_nand_attach(MT7621State *s)
{
    const char *path = getenv("MT7621_NAND_IMAGE");
    BlockBackend *blk;
    int64_t len;
    int fd;
    off_t sz;

    s->nand_fd = -1;

    /*
     * Preferred: a plain image file we create and seed ourselves, so the
     * user does not have to know the flash size:
     *   MT7621_NAND_IMAGE=~/nand-state.img qemu-system-mipsel ...
     */
    if (path) {
        if (!s->nand) {
            warn_report("mt7621: MT7621_NAND_IMAGE needs a readable boot dump");
            return;
        }
        fd = open(path, O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            warn_report("mt7621: cannot open NAND image %s: %s, changes will "
                        "be kept in RAM only", path, strerror(errno));
            return;
        }
        sz = lseek(fd, 0, SEEK_END);
        if (sz < 0) {
            warn_report("mt7621: cannot size %s", path);
            close(fd);
            return;
        }
        if ((uint64_t)sz < s->nand_size) {
            if (mt7621_pwrite_all(fd, s->nand, s->nand_size, 0) < 0) {
                warn_report("mt7621: cannot seed %s", path);
                close(fd);
                return;
            }
            printf("mt7621: NAND image %s seeded from the boot dump "
                   "(%" PRIu64 " bytes)\n", path, s->nand_size);
        } else {
            if (pread(fd, s->nand, s->nand_size, 0) != (ssize_t)s->nand_size) {
                warn_report("mt7621: cannot read %s", path);
                close(fd);
                return;
            }
            printf("mt7621: using persistent NAND image %s\n", path);
        }
        s->nand_fd = fd;
        return;
    }

    /* Alternative, QEMU style (the file has to exist):
     *   -drive file=nand.img,if=none,id=nand,format=raw
     */
    blk = blk_by_name("nand");
    if (!blk) {
        return;
    }
    if (!s->nand) {
        warn_report("mt7621: -drive id=nand needs a readable boot dump");
        return;
    }

    len = blk_getlength(blk);
    if (len < 0) {
        warn_report("mt7621: cannot size the NAND image, ignoring it");
        return;
    }
    if (len < s->nand_size) {
        if (blk_pwrite(blk, 0, s->nand_size, s->nand, 0) < 0) {
            warn_report("mt7621: cannot seed the NAND image, changes will "
                        "be kept in RAM only");
            return;
        }
        printf("mt7621: NAND image seeded from the boot dump (%" PRIu64
               " bytes)\n", s->nand_size);
    } else {
        if (blk_pread(blk, 0, s->nand_size, s->nand, 0) < 0) {
            warn_report("mt7621: cannot read the NAND image, changes will "
                        "be kept in RAM only");
            return;
        }
        printf("mt7621: using the persistent NAND image (%" PRIu64
               " bytes)\n", s->nand_size);
    }
    s->nand_blk = blk;
}

/*
 * Read bytes of the flash's *logical* address space (the concatenation of
 * the main areas).  Dumps that include the spare area store 2112 bytes per
 * page, so a linear read would pick up 64 bytes of OOB every 2048 bytes and
 * corrupt anything larger than one page -- which is what a uImage payload
 * is.
 */
static ssize_t mt7621_flash_pread(MT7621State *s, int fd, uint64_t off,
                                  void *buf, size_t len)
{
    uint8_t *dst = buf;
    size_t done = 0;

    while (done < len) {
        uint64_t foff = off + done;
        size_t take = len - done;

        if (s->nand_stride && s->nand_stride != MT7621_NAND_PAGE_SIZE) {
            uint64_t page = foff / MT7621_NAND_PAGE_SIZE;
            uint64_t inpage = foff % MT7621_NAND_PAGE_SIZE;

            if (take > MT7621_NAND_PAGE_SIZE - inpage) {
                take = MT7621_NAND_PAGE_SIZE - inpage;
            }
            foff = page * s->nand_stride + inpage;
        }
        if (pread(fd, dst + done, take, foff) != (ssize_t)take) {
            return -1;
        }
        done += take;
    }
    return len;
}

/*
 * MT7621_FLASH_SAVE=<path> keeps a copy of the (writable) NOR buffer on disk
 * while the guest runs.  The memory window only exposes the first 4 MiB, so
 * pmemsave cannot reach the rest; netinstall writes the whole 16 MiB.
 */
static void mt7621_flash_save_cb(void *opaque)
{
    MT7621State *s = opaque;
    const char *path = getenv("MT7621_FLASH_SAVE");
    int fd;

    if (path && s->flash) {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
        if (fd >= 0) {
            if (write(fd, s->flash, s->flash_size) != (ssize_t)s->flash_size) {
                error_report("mt7621: short write saving %s", path);
            }
            close(fd);
        }
    }
    timer_mod(s->flash_save_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);
}

/*
 * SPI NOR boards boot from a memory-mapped flash: the BootROM maps the
 * first 4 MiB at 0x1fc00000 and enters at 0xbfc00000.  There is no RAM
 * load and no uImage header; RouterBOOT's own stage-1 header jumps into
 * its second stage at flash offset 0x10000.
 */
static void mt7621_nor_load(MT7621State *s, const char *filename)
{
    int fd;
    off_t sz;
    uint64_t window;

    fd = open(filename, O_RDONLY | O_BINARY);
    if (fd < 0) {
        error_report("mt7621: cannot open %s: %s", filename, strerror(errno));
        exit(1);
    }
    sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (sz <= 0) {
        error_report("mt7621: %s: empty flash image", filename);
        exit(1);
    }

    s->flash = g_malloc0(sz);
    if (pread(fd, s->flash, sz, 0) != sz) {
        error_report("mt7621: %s: short read", filename);
        exit(1);
    }
    close(fd);
    s->flash_size = sz;
    s->spi_nor = true;

    /*
     * The flash controller's memory window covers the whole part, not just the
     * 4 MiB the boot ROM maps for boot: RouterOS's mtd driver reads through
     * this window (its SPI reads never show up as controller transactions),
     * and a YAFFS scan that runs off the end of a 4 MiB alias sees nothing but
     * zeros and refuses to mount.  MT7621_FLASH_WINDOW=4m restores the old
     * behaviour for A/B testing.
     */
    window = (uint64_t)sz;
    {
        const char *w = getenv("MT7621_FLASH_WINDOW");

        if (w && (!strcmp(w, "4m") || !strcmp(w, "4M"))) {
            window = MIN((uint64_t)sz, (uint64_t)MT7621_SPI_WINDOW);
        }
    }
    memory_region_init_ram_ptr(&s->flash_mr, NULL, "mt7621.spi-nor",
                               window, s->flash);
    /*
     * Writable: RouterBOOT does not program settings through the SPI
     * controller at all, it stores them into the memory mapped window
     * ("writing settings to flash.."), which the SoC turns into a flash
     * program cycle.  Keeping the window read-only silently dropped every
     * setting the booter ever saved.
     */
    memory_region_add_subregion(get_system_memory(), MT7621_SPI_FLASH_BASE,
                                &s->flash_mr);

    if (getenv("MT7621_FLASH_SAVE")) {
        s->flash_save_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                          mt7621_flash_save_cb, s);
        timer_mod(s->flash_save_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);
    }

    s->entry = MT7621_SPI_FLASH_BASE | 0xa0000000;   /* KSEG1 0xbfc00000 */
    printf("mt7621: SPI NOR %0.1f MiB mapped at 0x%08x, entry 0x%08x\n",
           (double)sz / (1024 * 1024), MT7621_SPI_FLASH_BASE, s->entry);
}

static void mt7621_load_image(MT7621State *s, const char *filename)
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

/*
 * The MT7621 uses the legacy MIPS GIC, and its reset mapping sends every
 * shared interrupt to VP0 pin 0, i.e. CPU interrupt line 2.  That is why
 * Breed only programs SMASK/POL and then enables IM2: it never writes the
 * mapping registers.  QEMU's GIC model resets everything unmapped, so put
 * the SoC default back after the device reset has run.
 */
static void mt7621_gic_default_map(MT7621State *s)
{
    MIPSGICState *gic;
    int i;

    if (!s->gic) {
        return;
    }

    gic = MIPS_GIC(s->gic);
    for (i = 0; i < gic->num_irq; i++) {
        gic->irq_state[i].map_pin = 0;
        gic->irq_state[i].map_vp = 0;
    }
}

static void mt7621_gic_reset(void *opaque)
{
    mt7621_gic_default_map(opaque);
}

static void mt7621_sysctl_defaults(MT7621State *s)
{
    memset(s->sysctl_regs, 0, sizeof(s->sysctl_regs));
    /*
     * "MT76" / "21  ".  Linux's ralink SoC probe compares these two words
     * and panics with "mt7621: unknown SoC" if they do not match.
     */
    s->sysctl_regs[SYS_CHIP_NAME0 / 4]   = 0x3637544d;
    s->sysctl_regs[SYS_CHIP_NAME1 / 4]   = 0x20203132;
    /*
     * SYS_CHIP_REV_ID, decoded the way U-Boot, Breed and Linux's ralink probe
     * do it: bit 17 = CPU_ID (second core present), bit 16 = PKG_ID,
     * bits 11:8 = version, bits 3:0 = eco.
     *
     * The real lg-gapm7200 boards report "SoC Type: MediaTek MT7621 ver:1
     * eco:3", so model exactly that; Breed prints the same fields as
     * "MT7621A ver 1, eco 3".  MT7621_ECO overrides the eco nibble for A/B
     * tests (some kernels gate errata workarounds on it).
     */
    {
        const char *eco = getenv("MT7621_ECO");
        unsigned e = eco ? (unsigned)atoi(eco) & 0xf : 3;

        s->sysctl_regs[SYS_CHIP_REV_ID / 4]  = 0x00030000 | (1 << 8) | e;
    }
    /*
     * SYSCFG0: CHIP_MODE[3:0] tells the firmware which boot flash is wired
     * up; 0 means "no NAND" and makes board_init skip the NFI entirely.
     * Bits above it: DRAM type and xtal mode.
     */
    s->sysctl_regs[SYS_SYSCFG0 / 4]      = 0x00000180 | s->chip_mode;
    /*
     * CPLL clock config.  RouterBOOT programs 0x40800000 on the RB750Gr3,
     * which is how Linux ends up reporting the 875 MHz CPU clock (and the
     * 1.75 GHz GIC counter derived from it); the cold-reset value 0x40000000
     * makes it report 25 MHz instead, which then disagrees with the emulated
     * timers by a factor of 70.
     */
    s->sysctl_regs[SYS_CLKCFG0 / 4]      = 0x40800000; /* 880 MHz PLL */
    s->sysctl_regs[SYS_CUR_CLK_STS / 4]  = 0x00000101; /* /1 * 1 */
    /*
     * 0x3fc is polled for bit 10 before Breed's DDR3 init: it reports the
     * DDR/PLL calibration as finished. Our DDR is always ready.
     */
    s->sysctl_regs[0x3fc / 4]            = 0x00000400;
    /*
     * SYSC_RSTSTAT reports the reset cause.  Keep the bit we recorded when
     * the watchdog barked (Breed overwrites this register itself once it
     * runs, but a real SoC latches the cause across a watchdog reset).
     */
    if (s->wdt_rst_cause) {
        s->sysctl_regs[SYS_RSTSTAT / 4] = WDT_RST_CAUSE;
        s->wdt_rst_cause = false;
    }
}

static void mt7621_dramc_defaults(MT7621State *s)
{
    memset(s->dramc_regs, 0, sizeof(s->dramc_regs));
    s->dramc_regs[0x604 / 4] = 0x00000000;
    s->dramc_regs[0x618 / 4] = 0x00000190;
    /*
     * MEMC CPU PLL (0x648), decoded by Linux's clk-mt7621 as
     *   cpu = ((FBDIV + 1) * xtal) >> prediv_tbl[PREDIV], then / ffiv * ffrac
     * with FBDIV = bits 10:4 and PREDIV = bits 13:12.  The old value
     * 0x03800000 has both fields zero, so the kernel computed
     * (0 + 1) * 25 MHz = 25 MHz for the CPU and for the GIC clocksource:
     * every udelay() came out 35x too long (the NAND bad-block scan took a
     * minute and tripped the RCU stall detector) and the clock reported in
     * dmesg was nonsense.
     *
     * FBDIV 69 / PREDIV 1 gives (69 + 1) * 25 MHz / 2 = 875 MHz, the clock
     * the GIC counter is modelled at (mips_gictimer_set_freq) and the value
     * RouterBOOT reports for the RB750Gr3.
     */
    {
        const char *pll = getenv("MT7621_CPU_PLL");

        s->dramc_regs[0x648 / 4] = pll ? (uint32_t)strtoul(pll, NULL, 0)
                                       : ((69 << 4) | (1 << 12)); /* 875MHz */
    }
    s->dramc_regs[0x3fc / 4] = 0x00000400;
}

/*
 * `poweroff` and `halt` on this SoC both end in machine_hang(): interrupts
 * are disabled, all of them are masked and the CPU parks in WAIT.  Nothing
 * is written to the system controller, so the only way to notice is that a
 * CPU which can no longer be woken stays halted.
 */
static void mt7621_poweroff_poll(void *opaque)
{
    MT7621State *s = opaque;
    CPUState *cs = CPU(s->cpu);
    CPUMIPSState *env = &s->cpu->env;
    bool parked = cs->halted && !(env->CP0_Status & (1 << CP0St_IE));

    /*
     * Bring-up: report the CP0/TLB state the guest starts the kernel with.
     * The netboot path hands over from a bootloader that has been running for
     * seconds, so the state it leaves behind matters.
     */
    if (getenv("MT7621_ENTRYLOG") && s->cpu) {
        uint32_t hpc = env->active_tc.PC;

        s->pc_hist[s->pc_hist_idx++ % PC_HIST] = hpc;
        if (!s->rom_logged && hpc >= 0xbfc00000 && hpc < 0xbfc01000) {
            int k;

            s->rom_logged = true;
            fprintf(stderr, "rom reached: pc=%08x status=%08x cause=%08x "
                    "badvaddr=%08x epc=%08x errpc=%08x\n",
                    hpc, env->CP0_Status, env->CP0_Cause,
                    (uint32_t)env->CP0_BadVAddr, (uint32_t)env->CP0_EPC,
                    (uint32_t)env->CP0_ErrorEPC);
            fprintf(stderr, "pcs:");
            for (k = 0; k < PC_HIST; k++) {
                uint32_t v = s->pc_hist[(s->pc_hist_idx + k) % PC_HIST];

                if (v) {
                    fprintf(stderr, " %08x", v);
                }
            }
            fprintf(stderr, "\n");
        }
    }
    /* Diagnostic: report every architectural exception the guest takes. */
    if (getenv("MT7621_EXCLOG") && s->cpu) {
        static int exc_budget = 4096;
        uint32_t cause = env->CP0_Cause;
        unsigned code = (cause >> 2) & 0x1f;

        /* TLB misses (1,2,3,8) are normal user-space traffic and would eat
         * the budget long before the interesting fault; only report the
         * codes that mean something went wrong. */
        bool interesting = (code == 4 || code == 5 || code == 10 ||
                            code == 11 || code == 12 || code == 13);
        if (code && interesting && exc_budget-- > 0) {
            fprintf(stderr, "exc: code=%u pc=%08x epc=%08x badvaddr=%08x "
                    "status=%08x cause=%08x\n", code,
                    (uint32_t)env->active_tc.PC, (uint32_t)env->CP0_EPC,
                    (uint32_t)env->CP0_BadVAddr, env->CP0_Status, cause);
            /* Reserved Instruction: show the word the guest choked on and
             * the preceding few, which names the instruction and its delay
             * slot context without needing a guest-side disassembler. */
            if (code == 11) {
                uint32_t epc = (uint32_t)env->CP0_EPC;
                uint32_t insn[4];
                int k;

                for (k = 0; k < 4; k++) {
                    insn[k] = cpu_ldl_code(env, epc - 8 + 4 * k);
                }
                fprintf(stderr, "RI: %08x: %08x %08x [%08x] %08x\n", epc - 8,
                        insn[0], insn[1], insn[2], insn[3]);
            }
        }
    }

    if (getenv("MT7621_ENTRYLOG") && !s->entry_logged && s->cpu) {
        uint32_t pc = env->active_tc.PC;

        if (pc >= 0x80600000 && pc < 0x81000000) {
            int i;

            s->entry_logged = true;
            if (getenv("MT7621_ENTRY_FIX")) {
                int t;

                env->CP0_Status &= ~(1 << CP0St_ERL);
                env->CP0_Config0 = (env->CP0_Config0 & ~7u) | 2;
                for (t = 0; t < MIPS_TLB_MAX; t++) {
                    env->tlb->mmu.r4k.tlb[t].VPN = 0;
                    env->tlb->mmu.r4k.tlb[t].PageMask = 0;
                    env->tlb->mmu.r4k.tlb[t].PFN[0] = 0;
                    env->tlb->mmu.r4k.tlb[t].PFN[1] = 0;
                }
                fprintf(stderr, "entry: normalised status=%08x config0=%08x\n",
                        env->CP0_Status, (uint32_t)env->CP0_Config0);
            }
            fprintf(stderr, "entry: pc=%08x status=%08x cause=%08x wired=%u "
                    "entryhi=%08x index=%u config0=%08x config1=%08x\n",
                    pc, env->CP0_Status, env->CP0_Cause, env->CP0_Wired,
                    (uint32_t)env->CP0_EntryHi, (uint32_t)env->CP0_Index,
                    (uint32_t)env->CP0_Config0, (uint32_t)env->CP0_Config1);
            for (i = 0; i < 8; i++) {
                fprintf(stderr, "  tlb[%d] vpn=%08x mask=%08x asid=%02x "
                        "g=%d pfn0=%08x pfn1=%08x\n", i,
                        env->tlb->mmu.r4k.tlb[i].VPN,
                        env->tlb->mmu.r4k.tlb[i].PageMask,
                        env->tlb->mmu.r4k.tlb[i].ASID,
                        env->tlb->mmu.r4k.tlb[i].G,
                        (uint32_t)env->tlb->mmu.r4k.tlb[i].PFN[0],
                        (uint32_t)env->tlb->mmu.r4k.tlb[i].PFN[1]);
            }
        }
    }
    if (!s->halt_seen_running) {
        if (!cs->halted) {
            s->halt_seen_running = true;
        }
    } else if (parked) {
        if (++s->halt_ticks >= 4) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
    } else {
        s->halt_ticks = 0;
    }

    timer_mod(s->poweroff_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              (getenv("MT7621_ENTRYLOG") || getenv("MT7621_EXCLOG") ? 1 : 250));
}

/*
 * Kernel command line injection for bring-up.
 *
 * RouterBOOT assembles the kernel command line in a scratch buffer inside
 * its own RAM image and hands a pointer to the kernel, so there is nothing
 * to patch in the flash image.  The buffer only exists for the instant
 * between "loading kernel... OK" and the jump, and its physical address
 * moves with the bootloader build, so sweep the bootloader's RAM window and
 * append the extra options in place.
 *
 * The anchor also appears inside the printk ring buffer (as part of the
 * "Kernel command line: ..." line), so a NUL immediately before it is
 * required to stay on RouterBOOT's copy.
 */
#define MT7621_CMDLINE_BASE         0x01100000
#define MT7621_CMDLINE_SIZE         0x00400000
#define MT7621_CMDLINE_PAGE         0x1000
#define MT7621_CMDLINE_PAGES_TICK   16
#define MT7621_CMDLINE_LIFETIME_NS  (60 * 1000000000LL)

static bool mt7621_cmdline_patch(MT7621State *s, uint32_t phys)
{
    char buf[1024];
    char tmp[1024];
    size_t len, extra = strlen(s->cmdline_extra);
    size_t i;

    if (address_space_read(&address_space_memory, phys,
                           MEMTXATTRS_UNSPECIFIED, buf, sizeof(buf))) {
        return false;
    }
    buf[sizeof(buf) - 1] = 0;
    len = strnlen(buf, sizeof(buf) - 1);
    if (strstr(buf, s->cmdline_extra)) {
        return true;
    }
    if (len + extra + 3 > sizeof(buf)) {
        return false;
    }
    /* Only extend into padding; the buffer may be followed by live data. */
    for (i = len; i <= len + extra + 1; i++) {
        if (buf[i] != 0) {
            return false;
        }
    }
    snprintf(tmp, sizeof(tmp), "%s %s", buf, s->cmdline_extra);
    if (address_space_write(&address_space_memory, phys,
                            MEMTXATTRS_UNSPECIFIED, tmp, strlen(tmp) + 1)) {
        return false;
    }
    s->cmdline_patches++;
    if (s->cmdline_patches <= 10) {
        fprintf(stderr, "mt7621: cmdline #%u at tick %u: '%s'\n",
                s->cmdline_patches, s->cmdline_ticks, tmp);
    }
    return true;
}

static void mt7621_cmdline_poll(void *opaque)
{
    MT7621State *s = opaque;
    static const char anchor[] = "root=/dev/ram0 bootimage=1";
    uint8_t page[MT7621_CMDLINE_PAGE + 64];
    int n;

    s->cmdline_ticks++;

    if (s->cpu && !s->cmdline_kernel_seen) {
        uint32_t pc = s->cpu->env.active_tc.PC;

        if (pc >= 0x80100000 && pc < 0x81000000) {
            s->cmdline_kernel_seen = true;
            if (getenv("MT7621_TRACE")) {
                fprintf(stderr, "mt7621: kernel PC %08x at tick %u "
                        "(patches %u)\n", pc, s->cmdline_ticks,
                        s->cmdline_patches);
            }
        }
    }

    if (getenv("MT7621_TRACE") && !(s->cmdline_ticks % 10000)) {
        fprintf(stderr, "mt7621: cmdline poll #%u phys=%08x patches=%u\n",
                s->cmdline_ticks, s->cmdline_phys, s->cmdline_patches);
    }

    if (s->cmdline_phys) {
        /*
         * Once the buffer address is known, watch it closely: RouterBOOT
         * writes the string and jumps within a millisecond or so, and the
         * kernel parses it almost immediately after entry.
         */
        int r = address_space_read(&address_space_memory, s->cmdline_phys,
                                   MEMTXATTRS_UNSPECIFIED, page, 1024);
        if (getenv("MT7621_TRACE") && page[0] &&
            !(s->cmdline_ticks % 20000)) {
            fprintf(stderr, "mt7621: cmdline peek #%u r=%d '%c%c%c%c%c%c'\n",
                    s->cmdline_ticks, r, page[0], page[1], page[2],
                    page[3], page[4], page[5]);
        }
        if (!memcmp(page, anchor, sizeof(anchor) - 1)) {
            mt7621_cmdline_patch(s, s->cmdline_phys);
        }
    } else {
        /* Address unknown: sweep the bootloader's RAM window. */
        for (n = 0; n < MT7621_CMDLINE_PAGES_TICK; n++) {
            uint32_t off = s->cmdline_cursor;
            uint32_t phys = MT7621_CMDLINE_BASE + off;
            uint8_t *hit;

            if (address_space_read(&address_space_memory, phys,
                                   MEMTXATTRS_UNSPECIFIED, page,
                                   sizeof(page))) {
                break;
            }
            s->cmdline_cursor += MT7621_CMDLINE_PAGE;
            if (s->cmdline_cursor + sizeof(page) >= MT7621_CMDLINE_SIZE) {
                s->cmdline_cursor = 0;
            }

            hit = memmem(page, MT7621_CMDLINE_PAGE, anchor,
                         sizeof(anchor) - 1);
            if (!hit || hit == page || hit[-1] != 0) {
                continue;
            }
            s->cmdline_phys = phys + (hit - page);
            if (getenv("MT7621_TRACE")) {
                fprintf(stderr, "mt7621: cmdline buffer at phys %08x\n",
                        s->cmdline_phys);
            }
            mt7621_cmdline_patch(s, s->cmdline_phys);
            break;
        }
    }

    if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->cmdline_deadline_ns) {
        timer_mod(s->cmdline_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (s->cmdline_phys ? 100000 : 2000000));
    }
}

static void mt7621_reset(void *opaque)
{
    MT7621State *s = opaque;
    CPUMIPSState *env = &s->cpu->env;

    if (getenv("MT7621_RSTLOG")) {
        fprintf(stderr, "mt7621: machine reset, cpu pc=%08x ra=%08x\n",
                env->active_tc.PC, env->active_tc.gpr[31]);
    }

    /*
     * The real 1004Kc advertises ULRI + CMGCR, but with them visible this
     * model's OpenWrt guests take the MIPS CPS path and die before printing
     * anything (openwrt 5.15: zero bytes), while RouterOS 3.3.5 is unaffected.
     * Default to the pre-CPS view and let MT7621_CMGCR=1 (or MT7621_CFG3) opt
     * in - MikroTik's 5.6.3 netinstall kernel needs it to find the CM/GIC.
     */
    /*
     * ULRI + CMGCR are part of this part's Config3 and are now on by default:
     * the netinstall kernel needs the CM to find the GIC, and the LG board
     * boots identically either way (its earlier "dies as soon as CPS is
     * visible" was fixed with the SMP model).  MT7621_NO_CMGCR=1 restores the
     * old pre-CPS view for A/B testing.
     */
    if (mt7621_flag("MT7621_NO_CMGCR") && !getenv("MT7621_CFG3")) {
        env->CP0_Config3 &= ~((1u << 13) | (1u << 29));
    }
    if (getenv("MT7621_CFG3")) {
        env->CP0_Config3 = strtoul(getenv("MT7621_CFG3"), NULL, 0);
    }

    if (s->wdt_timer) {
        timer_del(s->wdt_timer);
    }

    if (s->cfg1_cache_fields) {
        env->CP0_Config1 = (env->CP0_Config1 & ~0xffffu) |
                           s->cfg1_cache_fields;
    }
    /*
     * Debug knob only (the real part is Dl=4, i.e. 32-byte lines): override
     * Config1.Dl (bits 12:10) to A/B the d-cache line size without rebuilding.
     * Breed's rt2880-eth init fails at Dl=4 and works at Dl=2, so this is the
     * handle used while the d-cache model itself is being understood.
     */
    if (s->board == MT7621_BOARD_LG_GAPM7200) {
        env->CP0_Config1 = (env->CP0_Config1 & ~(7u << 10)) | (2u << 10);
    }
    {
        const char *dl = getenv("MT7621_DL");

        if (dl) {
            env->CP0_Config1 = (env->CP0_Config1 & ~(7u << 10)) |
                               ((strtoul(dl, NULL, 0) & 7) << 10);
        }
    }

    /* Everything the guest programmed is gone after a real reset. */
    mt7621_eth_dev_reset(s);
    memset(s->nfi_regs, 0, sizeof(s->nfi_regs));
    memset(s->spi_regs, 0, sizeof(s->spi_regs));
    s->spi_read_mode = false;
    s->spi_addr = 0;
    s->nfi_fifo_len = 0;
    s->nfi_fifo_pos = 0;
    s->nfi_col = 0;
    s->nfi_row = 0;
    s->nfi_served = 0;
    s->nfi_last_cmd = 0;
    s->nfi_addr_len = 0;
    s->nfi_addr_pending = 0;
    s->nfi_wbuf_len = 0;
    s->nfi_writing = false;
    mt7621_sysctl_defaults(s);
    mt7621_dramc_defaults(s);
    mt7621_cm_defaults(s);
    memset(s->systick_regs, 0, sizeof(s->systick_regs));

    /*
     * Linux scribbled over the RAM the bootloader lived in, so the BootROM
     * has to load it again before the CPU is released.
     */
    if (s->image_file && (!s->spi_nor || s->elf_kernel)) {
        mt7621_load_image(s, s->image_file);
    }

    /* Malta does the same when a -kernel image is supplied. */
    env->CP0_Status &= ~(1 << 2); /* CP0St_ERL */
    env->active_tc.PC = s->entry;
    if (getenv("MT7621_ENTRY_DIRTY") && s->elf_kernel) {
        /* reproduce the CP0/TLB state RouterBOOT leaves at the hand-over */
        int t;

        env->CP0_Status |= (1 << CP0St_ERL) | (1 << 10);
        env->CP0_Config0 = 0x80000485;
        env->CP0_EntryHi = 0x800fc000;
        for (t = 0; t < 8; t++) {
            env->tlb->mmu.r4k.tlb[t].VPN = 0x800fc000 - t * 0x4000;
            env->tlb->mmu.r4k.tlb[t].PageMask = 0;
            env->tlb->mmu.r4k.tlb[t].PFN[0] = 0;
            env->tlb->mmu.r4k.tlb[t].PFN[1] = 0;
        }
    }
    if (s->elf_kernel) {
        uint32_t a0 = s->elf_argc, a1 = s->elf_argv, a2 = 0, a3 = 0;
        const char *ov;

        /*
         * Some bootloaders (MikroTik's, for the RouterOS kernel) use the
         * VxWorks-style convention where a0 == -2 marks a1 as the boot
         * argument block; the kernel only records that pointer in that case.
         */
        if ((ov = getenv("MT7621_ELF_A0")) != NULL) {
            a0 = strtoul(ov, NULL, 0);
        }
        if ((ov = getenv("MT7621_ELF_A1")) != NULL) {
            a1 = strtoul(ov, NULL, 0);
        }
        if ((ov = getenv("MT7621_ELF_A2")) != NULL) {
            a2 = strtoul(ov, NULL, 0);
        }
        if ((ov = getenv("MT7621_ELF_A3")) != NULL) {
            a3 = strtoul(ov, NULL, 0);
        }
        env->active_tc.gpr[4] = a0;
        env->active_tc.gpr[5] = a1;
        env->active_tc.gpr[6] = a2;
        env->active_tc.gpr[7] = a3;
    }

    /*
     * The MT7621 BootROM hands over with the 50 kHz systick already running
     * and its compare interrupt enabled; Breed only hooks GIC IRQ 5 and
     * relies on that.  Reproduce the hand-over state.
     */
    mt7621_systick_boot_handover(s);

}

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

static void mt7621_crypto_reset(MT7621State *s)
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

static void mt7621_machine_init(MachineState *machine)
{
    MT7621State *s = MT7621_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    qemu_irq uart_irq;
    uint64_t ram_size = machine->ram_size;
    int i;

    mt7621_trace = getenv("MT7621_TRACE") != NULL;
    s->board = MT7621_BOARD_LG_GAPM7200;
    /* resolves the preset (and its chip_mode) from -M board= or the image */
    mt7621_board_apply(s, machine->kernel_filename);

    if (ram_size > 512 * MiB) {
        error_report("mt7621: at most 512 MiB of RAM is supported");
        exit(1);
    }

    /* CPU */
    s->cpuclk = clock_new(OBJECT(s), "cpu-refclk");
    clock_set_hz(s->cpuclk, 880 * 1000 * 1000);

    s->ncpus = machine->smp.cpus;
    if (s->ncpus < 1) {
        s->ncpus = 1;
    }
    if (s->ncpus > MT7621_MAX_CPUS) {
        s->ncpus = MT7621_MAX_CPUS;
    }
    for (i = 0; i < (int)s->ncpus; i++) {
        MIPSCPU *cpu = mips_cpu_create_with_clock(machine->cpu_type,
                                                  s->cpuclk, false);

        s->cpus[i] = cpu;
        cpu_mips_irq_init_cpu(cpu);
        cpu_mips_clock_init(cpu);
        qemu_register_reset(mt7621_cpu_reset, cpu);
        if (i) {
            CPU(cpu)->halted = 1;
        }
    }
    s->cpu = s->cpus[0];

    if (getenv("MT7621_CFG1")) {
        /*
         * Only the cache descriptor bits (Ds/Dl/Da at 15:7).  Masking the
         * whole low half would also clobber the feature bits, in particular
         * Config1.FP, and hide the FPU from the guest.
         */
        s->cfg1_cache_fields = strtoul(getenv("MT7621_CFG1"), NULL, 0) & 0xff80;
    }
    if (s->cfg1_cache_fields) {
        s->cpu->env.CP0_Config1 = (s->cpu->env.CP0_Config1 & ~0xff80u) |
                                  s->cfg1_cache_fields;
    }
    qemu_register_reset(mt7621_reset, s);

    /*
     * The GIC shared counter runs off the CPU clock on this SoC.  RouterBOOT
     * reports the real rate (875 MHz on the RB750Gr3) and OpenWrt programs its
     * clock events against it, so the model has to tick at the same rate or
     * every guest timeout is off by the ratio.
     */
    /*
     * The compressed-kernel loader sits at its load address and is then
     * overwritten by its own decompression output; on real MIPS it survives
     * only because its instructions are still in the I-cache (the kernel even
     * says "FIXME: should we flush cache here?").  Shadow the megabyte the
     * RB750Gr3 netboot images load into (0x80b81000 / 0x80b91000) so that
     * instruction fetch keeps seeing the loader, and leave every other address
     * - including the whole NAND/RouterOS boot - on plain TCG semantics.
     */
    if (!getenv("MT7621_NO_ICACHE") && s->board == MT7621_BOARD_RB750GR3) {
        uint64_t lo = 0x80b00000ULL, hi = 0x80c00000ULL;
        const char *rng = getenv("MT7621_ICACHE_RANGE");

        if (rng) {
            unsigned long long rlo = 0, rsize = 0;

            if (sscanf(rng, "%lli+%lli", &rlo, &rsize) == 2 && rsize) {
                lo = rlo;
                hi = rlo + rsize;
            }
        }
        icache_shadow_enable(lo, hi);
    }

    if (getenv("MT7621_GIC_FREQ")) {
        mips_gictimer_set_freq(strtoul(getenv("MT7621_GIC_FREQ"), NULL, 0));
    } else {
        /*
         * Run the GIC shared counter at the real device's CPU clock (875 MHz,
         * the rate RouterBOOT reports and OpenWrt programs its clockevents
         * against) for the whole machine, instead of the 25 MHz reference the
         * old hardcoded constant used.
         */
        mips_gictimer_set_freq(875 * 1000 * 1000);
    }


    if (getenv("MT7621_RAMWIN")) {
        const char *spec = getenv("MT7621_RAMWIN");
        long first = strtol(spec, NULL, 0);

        s->ramwin_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mt7621_ramwin, s);
        timer_mod(s->ramwin_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + first);
    }

    if (getenv("MT7621_RAMDUMP")) {
        const char *spec = getenv("MT7621_RAMDUMP");
        long ms = strtol(spec, NULL, 0);

        s->ramdump_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mt7621_ramdump, s);
        timer_mod(s->ramdump_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + ms);
    }

    if (getenv("MT7621_CP_SIG")) {
        s->cp_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mt7621_cp_tick, s);
        timer_mod(s->cp_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 8000);
    }

    s->poweroff_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                     mt7621_poweroff_poll, s);
    timer_mod(s->poweroff_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              (getenv("MT7621_ENTRYLOG") || getenv("MT7621_EXCLOG") ? 1 : 250));

    if (getenv("MT7621_CMDLINE")) {
        s->cmdline_extra = g_strdup(getenv("MT7621_CMDLINE"));
        /*
         * RouterBOOT keeps the assembled command line in a scratch buffer
         * inside its RAM image.  Its address is build specific; the RB750Gr3
         * booter 3.35 uses this one.  Without a seed the poller falls back
         * to sweeping the whole bootloader window.
         */
        if (getenv("MT7621_CMDLINE_PHYS")) {
            s->cmdline_phys = strtoul(getenv("MT7621_CMDLINE_PHYS"), NULL, 0);
        } else if (s->board == MT7621_BOARD_RB750GR3) {
            s->cmdline_phys = 0x01200c80;
        }
        s->cmdline_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        mt7621_cmdline_poll, s);
        s->cmdline_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                 MT7621_CMDLINE_LIFETIME_NS;
        timer_mod(s->cmdline_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000000LL);
    }

    /* DDR */
    memory_region_init_ram(&s->ram, NULL, "mt7621.ddr", ram_size,
                           &error_fatal);
    memory_region_add_subregion(sysmem, MT7621_RAM_BASE, &s->ram);

    /*
     * The MT7621 DRAM controller only decodes the address bits that the
     * populated chips need, so accesses above the installed size wrap around
     * and alias onto low memory.  Breed detects the DDR size exactly this
     * way (write a pattern at 0, write 0 at 1<<n, read back 0), so the
     * aliases have to exist for it to measure less than 512 MiB.
     */
    if (ram_size & (ram_size - 1)) {
        error_report("mt7621: RAM size must be a power of two");
        exit(1);
    }
    s->ram_alias_count = MIN(MT7621_KSEG1_WINDOW / ram_size - 1,
                             ARRAY_SIZE(s->ram_alias));
    for (i = 0; i < s->ram_alias_count; i++) {
        memory_region_init_alias(&s->ram_alias[i], NULL, "mt7621.ddr-alias",
                                 &s->ram, 0, ram_size);
        memory_region_add_subregion(sysmem, (i + 1) * ram_size,
                                    &s->ram_alias[i]);
    }

    /*
     * The SoC interrupt controller is a MIPS GIC at 0x1fbc0000 (not the
     * RT305x-style INTC of older Ralink parts).  Directing a shared
     * interrupt to CPU line 0 gives Cause IP2, which is the line both
     * Breed and Linux expect for it.
     *
     * Map it after the DDR aliases: those cover 1 GiB..2 GiB, which
     * contains 0x1fbc0000, and a later subregion wins.
     */
    s->gic = qdev_new(TYPE_MIPS_GIC);
    qdev_prop_set_uint32(s->gic, "num-vp", s->ncpus);
    qdev_prop_set_uint32(s->gic, "num-irq", MT7621_GIC_NUM_IRQ);
    /*
     * The SoC's GIC has a 64-bit shared counter: Linux on the real board
     * reports "clocksource: GIC: mask 0xffffffffffffffff", which it derives
     * from GIC_SH_CONFIG.COUNTERBITS.  Keep the generic model's 32-bit
     * default for everything else.
     */
    {
        /*
         * Hardware is 64-bit on the MT7621; the knob exists to A/B other
         * guests (RouterOS) that may not cope with the wide counter.
         */
        const char *cb = getenv("MT7621_GIC_COUNTER_BITS");

        qdev_prop_set_uint32(s->gic, "counter-bits",
                             cb ? (uint32_t)atoi(cb) : 8);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->gic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gic), 0, MT7621_GIC_BASE);

    /*
     * Reset notifiers run in registration order, and the GIC registers its
     * own reset when the device is created, so register ours afterwards to
     * keep the SoC default mapping.
     */
    qemu_register_reset(mt7621_gic_reset, s);

    /*
     * Ethernet.  The frame engine raises GIC shared interrupt 3 and its
     * DMA rings move frames between guest memory and a QEMU net backend,
     * so a tap device on the host is bridged at layer 2.
     */
    mt7621_eth_init(s);

    /* palmbus */
    memory_region_init(&s->periph, NULL, "mt7621.periph",
                       MT7621_PERIPH_SIZE);

    mt7621_sysctl_defaults(s);
    memory_region_init_io(&s->sysctl, NULL, &mt7621_reg_ops, s->sysctl_regs,
                          "mt7621.sysctl", 0x1000);
    memory_region_init_io(&s->rstctl, NULL, &mt7621_rstctl_ops, s,
                          "mt7621.rstctl", 4);
    memory_region_add_subregion(&s->sysctl, SYS_RSTCTL, &s->rstctl);
    mt7621_wdt_init(s);
    mt7621_spi_init(s);
    memory_region_add_subregion(&s->periph, MT7621_SYSCTL_OFF, &s->sysctl);

    mt7621_systick_init(s);

    mt7621_dramc_defaults(s);
    memory_region_init_io(&s->dramc, NULL, &mt7621_reg_ops, s->dramc_regs,
                          "mt7621.dramc", 0x1000);
    memory_region_add_subregion(&s->periph, MT7621_DRAMC_OFF, &s->dramc);

    mt7621_nfi_init(s);



    mt7621_cm_init(s);

    uart_irq = qdev_get_gpio_in(s->gic, MT7621_UART0_IRQ);
    serial_mm_init(&s->periph, MT7621_UART0_OFF, 2, uart_irq,
                   MT7621_UART_BAUD_BASE, serial_hd(0),
                   DEVICE_LITTLE_ENDIAN);
    /*
     * The second UART gets a TX-only stand-in rather than a second chardev
     * frontend (qemu_chr_fe_init refuses to share 'ser0').  RouterOS can move
     * its console to 0x1e000e00, and when that output has nowhere to go the
     * kernel looks silent.  Bytes written to its THR are mirrored into the
     * host log with a "[uart1] " prefix.  MT7621_UART1=0 disables the mirror.
     */
    if (mt7621_flag("MT7621_UART1")) {
        mt7621_uart1_init(s);
    }

    mt7621_crypto_reset(s);
    memory_region_init_io(&s->crypto, NULL, &mt7621_crypto_ops, s,
                          "mt7621.crypto", MT7621_CRYPTO_SIZE);
    memory_region_add_subregion(&s->periph, MT7621_CRYPTO_OFF, &s->crypto);

    memory_region_add_subregion(sysmem, MT7621_PERIPH_BASE, &s->periph);

    {
        const char *e = getenv("MT7621_ENTRY");
        const char *l = getenv("MT7621_LOAD");

        if (e) {
            s->entry = strtoul(e, NULL, 0);
        }
        if (l) {
            s->load_addr = strtoul(l, NULL, 0);
        }
    }

    /* Boot image */
    if (machine->kernel_filename) {
        uint8_t magic[4] = { 0 };
        int mfd = open(machine->kernel_filename, O_RDONLY | O_BINARY);

        s->image_file = g_strdup(machine->kernel_filename);
        if (mfd >= 0) {
            if (read(mfd, magic, 4) != 4) {
                magic[0] = 0;
            }
            close(mfd);
        }
        /*
         * A raw ELF here is a kernel (e.g. an OpenWrt initramfs image), not a
         * flash dump: load it like a bootloader would and run it.  Handy for
         * separating machine-model problems from bootloader ones.
         */
        if (ldl_be_p(magic) == 0x7f454c46) {
            mt7621_load_image(s, machine->kernel_filename);
        } else if (s->board == MT7621_BOARD_RB750GR3) {
            mt7621_nor_load(s, machine->kernel_filename);
        } else {
            mt7621_nand_load(s, machine->kernel_filename);
            mt7621_nand_attach(s);
            mt7621_load_image(s, machine->kernel_filename);
        }
    } else {
        error_report("mt7621: no boot image (-kernel) supplied");
        exit(1);
    }
}


static void mt7621_set_board(Object *obj, const char *value,
                             Error **errp)
{
    MT7621State *s = MT7621_MACHINE(obj);

    g_free(s->board_name);
    s->board_name = g_strdup(value);
}

/*
 * Default RDUID value: a fixed, plausible Winbond unique ID.  Boards that
 * care about matching a real chip's identity override it with
 * -M mt7621,spi-uid=0x...
 */
#define MT7621_SPI_UID_DEFAULT 0xe4c1a75b3d9206f8ULL

static void mt7621_machine_instance_init(Object *obj)
{
    MT7621State *s = MT7621_MACHINE(obj);

    s->spi_uid = MT7621_SPI_UID_DEFAULT;
}

static void mt7621_set_spi_uid(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    MT7621State *s = MT7621_MACHINE(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    s->spi_uid = value;
}

static void mt7621_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "board", NULL, mt7621_set_board);
    object_class_property_set_description(oc, "board",
        "board preset: lg-gapm7200 (parallel NAND) or rb750gr3 (SPI NOR)");

    object_class_property_add(oc, "spi-uid", "uint64", NULL,
                              mt7621_set_spi_uid, NULL, NULL);
    object_class_property_set_description(oc, "spi-uid",
        "Winbond SPI NOR 64-bit unique ID reported by command 0x4b");

    mc->desc = "MediaTek MT7621 (MIPS 1004Kc)";
    mc->init = mt7621_machine_init;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("1004Kc");
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "mt7621.ram";
    mc->max_cpus = MT7621_MAX_CPUS;
    mc->default_cpus = 4;
}

static const TypeInfo mt7621_machine_type = {
    .name = TYPE_MT7621_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(MT7621State),
    .instance_init = mt7621_machine_instance_init,
    .class_init = mt7621_machine_class_init,
};

static void mt7621_machine_register_types(void)
{
    type_register_static(&mt7621_machine_type);
}

type_init(mt7621_machine_register_types);
