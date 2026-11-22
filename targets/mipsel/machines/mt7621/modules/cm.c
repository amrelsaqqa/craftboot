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
    } else if (off == 0xc010 || off == 0xc000 ||
               off == MT7621_GCR_OFF + 0x2000 ||
               off == MT7621_GCR_OFF + 0x2010) {
        /*
         * GCR_Cx_CONFIG (read through GCR_CL_OTHER) and its CM>=3.5 CPC
         * equivalent, CPC_Cx_CONFIG (0xc010 - the one this kernel reads):
         * PVPE+1 is the number of VPs (hardware threads) the core supports.
         * Linux builds its SMP topology from it (mips_cps_numvps()), so
         * reporting 0 here made a 2-cores x 2-VPs part look like four
         * single-VP cores - "VPE topology {1,1,1,1}" instead of {2,2}.
         */
        val = (s->cm_vps ? s->cm_vps : 1) - 1;
    } else if (off == MT7621_GCR_OFF + 0x2028) {
        /*
         * GCR_CL_ID: the core-local block is per-core, so the reader sees
         * its core's id, not its VPE's.  mips_cps_get_bootcfg() uses it to
         * index mips_cps_core_bootcfg[]; returning the VPE index made the
         * second VPE of a core (and both VPEs of core 1) read past the end
         * of that array, so the core never got its VP mask.
         */
        unsigned vps = s->cm_vps ? s->cm_vps : 1;
        uint32_t cpu = current_cpu ? (uint32_t)current_cpu->cpu_index
                                   : CPU(s->cpu)->cpu_index;

        val = cpu / vps;
    } else if (off == MT7621_GCR_OFF + 0x2008) {
        /*
         * GCR_CL_COHERENCE: also part of the core-local block, with one
         * register per core.  A single shared cell made a secondary core
         * read the 0xff the boot core had already written, so it skipped
         * mips_cps_core_init() - the step that binds and halts the sibling
         * VPEs - and cps-vec later skipped every VPE whose TC was not
         * halted ("Skip the VPE if its TC is not halted").
         */
        unsigned vps = s->cm_vps ? s->cm_vps : 1;
        uint32_t cpu = current_cpu ? (uint32_t)current_cpu->cpu_index
                                   : CPU(s->cpu)->cpu_index;

        val = s->cm_coherent[cpu / vps];
    } else {
        val = regs[off >> 2];
    }

    if (getenv("MT7621_CMLOG2")) {
        static int budget = 4000;

        if (budget-- > 0) {
            fprintf(stderr, "cmrd %04x -> %08x pc=%08x\n", (unsigned)off, val,
                    s->cpu ? (uint32_t)s->cpu->env.active_tc.PC : 0);
        }
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
    mt7621_cpu_hw_config(s, env);
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
    /*
     * CP0 Count timebase.  mips_cpu_reset() stores Count = 1, which leaves
     * the counter running from machine reset while the boot CPU's has been
     * rebased by the guest's own mtc0 Count writes - measured with
     * MT7621_COUNTLOG, the secondaries end up seconds ahead of CPU0, and
     * the kernel's "Synchronize counters for CPU N" then removes a huge
     * offset (that window is where an IRQ-context call through a NULL
     * pointer was observed).  Bring the released core onto the boot CPU's
     * timebase, which is what a TC activated on a running counter sees.
     */
    cpu_mips_store_count(env, cpu_mips_get_count(&s->cpu->env));

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
    /*
     * With hardware multithreading the VCPU index is not the core number:
     * core N is made of VPs N*vps .. N*vps+vps-1 (2 VPs per 1004Kc core),
     * so the core's first VP is the CPU that runs cps-vec.S and then starts
     * its sibling VPs.  Mapping core N to cpus[N] made core 1 start VP1 of
     * core 0 instead, and the kernel brought up only 2 of the 4 VPs.
     */
    if (s->cm_vps > 1) {
        unsigned vps = s->cm_vps;

        if (core * vps >= s->ncpus) {
            return;
        }
        core = core * vps;
    }
    if (core == 0 || core >= s->ncpus) {
        return;
    }
    if (mt7621_flag("MT7621_SMPLOG")) {
        fprintf(stderr, "mt7621: start core at cpu %u, pc=%08x (cpc_other=%08x)\n",
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
    CPUMIPSState *env = &cpu->env;
    MT7621State *s = MT7621_MACHINE(qdev_get_machine());

    cpu_reset(cs);
    mt7621_cpu_hw_config(s, env);
    if (getenv("MT7621_RSTLOG") && cs->cpu_index == 0) {
        fprintf(stderr, "cpu0 after hw cfg: cfg0=%08x cfg1=%08x cfg2=%08x "
                "cfg3=%08x cfg7=%08x\n",
                (uint32_t)env->CP0_Config0, (uint32_t)env->CP0_Config1,
                (uint32_t)env->CP0_Config2, (uint32_t)env->CP0_Config3,
                (uint32_t)env->CP0_Config7);
    }
    if (cs->cpu_index != 0) {
        /* Secondary cores wait for the CPC to release them. */
        cs->halted = 1;
        return;
    }
    /*
     * Warm reboot.  mt7621_reset() is registered after these per-CPU
     * handlers, so it runs *first* (QEMU resets LIFO): it re-loads the boot
     * image and points s->entry at the hand-over address, and then
     * cpu_reset() below would put PC back on the CPU reset vector - the
     * BootROM, which this model does not execute for a NAND image.  That is
     * why a guest-triggered reboot hung with nothing after the loader lines.
     * Restore the hand-over PC for the boot CPU.
     */
    if (s && s->entry) {
        env->active_tc.PC = s->entry;
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
    } else if (off == MT7621_GCR_OFF + 0x2008) {
        /* GCR_CL_COHERENCE is per core (see mt7621_cm_read). */
        unsigned vps = s->cm_vps ? s->cm_vps : 1;
        uint32_t cpu = current_cpu ? (uint32_t)current_cpu->cpu_index
                                   : CPU(s->cpu)->cpu_index;

        s->cm_coherent[cpu / vps] = (uint8_t)(val & 0xff);
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

void mt7621_cm_reset(MT7621State *s)
{
    memset(s->cm_regs, 0, sizeof(s->cm_regs));
    memset(s->cm_coherent, 0, sizeof(s->cm_coherent));
    /* The core-selector latches are write-only scratch; a reset clears them. */
    s->co_other = 0;
    s->cpc_other = 0;
    s->co_reset_base = 0;
    /*
     * GCR_CONFIG.PCORES is "cores minus one".  The 1004K core is 2-way
     * multithreaded, so 4 VPs is 2 cores x 2 VPs, not 4 cores: the kernel
     * groups VPs into cores from this count and from GCR_Cx_CONFIG.PVPE.
     * MT7621_VPS=1 restores the old one-VP-per-core view for A/B.
     */
    {
        const char *v = getenv("MT7621_VPS");
        unsigned vps = v ? (unsigned)atoi(v) : 2;
        unsigned ncores;

        if (vps < 1) {
            vps = 1;
        }
        if (vps > s->ncpus) {
            vps = s->ncpus;
        }
        s->cm_vps = vps;
        ncores = s->ncpus / vps;
        s->cm_regs[(MT7621_GCR_OFF + GCR_CONFIG) / 4] =
            (ncores > 1 ? ncores - 1 : 0);
    }
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


/*
 * The CP0 count/compare interrupt on a 1004Kc covered by a CM is the GIC's
 * VP-local interrupt GIC_LOCAL_INT_TIMER (local 2), not CPU interrupt pin 7.
 * The GIC latches it in its VP pending register and only forwards it to a CPU
 * pin when the VP mask bit and the GIC_VPE_TIMER_MAP value say so - which a
 * guest does when it routes the compare interrupt (Linux:
 * gic_all_vpes_irq_cpu_online() writes BIT(GIC_LOCAL_INT_TIMER) to
 * GIC_VPE_SMASK and the map register).  A guest that leaves that mask clear,
 * as this OpenWrt build does because it never registers the r4k clockevent,
 * therefore gets no interrupt at all - the board shows no irq 7 activity.
 *
 * The default core hook drives Cause.IP7/IntCtl.IPTI instead; this machine
 * always has the CM/GIC, so it overrides it.
 */
void cpu_mips_timer_irq(CPUMIPSState *env, int level)
{
    MT7621State *s = MT7621_MACHINE(qdev_get_machine());

    if (s->gic) {
        mips_gic_local_irq(MIPS_GIC(s->gic), env_cpu(env)->cpu_index,
                           GIC_LOCAL_INT_TIMER, level);
        return;
    }

    /* Before the GIC exists (early machine setup): the bare-core behaviour. */
    if (!(env->CP0_Config3 & (1 << CP0C3_VEIC))) {
        int irq = (env->CP0_IntCtl >> CP0IntCtl_IPTI) & 0x7;

        if (level) {
            qemu_irq_raise(env->irq[irq]);
        } else {
            qemu_irq_lower(env->irq[irq]);
        }
    }
}

void mt7621_cm_init(MT7621State *s)
{
    mt7621_cm_reset(s);
    memory_region_init_io(&s->cm, NULL, &mt7621_cm_ops, s,
                          "mt7621.cm", MT7621_CM_SIZE);
    memory_region_add_subregion(get_system_memory(), MT7621_CM_BASE, &s->cm);
}
