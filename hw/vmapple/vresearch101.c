/*
 * Apple "vresearch101" research VM machine emulation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * vresearch101 is the Virtualization.framework platform (PV=3) that Apple's
 * Private Cloud Compute research VMs and vphone-cli's virtual iPhones
 * (vphone600) run on. Its device tree (DeviceTree.vresearch101ap /
 * DeviceTree.vphone600ap, arm-io compatible "arm-io,vmapple1") places every
 * device the vmapple machine has at the same address, and adds:
 *
 *   avp-rtc   0x30240000  SPI 0x13
 *   sep       0x30250000  SPI 0x14, 0x15  (iop-sep,vpiop)
 *   avp-ctrr  0x30260000
 *
 * The SEP is a second core with a private address space (from
 * AVPSEPBooter's page tables): ROM at 0x2000000, SRAM at 0x4000000, and the
 * rest of the system address space (the vpiop mailbox, AP RAM) shared.
 *
 * Unlike vmapple this machine does not depend on HVF: the guest runs under
 * TCG on the apple-gxf CPU (GXF/SPRR for SPTM and TXM). It shares the vmapple
 * cfg, bdif, aes and virtio-blk devices. Nothing here uses code from
 * Virtualization.framework.
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "hw/usb.h"
#include "hw/arm/boot.h"
#include "hw/arm/apple-silicon/a13_gxf.h"
#include "hw/char/pl011.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/misc/pvpanic.h"
#include "hw/misc/unimp.h"
#include "hw/pci-host/gpex.h"
#include "hw/usb/hcd-xhci-pci.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/vmapple/vmapple.h"
#include "net/net.h"
#include "qapi/error.h"
#include "cpu.h"
#include "cpregs.h"
#include "qemu/bitmap.h"
#include "qobject/qlist.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"

struct VResearchMachineState {
    MachineState parent;

    struct arm_boot_info bootinfo;
    const MemMapEntry *memmap;
    const int *irqmap;
    DeviceState *gic;
    DeviceState *cfg;
    DeviceState *pvpanic;
    DeviceState *gpio_key;
    DeviceState *vpiop;
    Notifier powerdown_notifier;
    PCIBus *bus;
    MemoryRegion fw_mr;
    MemoryRegion scratch_mr;
    MemoryRegion ecam_alias;
    uint64_t ecid;
    char *gfx_device;
    char *sep_rom;

    /* SEP coprocessor: its own core and address space */
    ARMCPU *sep_cpu;
    MemoryRegion sep_mem;
    MemoryRegion sep_sys_alias;
    MemoryRegion sep_rom_mr;
    MemoryRegion sep_sram_mr;
};

#define TYPE_VRESEARCH_MACHINE   MACHINE_TYPE_NAME("vresearch101")
OBJECT_DECLARE_SIMPLE_TYPE(VResearchMachineState, VRESEARCH_MACHINE)

/* Number of external interrupt lines to configure the GIC with */
#define NUM_IRQS 256

enum {
    VR_FIRMWARE,
    VR_CONFIG,
    VR_SCRATCH,
    VR_MEM,
    VR_GIC_DIST,
    VR_GIC_REDIST,
    VR_UART,
    VR_RTC,
    VR_PCIE,
    VR_PCIE_MMIO,
    VR_PCIE_ECAM,
    VR_GPIO,
    VR_PVPANIC,
    VR_BDOOR,
    VR_APV_GFX,
    VR_APV_IOSFC,
    VR_AES_1,
    VR_AES_2,
    VR_AVP_RTC,
    VR_SEP,
    VR_AVP_CTRR,
    VR_MEMMAP_LAST,
};

