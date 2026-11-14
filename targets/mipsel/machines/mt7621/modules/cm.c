/*
 * MediaTek MT7621 -- the MIPS Coherence Manager (GCR at 0x1fbf8000) and the
 * Cluster Power Controller (CPC at 0x1fbf0000): the register windows, the
 * core-other redirect that starts a secondary core, and the per-CPU reset
 * hook.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "mt7621.h"

#define MT7621_CM_BASE          0x1fbf0000
#define MT7621_GCR_OFF          0x00008000

/* GCR register offsets */
#define GCR_CONFIG              0x0000
#define GCR_BASE                0x0008
#define GCR_REV                 0x0030
#define GCR_GIC_BASE            0x0080
#define GCR_CPC_BASE            0x0088
#define GCR_GIC_STATUS          0x00d0
#define GCR_CPC_STATUS          0x00f0
#define GCR_CL_OTHER            0x2018
#define GCR_CL_ID               0x2028
#define GCR_CO_RESET_RELEASE    0x4000

/* CPC register offsets (relative to the CPC base) */
#define CPC_CL_OTHER            0x0010
#define CPC_CL_BASE             0x2010
#define CPC_CO_CMD              0x4000

/* The CM/CPC block is 64 KiB, so it needs a wider index than mt7621_reg_ops. */
static uint64_t mt7621_cm_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    uint32_t *regs = s->cm_regs;
    uint32_t off = addr & (MT7621_CM_SIZE - 1);
    uint32_t val;

    /*
     * COCB+0x8 is aliased, and both aliases are polled during bring-up:
     *
     *   0x4008  CPC_CO_STAT_CONF - what Linux polls; CPC base is 0x1fbf0000
     *   0xc008  GCR_CO_COHERENCE - what RouterBOOT polls (GCR at 0x1fbf8000)
     *
     * Report the core as coherent (bit 0) and in sequence state U6
     * (bits 22:19 == 7), the "coherent execution" state cps_smp_setup() /
     * boot_core() waits for before it considers a secondary core started.
     */
    if (off == 0x4008 || off == MT7621_GCR_OFF + 0x4008) {
        val = 0x00380001;
    } else if (off == MT7621_GCR_OFF + 0x2028) {
        /*
         * GCR_CL_ID: the core-local block is per-core, so the reader sees
         * its own id.  mips_cps_core_entry uses this to find its entry in
         * mips_cps_core_bootcfg[]; returning 0 for everyone made every
         * secondary jump through cpu 0's (empty) VPE config, i.e. to PC 0.
         */
        val = current_cpu ? (uint32_t)current_cpu->cpu_index : 0;
    } else {
        val = regs[off >> 2];
    }

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: cm read  %08" HWADDR_PRIx
                      " -> %08x\n", addr, val);
    }
    /*
     * MT7621_CMLOG traces the registers the firmware/kernel actually use.
     * RouterBOOT takes a catastrophic path as soon as GCR_GIC_STATUS.EX is
     * set, so this tells us who read it and from where.
     */
    if (mt7621_flag("MT7621_CMLOG") &&
        (off == MT7621_GCR_OFF + GCR_GIC_STATUS ||
         off == MT7621_GCR_OFF + GCR_GIC_BASE ||
         off == MT7621_GCR_OFF + GCR_CONFIG)) {
        fprintf(stderr, "mt7621: cm read %04x pc=%08x -> %08x\n", off,
                s->cpu ? (uint32_t)s->cpu->env.active_tc.PC : 0, val);
    }
    return val;
}

/*
 * Start a secondary core the way the CM/CPC pair does it: CPU reset, then
 * fetch from GCR_Cx_RESET_BASE (the kernel puts CKSEG1ADDR(mips_cps_core_entry)
 * there) with BEV/ERL cleared, because cps-vec.S expects to come out of reset
 * in a normal privileged state.
 */
static void mt7621_start_core_work(CPUState *cs, run_on_cpu_data data)
{
    MT7621State *s = data.host_ptr;
    CPUMIPSState *env = cpu_env(cs);

    cpu_reset(cs);
    env->active_tc.PC = s->co_reset_base & 0xfffff000u;  /* keep KSEG1 */
    env->CP0_Status &= ~((1u << CP0St_BEV) | (1u << CP0St_ERL));
    /*
     * Mark the VPE enabled & activated, like firmware does on the real
     * 1004Kc.  mips_cpu_has_work() forces "no work" when mips_vpe_active()
     * is false, so without VPEConf0.VPA a halted secondary can never wake
     * on an interrupt (CPU1 sat in r4k_wait with IP7+IP2 pending forever).
     */
    env->CP0_VPEConf0 |= (1u << CP0VPEC0_VPA);
    env->active_tc.CP0_TCStatus |= (1u << CP0TCSt_A);
    env->active_tc.CP0_TCHalt &= ~1u;
    if (env->mvp) {
        env->mvp->CP0_MVPControl |= (1u << CP0MVPCo_EVP);
    }
    cs->halted = 0;
}

