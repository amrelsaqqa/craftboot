/*
 * MediaTek MT7621 (MIPS 1004Kc) machine: the interface between the machine
 * and its controller modules.
 *
 * Everything a module may legitimately touch lives here: the state, the
 * sizes the state is built from, and the board enum.  Register offsets and
 * per-block state belong to the module that owns the block (see
 * docs/machines.md), not in this file.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MIPS_MT7621_H
#define HW_MIPS_MT7621_H

#include "qemu/osdep.h"
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

#define TYPE_MT7621_MACHINE MACHINE_TYPE_NAME("mt7621")
OBJECT_DECLARE_SIMPLE_TYPE(MT7621State, MT7621_MACHINE)

typedef enum MT7621Board {
    MT7621_BOARD_AUTO = 0,
    MT7621_BOARD_LG_GAPM7200,     /* 256 MiB DDR, parallel NAND */
    MT7621_BOARD_RB750GR3,        /* 256 MiB DDR, 16 MiB SPI NOR */
} MT7621Board;

/* sizes the machine state is built from */
#define MT7621_NAND_PAGE_SIZE   2048
#define MT7621_NAND_OOB_SIZE    64
#define MT7621_ETH_SIZE         0x00020000
#define MT7621_SPI_FLASH_BASE   0x1fc00000
#define MT7621_SPI_WINDOW       0x00400000
#define MT7621_CM_SIZE          0x00010000
#define MT7621_GIC_BASE         0x1fbc0000
#define MT7621_MAX_CPUS         4
#define SYSCTL_WORDS            (0x1000 / 4)
#define DRAMC_WORDS             (0x1000 / 4)
#define NFI_WORDS               (0x1000 / 4)

/* One L1D line: tag, LRU stamp and the resident copy of the bytes. */
typedef struct MT7621DLine {
    uint64_t tag;
    bool valid;
    uint64_t lru;
    uint8_t data[64];           /* MT7621_DCACHE_MAXLINE */
} MT7621DLine;

struct MT7621State {
    MachineState parent;

    MIPSCPU *cpu;               /* CPU0: keeps the existing tracing paths */
    MIPSCPU *cpus[MT7621_MAX_CPUS];
    unsigned ncpus;
    Clock *cpuclk;
    /* Latched by mt7621_cm_write() so a CPC reset can start that core. */
    uint32_t co_other;          /* GCR_CL_OTHER: core selector (CM < 3) */
    uint32_t cpc_other;         /* CPC_CL_OTHER: core selector for the CPC */
    uint32_t co_reset_base;
    DeviceState *gic;

    MemoryRegion ram;
    MemoryRegion periph;
    MemoryRegion sysctl;
    MemoryRegion rstctl;
    MemoryRegion wdt;
    MemoryRegion systick;
    MemoryRegion uart1;            /* TX-only mirror of uartlite@e00 */
    MemoryRegion crypto;           /* EIP-93 window at crypto@1e004000 */
    uint32_t crypto_regs[0x800 / 4];   /* EIP-93 register file */
    MemoryRegion dramc;
    MemoryRegion nfi;
    MemoryRegion sram;            /* unused, kept for layout stability */
    uint32_t eth_regs[MT7621_ETH_SIZE / 4];
    /* MT7530 indirect register file (page/lo/hi via MDIO addr 0x1f) */
    uint32_t sw_regs[0x400 * 16];
    uint16_t sw_page;
    uint16_t sw_r;

    NICState *nic;
    NICConf nic_conf;
    MemReentrancyGuard nic_guard;
    qemu_irq eth_irq;
    uint32_t rx_drx_idx;
    uint32_t ptx_idx[4];        /* PDMA transmit ring consumer index */
    int64_t phy_link_ns;        /* when the tapped PHY reports link up */
    uint16_t phy_regs[32][32];  /* MDIO register file per address */