static const MemMapEntry memmap[] = {
    [VR_FIRMWARE] =           { 0x00100000, 0x00100000 },
    [VR_CONFIG] =             { 0x00400000, 0x00010000 },
    /*
     * RAM above the cfg page, not in vmapple: AVPBooter keeps state at
     * 0x600000 (tables at +0x90, +0xd8), iBoot works in 0x4fc000-0x504000.
     */
    [VR_SCRATCH] =            { 0x00410000, 0x002f0000 },

    [VR_GIC_DIST] =           { 0x10000000, 0x00010000 },
    [VR_GIC_REDIST] =         { 0x10010000, 0x00400000 },

    [VR_UART] =               { 0x20010000, 0x00010000 },
    [VR_RTC] =                { 0x20050000, 0x00001000 },
    [VR_GPIO] =               { 0x20060000, 0x00001000 },
    [VR_PVPANIC] =            { 0x20070000, 0x00000002 },
    [VR_BDOOR] =              { 0x30000000, 0x00200000 },
    [VR_APV_GFX] =            { 0x30200000, 0x00010000 },
    [VR_APV_IOSFC] =          { 0x30210000, 0x00010000 },
    [VR_AES_1] =              { 0x30220000, 0x00004000 },
    [VR_AES_2] =              { 0x30230000, 0x00004000 },
    [VR_AVP_RTC] =            { 0x30240000, 0x00001000 },
    [VR_SEP] =                { 0x30250000, 0x00001000 },
    [VR_AVP_CTRR] =           { 0x30260000, 0x00010000 },
    [VR_PCIE_ECAM] =          { 0x40000000, 0x10000000 },
    [VR_PCIE_MMIO] =          { 0x50000000, 0x1fff0000 },

    /* Actual RAM size depends on configuration */
    [VR_MEM] =                { 0x70000000ULL, GiB },
};

/* GIC SPI numbers; the ADT lists them as INTID (SPI + 32) */
static const int irqmap[] = {
    [VR_UART] = 1,
    [VR_RTC] = 2,
    [VR_GPIO] = 0x5,
    [VR_APV_IOSFC] = 0x10,
    [VR_APV_GFX] = 0x11,
    [VR_AES_1] = 0x12,
    [VR_AVP_RTC] = 0x13,
    [VR_SEP] = 0x14,
    [VR_PCIE] = 0x20,
};

#define GPEX_NUM_IRQS 16

static void create_bdif(VResearchMachineState *vms)
{
    DeviceState *bdif;
    SysBusDevice *bdif_sb;
    DriveInfo *di_aux = drive_get(IF_PFLASH, 0, 0);
    DriveInfo *di_root = drive_get(IF_PFLASH, 0, 1);

    if (!di_aux) {
        error_report("No AUX device. Please specify one as pflash drive.");
        exit(1);
    }

    if (!di_root) {
        /* Fall back to the first IF_VIRTIO device as root device */
        di_root = drive_get(IF_VIRTIO, 0, 0);
    }

    if (!di_root) {
        error_report("No root device. Please specify one as virtio drive.");
        exit(1);
    }

    /* PV backdoor device */
    bdif = qdev_new(TYPE_VMAPPLE_BDIF);
    bdif_sb = SYS_BUS_DEVICE(bdif);
    sysbus_mmio_map(bdif_sb, 0, vms->memmap[VR_BDOOR].base);

    qdev_prop_set_drive(DEVICE(bdif), "aux", blk_by_legacy_dinfo(di_aux));
    qdev_prop_set_drive(DEVICE(bdif), "root", blk_by_legacy_dinfo(di_root));

    sysbus_realize_and_unref(bdif_sb, &error_fatal);
}

static void create_pvpanic(VResearchMachineState *vms)
{
    SysBusDevice *pvpanic;

    vms->pvpanic = qdev_new(TYPE_PVPANIC_MMIO_DEVICE);
    pvpanic = SYS_BUS_DEVICE(vms->pvpanic);
    sysbus_mmio_map(pvpanic, 0, vms->memmap[VR_PVPANIC].base);

    sysbus_realize_and_unref(pvpanic, &error_fatal);
}

static void create_cfg(VResearchMachineState *vms)
{
    SysBusDevice *cfg;
    MachineState *machine = MACHINE(vms);
    uint32_t rnd = 1;

    vms->cfg = qdev_new(TYPE_VMAPPLE_CFG);
    cfg = SYS_BUS_DEVICE(vms->cfg);
    sysbus_mmio_map(cfg, 0, vms->memmap[VR_CONFIG].base);

    qemu_guest_getrandom_nofail(&rnd, sizeof(rnd));

    qdev_prop_set_uint32(vms->cfg, "nr-cpus", machine->smp.cpus);
    qdev_prop_set_uint64(vms->cfg, "ecid", vms->ecid);
    qdev_prop_set_uint64(vms->cfg, "ram-size", machine->ram_size);
    qdev_prop_set_uint32(vms->cfg, "rnd", rnd);

    sysbus_realize_and_unref(cfg, &error_fatal);
}