static void mt7621_start_core(MT7621State *s)
{
    /*
     * CM 2.x semantics: GCR_CL_OTHER (0xa018) selects the core for the
     * COCB redirect, CPC_CL_OTHER (0x2010) selects it for the CPC.  The
     * kernel writes both (mips_cm_lock_other / mips_cpc_lock_other); the
     * CPC one is the authoritative target for CPC_Cx_CMD_RESET.
     */
    unsigned core = (s->cpc_other >> 16) & 0xff;
    CPUState *cs;

    if (!core) {
        core = (s->co_other >> 16) & 0x1f;
    }
    if (core == 0 || core >= s->ncpus) {
        return;
    }
    if (mt7621_flag("MT7621_SMPLOG")) {
        fprintf(stderr, "mt7621: start core %u at %08x (cpc_other=%08x)\n",
                core, s->co_reset_base & 0xfffff000u, s->cpc_other);
    }
    cs = CPU(s->cpus[core]);
    /*
     * The target vCPU is halted; run the reset on its own thread so the
     * halt is cleared and the thread is kicked out of its wait.
     */
    async_safe_run_on_cpu(cs, mt7621_start_core_work,
                          RUN_ON_CPU_HOST_PTR(s));
}

void mt7621_cpu_reset(void *opaque)
{
    MIPSCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    if (cs->cpu_index != 0) {
        /* Secondary cores wait for the CPC to release them. */
        cs->halted = 1;
    }
}

static void mt7621_cm_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    MT7621State *s = opaque;
    uint32_t *regs = s->cm_regs;
    uint32_t off = addr & (MT7621_CM_SIZE - 1);

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: cm write %08" HWADDR_PRIx
                      " <- %08" PRIx64 "\n", addr, val);
    }
    regs[off >> 2] = val;

    if (mt7621_flag("MT7621_SMPLOG") &&
        ((off >= 0x2000 && off < 0x2040) || (off >= 0x4000 && off < 0x4040) ||
         (off >= 0xa000 && off < 0xa040) || (off >= 0xc000 && off < 0xc040))) {
        fprintf(stderr, "mt7621: cm w %04x <- %08x\n", off, (uint32_t)val);
    }

    /* GCR core-other block (selected core + its reset vector). */
    if (off == MT7621_GCR_OFF + 0x2018) {
        /* GCR_CL_OTHER (CM < 3): core << 16 */
        s->co_other = val;
    } else if (off == 0x2010) {
        /* CPC_CL_OTHER (CM < 3): core << 16 */
        s->cpc_other = val;
    } else if (off == MT7621_GCR_OFF + 0x4018) {
        s->co_other = val;
    } else if (off == MT7621_GCR_OFF + 0x4020) {
        s->co_reset_base = val;
    } else if (off == 0x4000 && (val & 0xf) == 0x4) {
        /* CPC_Cx_CMD_RESET through the core-other redirect block. */
        mt7621_start_core(s);
    }
}

static const MemoryRegionOps mt7621_cm_ops = {
    .read = mt7621_cm_read,
    .write = mt7621_cm_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void mt7621_cm_defaults(MT7621State *s)
{
    memset(s->cm_regs, 0, sizeof(s->cm_regs));
    /* GCR_CONFIG.PCORES is "cores minus one". */
    s->cm_regs[(MT7621_GCR_OFF + GCR_CONFIG) / 4] =
        (s->ncpus > 1 ? s->ncpus - 1 : 0);
    s->cm_regs[(MT7621_GCR_OFF + GCR_BASE) / 4]   = 0x1fbf8000;
    s->cm_regs[(MT7621_GCR_OFF + GCR_REV) / 4]    = 0x00000002;
    /*
     * The MT7621 GIC is the legacy MIPS GIC at 0x1fbc0000, not a CM-managed
     * GIC; advertising one in the GCR makes Breed take a CM-GIC path that
     * does not exist on this SoC and crash.
     */
    s->cm_regs[(MT7621_GCR_OFF + GCR_GIC_STATUS) / 4] = 0x00000000;
    s->cm_regs[(MT7621_GCR_OFF + GCR_CPC_STATUS) / 4] = 0x00000001;

    /*
     * The (legacy) GIC is advertised through the CM by default: Linux 5.6's
     * mips_cps_init() insists on CM_GCR_GIC_STATUS.EX before it registers its
     * SMP ops, and without it the kernel dies on the first indirect call.
     * RouterBOOT used to jump through a bogus pointer when this was set, but
     * that was the missing SMP model, not the advertisement; both boards boot
     * with it on.  MT7621_NO_CMGIC=1 restores the old behaviour.
     */
    if (!mt7621_flag("MT7621_NO_CMGIC")) {
        s->cm_regs[(MT7621_GCR_OFF + GCR_GIC_STATUS) / 4] = 0x00000001;
        s->cm_regs[(MT7621_GCR_OFF + GCR_GIC_BASE) / 4] =
            (MT7621_GIC_BASE & 0x3fff8000) | 0x1;
    }
    /*
     * MT7621_UP=1 reports a single core (no secondary bring-up).  PCORES is
     * "cores minus one" (mips_cps_numcores() adds one back), so a single
     * core is 0, not 1.
     */
    if (mt7621_flag("MT7621_UP")) {
        s->cm_regs[(MT7621_GCR_OFF + GCR_CONFIG) / 4] = 0x00000000;
    }
}


void mt7621_cm_init(MT7621State *s)
{
    mt7621_cm_defaults(s);
    memory_region_init_io(&s->cm, NULL, &mt7621_cm_ops, s,
                          "mt7621.cm", MT7621_CM_SIZE);
    memory_region_add_subregion(get_system_memory(), MT7621_CM_BASE, &s->cm);
}