    /* optional writable NAND image (-drive if=none,id=nand) */
    BlockBackend *nand_blk;
    int nand_fd;
    bool nand_wr_error;
    QEMUTimer *wdt_timer;
    bool wdt_rst_cause;
    unsigned eth_port;          /* DSA user port exposed to the backend */
    uint32_t qtx_dtx;           /* forward DMA pointer (next to send) */
    uint32_t qtx_base;          /* start of the TX descriptor ring */
    bool qtx_en;                /* QDMA TX DMA enable */
    MemoryRegion cm;
    MemoryRegion eth;
    MemoryRegion fe_sram;       /* 0x1e108000: FE/PSE SRAM used before DDR */
    MemoryRegion ram_alias[8];
    int ram_alias_count;

    uint32_t sysctl_regs[SYSCTL_WORDS];
    uint32_t systick_regs[4];
    uint32_t dramc_regs[DRAMC_WORDS];
    uint32_t nfi_regs[NFI_WORDS];

    /* NAND flash contents and the current PIO transfer */
    uint8_t *nand;
    uint64_t nand_size;
    uint32_t nand_stride;       /* bytes per page in the dump (2048/2112) */
    uint32_t nand_pages;        /* pages in the dump */
    uint8_t nfi_fifo[MT7621_NAND_PAGE_SIZE + MT7621_NAND_OOB_SIZE];
    uint32_t nfi_fifo_len;
    uint32_t nfi_fifo_pos;
    uint32_t nfi_col;
    uint32_t nfi_row;
    uint32_t nfi_served;
    uint32_t nfi_last_cmd;
    /* Address bytes the kernel driver sends one at a time via COLADDR. */
    uint8_t nfi_addr[8];
    uint32_t nfi_addr_len;
    uint32_t nfi_addr_pending;

    /* page program buffer */
    uint8_t nfi_wbuf[MT7621_NAND_PAGE_SIZE + MT7621_NAND_OOB_SIZE];
    uint32_t nfi_wbuf_len;
    bool nfi_writing;
    uint32_t cm_regs[MT7621_CM_SIZE / 4];

    /* systick state: count = base + elapsed, anchored at base_ns */
    QEMUTimer *poweroff_timer;  /* detects a parked CPU (poweroff/halt) */
    QEMUTimer *nitest_timer;    /* MT7621_NITEST: synthetic RX probes */
    QEMUTimer *cp_timer;        /* MT7621_CP_*: guest code patcher */
    QEMUTimer *ramdump_timer;   /* MT7621_RAMDUMP=<ms>:<path> */
    QEMUTimer *ramwin_timer;    /* MT7621_RAMWIN=first:step:count:start:len:prefix */
    unsigned ramwin_n;
    bool cp_done;
    unsigned halt_ticks;
    bool halt_seen_running;

    /*
     * Kernel command line injection for bring-up.  RouterBOOT assembles its
     * command line in a scratch buffer and hands the pointer to the kernel;
     * MT7621_CMDLINE appends options to that buffer before the kernel gets
     * to parse it (see mt7621_cmdline_poll()).
     */
    QEMUTimer *cmdline_timer;
    QEMUTimer *flash_save_timer;   /* MT7621_FLASH_SAVE: periodic flash dump */
    char *cmdline_extra;
    uint32_t cmdline_cursor;
    uint32_t cmdline_phys;      /* discovered buffer address, 0 = unknown */
    unsigned cmdline_ticks;
    bool cmdline_kernel_seen;
    unsigned cmdline_patches;
    int64_t cmdline_deadline_ns;

    QEMUTimer *systick_timer;
    QEMUTimer *systick_pulse_off;
    qemu_irq systick_irq;
    int64_t systick_base_ns;
    uint32_t systick_base_count;
    bool systick_pending;

    /* board preset */
    char *board_name;
    MT7621Board board;
    uint32_t chip_mode;         /* SYSCFG0[3:0] boot flash select */