static void create_gfx(VResearchMachineState *vms)
{
    int irq_gfx = vms->irqmap[VR_APV_GFX];
    int irq_iosfc = vms->irqmap[VR_APV_IOSFC];
    SysBusDevice *gfx;

    if (!vms->gfx_device || !strcmp(vms->gfx_device, "none")) {
        create_unimplemented_device("apv-gfx", vms->memmap[VR_APV_GFX].base,
                                    vms->memmap[VR_APV_GFX].size);
        create_unimplemented_device("apv-iosfc",
                                    vms->memmap[VR_APV_IOSFC].base,
                                    vms->memmap[VR_APV_IOSFC].size);
        return;
    }

    if (!object_class_by_name(vms->gfx_device)) {
        error_report("gfx-device '%s' is not available in this build",
                     vms->gfx_device);
        exit(1);
    }

    /* mmio/irq 0 = gfx window, mmio/irq 1 = IOSurface mapper window */
    gfx = SYS_BUS_DEVICE(qdev_new(vms->gfx_device));
    sysbus_mmio_map(gfx, 0, vms->memmap[VR_APV_GFX].base);
    sysbus_mmio_map(gfx, 1, vms->memmap[VR_APV_IOSFC].base);
    sysbus_connect_irq(gfx, 0, qdev_get_gpio_in(vms->gic, irq_gfx));
    sysbus_connect_irq(gfx, 1, qdev_get_gpio_in(vms->gic, irq_iosfc));
    sysbus_realize_and_unref(gfx, &error_fatal);
}

static void create_aes(VResearchMachineState *vms)
{
    int irq = vms->irqmap[VR_AES_1];
    SysBusDevice *aes;

    aes = SYS_BUS_DEVICE(qdev_new(TYPE_VMAPPLE_AES));
    sysbus_mmio_map(aes, 0, vms->memmap[VR_AES_1].base);
    sysbus_mmio_map(aes, 1, vms->memmap[VR_AES_2].base);
    sysbus_connect_irq(aes, 0, qdev_get_gpio_in(vms->gic, irq));
    sysbus_realize_and_unref(aes, &error_fatal);
}



/*
 * Devices vresearch101 has and vmapple does not. Their register interfaces
 * are not known yet; map them as logged holes (-d unimp) so the guest's
 * accesses show what they need.
 */
static void create_avp_stubs(VResearchMachineState *vms)
{
    create_unimplemented_device("avp-rtc", vms->memmap[VR_AVP_RTC].base,
                                vms->memmap[VR_AVP_RTC].size);
    create_unimplemented_device("avp-ctrr", vms->memmap[VR_AVP_CTRR].base,
                                vms->memmap[VR_AVP_CTRR].size);
}


/*
 * SEP mailbox: mmio 0 is the AP's bank, mmio 1 the SEP's, both at the same
 * physical address in their own address spaces. INTIDs 0x35 and 0x34 in the
 * ADT: inbox not empty, outbox empty.
 */
static void create_vpiop(VResearchMachineState *vms)
{
    SysBusDevice *sbd;

    vms->vpiop = qdev_new(TYPE_VMAPPLE_VPIOP);
    sbd = SYS_BUS_DEVICE(vms->vpiop);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, vms->memmap[VR_SEP].base);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(vms->gic, vms->irqmap[VR_SEP] + 1));
    sysbus_connect_irq(sbd, 1,
                       qdev_get_gpio_in(vms->gic, vms->irqmap[VR_SEP]));
}

/*
 * Apple's implementation-defined system registers (S3_<op1>_C15_<crm>_<op2>)
 * that neither QEMU nor the GXF/SPRR set defines. A Virtualization.framework
 * guest sees whatever the hypervisor exposes there; until each is known,
 * keep what the guest writes, read it back (0 at first), and log the first
 * access to each register so the log shows which ones matter.
 */
#define IMPDEF_MAX_CPUS 64

static uint64_t impdef_val[IMPDEF_MAX_CPUS][8][16][8];
static DECLARE_BITMAP(impdef_seen, 8 * 16 * 8);

static uint64_t *impdef_slot(CPUARMState *env, const ARMCPRegInfo *ri)
{
    int idx = MIN(env_cpu(env)->cpu_index, IMPDEF_MAX_CPUS - 1);
    int bit = (ri->opc1 * 16 + ri->crm) * 8 + ri->opc2;

    if (!test_and_set_bit(bit, impdef_seen)) {
        qemu_log_mask(LOG_UNIMP, "vresearch101: impdef sysreg S3_%d_C15_C%d_%d"
                      " first used by cpu%d at pc 0x%" PRIx64 "\n",
                      ri->opc1, ri->crm, ri->opc2, idx, env->pc);
    }
    return &impdef_val[idx][ri->opc1][ri->crm][ri->opc2];
}

static uint64_t impdef_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return *impdef_slot(env, ri);
}

static void impdef_write(CPUARMState *env, const ARMCPRegInfo *ri,
                         uint64_t value)
{
    *impdef_slot(env, ri) = value;
}

/* Reset values of the ones that matter (names as in Asahi Linux / m1n1) */
static const struct {
    int op1, crm, op2;
    uint64_t value;
} impdef_reset[] = {
    /* APSTS_EL1.MKEYVld: the pointer-auth master key is valid (pauth no-op) */
    { 6, 12, 4, 1 },
};

static void define_impdef_sink(ARMCPU *cpu)
{
    int idx = MIN(CPU(cpu)->cpu_index, IMPDEF_MAX_CPUS - 1);

    for (int i = 0; i < ARRAY_SIZE(impdef_reset); i++) {
        impdef_val[idx][impdef_reset[i].op1][impdef_reset[i].crm]
                  [impdef_reset[i].op2] = impdef_reset[i].value;
    }
    for (int op1 = 0; op1 < 8; op1++) {
        for (int crm = 0; crm < 16; crm++) {
            for (int op2 = 0; op2 < 8; op2++) {
                uint32_t key = ENCODE_AA64_CP_REG(CP_REG_ARM64_SYSREG_CP, 15,
                                                  crm, 3, op1, op2);
                ARMCPRegInfo ri = {
                    .state = ARM_CP_STATE_AA64,
                    .opc0 = 3, .opc1 = op1, .crn = 15, .crm = crm,
                    .opc2 = op2,
                    .access = PL1_RW,
                    .type = ARM_CP_IO | ARM_CP_NO_RAW,
                    .readfn = impdef_read,
                    .writefn = impdef_write,
                };

                if (ARMCPRegTable_cget(cpu->cp_regs, key)) {
                    continue;
                }
                ri.name = g_strdup_printf("IMPDEF_S3_%d_C15_C%d_%d",
                                          op1, crm, op2);
                define_one_arm_cp_reg(cpu, &ri);
            }
        }
    }
}

#define SEP_ROM_BASE   0x02000000
#define SEP_ROM_SIZE   0x00080000
#define SEP_SRAM_BASE  0x04000000
#define SEP_SRAM_SIZE  0x00800000

/*
 * The SEP core. AVPSEPBooter is position independent; its page tables put
 * the ROM at 0x2000000 (read-only) and its working memory at 0x4000000,
 * and map the mailbox at the AP's physical 0x30250000 and AP memory from
 * 0x6000000 up. Model that as a container holding the private ROM and SRAM
 * over an alias of the whole system address space.
 */