    /* SPI NOR image mapped at 0x1fc00000 */
    uint8_t *flash;
    uint64_t flash_size;
    MemoryRegion flash_mr;
    bool spi_nor;
    /*
     * L1 data cache model (MT7621_DCACHE=1): a real 32 KiB 4-way write-through
     * cache, sized from Config1, that serves every cacheable data access.
     */
    struct {
        bool enabled;
        uint64_t size;          /* DRAM size */
        uint64_t alias_limit;   /* end of DRAM plus its wraparound aliases */
        void *ram;              /* host pointer to DRAM */
        MT7621DLine *lines;     /* nsets * MT7621_DCACHE_WAYS entries */
        unsigned line_size;
        unsigned line_shift;
        unsigned nsets;
        uint64_t clock;         /* LRU stamp source */
        QEMUTimer *timer;       /* diagnostics only */
    } dcache;
    /* NAND die identity the NFI answers with (-M mt7621,nand-id=...) */
    uint8_t nand_id[8];
    bool nand_id_set;           /* property overrides the board's part */
    bool elf_kernel;            /* -kernel is an ELF to load, not a dump */
    uint32_t elf_argc;
    uint32_t elf_argv;
    bool entry_logged;
    bool rom_logged;
#define PC_HIST 256
    uint32_t pc_hist[PC_HIST];
    unsigned pc_hist_idx;

    /*
     * SPI controller at 0x1e000b00.  The MT7621 maps the NOR chip into a
     * 4 MiB window at 0x1fc00000 and also exposes the controller's PIO
     * FIFO; RouterBOOT and Linux both use the latter to read the rest of
     * the chip.
     */
    MemoryRegion spi;
    uint32_t spi_regs[0x100 / 4];
    uint8_t spi_rx[32];
    bool spi_read_mode;
    uint32_t spi_addr;
    /*
     * Winbond RDUID (0x4B) value.  RouterOS derives its software-id (and
     * therefore validates its license key) from this, so it has to be
     * stable and configurable per board: -M mt7621,spi-uid=0x...
     */
    uint64_t spi_uid;

    /* boot image */
    uint32_t load_addr;
    uint32_t entry;
    bool uimage;
    char *image_file;           /* boot image, reloaded on reset */

    /*
     * Bring-up knob: L1 cache geometry reported in Config1 bits 15:0
     * (IS/IL/IA/DS/DL/DA).  Linux picks its cache flush strategy from
     * these fields, so a value that lies about the geometry can make the
     * guest call a flush routine pointer that was never installed.
     */
    uint32_t cfg1_cache_fields;
};

/* shared machine state the modules read */
extern bool mt7621_trace;
bool mt7621_flag(const char *name);

/* modules (each one is modules/<block>.c) */
void mt7621_uart1_init(MT7621State *s);
void mt7621_wdt_init(MT7621State *s);
void mt7621_systick_init(MT7621State *s);
void mt7621_board_apply(MT7621State *s, const char *filename);
void mt7621_spi_init(MT7621State *s);
void mt7621_nfi_init(MT7621State *s);
void mt7621_nand_load(MT7621State *s, const char *filename);
void mt7621_eth_init(MT7621State *s);
void mt7621_eth_dev_reset(MT7621State *s);
void mt7621_cm_init(MT7621State *s);
void mt7621_cm_defaults(MT7621State *s);
void mt7621_cpu_reset(void *opaque);
void mt7621_crypto_init(MT7621State *s);
void mt7621_dump_init(MT7621State *s);
void mt7621_load_image(MT7621State *s, const char *filename);
void mt7621_nor_load(MT7621State *s, const char *filename);
ssize_t mt7621_flash_pread(MT7621State *s, int fd, uint64_t off,
                           void *buf, size_t len);
void mt7621_nand_attach(MT7621State *s);
/* write-all helper for the persistent NAND image */
int mt7621_pwrite_all(int fd, const void *buf, size_t len, off_t off);
/* the CM/CPC reset path reprograms the systick compare */
void mt7621_systick_set_pending(MT7621State *s, bool pending);
void mt7621_systick_rearm(MT7621State *s);
void mt7621_systick_boot_handover(MT7621State *s);
extern const MemoryRegionOps mt7621_reg_ops;

#endif /* HW_MIPS_MT7621_H */

/* modules/dcache.c */
void mt7621_dcache_init(MT7621State *s, uint64_t ram_size);
void mt7621_dcache_reset(MT7621State *s);
void mt7621_dcache_cache_op(CPUState *cs, vaddr addr, unsigned type,
                            unsigned op);