static void create_sep(VResearchMachineState *vms)
{
    MachineState *ms = MACHINE(vms);
    g_autofree char *fname = NULL;
    Object *cpu;

    if (!vms->sep_rom) {
        return;
    }

    /* TCG sizes its per-vCPU contexts by maxcpus; the SEP core needs one */
    if (ms->smp.max_cpus <= ms->smp.cpus) {
        error_report("sep-rom needs a spare CPU slot for the SEP core: "
                     "use -smp %u,maxcpus=%u", ms->smp.cpus, ms->smp.cpus + 1);
        exit(1);
    }

    fname = qemu_find_file(QEMU_FILE_TYPE_BIOS, vms->sep_rom);
    if (!fname) {
        error_report("Could not find SEP ROM image '%s'", vms->sep_rom);
        exit(1);
    }

    memory_region_init(&vms->sep_mem, OBJECT(vms), "sep-memory", UINT64_MAX);
    memory_region_init_alias(&vms->sep_sys_alias, OBJECT(vms), "sep-system",
                             get_system_memory(), 0, UINT64_MAX);
    memory_region_add_subregion_overlap(&vms->sep_mem, 0,
                                        &vms->sep_sys_alias, -1);

    memory_region_init_rom(&vms->sep_rom_mr, NULL, "sep-rom",
                           SEP_ROM_SIZE, &error_fatal);
    if (load_image_mr(fname, &vms->sep_rom_mr) < 0) {
        error_report("Could not load SEP ROM image '%s'", vms->sep_rom);
        exit(1);
    }
    memory_region_add_subregion(&vms->sep_mem, SEP_ROM_BASE,
                                &vms->sep_rom_mr);

    memory_region_init_ram(&vms->sep_sram_mr, NULL, "sep-sram",
                           SEP_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(&vms->sep_mem, SEP_SRAM_BASE,
                                &vms->sep_sram_mr);
    memory_region_add_subregion(&vms->sep_mem, vms->memmap[VR_SEP].base,
                                sysbus_mmio_get_region(
                                    SYS_BUS_DEVICE(vms->vpiop), 1));

    cpu = object_new(ms->cpu_type);
    CPU(cpu)->cpu_index = ms->smp.cpus;
    /* Its own cluster, so no AP's MPIDR collides with it */
    object_property_set_int(cpu, "mp-affinity", 0x10000, &error_fatal);
    if (object_property_find(cpu, "has_el3")) {
        object_property_set_bool(cpu, "has_el3", false, &error_fatal);
    }
    if (object_property_find(cpu, "has_el2")) {
        object_property_set_bool(cpu, "has_el2", false, &error_fatal);
    }
    object_property_set_uint(cpu, "rvbar", SEP_ROM_BASE, &error_fatal);
    object_property_set_link(cpu, "memory", OBJECT(&vms->sep_mem),
                             &error_abort);
    qdev_realize(DEVICE(cpu), NULL, &error_fatal);
    vms->sep_cpu = ARM_CPU(cpu);
    object_unref(cpu);
}

static int arm_gic_ppi_index(int cpu_nr, int ppi_index)
{
    return NUM_IRQS + cpu_nr * GIC_INTERNAL + ppi_index;
}

static void create_gic(VResearchMachineState *vms)
{
    MachineState *ms = MACHINE(vms);
    SysBusDevice *gicbusdev;
    QList *redist_region_count;
    unsigned int smp_cpus = ms->smp.cpus;
    uint32_t redist0_capacity, redist0_count;
    int i;

    vms->gic = qdev_new(gicv3_class_name());
    qdev_prop_set_uint32(vms->gic, "revision", 3);
    qdev_prop_set_uint32(vms->gic, "num-cpu", smp_cpus);
    /*
     * Note that the num-irq property counts both internal and external
     * interrupts; there are always 32 of the former (mandated by GIC spec).
     */
    qdev_prop_set_uint32(vms->gic, "num-irq", NUM_IRQS + 32);

    redist0_capacity = vms->memmap[VR_GIC_REDIST].size / GICV3_REDIST_SIZE;
    redist0_count = MIN(smp_cpus, redist0_capacity);

    redist_region_count = qlist_new();
    qlist_append_int(redist_region_count, redist0_count);
    qdev_prop_set_array(vms->gic, "redist-region-count", redist_region_count);

    gicbusdev = SYS_BUS_DEVICE(vms->gic);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, vms->memmap[VR_GIC_DIST].base);
    sysbus_mmio_map(gicbusdev, 1, vms->memmap[VR_GIC_REDIST].base);

    /*
     * Wire the outputs from each CPU's generic timer to the appropriate GIC
     * PPI inputs, and the GIC's IRQ/FIQ outputs to the CPU's inputs.
     */
    for (i = 0; i < smp_cpus; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));

        /* Map the virt timer to PPI 27 */
        qdev_connect_gpio_out(cpudev, GTIMER_VIRT,
                              qdev_get_gpio_in(vms->gic,
                                               arm_gic_ppi_index(i, 27)));

        sysbus_connect_irq(gicbusdev, i, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
    }
}

static void create_uart(const VResearchMachineState *vms, MemoryRegion *mem,
                        Chardev *chr)
{
    hwaddr base = vms->memmap[VR_UART].base;
    int irq = vms->irqmap[VR_UART];
    DeviceState *dev = qdev_new(TYPE_PL011);
    SysBusDevice *s = SYS_BUS_DEVICE(dev);

    qdev_prop_set_chr(dev, "chardev", chr);
    sysbus_realize_and_unref(s, &error_fatal);
    memory_region_add_subregion(mem, base, sysbus_mmio_get_region(s, 0));
    sysbus_connect_irq(s, 0, qdev_get_gpio_in(vms->gic, irq));
}

static void create_rtc(const VResearchMachineState *vms)
{
    hwaddr base = vms->memmap[VR_RTC].base;
    int irq = vms->irqmap[VR_RTC];

    sysbus_create_simple("pl031", base, qdev_get_gpio_in(vms->gic, irq));
}

static void vresearch_powerdown_req(Notifier *n, void *opaque)
{
    VResearchMachineState *vms = container_of(n, VResearchMachineState,
                                              powerdown_notifier);

    /* GPIO pin 3 is the power button (pl061-buttons, VMA1_BUTTONS) */
    qemu_set_irq(qdev_get_gpio_in(vms->gpio_key, 0), 1);
}

static void create_gpio_devices(VResearchMachineState *vms, MemoryRegion *mem)
{
    DeviceState *pl061_dev;
    hwaddr base = vms->memmap[VR_GPIO].base;
    int irq = vms->irqmap[VR_GPIO];
    SysBusDevice *s;

    pl061_dev = qdev_new("pl061");
    /* Pull lines down to 0 if not driven by the PL061 */
    qdev_prop_set_uint32(pl061_dev, "pullups", 0);
    qdev_prop_set_uint32(pl061_dev, "pulldowns", 0xff);
    s = SYS_BUS_DEVICE(pl061_dev);
    sysbus_realize_and_unref(s, &error_fatal);
    memory_region_add_subregion(mem, base, sysbus_mmio_get_region(s, 0));
    sysbus_connect_irq(s, 0, qdev_get_gpio_in(vms->gic, irq));
    vms->gpio_key = sysbus_create_simple("gpio-key", -1,
                                         qdev_get_gpio_in(pl061_dev, 3));
}

static void vresearch_firmware_init(VResearchMachineState *vms,
                                    MemoryRegion *sysmem)
{
    hwaddr size = vms->memmap[VR_FIRMWARE].size;
    hwaddr base = vms->memmap[VR_FIRMWARE].base;
    const char *bios_name = MACHINE(vms)->firmware;
    g_autofree char *fname = NULL;
    int image_size;

    if (!bios_name) {
        error_report("No firmware specified (-bios AVPBooter.vresearch1.bin)");
        exit(1);
    }

    fname = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (!fname) {
        error_report("Could not find ROM image '%s'", bios_name);
        exit(1);
    }

    memory_region_init_ram(&vms->fw_mr, NULL, "firmware", size, &error_fatal);
    image_size = load_image_mr(fname, &vms->fw_mr);
    if (image_size < 0) {
        error_report("Could not load ROM image '%s'", bios_name);
        exit(1);
    }

    memory_region_add_subregion(sysmem, base, &vms->fw_mr);

    memory_region_init_ram(&vms->scratch_mr, NULL, "avp-scratch",
                           vms->memmap[VR_SCRATCH].size, &error_fatal);
    memory_region_add_subregion(sysmem, vms->memmap[VR_SCRATCH].base,
                                &vms->scratch_mr);
}

static void create_pcie(VResearchMachineState *vms)
{
    hwaddr base_mmio = vms->memmap[VR_PCIE_MMIO].base;
    hwaddr size_mmio = vms->memmap[VR_PCIE_MMIO].size;
    hwaddr base_ecam = vms->memmap[VR_PCIE_ECAM].base;
    hwaddr size_ecam = vms->memmap[VR_PCIE_ECAM].size;
    int irq = vms->irqmap[VR_PCIE];
    MemoryRegion *mmio_alias;
    MemoryRegion *mmio_reg;
    MemoryRegion *ecam_reg;
    DeviceState *dev;
    PCIHostState *pci;
    int i;

    dev = qdev_new(TYPE_GPEX_HOST);
    qdev_prop_set_uint32(dev, "num-irqs", GPEX_NUM_IRQS);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /* Map only the first size_ecam bytes of ECAM space */
    ecam_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0);
    memory_region_init_alias(&vms->ecam_alias, OBJECT(dev), "pcie-ecam",
                             ecam_reg, 0, size_ecam);
    memory_region_add_subregion(get_system_memory(), base_ecam,
                                &vms->ecam_alias);

    /* PCI MMIO window is identity mapped at [0x50000000-0x6fff0000] */
    mmio_alias = g_new0(MemoryRegion, 1);
    mmio_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 1);
    memory_region_init_alias(mmio_alias, OBJECT(dev), "pcie-mmio",
                             mmio_reg, base_mmio, size_mmio);
    memory_region_add_subregion(get_system_memory(), base_mmio, mmio_alias);

    for (i = 0; i < GPEX_NUM_IRQS; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i,
                           qdev_get_gpio_in(vms->gic, irq + i));
        gpex_set_irq_num(GPEX_HOST(dev), i, irq + i);
    }

    pci = PCI_HOST_BRIDGE(dev);
    vms->bus = pci->bus;
    assert(vms->bus);

    while ((dev = qemu_create_nic_device("virtio-net-pci", true, NULL))) {
        qdev_realize_and_unref(dev, BUS(vms->bus), &error_fatal);
    }

    if (defaults_enabled()) {
        DeviceState *usb_controller = qdev_new(TYPE_QEMU_XHCI);
        USBBus *usb_bus;

        qdev_realize_and_unref(usb_controller, BUS(pci->bus), &error_fatal);
        usb_bus = USB_BUS(object_resolve_type_unambiguous(TYPE_USB_BUS,
                                                          &error_fatal));
        usb_create_simple(usb_bus, "usb-kbd");
    }
}

static void vresearch_reset(void *opaque)
{
    VResearchMachineState *vms = opaque;

    cpu_set_pc(first_cpu, vms->memmap[VR_FIRMWARE].base);
    if (vms->sep_cpu) {
        cpu_set_pc(CPU(vms->sep_cpu), SEP_ROM_BASE);
    }
}

static void mach_vresearch_init(MachineState *machine)
{
    VResearchMachineState *vms = VRESEARCH_MACHINE(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    const CPUArchIdList *possible_cpus;
    MemoryRegion *sysmem = get_system_memory();
    unsigned int smp_cpus = machine->smp.cpus;
    int n;

    vms->memmap = memmap;
    machine->usb = true;

    possible_cpus = mc->possible_cpu_arch_ids(machine);
    assert(possible_cpus->len == machine->smp.max_cpus);
    for (n = 0; n < possible_cpus->len && n < smp_cpus; n++) {
        Object *cpu = object_new(possible_cpus->cpus[n].type);
        CPUState *cs = CPU(cpu);

        object_property_set_int(cpu, "mp-affinity",
                                possible_cpus->cpus[n].arch_id, &error_fatal);
        cs->cpu_index = n;

        numa_cpu_pre_plug(&possible_cpus->cpus[cs->cpu_index], DEVICE(cpu),
                          &error_fatal);

        /* Virtualization.framework guests run at EL1 with PSCI over HVC */
        if (object_property_find(cpu, "has_el3")) {
            object_property_set_bool(cpu, "has_el3", false, &error_fatal);
        }
        if (object_property_find(cpu, "has_el2")) {
            object_property_set_bool(cpu, "has_el2", false, &error_fatal);
        }
        object_property_set_int(cpu, "psci-conduit", QEMU_PSCI_CONDUIT_HVC,
                                &error_fatal);

        /* Secondary CPUs start in PSCI powered-down state */
        if (n > 0) {
            object_property_set_bool(cpu, "start-powered-off", true,
                                     &error_fatal);
        }

        object_property_set_link(cpu, "memory", OBJECT(sysmem), &error_abort);
        /* SPTM and TXM run in GXF guarded levels and program SPRR */
        apple_gxf_define_cpregs(ARM_CPU(cpu));
        apple_gxf_define_m1_gl_cpregs(ARM_CPU(cpu));
        /*
         * The M1-style SPRR layout. SPTM enables SPRR before it writes the
         * EL1 table, so the table must allow its own code at reset; start it
         * at the value SPTM programs (and later checks).
         */
        ARM_CPU(cpu)->env.sprr_cfg.unified_perms = true;
        ARM_CPU(cpu)->env.sprr_cfg.el1_perm_reset = 0x2020a52a302abaf5ULL;
        qdev_realize(DEVICE(cpu), NULL, &error_fatal);
        apple_gxf_define_override_cpregs(ARM_CPU(cpu));
        define_impdef_sink(ARM_CPU(cpu));
        object_unref(cpu);
    }

    memory_region_add_subregion(sysmem, vms->memmap[VR_MEM].base,
                                machine->ram);

    create_gic(vms);
    create_bdif(vms);
    create_pvpanic(vms);
    create_aes(vms);
    /*
     * Before the firmware, scratch and cfg RAM are mapped: apple-gfx hands the
     * library the guest RAM present at realize, which on Linux must all be
     * shared file mappings (-machine memory-backend= a memfd backend with
     * share=on), and those small anonymous regions are not.
     */
    create_gfx(vms);
    create_avp_stubs(vms);
    create_vpiop(vms);
    create_uart(vms, sysmem, serial_hd(0));
    create_rtc(vms);
    create_pcie(vms);
    create_gpio_devices(vms, sysmem);

    vresearch_firmware_init(vms, sysmem);
    create_sep(vms);
    create_cfg(vms);

    vms->powerdown_notifier.notify = vresearch_powerdown_req;
    qemu_register_powerdown_notifier(&vms->powerdown_notifier);

    vms->bootinfo.ram_size = machine->ram_size;
    vms->bootinfo.board_id = -1;
    vms->bootinfo.loader_start = vms->memmap[VR_MEM].base;
    vms->bootinfo.skip_dtb_autoload = true;
    vms->bootinfo.firmware_loaded = true;
    arm_load_kernel(ARM_CPU(first_cpu), machine, &vms->bootinfo);

    qemu_register_reset(vresearch_reset, vms);
}

static CpuInstanceProperties
vresearch_cpu_index_to_props(MachineState *ms, unsigned cpu_index)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *possible_cpus = mc->possible_cpu_arch_ids(ms);

    assert(cpu_index < possible_cpus->len);
    return possible_cpus->cpus[cpu_index].props;
}

static int64_t vresearch_get_default_cpu_node_id(const MachineState *ms,
                                                 int idx)
{
    return idx % ms->numa_state->num_nodes;
}

static const CPUArchIdList *vresearch_possible_cpu_arch_ids(MachineState *ms)
{
    unsigned int max_cpus = ms->smp.max_cpus;
    int n;

    if (ms->possible_cpus) {
        assert(ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }

    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;
    for (n = 0; n < ms->possible_cpus->len; n++) {
        ms->possible_cpus->cpus[n].type = ms->cpu_type;
        ms->possible_cpus->cpus[n].arch_id =
            arm_build_mp_affinity(n, GICV3_TARGETLIST_BITS);
        ms->possible_cpus->cpus[n].props.has_thread_id = true;
        ms->possible_cpus->cpus[n].props.thread_id = n;
    }
    return ms->possible_cpus;
}

static GlobalProperty vresearch_compat_defaults[] = {
    { TYPE_VIRTIO_PCI, "disable-legacy", "on" },
    /* Same XHCI interrupter workaround as vmapple (no MSI(-X) support) */
    { TYPE_XHCI_PCI, "conditional-intr-mapping", "on" },
};

static char *vresearch_get_gfx_device(Object *obj, Error **errp)
{
    return g_strdup(VRESEARCH_MACHINE(obj)->gfx_device);
}

static void vresearch_set_gfx_device(Object *obj, const char *value,
                                     Error **errp)
{
    VResearchMachineState *vms = VRESEARCH_MACHINE(obj);

    g_free(vms->gfx_device);
    vms->gfx_device = g_strdup(value);
}

static char *vresearch_get_sep_rom(Object *obj, Error **errp)
{
    return g_strdup(VRESEARCH_MACHINE(obj)->sep_rom);
}

static void vresearch_set_sep_rom(Object *obj, const char *value, Error **errp)
{
    VResearchMachineState *vms = VRESEARCH_MACHINE(obj);

    g_free(vms->sep_rom);
    vms->sep_rom = g_strdup(value);
}

static void vresearch_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = mach_vresearch_init;
    mc->max_cpus = 32;
    mc->block_default_type = IF_VIRTIO;
    mc->no_cdrom = 1;
    mc->pci_allow_0_address = true;
    mc->minimum_page_bits = 12;
    mc->possible_cpu_arch_ids = vresearch_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = vresearch_cpu_index_to_props;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("apple-gxf");
    mc->get_default_cpu_node_id = vresearch_get_default_cpu_node_id;
    mc->default_ram_id = "mach-vresearch101.ram";
    mc->default_ram_size = 8 * GiB;
    mc->default_cpus = 8;
    mc->desc = "Apple vresearch101 research VM (vphone600)";

    compat_props_add(mc->compat_props, vresearch_compat_defaults,
                     G_N_ELEMENTS(vresearch_compat_defaults));

    object_class_property_add_str(oc, "gfx-device", vresearch_get_gfx_device,
                                  vresearch_set_gfx_device);
    object_class_property_set_description(oc, "gfx-device",
        "Paravirtual GPU device type (apple-gfx-mmio, or none)");
    object_class_property_add_str(oc, "sep-rom", vresearch_get_sep_rom,
                                  vresearch_set_sep_rom);
    object_class_property_set_description(oc, "sep-rom",
        "SEP boot ROM (AVPSEPBooter.vresearch1.bin); no SEP core without it");
}

static void vresearch_instance_init(Object *obj)
{
    VResearchMachineState *vms = VRESEARCH_MACHINE(obj);

    vms->irqmap = irqmap;
    vms->gfx_device = g_strdup("none");

    object_property_add_uint64_ptr(obj, "ecid", &vms->ecid,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "ecid",
        "Chip ECID (VZMacMachineIdentifier ECID; binds IMG4 tickets)");
}

static const TypeInfo vresearch_machine_info = {
    .name          = TYPE_VRESEARCH_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(VResearchMachineState),
    .class_init    = vresearch_machine_class_init,
    .instance_init = vresearch_instance_init,
};

static void vresearch_machine_init(void)
{
    type_register_static(&vresearch_machine_info);
}
type_init(vresearch_machine_init);
