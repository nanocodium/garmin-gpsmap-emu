/*
 * Garmin GPSMAP 7x08 / 7x10 / 7x16 chartplotter (TI OMAP4460) machine model
 *
 * Board facts recovered from the firmware update card:
 *   - TI OMAP4460 (dual Cortex-A9 MPCore, PL310 L2), 1 GiB LPDDR2 at 0x80000000
 *   - 56 KiB on-chip SRAM at 0x40300000 (early stacks, boot-config copy,
 *     loader "services" table at 0x40300204)
 *   - Boot ROM at 0x40000000, aliased at 0x00000000 (exception/monitor vectors)
 *   - GPMC CS0 window at 0x08000000 holding a 0xA55A-tagged boot-config record
 *   - Loader and main images both link at 0x80050000 (mutually exclusive)
 *   - GPTIMER3 (v2 layout) and GPTIMER10 (legacy layout) used by the RTOS
 *   - eMMC ("mnand") on HSMMC1, LAN9221 Ethernet on GPMC, UART console
 *
 * Usage:
 *   qemu-system-arm -M gpsmap7x08 -kernel loader.bin \
 *       -device loader,file=main.bin,addr=0x80050000,force-raw=on \
 *       -serial stdio -d unimp,guest_errors
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "hw/boards.h"
#include "hw/arm/machines-qom.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/char/serial-mm.h"
#include "hw/cpu/a9mpcore.h"
#include "hw/misc/unimp.h"
#include "hw/sd/sdhci.h"
#include "hw/sd/sd.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "hw/loader.h"
#include "hw/net/lan9118.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/pixel_ops.h"
#include "net/net.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "target/arm/cpu.h"
#include "qom/object.h"
#include "garmin_gl.h"

/* ------------------------------------------------------------------ */
/* Memory map                                                          */
/* ------------------------------------------------------------------ */

#define OMAP4_ROM_BASE          0x40000000
#define OMAP4_ROM_SIZE          0x00010000
#define OMAP4_SRAM_BASE         0x40300000
#define OMAP4_SRAM_SIZE         0x00010000
#define OMAP4_SAR_RAM_BASE      0x4a326000
#define OMAP4_SAR_RAM_SIZE      0x00002000
#define OMAP4_DDR_BASE          0x80000000
#define GPMC_CS0_BASE           0x08000000
#define GPMC_CS0_SIZE           0x01000000

#define OMAP4_MPCORE_BASE       0x48240000
#define OMAP4_PL310_BASE        0x48242000
#define OMAP4_WKUPGEN_BASE      0x48281000
#define OMAP4_WKUPGEN_AUX_BOOT0 0x800
#define OMAP4_WKUPGEN_AUX_BOOT1 0x804

#define OMAP4_NUM_SPI           128
#define OMAP4_GIC_NUM_IRQ       (32 + OMAP4_NUM_SPI)

/* SPI interrupt numbers (TRM table 17-2) */
#define IRQ_GPT1    37
#define IRQ_GPT2    38
#define IRQ_GPT3    39
#define IRQ_GPT4    40
#define IRQ_GPT5    41
#define IRQ_GPT6    42
#define IRQ_GPT7    43
#define IRQ_GPT8    44
#define IRQ_GPT9    45
#define IRQ_GPT10   46
#define IRQ_GPT11   47
#define IRQ_GPIO1   29
#define IRQ_GPIO2   30
#define IRQ_GPIO3   31
#define IRQ_GPIO4   32
#define IRQ_GPIO5   33
#define IRQ_GPIO6   34
#define IRQ_UART1   72
#define IRQ_UART2   73
#define IRQ_UART3   74
#define IRQ_UART4   70
#define IRQ_MMC1    83
#define IRQ_MMC2    86
#define IRQ_MMC3    94
#define IRQ_MMC4    96
#define IRQ_MMC5    59
#define IRQ_I2C1    56
#define IRQ_I2C2    57
#define IRQ_I2C3    61
#define IRQ_I2C4    62

#define LOADER_LINK_ADDR        0x80050000   /* same slot as main */
#define MAIN_LINK_ADDR          0x80050000

static bool gpsmap_stub_log;
static bool gpsmap_sgx_shim = true;   /* answer the PowerVR microkernel handshakes */

/* Collapse runs of identical log lines (polling loops) */
static void gpsmap_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void gpsmap_log(const char *fmt, ...)
{
    static char last[160];
    static unsigned repeat;
    char buf[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (strcmp(buf, last) == 0) {
        repeat++;
        if (repeat == 3) {
            qemu_log("  (repeating...)\n");
        }
        return;
    }
    if (repeat >= 3) {
        qemu_log("  (last line repeated %u times)\n", repeat);
    }
    repeat = 0;
    g_strlcpy(last, buf, sizeof(last));
    qemu_log("%s", buf);
}

/* "pc=xxxxxxxx task=NAME" for the current guest context (main image RTOS) */
static const char *gpsmap_ctx(void)
{
    static char buf[64];
    uint32_t pc = 0, tcb = 0;
    char name[17] = "";

    if (current_cpu) {
        pc = ARM_CPU(current_cpu)->env.regs[15];
        /* task control blocks may live in a virtual-only mapping */
        cpu_memory_rw_debug(current_cpu, 0xA4AACAB8, &tcb, 4, false);
        if (tcb >= 0x80000000 && tcb != 0xffffffff) {
            cpu_memory_rw_debug(current_cpu, tcb + 0x7c, name, 16, false);
            name[16] = 0;
            for (char *c = name; *c; c++) {
                if (*c < 0x20 || *c > 0x7e) { *c = 0; break; }
            }
        }
    }
    snprintf(buf, sizeof(buf), "pc=%08x task=%s", pc, name);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Generic register-file stub                                          */
/*                                                                     */
/* Reads return the last written value (minus rdclear bits); writes are */
/* stored.  Individual registers may be preset by the board code.       */
/* ------------------------------------------------------------------ */

#define TYPE_GARMIN_REGSTUB "garmin.regstub"
OBJECT_DECLARE_SIMPLE_TYPE(GarminRegStub, GARMIN_REGSTUB)

struct GarminRegStub {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    char *name;
    uint32_t size;
    uint32_t rdclear;
    bool quiet;
    uint32_t *regs;
    uint32_t nregs;
    /* bits at this offset read back as 0 (self-clearing SOFTRESET etc.) */
    uint32_t selfclear_off;
    uint32_t selfclear_mask;
    /* optional per-instance read fixup */
    uint64_t (*read_fixup)(GarminRegStub *s, hwaddr addr, uint64_t val);
    /* optional per-instance write side effect */
    void (*write_hook)(GarminRegStub *s, hwaddr addr, uint64_t val);
};

static uint64_t regstub_read(void *opaque, hwaddr addr, unsigned size)
{
    GarminRegStub *s = opaque;
    uint32_t word = s->regs[(addr & ~3) >> 2] & ~s->rdclear;

    if (s->selfclear_mask && (addr & ~3) == s->selfclear_off) {
        word &= ~s->selfclear_mask;
    }
    uint64_t val = (word >> ((addr & 3) * 8)) & ((1ull << (size * 8)) - 1);

    if (s->read_fixup) {
        val = s->read_fixup(s, addr, val);
    }
    if (gpsmap_stub_log && !s->quiet) {
        qemu_log("%s: rd  %08" HWADDR_PRIx " -> %08" PRIx64 " [%s]\n",
                 s->name, (hwaddr)(s->iomem.addr + addr), val, gpsmap_ctx());
    }
    return val;
}

static void regstub_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    GarminRegStub *s = opaque;
    uint32_t idx = (addr & ~3) >> 2;
    uint32_t shift = (addr & 3) * 8;
    uint32_t mask = (size == 4) ? 0xffffffffu : (((1u << (size * 8)) - 1) << shift);

    s->regs[idx] = (s->regs[idx] & ~mask) | ((uint32_t)(val << shift) & mask);
    if (s->write_hook) {
        s->write_hook(s, addr, val);
    }
    if (gpsmap_stub_log && !s->quiet) {
        qemu_log("%s: wr  %08" HWADDR_PRIx " <- %08" PRIx64 " [%s]\n",
                 s->name, (hwaddr)(s->iomem.addr + addr), val, gpsmap_ctx());
    }
}

static const MemoryRegionOps regstub_ops = {
    .read = regstub_read,
    .write = regstub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void regstub_realize(DeviceState *dev, Error **errp)
{
    GarminRegStub *s = GARMIN_REGSTUB(dev);

    if (!s->name) {
        s->name = g_strdup("garmin.regstub");
    }
    s->nregs = s->size / 4;
    s->regs = g_new0(uint32_t, s->nregs);
    memory_region_init_io(&s->iomem, OBJECT(s), &regstub_ops, s, s->name,
                          s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const Property regstub_properties[] = {
    DEFINE_PROP_STRING("name", GarminRegStub, name),
    DEFINE_PROP_UINT32("size", GarminRegStub, size, 0x1000),
    DEFINE_PROP_UINT32("rdclear", GarminRegStub, rdclear, 0),
};

static const VMStateDescription vmstate_regstub = {
    .name = "garmin.regstub",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VARRAY_UINT32(regs, GarminRegStub, nregs, 0,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_END_OF_LIST()
    }
};

static void regstub_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_regstub;

    dc->realize = regstub_realize;
    device_class_set_props(dc, regstub_properties);
}

static const TypeInfo regstub_info = {
    .name = TYPE_GARMIN_REGSTUB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GarminRegStub),
    .class_init = regstub_class_init,
};

static GarminRegStub *make_stub(const char *name, hwaddr base, uint32_t size,
                                uint32_t rdclear)
{
    DeviceState *dev = qdev_new(TYPE_GARMIN_REGSTUB);

    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint32(dev, "size", size);
    qdev_prop_set_uint32(dev, "rdclear", rdclear);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    return GARMIN_REGSTUB(dev);
}

static inline void stub_set(GarminRegStub *s, hwaddr off, uint32_t val)
{
    s->regs[off >> 2] = val;
}

static inline void stub_selfclear(GarminRegStub *s, hwaddr off, uint32_t mask)
{
    s->selfclear_off = off;
    s->selfclear_mask = mask;
}

/* PRCM: DPLL blocks report "locked", CLKCTRL registers report IDLEST=0 */
static const uint16_t cm1_dpll_blocks[] = { 0x120, 0x160, 0x1a0, 0x1e0, 0x220 };
static const uint16_t cm2_dpll_blocks[] = { 0x140, 0x180, 0x1c0 };

static uint64_t prcm_read_fixup_common(GarminRegStub *s, hwaddr addr,
                                       uint64_t val, const uint16_t *blocks,
                                       int n)
{
    for (int i = 0; i < n; i++) {
        if (addr >= blocks[i] && addr < blocks[i] + 0x40) {
            if (addr == blocks[i] + 0x04) {
                /*
                 * CM_IDLEST_DPLL_x follows CM_CLKMODE_DPLL_x.DPLL_EN:
                 * 7 = locked (ST_DPLL_CLK), 4/5/6 = bypass (ST_MN_BYPASS),
                 * anything else = stopped.
                 */
                uint32_t mode = s->regs[blocks[i] >> 2] & 7;
                if (mode == 7) {
                    return 0x1;
                } else if (mode >= 4) {
                    return 0x100;
                }
                return 0;
            }
            return val;     /* DPLL CLKSEL etc: plain readback */
        }
    }
    /*
     * CM_<dom>_CLKSTCTRL (domain block offset 0x00): CLKTRCTRL 2 (SW_WKUP)
     * or 3 (HW_AUTO) makes every CLKACTIVITY bit (15:8) read as running.
     */
    if ((addr & 0xff) == 0) {
        if ((val & 3) >= 2) {
            val |= 0xff00;
        } else {
            val &= ~0xff00ull;
        }
        return val;
    }
    /*
     * CM_x_CLKCTRL: IDLEST (17:16) follows MODULEMODE (1:0): a disabled
     * module reports 3 (disabled), an enabled one 0 (fully functional).
     * STBYST (18) = 0.
     */
    val &= ~0x70000ull;
    if ((val & 3) == 0) {
        val |= 0x30000;
    }
    return val;
}

static uint64_t cm1_read_fixup(GarminRegStub *s, hwaddr addr, uint64_t val)
{
    return prcm_read_fixup_common(s, addr, val, cm1_dpll_blocks,
                                  ARRAY_SIZE(cm1_dpll_blocks));
}

static uint64_t cm2_read_fixup(GarminRegStub *s, hwaddr addr, uint64_t val)
{
    return prcm_read_fixup_common(s, addr, val, cm2_dpll_blocks,
                                  ARRAY_SIZE(cm2_dpll_blocks));
}

/*
 * PRM: PM_<dom>_PWRSTST (block offset +0x04) reflects the POWERSTATE
 * requested in PM_<dom>_PWRSTCTRL (+0x00): domain transitions complete
 * instantly, logic and memories report ON when the domain is ON.
 */
static uint64_t prm_read_fixup(GarminRegStub *s, hwaddr addr, uint64_t val)
{
    switch (addr) {
    case 0x0010:    /* PRM_IRQSTATUS_MPU: VC bypass ack, VP CORE/IVA tranxdone */
        return val | (1u << 12) | (1u << 21) | (1u << 29);
    case 0x0014:    /* PRM_IRQSTATUS_MPU_2: VP MPU tranxdone */
        return val | (1u << 5);
    case 0x1b44:    /* PRM_VP_CORE_STATUS */
    case 0x1b5c:    /* PRM_VP_MPU_STATUS */
    case 0x1b74:    /* PRM_VP_IVA_STATUS: VPINIDLE */
        return 1;
    case 0x1b4c:    /* PRM_VP_CORE_VOLTAGE */
    case 0x1b64:    /* PRM_VP_MPU_VOLTAGE */
    case 0x1b7c:    /* PRM_VP_IVA_VOLTAGE: report INITVOLTAGE from _CONFIG */
        return (s->regs[(addr - 0x0c) >> 2] >> 8) & 0xff;
    case 0x1ba0:    /* PRM_VC_VAL_BYPASS: VALID self-clears when done */
        return val & ~(1ull << 24);
    default:
        break;
    }
    if (addr >= 0x300 && addr < 0x1a00 && (addr & 0xff) == 0x04) {
        uint32_t ctrl = s->regs[(addr - 4) >> 2];
        uint32_t st = ctrl & 3;
        if (st == 3) {
            st |= 0x4 | 0x3f0;          /* LOGICSTATEST, mem states ON */
        }
        return st;
    }
    return val;
}

/*
 * OMAP4 TRNG at 0x48090000: OUTPUT_L/H (+0x00/+0x04) deliver random data,
 * STATUS (+0x08) bit 0 READY is always set, SYSCONFIG SOFTRESET (+0x1ff0
 * bit 0) self-clears.  The firmware polls READY, reads the output words and
 * acknowledges via INTACK (+0x10).
 */
static uint64_t trng_read_fixup(GarminRegStub *s, hwaddr addr, uint64_t val)
{
    uint32_t r;

    switch (addr) {
    case 0x00:
    case 0x04:
        qemu_guest_getrandom_nofail(&r, sizeof(r));
        return r;
    case 0x08:
        return 1;                       /* READY */
    case 0x1fe0:
        return 0x00000020;              /* REV */
    case 0x1ff0:
        return val & ~1ull;
    default:
        return val;
    }
}

/* SmartReflex: status/interrupt bits report valid sensor results */
static uint64_t sr_read_fixup(GarminRegStub *s, hwaddr addr, uint64_t val)
{
    switch (addr) {
    case 0x04:              /* SRSTATUS: all *VALID bits */
    case 0x24:              /* IRQSTATUS_RAW */
    case 0x28:              /* IRQSTATUS: MCUACCUM|MCUVALID|MCUBOUNDS|MCUDISABLEACK */
        return 0xf;
    case 0x08:              /* SENVAL: nominal sensor counts */
    case 0x0c:
    case 0x10:
    case 0x14:
        return 0x01000100;
    default:
        return val;
    }
}

/* OMAP4 McSPI: channels always report RXS|TXS|EOT */
static uint64_t mcspi_read_fixup(GarminRegStub *s, hwaddr addr, uint64_t val)
{
    if (addr == 0x114) {
        return 1;                   /* SYSSTATUS RESETDONE */
    }
    if (addr >= 0x130 && addr < 0x130 + 4 * 0x14 &&
        ((addr - 0x130) % 0x14) == 0) {
        return val | 0x7;           /* CHxSTAT */
    }
    return val;
}

/* ------------------------------------------------------------------ */
/* OMAP4 32 kHz sync counter                                           */
/* ------------------------------------------------------------------ */

#define TYPE_GARMIN_OMAP4_32K "garmin.omap4-32ksync"
OBJECT_DECLARE_SIMPLE_TYPE(Omap4Sync32k, GARMIN_OMAP4_32K)

struct Omap4Sync32k {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
};

static uint64_t sync32k_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case 0x00:
        return 0x00000040;      /* REV */
    case 0x04:
        return 0x00000000;      /* SYSCONFIG */
    case 0x10:
        return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 32768,
                        NANOSECONDS_PER_SECOND) & 0xffffffffu;
    default:
        return 0;
    }
}

static void sync32k_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
}

static const MemoryRegionOps sync32k_ops = {
    .read = sync32k_read,
    .write = sync32k_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void sync32k_init(Object *obj)
{
    Omap4Sync32k *s = GARMIN_OMAP4_32K(obj);

    memory_region_init_io(&s->iomem, obj, &sync32k_ops, s, "omap4.32ksync",
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const TypeInfo sync32k_info = {
    .name = TYPE_GARMIN_OMAP4_32K,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Omap4Sync32k),
    .instance_init = sync32k_init,
};

/* ------------------------------------------------------------------ */
/* OMAP4 general purpose timer (GPTIMER), both register layouts          */
/* ------------------------------------------------------------------ */

#define TYPE_GARMIN_OMAP4_GPTIMER "garmin.omap4-gptimer"
OBJECT_DECLARE_SIMPLE_TYPE(Omap4GPTimer, GARMIN_OMAP4_GPTIMER)

/* logical register ids */
enum {
    GPT_TIDR, GPT_TIOCP_CFG, GPT_TISTAT, GPT_IRQ_EOI, GPT_IRQSTATUS_RAW,
    GPT_IRQSTATUS, GPT_IRQENABLE_SET, GPT_IRQENABLE_CLR, GPT_IRQWAKEEN,
    GPT_TCLR, GPT_TCRR, GPT_TLDR, GPT_TTGR, GPT_TWPS, GPT_TMAR, GPT_TCAR1,
    GPT_TSICR, GPT_TCAR2, GPT_TPIR, GPT_TNIR, GPT_TCVR, GPT_TOCR, GPT_TOWR,
    GPT_UNKNOWN
};

#define TCLR_ST   (1 << 0)
#define TCLR_AR   (1 << 1)
#define TCLR_PTV(x) (((x) >> 2) & 7)
#define TCLR_PRE  (1 << 5)
#define TCLR_CE   (1 << 6)

#define GPT_IRQ_MAT (1 << 0)
#define GPT_IRQ_OVF (1 << 1)
#define GPT_IRQ_TCAR (1 << 2)

struct Omap4GPTimer {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;
    bool legacy;
    uint32_t freq;
    char *name;

    uint32_t tiocp_cfg;
    uint32_t irqstatus;
    uint32_t irqenable;
    uint32_t irqwakeen;
    uint32_t tclr;
    uint32_t tldr;
    uint32_t tmar;
    uint32_t tsicr;
    uint32_t tpir, tnir, tcvr, tocr, towr;

    /* counter state: value at last_ns */
    uint32_t tcrr_base;
    int64_t last_ns;
};

static int gpt_decode(Omap4GPTimer *s, hwaddr addr)
{
    if (s->legacy) {
        switch (addr) {
        case 0x00: return GPT_TIDR;
        case 0x10: return GPT_TIOCP_CFG;
        case 0x14: return GPT_TISTAT;
        case 0x18: return GPT_IRQSTATUS;      /* TISR */
        case 0x1c: return GPT_IRQENABLE_SET;  /* TIER (rw) */
        case 0x20: return GPT_IRQWAKEEN;      /* TWER */
        case 0x24: return GPT_TCLR;
        case 0x28: return GPT_TCRR;
        case 0x2c: return GPT_TLDR;
        case 0x30: return GPT_TTGR;
        case 0x34: return GPT_TWPS;
        case 0x38: return GPT_TMAR;
        case 0x3c: return GPT_TCAR1;
        case 0x40: return GPT_TSICR;
        case 0x44: return GPT_TCAR2;
        case 0x48: return GPT_TPIR;
        case 0x4c: return GPT_TNIR;
        case 0x50: return GPT_TCVR;
        case 0x54: return GPT_TOCR;
        case 0x58: return GPT_TOWR;
        }
    } else {
        switch (addr) {
        case 0x00: return GPT_TIDR;
        case 0x10: return GPT_TIOCP_CFG;
        case 0x20: return GPT_IRQ_EOI;
        case 0x24: return GPT_IRQSTATUS_RAW;
        case 0x28: return GPT_IRQSTATUS;
        case 0x2c: return GPT_IRQENABLE_SET;
        case 0x30: return GPT_IRQENABLE_CLR;
        case 0x34: return GPT_IRQWAKEEN;
        case 0x38: return GPT_TCLR;
        case 0x3c: return GPT_TCRR;
        case 0x40: return GPT_TLDR;
        case 0x44: return GPT_TTGR;
        case 0x48: return GPT_TWPS;
        case 0x4c: return GPT_TMAR;
        case 0x50: return GPT_TCAR1;
        case 0x54: return GPT_TSICR;
        case 0x58: return GPT_TCAR2;
        }
    }
    return GPT_UNKNOWN;
}

static uint64_t gpt_rate(Omap4GPTimer *s)
{
    uint64_t rate = s->freq;

    if (s->tclr & TCLR_PRE) {
        rate >>= (TCLR_PTV(s->tclr) + 1);
    }
    return rate ? rate : 1;
}

static void gpt_update_irq(Omap4GPTimer *s)
{
    bool level = (s->irqstatus & s->irqenable & 7) != 0;

    if (gpsmap_stub_log && level) {
        gpsmap_log("%s: irq status=%x\n", s->name, s->irqstatus);
    }
    qemu_set_irq(s->irq, level);
}

/* Current counter value, folding in elapsed time (does not touch state) */
static uint32_t gpt_current(Omap4GPTimer *s, int64_t now)
{
    if (!(s->tclr & TCLR_ST)) {
        return s->tcrr_base;
    }
    uint64_t ticks = muldiv64(now - s->last_ns, gpt_rate(s),
                              NANOSECONDS_PER_SECOND);
    uint64_t v = (uint64_t)s->tcrr_base + ticks;
    if (v <= 0xffffffffull) {
        return (uint32_t)v;
    }
    if (!(s->tclr & TCLR_AR)) {
        return 0xffffffffu;
    }
    uint64_t period = 0x100000000ull - s->tldr;
    return (uint32_t)(s->tldr + ((v - 0x100000000ull) % period));
}

static void gpt_schedule(Omap4GPTimer *s)
{
    if (!(s->tclr & TCLR_ST)) {
        timer_del(s->timer);
        return;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t cur = gpt_current(s, now);
    uint64_t rate = gpt_rate(s);
    uint64_t to_ovf = 0x100000000ull - cur;
    uint64_t next = to_ovf;

    if ((s->tclr & TCLR_CE) && s->tmar > cur) {
        uint64_t to_mat = (uint64_t)s->tmar - cur;
        if (to_mat < next) {
            next = to_mat;
        }
    }
    if (next == 0) {
        next = 1;
    }
    /* resync base to avoid drift accumulation */
    s->tcrr_base = cur;
    s->last_ns = now;
    timer_mod(s->timer, now + muldiv64(next, NANOSECONDS_PER_SECOND, rate));
}

static void gpt_tick(void *opaque)
{
    Omap4GPTimer *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t ticks = muldiv64(now - s->last_ns, gpt_rate(s),
                              NANOSECONDS_PER_SECOND);
    uint64_t v = (uint64_t)s->tcrr_base + ticks;

    if ((s->tclr & TCLR_CE) && s->tcrr_base <= s->tmar && v >= s->tmar) {
        s->irqstatus |= GPT_IRQ_MAT;
    }
    if (v > 0xffffffffull) {
        s->irqstatus |= GPT_IRQ_OVF;
        if (s->tclr & TCLR_AR) {
            uint64_t period = 0x100000000ull - s->tldr;
            s->tcrr_base = (uint32_t)(s->tldr + ((v - 0x100000000ull) % period));
        } else {
            s->tcrr_base = 0xffffffffu;
            s->tclr &= ~TCLR_ST;
        }
    } else {
        s->tcrr_base = (uint32_t)v;
    }
    s->last_ns = now;
    gpt_update_irq(s);
    gpt_schedule(s);
}

static uint64_t gpt_read(void *opaque, hwaddr addr, unsigned size)
{
    Omap4GPTimer *s = opaque;

    switch (gpt_decode(s, addr)) {
    case GPT_TIDR:          return s->legacy ? 0x00000021 : 0x40000100;
    case GPT_TIOCP_CFG:     return s->tiocp_cfg & ~1u;   /* SOFTRESET self-clears */
    case GPT_TISTAT:        return 1;                    /* RESETDONE */
    case GPT_IRQ_EOI:       return 0;
    case GPT_IRQSTATUS_RAW: return s->irqstatus;
    case GPT_IRQSTATUS:     return s->irqstatus & (s->legacy ? 0xffffffffu : s->irqenable);
    case GPT_IRQENABLE_SET:
    case GPT_IRQENABLE_CLR: return s->irqenable;
    case GPT_IRQWAKEEN:     return s->irqwakeen;
    case GPT_TCLR:          return s->tclr;
    case GPT_TCRR:          return gpt_current(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    case GPT_TLDR:          return s->tldr;
    case GPT_TTGR:          return 0xffffffffu;
    case GPT_TWPS:          return 0;                    /* no pending writes */
    case GPT_TMAR:          return s->tmar;
    case GPT_TCAR1:
    case GPT_TCAR2:         return 0;
    case GPT_TSICR:         return s->tsicr;
    case GPT_TPIR:          return s->tpir;
    case GPT_TNIR:          return s->tnir;
    case GPT_TCVR:          return s->tcvr;
    case GPT_TOCR:          return s->tocr;
    case GPT_TOWR:          return s->towr;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @0x%" HWADDR_PRIx "\n",
                      s->name, addr);
        return 0;
    }
}

static void gpt_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Omap4GPTimer *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (gpsmap_stub_log) {
        gpsmap_log("%s: wr  %08" HWADDR_PRIx " <- %08" PRIx64 " [%s]\n", s->name,
                   (hwaddr)(s->iomem.addr + addr), val, gpsmap_ctx());
    }

    switch (gpt_decode(s, addr)) {
    case GPT_TIOCP_CFG:
        s->tiocp_cfg = val;
        if (val & 1) {          /* SOFTRESET */
            s->tclr = 0; s->tcrr_base = 0; s->tldr = 0; s->tmar = 0;
            s->irqstatus = 0; s->irqenable = 0;
            timer_del(s->timer);
            gpt_update_irq(s);
        }
        break;
    case GPT_IRQ_EOI:
        break;
    case GPT_IRQSTATUS_RAW:     /* write 1 sets (debug) */
        s->irqstatus |= val & 7;
        gpt_update_irq(s);
        break;
    case GPT_IRQSTATUS:         /* write 1 to clear */
        s->irqstatus &= ~val;
        gpt_update_irq(s);
        break;
    case GPT_IRQENABLE_SET:
        if (s->legacy) {
            s->irqenable = val & 7;     /* TIER is plain rw */
        } else {
            s->irqenable |= val & 7;
        }
        gpt_update_irq(s);
        break;
    case GPT_IRQENABLE_CLR:
        s->irqenable &= ~val;
        gpt_update_irq(s);
        break;
    case GPT_IRQWAKEEN:
        s->irqwakeen = val;
        break;
    case GPT_TCLR: {
        uint32_t cur = gpt_current(s, now);
        s->tcrr_base = cur;
        s->last_ns = now;
        s->tclr = val;
        gpt_schedule(s);
        break;
    }
    case GPT_TCRR:
        s->tcrr_base = val;
        s->last_ns = now;
        gpt_schedule(s);
        break;
    case GPT_TLDR:
        s->tldr = val;
        break;
    case GPT_TTGR:
        s->tcrr_base = s->tldr;
        s->last_ns = now;
        gpt_schedule(s);
        break;
    case GPT_TMAR:
        s->tmar = val;
        gpt_schedule(s);
        break;
    case GPT_TSICR:
        s->tsicr = val & ~2u;
        break;
    case GPT_TPIR: s->tpir = val; break;
    case GPT_TNIR: s->tnir = val; break;
    case GPT_TCVR: s->tcvr = val; break;
    case GPT_TOCR: s->tocr = val; break;
    case GPT_TOWR: s->towr = val; break;
    case GPT_TIDR:
    case GPT_TISTAT:
    case GPT_TWPS:
    case GPT_TCAR1:
    case GPT_TCAR2:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @0x%" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", s->name, addr, val);
    }
}

static const MemoryRegionOps gpt_ops = {
    .read = gpt_read,
    .write = gpt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void gpt_reset(DeviceState *dev)
{
    Omap4GPTimer *s = GARMIN_OMAP4_GPTIMER(dev);

    s->tiocp_cfg = 0; s->irqstatus = 0; s->irqenable = 0; s->irqwakeen = 0;
    s->tclr = 0; s->tldr = 0; s->tmar = 0; s->tsicr = 0;
    s->tcrr_base = 0; s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(s->timer);
}

static void gpt_realize(DeviceState *dev, Error **errp)
{
    Omap4GPTimer *s = GARMIN_OMAP4_GPTIMER(dev);

    if (!s->name) {
        s->name = g_strdup("omap4.gptimer");
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gpt_tick, s);
    memory_region_init_io(&s->iomem, OBJECT(s), &gpt_ops, s, s->name, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const Property gpt_properties[] = {
    DEFINE_PROP_BOOL("legacy", Omap4GPTimer, legacy, false),
    DEFINE_PROP_UINT32("freq", Omap4GPTimer, freq, 32768),
    DEFINE_PROP_STRING("name", Omap4GPTimer, name),
};

static const VMStateDescription vmstate_gpt = {
    .name = "omap4.gptimer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, Omap4GPTimer),
        VMSTATE_UINT32(tiocp_cfg, Omap4GPTimer),
        VMSTATE_UINT32(irqstatus, Omap4GPTimer),
        VMSTATE_UINT32(irqenable, Omap4GPTimer),
        VMSTATE_UINT32(irqwakeen, Omap4GPTimer),
        VMSTATE_UINT32(tclr, Omap4GPTimer),
        VMSTATE_UINT32(tldr, Omap4GPTimer),
        VMSTATE_UINT32(tmar, Omap4GPTimer),
        VMSTATE_UINT32(tsicr, Omap4GPTimer),
        VMSTATE_UINT32(tpir, Omap4GPTimer),
        VMSTATE_UINT32(tnir, Omap4GPTimer),
        VMSTATE_UINT32(tcvr, Omap4GPTimer),
        VMSTATE_UINT32(tocr, Omap4GPTimer),
        VMSTATE_UINT32(towr, Omap4GPTimer),
        VMSTATE_UINT32(tcrr_base, Omap4GPTimer),
        VMSTATE_INT64(last_ns, Omap4GPTimer),
        VMSTATE_END_OF_LIST()
    }
};

static void gpt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gpt;

    dc->realize = gpt_realize;
    device_class_set_legacy_reset(dc, gpt_reset);
    device_class_set_props(dc, gpt_properties);
}

static const TypeInfo gpt_info = {
    .name = TYPE_GARMIN_OMAP4_GPTIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Omap4GPTimer),
    .class_init = gpt_class_init,
};

static DeviceState *make_gpt(const char *name, hwaddr base, bool legacy,
                             uint32_t freq, qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_GARMIN_OMAP4_GPTIMER);

    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_bit(dev, "legacy", legacy);
    qdev_prop_set_uint32(dev, "freq", freq);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
    return dev;
}

/* ------------------------------------------------------------------ */
/* OMAP4 UART: 16750-compatible core plus OMAP extension registers        */
/* ------------------------------------------------------------------ */

#define TYPE_GARMIN_OMAP4_UART "garmin.omap4-uart"
OBJECT_DECLARE_SIMPLE_TYPE(Omap4Uart, GARMIN_OMAP4_UART)

struct Omap4Uart {
    SysBusDevice parent_obj;
    MemoryRegion container;
    MemoryRegion ext;
    SerialMM *serial;
    Chardev *chr;
    qemu_irq irq;
    uint32_t regs[0x40];   /* 0x20..0xff */
};

static uint64_t omap4_uart_ext_read(void *opaque, hwaddr addr, unsigned size)
{
    Omap4Uart *s = opaque;
    hwaddr off = addr + 0x20;

    switch (off) {
    case 0x44: return 0;            /* SSR: TX FIFO not full, no RX CTS */
    case 0x50: return 0x0601;       /* MVR */
    case 0x58: return 1;            /* SYSS: RESETDONE */
    case 0x64: return 0;            /* RXFIFO_LVL */
    case 0x68: return 0;            /* TXFIFO_LVL */
    default:   return s->regs[addr >> 2];
    }
}

static void omap4_uart_ext_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    Omap4Uart *s = opaque;

    s->regs[addr >> 2] = val;
}

static const MemoryRegionOps omap4_uart_ext_ops = {
    .read = omap4_uart_ext_read,
    .write = omap4_uart_ext_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void omap4_uart_realize(DeviceState *dev, Error **errp)
{
    Omap4Uart *s = GARMIN_OMAP4_UART(dev);

    memory_region_init(&s->container, OBJECT(s), "omap4.uart", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->container);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    memory_region_init_io(&s->ext, OBJECT(s), &omap4_uart_ext_ops, s,
                          "omap4.uart.ext", 0xe0);
    memory_region_add_subregion(&s->container, 0x20, &s->ext);
}

static const VMStateDescription vmstate_omap4_uart = {
    .name = "omap4.uart.ext",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Omap4Uart, 0x40),
        VMSTATE_END_OF_LIST()
    }
};

static void omap4_uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_omap4_uart;

    dc->realize = omap4_uart_realize;
}

static const TypeInfo omap4_uart_info = {
    .name = TYPE_GARMIN_OMAP4_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Omap4Uart),
    .class_init = omap4_uart_class_init,
};

static void make_uart(hwaddr base, qemu_irq irq, Chardev *chr)
{
    DeviceState *dev = qdev_new(TYPE_GARMIN_OMAP4_UART);
    Omap4Uart *s = GARMIN_OMAP4_UART(dev);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
    /* 16750 core at offset 0 with 4-byte register stride */
    s->serial = serial_mm_init(&s->container, 0, 2, irq, 48000000 / 16, chr,
                               DEVICE_LITTLE_ENDIAN);
}

/* ------------------------------------------------------------------ */
/* OMAP4 GPIO bank                                                       */
/* ------------------------------------------------------------------ */

#define TYPE_GARMIN_OMAP4_GPIO "garmin.omap4-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Omap4Gpio, GARMIN_OMAP4_GPIO)

struct Omap4Gpio {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    char *name;
    uint32_t datain;        /* property: default input level */
    uint32_t sysconfig, ctrl, oe, dataout;
    uint32_t irqstatus[2], irqenable[2], irqwaken[2];
    uint32_t leveldetect0, leveldetect1, risingdetect, fallingdetect;
    uint32_t debounceenable, debouncingtime;
};

static uint32_t omap4_gpio_input(Omap4Gpio *s)
{
    /* pins driven as outputs read back their DATAOUT value */
    return (s->datain & s->oe) | (s->dataout & ~s->oe);
}

/*
 * Level-sensitive detection on the current input state (edge detection
 * would need input changes, which the board model does not generate yet).
 * Real hardware fires immediately for a level that is already present,
 * which is how boot-time "power good"/"present" lines wake tasks.
 */
static void omap4_gpio_update_irq(Omap4Gpio *s)
{
    uint32_t in = omap4_gpio_input(s);
    uint32_t level = (s->leveldetect0 & ~in) | (s->leveldetect1 & in);
    uint32_t newpend = level & (s->irqenable[0] | s->irqenable[1]) & ~s->irqstatus[0];

    if (newpend && gpsmap_stub_log) {
        gpsmap_log("%s: level irq pending %08x (in=%08x ld0=%08x ld1=%08x)\n",
                   s->name, newpend, in, s->leveldetect0, s->leveldetect1);
    }
    s->irqstatus[0] |= level & s->irqenable[0];
    s->irqstatus[1] |= level & s->irqenable[1];
    qemu_set_irq(s->irq, (s->irqstatus[0] & s->irqenable[0]) != 0);
}

/* Board devices drive input pins through qdev GPIO-in lines (index = pin) */
static void omap4_gpio_set_pin(void *opaque, int pin, int level)
{
    Omap4Gpio *s = opaque;
    uint32_t bit = 1u << pin;
    uint32_t old = s->datain;

    s->datain = level ? (old | bit) : (old & ~bit);
    if (s->datain != old) {
        if (gpsmap_stub_log) {
            gpsmap_log("%s: pin %d -> %d\n", s->name, pin, level);
        }
        omap4_gpio_update_irq(s);
    }
}

static uint64_t omap4_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    Omap4Gpio *s = opaque;

    switch (addr) {
    case 0x000: return 0x50600801;     /* REVISION */
    case 0x010: return s->sysconfig;
    case 0x020: return 0;              /* EOI */
    case 0x024: return s->irqstatus[0];
    case 0x028: return s->irqstatus[1];
    case 0x02c: return s->irqstatus[0] & s->irqenable[0];
    case 0x030: return s->irqstatus[1] & s->irqenable[1];
    case 0x034: case 0x03c: return s->irqenable[0];
    case 0x038: case 0x040: return s->irqenable[1];
    case 0x044: return s->irqwaken[0];
    case 0x048: return s->irqwaken[1];
    case 0x114: return 1;              /* SYSSTATUS RESETDONE */
    case 0x130: return s->ctrl;
    case 0x134: return s->oe;
    case 0x138: return omap4_gpio_input(s);
    case 0x13c: return s->dataout;
    case 0x140: return s->leveldetect0;
    case 0x144: return s->leveldetect1;
    case 0x148: return s->risingdetect;
    case 0x14c: return s->fallingdetect;
    case 0x150: return s->debounceenable;
    case 0x154: return s->debouncingtime;
    case 0x190: case 0x194: return s->dataout;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @0x%" HWADDR_PRIx "\n",
                      s->name, addr);
        return 0;
    }
}

static void omap4_gpio_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Omap4Gpio *s = opaque;

    if (gpsmap_stub_log && addr != 0x190 && addr != 0x194 && addr != 0x13c) {
        gpsmap_log("%s: wr  %08" HWADDR_PRIx " <- %08" PRIx64 " [%s]\n", s->name,
                   (hwaddr)(s->iomem.addr + addr), val, gpsmap_ctx());
    }
    switch (addr) {
    case 0x010: s->sysconfig = val & ~2u; break;
    case 0x020: break;
    case 0x024: s->irqstatus[0] |= val; omap4_gpio_update_irq(s); break;
    case 0x028: s->irqstatus[1] |= val; break;
    case 0x02c: s->irqstatus[0] &= ~val; omap4_gpio_update_irq(s); break;
    case 0x030: s->irqstatus[1] &= ~val; break;
    case 0x034: s->irqenable[0] |= val; omap4_gpio_update_irq(s); break;
    case 0x038: s->irqenable[1] |= val; omap4_gpio_update_irq(s); break;
    case 0x03c: s->irqenable[0] &= ~val; omap4_gpio_update_irq(s); break;
    case 0x040: s->irqenable[1] &= ~val; break;
    case 0x044: s->irqwaken[0] = val; break;
    case 0x048: s->irqwaken[1] = val; break;
    case 0x130: s->ctrl = val; break;
    case 0x134: s->oe = val; omap4_gpio_update_irq(s); break;
    case 0x13c: s->dataout = val; omap4_gpio_update_irq(s); break;
    case 0x140: s->leveldetect0 = val; omap4_gpio_update_irq(s); break;
    case 0x144: s->leveldetect1 = val; omap4_gpio_update_irq(s); break;
    case 0x148: s->risingdetect = val; omap4_gpio_update_irq(s); break;
    case 0x14c: s->fallingdetect = val; omap4_gpio_update_irq(s); break;
    case 0x150: s->debounceenable = val; break;
    case 0x154: s->debouncingtime = val; break;
    case 0x190: s->dataout &= ~val; omap4_gpio_update_irq(s); break;
    case 0x194: s->dataout |= val; omap4_gpio_update_irq(s); break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @0x%" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", s->name, addr, val);
    }
}

static const MemoryRegionOps omap4_gpio_ops = {
    .read = omap4_gpio_read,
    .write = omap4_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void omap4_gpio_reset(DeviceState *dev)
{
    Omap4Gpio *s = GARMIN_OMAP4_GPIO(dev);

    s->sysconfig = 0; s->ctrl = 0; s->oe = 0xffffffffu; s->dataout = 0;
    memset(s->irqstatus, 0, sizeof(s->irqstatus));
    memset(s->irqenable, 0, sizeof(s->irqenable));
    memset(s->irqwaken, 0, sizeof(s->irqwaken));
    s->leveldetect0 = s->leveldetect1 = s->risingdetect = s->fallingdetect = 0;
    s->debounceenable = s->debouncingtime = 0;
}

static void omap4_gpio_realize(DeviceState *dev, Error **errp)
{
    Omap4Gpio *s = GARMIN_OMAP4_GPIO(dev);

    if (!s->name) {
        s->name = g_strdup("omap4.gpio");
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &omap4_gpio_ops, s, s->name,
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    qdev_init_gpio_in(dev, omap4_gpio_set_pin, 32);
}

static const Property omap4_gpio_properties[] = {
    DEFINE_PROP_UINT32("datain", Omap4Gpio, datain, 0),
    DEFINE_PROP_STRING("name", Omap4Gpio, name),
};

static const VMStateDescription vmstate_omap4_gpio = {
    .name = "omap4.gpio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(datain, Omap4Gpio),
        VMSTATE_UINT32(sysconfig, Omap4Gpio),
        VMSTATE_UINT32(ctrl, Omap4Gpio),
        VMSTATE_UINT32(oe, Omap4Gpio),
        VMSTATE_UINT32(dataout, Omap4Gpio),
        VMSTATE_UINT32_ARRAY(irqstatus, Omap4Gpio, 2),
        VMSTATE_UINT32_ARRAY(irqenable, Omap4Gpio, 2),
        VMSTATE_UINT32_ARRAY(irqwaken, Omap4Gpio, 2),
        VMSTATE_UINT32(leveldetect0, Omap4Gpio),
        VMSTATE_UINT32(leveldetect1, Omap4Gpio),
        VMSTATE_UINT32(risingdetect, Omap4Gpio),
        VMSTATE_UINT32(fallingdetect, Omap4Gpio),
        VMSTATE_UINT32(debounceenable, Omap4Gpio),
        VMSTATE_UINT32(debouncingtime, Omap4Gpio),
        VMSTATE_END_OF_LIST()
    }
};

static void omap4_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_omap4_gpio;

    dc->realize = omap4_gpio_realize;
    device_class_set_legacy_reset(dc, omap4_gpio_reset);
    device_class_set_props(dc, omap4_gpio_properties);
}

static const TypeInfo omap4_gpio_info = {
    .name = TYPE_GARMIN_OMAP4_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Omap4Gpio),
    .class_init = omap4_gpio_class_init,
};

static DeviceState *make_gpio(const char *name, hwaddr base, qemu_irq irq,
                              uint32_t datain)
{
    DeviceState *dev = qdev_new(TYPE_GARMIN_OMAP4_GPIO);

    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint32(dev, "datain", datain);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
    return dev;
}

/* ------------------------------------------------------------------ */
/* OMAP4 I2C controller (polled master transfers with a QEMU I2C bus)     */
/* ------------------------------------------------------------------ */

#define TYPE_GARMIN_OMAP4_I2C "garmin.omap4-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(Omap4I2C, GARMIN_OMAP4_I2C)

#define I2C_STAT_AL     (1 << 0)
#define I2C_STAT_NACK   (1 << 1)
#define I2C_STAT_ARDY   (1 << 2)
#define I2C_STAT_RRDY   (1 << 3)
#define I2C_STAT_XRDY   (1 << 4)
#define I2C_STAT_BB     (1 << 12)
#define I2C_STAT_RDR    (1 << 13)
#define I2C_STAT_XDR    (1 << 14)

#define I2C_CON_STT     (1 << 0)
#define I2C_CON_STP     (1 << 1)
#define I2C_CON_TRX     (1 << 9)
#define I2C_CON_MST     (1 << 10)
#define I2C_CON_EN      (1 << 15)

#define I2C_FIFO_SIZE   64

struct Omap4I2C {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;
    char *name;

    uint32_t sysc, ie, stat, we, buf, cnt, con, oa, sa, psc, scll, sclh, systest;
    uint8_t fifo[I2C_FIFO_SIZE];
    int fifo_len, fifo_pos;
    int tx_remaining;
    bool tx_active;
    bool rx_active;
    /*
     * Real transfers take ~100 us per byte; completing them synchronously
     * inside the DATA write raises the interrupt while the driver still has
     * IRQs masked and then clears the status itself, so the interrupt is
     * lost.  Completion (ARDY, RRDY, STT clear) is therefore deferred.
     */
    QEMUTimer *done_timer;
    uint32_t pend_stat;
    bool pend_finish;
    bool irq_level;
};

/* Level-type status bits re-assert while their condition holds */
static void omap4_i2c_refresh(Omap4I2C *s)
{
    if (s->tx_active && s->tx_remaining > 0 && !s->pend_finish) {
        s->stat |= I2C_STAT_XRDY;
    }
    if (s->rx_active && s->fifo_pos < s->fifo_len && !s->pend_finish &&
        !(s->pend_stat & I2C_STAT_RRDY)) {
        s->stat |= I2C_STAT_RRDY;
    }
}

static void omap4_i2c_update_irq(Omap4I2C *s)
{
    bool level;

    omap4_i2c_refresh(s);
    level = (s->stat & s->ie) != 0;
    if (gpsmap_stub_log && level != s->irq_level) {
        gpsmap_log("%s: irq %d (stat=%04x ie=%04x con=%04x)\n", s->name,
                   level, s->stat, s->ie, s->con);
    }
    s->irq_level = level;
    qemu_set_irq(s->irq, level);
}

#define I2C_BYTE_NS 100000   /* ~100 us per byte at 100 kHz */

static void omap4_i2c_update_irq(Omap4I2C *s);

static void omap4_i2c_done_cb(void *opaque)
{
    Omap4I2C *s = opaque;

    if (s->pend_finish) {
        s->tx_active = s->rx_active = false;
        s->stat &= ~(I2C_STAT_XRDY | I2C_STAT_BB | I2C_STAT_XDR | I2C_STAT_RDR);
        /*
         * BF (bus free) only after a STOP condition.  The firmware's ISR
         * signals its completion semaphore on BF unconditionally and on ARDY
         * only for "write without STOP" transfers, so a BF after a STOP-less
         * write would leave a stale semaphore count and the following
         * receive would return before its data arrived.
         */
        if (s->con & I2C_CON_STP) {
            s->stat |= 1 << 8;
        }
        s->con &= ~(I2C_CON_STT | I2C_CON_STP);
        s->pend_finish = false;
        if (gpsmap_stub_log) {
            gpsmap_log("%s: transfer done sa=0x%02x\n", s->name, s->sa);
        }
    }
    s->stat |= s->pend_stat;
    s->pend_stat = 0;
    omap4_i2c_update_irq(s);
}

static void omap4_i2c_defer(Omap4I2C *s, uint32_t stat_bits, bool finish, int bytes)
{
    s->pend_stat |= stat_bits;
    s->pend_finish |= finish;
    timer_mod(s->done_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)I2C_BYTE_NS * (bytes ? bytes : 1));
}

static void omap4_i2c_finish(Omap4I2C *s)
{
    i2c_end_transfer(s->bus);
    /* keep tx/rx active until the deferred completion so polls see BB */
    omap4_i2c_defer(s, I2C_STAT_ARDY, true, 1);
}

static void omap4_i2c_tx_byte(Omap4I2C *s, uint8_t byte)
{
    if (gpsmap_stub_log) {
        qemu_log("%s: tx sa=0x%02x byte=0x%02x\n", s->name, s->sa, byte);
    }
    if (i2c_send(s->bus, byte)) {
        s->stat |= I2C_STAT_NACK;
    }
    if (s->tx_remaining > 0) {
        s->tx_remaining--;
    }
    if (s->tx_remaining == 0) {
        omap4_i2c_finish(s);
    }
}

static void omap4_i2c_start(Omap4I2C *s)
{
    bool recv = !(s->con & I2C_CON_TRX);
    uint8_t addr = s->sa & 0x7f;

    s->stat |= I2C_STAT_BB;
    if (i2c_start_transfer(s->bus, addr, recv)) {
        if (gpsmap_stub_log) {
            qemu_log("%s: NACK sa=0x%02x (%s)\n", s->name, addr,
                     recv ? "read" : "write");
        }
        s->fifo_len = s->fifo_pos = 0;
        omap4_i2c_defer(s, I2C_STAT_NACK | I2C_STAT_ARDY, true, 1);
        return;
    }
    if (recv) {
        int n = MIN((int)(s->cnt ? s->cnt : 1), I2C_FIFO_SIZE);
        s->rx_active = true;
        s->fifo_len = 0; s->fifo_pos = 0;
        for (int i = 0; i < n; i++) {
            s->fifo[s->fifo_len++] = i2c_recv(s->bus);
        }
        if (gpsmap_stub_log) {
            qemu_log("%s: rx sa=0x%02x n=%d first=0x%02x\n", s->name, addr, n,
                     s->fifo[0]);
        }
        omap4_i2c_defer(s, I2C_STAT_RRDY, false, n);
    } else {
        s->tx_active = true;
        s->tx_remaining = s->cnt;
        /* bytes already queued in the FIFO before STT */
        while (s->fifo_pos < s->fifo_len && s->tx_active) {
            omap4_i2c_tx_byte(s, s->fifo[s->fifo_pos++]);
        }
        s->fifo_len = s->fifo_pos = 0;
        if (s->tx_active) {
            if (s->tx_remaining == 0) {
                omap4_i2c_finish(s);
            } else {
                s->stat |= I2C_STAT_XRDY;
            }
        }
    }
}

static uint64_t omap4_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    Omap4I2C *s = opaque;
    uint64_t val;

    omap4_i2c_refresh(s);
    switch (addr) {
    case 0x00: val = 0x0a; break;               /* REVNB_LO */
    case 0x04: val = 0x50; break;               /* REVNB_HI */
    case 0x10: val = s->sysc & ~2u; break;
    case 0x24: val = s->stat; break;            /* IRQSTATUS_RAW */
    case 0x28: val = s->stat & s->ie; break;    /* IRQSTATUS */
    case 0x2c: case 0x30: val = s->ie; break;
    case 0x34: val = s->we; break;
    case 0x84: val = s->ie; break;              /* IE (legacy) */
    case 0x88: val = s->stat; break;            /* STAT (legacy) */
    case 0x90: val = 1; break;                  /* SYSS RDONE */
    case 0x94: val = s->buf; break;
    case 0x98: val = s->cnt; break;
    case 0x9c:                                  /* DATA */
        if (s->rx_active && s->fifo_pos < s->fifo_len) {
            val = s->fifo[s->fifo_pos++];
            if (s->fifo_pos >= s->fifo_len) {
                omap4_i2c_finish(s);
            }
        } else {
            val = 0;
        }
        break;
    case 0xa4: val = s->con; break;
    case 0xa8: val = s->oa; break;
    case 0xac: val = s->sa; break;
    case 0xb0: val = s->psc; break;
    case 0xb4: val = s->scll; break;
    case 0xb8: val = s->sclh; break;
    case 0xbc: val = s->systest; break;
    case 0xc0:                                  /* BUFSTAT */
        val = ((s->rx_active ? (s->fifo_len - s->fifo_pos) : 0) & 0x3f) << 8;
        val |= (s->tx_active ? s->tx_remaining : 0) & 0x3f;
        val |= 0x2 << 14;                       /* FIFODEPTH: 64 bytes */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @0x%" HWADDR_PRIx "\n",
                      s->name, addr);
        val = 0;
    }
    if (gpsmap_stub_log && addr != 0x24 && addr != 0x28) {
        gpsmap_log("%s: rd  %08" HWADDR_PRIx " -> %08" PRIx64 " [%s]\n", s->name,
                   (hwaddr)(s->iomem.addr + addr), val, gpsmap_ctx());
    }
    return val;
}

static void omap4_i2c_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    Omap4I2C *s = opaque;

    if (gpsmap_stub_log) {
        gpsmap_log("%s: wr  %08" HWADDR_PRIx " <- %08" PRIx64 " [%s]\n", s->name,
                   (hwaddr)(s->iomem.addr + addr), val, gpsmap_ctx());
    }
    switch (addr) {
    case 0x10:
        s->sysc = val;
        if (val & 2) {                          /* SOFTRESET */
            s->stat = 0; s->con = 0; s->ie = 0; s->cnt = 0;
            s->fifo_len = s->fifo_pos = 0;
            s->tx_active = s->rx_active = false;
            s->pend_stat = 0; s->pend_finish = false;
            timer_del(s->done_timer);
        }
        break;
    case 0x24: s->stat |= val; break;
    case 0x28: case 0x88: s->stat &= ~val; break;       /* W1C */
    case 0x2c: s->ie |= val; break;
    case 0x30: s->ie &= ~val; break;
    case 0x34: s->we = val; break;
    case 0x84: s->ie = val; break;
    case 0x94:
        s->buf = val & ~0x4040u;
        if (val & 0x40) {                       /* TXFIFO_CLR */
            s->fifo_len = s->fifo_pos = 0;
        }
        break;
    case 0x98: s->cnt = val & 0xffff; break;
    case 0x9c:                                  /* DATA */
        if (s->tx_active && !s->pend_finish) {
            omap4_i2c_tx_byte(s, val);
        } else if (s->fifo_len < I2C_FIFO_SIZE) {
            s->fifo[s->fifo_len++] = val;       /* queued before STT */
        }
        break;
    case 0xa4:
        if (!(val & I2C_CON_EN)) {
            s->con = val;
            s->tx_active = s->rx_active = false;
            s->fifo_len = s->fifo_pos = 0;
            break;
        }
        s->con = val;
        if ((val & I2C_CON_STT) && (val & I2C_CON_MST) &&
            !s->tx_active && !s->rx_active) {
            omap4_i2c_start(s);
        }
        break;
    case 0xa8: s->oa = val; break;
    case 0xac: s->sa = val & 0x3ff; break;
    case 0xb0: s->psc = val; break;
    case 0xb4: s->scll = val; break;
    case 0xb8: s->sclh = val; break;
    case 0xbc: s->systest = val; break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @0x%" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", s->name, addr, val);
    }
    omap4_i2c_update_irq(s);
}

static const MemoryRegionOps omap4_i2c_ops = {
    .read = omap4_i2c_read,
    .write = omap4_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void omap4_i2c_reset(DeviceState *dev)
{
    Omap4I2C *s = GARMIN_OMAP4_I2C(dev);

    s->sysc = 0; s->ie = 0; s->stat = 0; s->we = 0; s->buf = 0; s->cnt = 0;
    s->con = 0; s->oa = 0; s->sa = 0; s->psc = 0; s->scll = 0; s->sclh = 0;
    s->systest = 0;
    s->fifo_len = s->fifo_pos = 0; s->tx_remaining = 0;
    s->tx_active = s->rx_active = false;
    s->pend_stat = 0; s->pend_finish = false;
    timer_del(s->done_timer);
}

static void omap4_i2c_realize(DeviceState *dev, Error **errp)
{
    Omap4I2C *s = GARMIN_OMAP4_I2C(dev);

    if (!s->name) {
        s->name = g_strdup("omap4.i2c");
    }
    s->done_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, omap4_i2c_done_cb, s);
    s->bus = i2c_init_bus(dev, "i2c");
    memory_region_init_io(&s->iomem, OBJECT(s), &omap4_i2c_ops, s, s->name,
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const Property omap4_i2c_properties[] = {
    DEFINE_PROP_STRING("name", Omap4I2C, name),
};

static const VMStateDescription vmstate_omap4_i2c = {
    .name = "omap4.i2c",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sysc, Omap4I2C),
        VMSTATE_UINT32(ie, Omap4I2C),
        VMSTATE_UINT32(stat, Omap4I2C),
        VMSTATE_UINT32(we, Omap4I2C),
        VMSTATE_UINT32(buf, Omap4I2C),
        VMSTATE_UINT32(cnt, Omap4I2C),
        VMSTATE_UINT32(con, Omap4I2C),
        VMSTATE_UINT32(oa, Omap4I2C),
        VMSTATE_UINT32(sa, Omap4I2C),
        VMSTATE_UINT32(psc, Omap4I2C),
        VMSTATE_UINT32(scll, Omap4I2C),
        VMSTATE_UINT32(sclh, Omap4I2C),
        VMSTATE_UINT32(systest, Omap4I2C),
        VMSTATE_UINT8_ARRAY(fifo, Omap4I2C, I2C_FIFO_SIZE),
        VMSTATE_INT32(fifo_len, Omap4I2C),
        VMSTATE_INT32(fifo_pos, Omap4I2C),
        VMSTATE_INT32(tx_remaining, Omap4I2C),
        VMSTATE_BOOL(tx_active, Omap4I2C),
        VMSTATE_BOOL(rx_active, Omap4I2C),
        VMSTATE_TIMER_PTR(done_timer, Omap4I2C),
        VMSTATE_UINT32(pend_stat, Omap4I2C),
        VMSTATE_BOOL(pend_finish, Omap4I2C),
        VMSTATE_BOOL(irq_level, Omap4I2C),
        VMSTATE_END_OF_LIST()
    }
};

static void omap4_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_omap4_i2c;

    dc->realize = omap4_i2c_realize;
    device_class_set_legacy_reset(dc, omap4_i2c_reset);
    device_class_set_props(dc, omap4_i2c_properties);
}

static const TypeInfo omap4_i2c_info = {
    .name = TYPE_GARMIN_OMAP4_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Omap4I2C),
    .class_init = omap4_i2c_class_init,
};

static Omap4I2C *make_i2c(const char *name, hwaddr base, qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_GARMIN_OMAP4_I2C);

    qdev_prop_set_string(dev, "name", name);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
    return GARMIN_OMAP4_I2C(dev);
}

/* ------------------------------------------------------------------ */
/* Generic register-file I2C slave (PMIC, EEPROM, sensors...)             */
/* ------------------------------------------------------------------ */

#define TYPE_GARMIN_I2C_REGFILE "garmin.i2c-regfile"
OBJECT_DECLARE_SIMPLE_TYPE(GarminI2CRegfile, GARMIN_I2C_REGFILE)

struct GarminI2CRegfile {
    I2CSlave parent_obj;
    char *name;
    uint8_t regs[256];
    uint8_t ptr;
    bool have_ptr;
};

static int regfile_event(I2CSlave *i2c, enum i2c_event event)
{
    GarminI2CRegfile *s = GARMIN_I2C_REGFILE(i2c);

    if (event == I2C_START_SEND) {
        s->have_ptr = false;
    }
    return 0;
}

static int regfile_send(I2CSlave *i2c, uint8_t data)
{
    GarminI2CRegfile *s = GARMIN_I2C_REGFILE(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
    } else {
        if (gpsmap_stub_log) {
            qemu_log("%s: reg[0x%02x] <- 0x%02x\n", s->name, s->ptr, data);
        }
        s->regs[s->ptr++] = data;
    }
    return 0;
}

static uint8_t regfile_recv(I2CSlave *i2c)
{
    GarminI2CRegfile *s = GARMIN_I2C_REGFILE(i2c);
    uint8_t v = s->regs[s->ptr];

    if (gpsmap_stub_log) {
        qemu_log("%s: reg[0x%02x] -> 0x%02x\n", s->name, s->ptr, v);
    }
    s->ptr++;
    return v;
}

static const Property regfile_properties[] = {
    DEFINE_PROP_STRING("name", GarminI2CRegfile, name),
};

static const VMStateDescription vmstate_regfile = {
    .name = "garmin.i2c-regfile",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, GarminI2CRegfile),
        VMSTATE_UINT8_ARRAY(regs, GarminI2CRegfile, 256),
        VMSTATE_UINT8(ptr, GarminI2CRegfile),
        VMSTATE_BOOL(have_ptr, GarminI2CRegfile),
        VMSTATE_END_OF_LIST()
    }
};

static void regfile_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_regfile;
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = regfile_event;
    k->send = regfile_send;
    k->recv = regfile_recv;
    device_class_set_props(dc, regfile_properties);
}

static const TypeInfo regfile_info = {
    .name = TYPE_GARMIN_I2C_REGFILE,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(GarminI2CRegfile),
    .class_init = regfile_class_init,
};

static GarminI2CRegfile *make_i2c_regfile(I2CBus *bus, uint8_t addr,
                                          const char *name)
{
    DeviceState *dev = qdev_new(TYPE_GARMIN_I2C_REGFILE);

    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint8(dev, "address", addr);
    qdev_realize_and_unref(dev, BUS(bus), &error_fatal);
    return GARMIN_I2C_REGFILE(dev);
}

/* ------------------------------------------------------------------ */
/* EETI eGalax touch controller on I2C4 address 0x04 (docs/egalax_touch.md); INT = GPIO5.12 */
/*                                                                        */
/* Every I2C transfer is a 10-byte wire frame.  Host -> controller:        */
/*   03 <len> <up to 8 payload bytes>   chunks of an "inner" frame          */
/*   inner frame = 03 <n> <n command bytes>, e.g. 03 01 'A' (version)      */
/* Controller -> host frames are read one per 10-byte I2C read while INT   */
/* (GPIO5.12, active low) is asserted:                                     */
/*   03 <len> <payload>                  command reply                     */
/*   04 <status> Xlo Xhi Ylo Yhi Zlo Zhi 00 00   touch report              */
/* status: bit7 valid, bits 6..2 contact id, bit0 down; X/Y 0..32760.      */
/* The board reports no bundled touch firmware, so the firmware always     */
/* attempts a touch-controller update at boot; answering the "3a fe"       */
/* query with a zero byte makes that fail fast (~3 s) and boot continues.  */
/* ------------------------------------------------------------------ */

#define TYPE_GARMIN_EGALAX "garmin.egalax"
OBJECT_DECLARE_SIMPLE_TYPE(GarminEgalax, GARMIN_EGALAX)

#define EGALAX_FRAME    10
#define EGALAX_QUEUE    32
#define EGALAX_MAX_XY   0x7ff8

struct GarminEgalax {
    I2CSlave parent_obj;
    qemu_irq irq;                   /* active-low INT */
    uint8_t rx[EGALAX_FRAME];
    uint8_t rx_len;
    uint8_t inner[64];              /* inner command frame being assembled */
    uint8_t inner_len;
    uint8_t queue[EGALAX_QUEUE][EGALAX_FRAME];
    uint8_t q_head, q_count;
    uint8_t tx_pos;
    /* input state */
    int32_t x, y;
    bool pressed, last_sent_pressed;
    bool moved;
    QemuInputHandlerState *hs;
    QEMUTimer *repeat;                /* report stream while pressed */
    int64_t press_ns;
    bool release_pending;
};

static void egalax_update_irq(GarminEgalax *s)
{
    qemu_set_irq(s->irq, s->q_count == 0);      /* line high = idle */
}

static void egalax_queue_frame(GarminEgalax *s, const uint8_t *f, int n)
{
    uint8_t *slot;

    if (s->q_count == EGALAX_QUEUE) {
        return;
    }
    slot = s->queue[(s->q_head + s->q_count) % EGALAX_QUEUE];
    memset(slot, 0, EGALAX_FRAME);
    memcpy(slot, f, MIN(n, EGALAX_FRAME));
    s->q_count++;
    egalax_update_irq(s);
}

/* queue a command reply: outer frame 03 <len> + payload */
static void egalax_reply(GarminEgalax *s, const uint8_t *payload, int n)
{
    uint8_t f[EGALAX_FRAME] = { 0x03, n };

    memcpy(f + 2, payload, MIN(n, 8));
    egalax_queue_frame(s, f, EGALAX_FRAME);
}

static void egalax_queue_touch(GarminEgalax *s, bool down)
{
    /* The panel is mounted rotated: the firmware maps controller (x, y) to
     * screen (W - x*W/32768, H - y*H/32768) (inversion flags 0xa4943cb0/1),
     * so report the mirrored position to make window clicks land where
     * they were made. */
    int x = EGALAX_MAX_XY - s->x, y = EGALAX_MAX_XY - s->y;
    uint8_t f[EGALAX_FRAME] = { 0x04, 0x80 | (down ? 1 : 0),
                                x & 0xff, x >> 8, y & 0xff, y >> 8,
                                down ? 0x40 : 0, 0, 0, 0 };
    egalax_queue_frame(s, f, EGALAX_FRAME);
    gpsmap_log("egalax: touch %s at %d,%d (queue %u)\n", down ? "down" : "up",
               s->x, s->y, s->q_count);
}

/* A complete inner frame arrived: 03 <n> <cmd...>.  Replies carry the whole
 * inner frame as payload (the firmware strips the outer 03 <len> and then
 * compares/parses from the inner 03 <n> header on). */
static void egalax_command(GarminEgalax *s)
{
    uint8_t inner[EGALAX_FRAME];
    const uint8_t *c = s->inner + 2;
    int n = s->inner[1];
    int ilen = MIN(n + 2, 8);

    memset(inner, 0, sizeof(inner));
    memcpy(inner, s->inner, ilen);
    gpsmap_log("egalax: cmd %02x %02x %02x %02x %02x (n=%d)\n",
               n > 0 ? c[0] : 0, n > 1 ? c[1] : 0, n > 2 ? c[2] : 0,
               n > 3 ? c[3] : 0, n > 4 ? c[4] : 0, n);
    /*
     * Firmware-update dialogue (docs/egalax_touch.md 3.1): the bundled image
     * for this hardware type is empty, so the firmware always wants to update
     * the controller.  Answering the "3a fe" capability query with a zero
     * status byte makes the attempt fail fast (~3 s) and boot continues;
     * letting it "succeed" was observed to reboot the unit.
     */
    if (n >= 2 && c[0] == 0x3a && c[1] == 0xfe) {
        inner[4] = 0;
    }
    egalax_reply(s, inner, ilen);
    if (n >= 1 && (c[0] == 0x44 || c[0] == 0x45)) {
        egalax_reply(s, inner, ilen);           /* 'D'/'E': a second frame is read */
    }
}

static int egalax_event(I2CSlave *i2c, enum i2c_event event)
{
    GarminEgalax *s = GARMIN_EGALAX(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->rx_len = 0;
        break;
    case I2C_START_RECV:
        s->tx_pos = 0;
        break;
    case I2C_FINISH:
        if (s->rx_len) {
            /* outer chunk -> inner frame assembly */
            if (s->rx[0] == 0x03) {
                int n = MIN(s->rx[1], 8);

                if (s->inner_len + n <= sizeof(s->inner)) {
                    memcpy(s->inner + s->inner_len, s->rx + 2, n);
                    s->inner_len += n;
                }
                if (s->inner_len >= 2 && s->inner_len >= s->inner[1] + 2) {
                    egalax_command(s);
                    s->inner_len = 0;
                }
            } else {
                s->inner_len = 0;
            }
            s->rx_len = 0;
        } else if (s->tx_pos && s->tx_pos < EGALAX_FRAME && s->q_count) {
            /* short read: drop the partially read frame */
            s->q_head = (s->q_head + 1) % EGALAX_QUEUE;
            s->q_count--;
            egalax_update_irq(s);
        }
        s->tx_pos = 0;
        break;
    default:
        break;
    }
    return 0;
}

static int egalax_send(I2CSlave *i2c, uint8_t data)
{
    GarminEgalax *s = GARMIN_EGALAX(i2c);

    if (s->rx_len < EGALAX_FRAME) {
        s->rx[s->rx_len++] = data;
    }
    return 0;
}

static uint8_t egalax_recv(I2CSlave *i2c)
{
    GarminEgalax *s = GARMIN_EGALAX(i2c);
    uint8_t v = 0;

    if (s->q_count && s->tx_pos < EGALAX_FRAME) {
        v = s->queue[s->q_head][s->tx_pos];
    }
    s->tx_pos++;
    /* A frame is consumed once its 10 bytes were clocked out.  The OMAP I2C
     * master may not issue STOP (I2C_FINISH) before the next START, so
     * waiting for FINISH left the frame queued and INT low, and the pen
     * poll re-read the same report. */
    if (s->tx_pos == EGALAX_FRAME && s->q_count) {
        s->q_head = (s->q_head + 1) % EGALAX_QUEUE;
        s->q_count--;
        egalax_update_irq(s);
    }
    return v;
}

/* QEMU input layer -> touch reports (absolute 0..0x7fff maps 1:1) */
static void egalax_input_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    GarminEgalax *s = GARMIN_EGALAX(dev);
    InputMoveEvent *move;
    InputBtnEvent *btn;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        move = evt->u.abs.data;
        if (move->axis == INPUT_AXIS_X) {
            s->x = qemu_input_scale_axis(move->value, INPUT_EVENT_ABS_MIN,
                                         INPUT_EVENT_ABS_MAX, 0, EGALAX_MAX_XY) & ~7;
        } else if (move->axis == INPUT_AXIS_Y) {
            s->y = qemu_input_scale_axis(move->value, INPUT_EVENT_ABS_MIN,
                                         INPUT_EVENT_ABS_MAX, 0, EGALAX_MAX_XY) & ~7;
        }
        s->moved = true;
        break;
    case INPUT_EVENT_KIND_REL:
        move = evt->u.rel.data;
        if (move->axis == INPUT_AXIS_X) {
            s->x = CLAMP(s->x + move->value * 32, 0, EGALAX_MAX_XY) & ~7;
        } else if (move->axis == INPUT_AXIS_Y) {
            s->y = CLAMP(s->y + move->value * 32, 0, EGALAX_MAX_XY) & ~7;
        }
        s->moved = true;
        break;
    case INPUT_EVENT_KIND_BTN:
        btn = evt->u.btn.data;
        if (btn->button == INPUT_BUTTON_LEFT) {
            s->pressed = btn->down;
        }
        break;
    default:
        break;
    }
}

/*
 * A real eGalax controller streams reports (~100 Hz) for as long as the
 * finger is down; the firmware's pen poll only sees a contact in polls that
 * read a report, and its debounce wants several consecutive such polls.  So
 * while pressed we keep re-queueing the current position.
 */
#define EGALAX_REPEAT_NS    (10 * SCALE_MS)
/* The firmware's pen poll runs every few hundred ms under emulation and its
 * debounce wants ~8 consecutive polls with a contact, so a short mouse click
 * would be lost: a press is reported for at least this long. */
#define EGALAX_MIN_HOLD_NS  (1500 * SCALE_MS)

static void egalax_release(GarminEgalax *s)
{
    s->release_pending = false;
    egalax_queue_touch(s, false);
    timer_del(s->repeat);
}

static void egalax_repeat_cb(void *opaque)
{
    GarminEgalax *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->release_pending && now - s->press_ns >= EGALAX_MIN_HOLD_NS) {
        egalax_release(s);
        return;
    }
    if (!s->pressed && !s->release_pending) {
        return;
    }
    if (s->q_count == 0) {                      /* don't pile up unread frames */
        egalax_queue_touch(s, true);
    }
    timer_mod(s->repeat, now + EGALAX_REPEAT_NS);
}

static void egalax_input_sync(DeviceState *dev)
{
    GarminEgalax *s = GARMIN_EGALAX(dev);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->pressed != s->last_sent_pressed) {
        s->last_sent_pressed = s->pressed;
        if (s->pressed) {
            s->release_pending = false;
            s->press_ns = now;
            egalax_queue_touch(s, true);
            timer_mod(s->repeat, now + EGALAX_REPEAT_NS);
        } else if (now - s->press_ns >= EGALAX_MIN_HOLD_NS) {
            egalax_release(s);
        } else {
            s->release_pending = true;          /* released by the timer */
        }
    } else if (s->pressed && s->moved) {
        egalax_queue_touch(s, true);
    }
    s->moved = false;
}

static const QemuInputHandler egalax_input_handler = {
    .name  = "eGalax touch",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_REL,
    .event = egalax_input_event,
    .sync  = egalax_input_sync,
};

static void egalax_realize(DeviceState *dev, Error **errp)
{
    GarminEgalax *s = GARMIN_EGALAX(dev);

    qdev_init_gpio_out(dev, &s->irq, 1);
    s->x = EGALAX_MAX_XY / 2;
    s->y = EGALAX_MAX_XY / 2;
    s->repeat = timer_new_ns(QEMU_CLOCK_VIRTUAL, egalax_repeat_cb, s);
    s->hs = qemu_input_handler_register(dev, &egalax_input_handler);
}

static void egalax_reset(DeviceState *dev)
{
    GarminEgalax *s = GARMIN_EGALAX(dev);

    s->rx_len = s->inner_len = s->q_head = s->q_count = s->tx_pos = 0;
    s->pressed = s->last_sent_pressed = s->moved = s->release_pending = false;
    timer_del(s->repeat);
    egalax_update_irq(s);
}

static const VMStateDescription vmstate_egalax = {
    .name = "garmin.egalax",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, GarminEgalax),
        VMSTATE_UINT8_ARRAY(rx, GarminEgalax, EGALAX_FRAME),
        VMSTATE_UINT8(rx_len, GarminEgalax),
        VMSTATE_UINT8_ARRAY(inner, GarminEgalax, 64),
        VMSTATE_UINT8(inner_len, GarminEgalax),
        VMSTATE_UINT8_2DARRAY(queue, GarminEgalax, EGALAX_QUEUE, EGALAX_FRAME),
        VMSTATE_UINT8(q_head, GarminEgalax),
        VMSTATE_UINT8(q_count, GarminEgalax),
        VMSTATE_UINT8(tx_pos, GarminEgalax),
        VMSTATE_INT32(x, GarminEgalax),
        VMSTATE_INT32(y, GarminEgalax),
        VMSTATE_BOOL(pressed, GarminEgalax),
        VMSTATE_BOOL(last_sent_pressed, GarminEgalax),
        VMSTATE_END_OF_LIST()
    }
};

static void egalax_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->realize = egalax_realize;
    dc->vmsd = &vmstate_egalax;
    device_class_set_legacy_reset(dc, egalax_reset);
    k->event = egalax_event;
    k->send = egalax_send;
    k->recv = egalax_recv;
}

static const TypeInfo egalax_info = {
    .name = TYPE_GARMIN_EGALAX,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(GarminEgalax),
    .class_init = egalax_class_init,
};

/* ------------------------------------------------------------------ */
/* OMAP4 HSMMC: OMAP wrapper registers + SDHCI core at +0x200            */
/*                                                                        */
/* Differences from a plain SD Host Controller that the firmware relies   */
/* on:                                                                    */
/*  - 136-bit (R2) responses are presented unshifted: RSP76 = bits 127:96 */
/*    (SDHCI presents bits 127:8 in RSP[119:0]).                          */
/*  - Bulk data moves through the OMAP4 system DMA (sDMA), not an SDHCI   */
/*    SDMA master; the DE bit of the command register is therefore masked */
/*    so the SDHCI core stays in PIO mode and sDMA drains the data port.  */
/* ------------------------------------------------------------------ */

#define TYPE_GARMIN_OMAP4_HSMMC "garmin.omap4-hsmmc"
OBJECT_DECLARE_SIMPLE_TYPE(Omap4Hsmmc, GARMIN_OMAP4_HSMMC)

struct Omap4Hsmmc {
    SysBusDevice parent_obj;
    MemoryRegion container;
    MemoryRegion wrap;
    MemoryRegion core;
    DeviceState *sdhci;
    MemoryRegion *sdhci_mr;
    char *name;
    uint32_t wrapregs[0x200 / 4];
    bool last_r2;
    uint32_t rsp[4];        /* re-shifted response cache */
    bool rsp_valid;
};

static uint64_t hsmmc_wrap_read(void *opaque, hwaddr addr, unsigned size)
{
    Omap4Hsmmc *s = opaque;

    switch (addr) {
    case 0x000: return 0x50000000;          /* HL_REV */
    case 0x004: return 0x00000000;          /* HL_HWINFO */
    case 0x010: return s->wrapregs[4] & ~1u;/* HL_SYSCONFIG SOFTRESET */
    case 0x110: return s->wrapregs[0x110 / 4] & ~2u;   /* SYSCONFIG */
    case 0x114: return 1;                   /* SYSSTATUS RESETDONE */
    default:    return s->wrapregs[addr / 4];
    }
}

static void hsmmc_wrap_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Omap4Hsmmc *s = opaque;

    s->wrapregs[addr / 4] = val;
    if (gpsmap_stub_log) {
        gpsmap_log("%s: wr  %08" HWADDR_PRIx " <- %08" PRIx64 " [%s]\n", s->name,
                   (hwaddr)(s->container.addr + addr), val, gpsmap_ctx());
    }
}

static const MemoryRegionOps hsmmc_wrap_ops = {
    .read = hsmmc_wrap_read,
    .write = hsmmc_wrap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static uint64_t hsmmc_core_read(void *opaque, hwaddr addr, unsigned size)
{
    Omap4Hsmmc *s = opaque;
    uint64_t val = 0;

    if (s->last_r2 && addr >= 0x10 && addr < 0x20 && size == 4) {
        if (!s->rsp_valid) {
            uint64_t w[4];
            for (int i = 0; i < 4; i++) {
                memory_region_dispatch_read(s->sdhci_mr, 0x10 + 4 * i, &w[i],
                                            MO_32 | MO_LE,
                                            MEMTXATTRS_UNSPECIFIED);
            }
            /* SDHCI: w[3..0] = CSD[127:8]; OMAP: rsp[3] = CSD[127:96] ... */
            s->rsp[3] = (w[3] << 8) | (w[2] >> 24);
            s->rsp[2] = (w[2] << 8) | (w[1] >> 24);
            s->rsp[1] = (w[1] << 8) | (w[0] >> 24);
            s->rsp[0] = (w[0] << 8) | 0x01;   /* CRC7|end bit placeholder */
            s->rsp_valid = true;
        }
        return s->rsp[(addr - 0x10) / 4];
    }
    memory_region_dispatch_read(s->sdhci_mr, addr, &val,
                                size_memop(size) | MO_LE,
                                MEMTXATTRS_UNSPECIFIED);
    return val;
}

static void hsmmc_core_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Omap4Hsmmc *s = opaque;

    if (addr == 0x0c && size == 4) {
        /* MMCHS_CMD: RSP_TYPE bits 17:16 (1 = 136-bit), DE bit 0 */
        s->last_r2 = ((val >> 16) & 3) == 1;
        s->rsp_valid = false;
        val &= ~1ull;
    } else if (addr == 0x0c && size == 2) {
        val &= ~1ull;
    } else if (addr == 0x0e && size == 2) {
        s->last_r2 = (val & 3) == 1;
        s->rsp_valid = false;
    }
    memory_region_dispatch_write(s->sdhci_mr, addr, val,
                                 size_memop(size) | MO_LE,
                                 MEMTXATTRS_UNSPECIFIED);
}

static const MemoryRegionOps hsmmc_core_ops = {
    .read = hsmmc_core_read,
    .write = hsmmc_core_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void hsmmc_realize(DeviceState *dev, Error **errp)
{
    Omap4Hsmmc *s = GARMIN_OMAP4_HSMMC(dev);

    if (!s->name) {
        s->name = g_strdup("omap4.hsmmc");
    }
    memory_region_init(&s->container, OBJECT(s), s->name, 0x1000);
    memory_region_init_io(&s->wrap, OBJECT(s), &hsmmc_wrap_ops, s,
                          "hsmmc.wrap", 0x200);
    memory_region_add_subregion(&s->container, 0, &s->wrap);

    s->sdhci = qdev_new(TYPE_SYSBUS_SDHCI);
    qdev_prop_set_uint8(s->sdhci, "sd-spec-version", 2);
    qdev_prop_set_uint64(s->sdhci, "capareg", 0x057834b4);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->sdhci), &error_fatal);
    s->sdhci_mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(s->sdhci), 0);

    memory_region_init_io(&s->core, OBJECT(s), &hsmmc_core_ops, s,
                          "hsmmc.core", 0x100);
    memory_region_add_subregion(&s->container, 0x200, &s->core);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->container);
}

static const Property hsmmc_properties[] = {
    DEFINE_PROP_STRING("name", Omap4Hsmmc, name),
};

static const VMStateDescription vmstate_hsmmc = {
    .name = "omap4.hsmmc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(wrapregs, Omap4Hsmmc, 0x200 / 4),
        VMSTATE_BOOL(last_r2, Omap4Hsmmc),
        VMSTATE_UINT32_ARRAY(rsp, Omap4Hsmmc, 4),
        VMSTATE_BOOL(rsp_valid, Omap4Hsmmc),
        VMSTATE_END_OF_LIST()
    }
};

static void hsmmc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_hsmmc;

    dc->realize = hsmmc_realize;
    device_class_set_props(dc, hsmmc_properties);
}

static const TypeInfo hsmmc_info = {
    .name = TYPE_GARMIN_OMAP4_HSMMC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Omap4Hsmmc),
    .class_init = hsmmc_class_init,
};

static hwaddr hsmmc_bases[5];

static void make_hsmmc(const char *name, int idx, hwaddr base, qemu_irq irq,
                       int drive_index, bool emmc)
{
    DeviceState *dev = qdev_new(TYPE_GARMIN_OMAP4_HSMMC);
    Omap4Hsmmc *s = GARMIN_OMAP4_HSMMC(dev);
    DriveInfo *di;

    qdev_prop_set_string(dev, "name", name);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->sdhci), 0, irq);
    hsmmc_bases[idx] = base;

    di = drive_get(IF_SD, 0, drive_index);
    if (di) {
        DeviceState *card = qdev_new(emmc ? TYPE_EMMC : TYPE_SD_CARD);
        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(di),
                                &error_fatal);
        qdev_realize_and_unref(card, qdev_get_child_bus(s->sdhci, "sd-bus"),
                               &error_fatal);
    }
}

/* ------------------------------------------------------------------ */
/* OMAP4 system DMA (DMA4/sDMA), enough for the HSMMC and mem-to-mem     */
/* ------------------------------------------------------------------ */

#define TYPE_GARMIN_OMAP4_SDMA "garmin.omap4-sdma"
OBJECT_DECLARE_SIMPLE_TYPE(Omap4Sdma, GARMIN_OMAP4_SDMA)

#define SDMA_NCH 32

#define CCR_ENABLE      (1 << 7)
#define CCR_SRC_AMODE(c) (((c) >> 12) & 3)
#define CCR_DST_AMODE(c) (((c) >> 14) & 3)
#define CCR_SYNC(c)     ((((c) >> 19) & 3) << 5 | ((c) & 0x1f))

#define CSR_DROP        (1 << 1)
#define CSR_HALF        (1 << 2)
#define CSR_FRAME       (1 << 3)
#define CSR_LAST        (1 << 4)
#define CSR_BLOCK       (1 << 5)
#define CSR_SYNC        (1 << 6)

typedef struct {
    uint32_t ccr, clnk, cicr, csr, csdp, cen, cfn, cssa, cdsa, csei, csfi,
             cdei, cdfi, csac, cdac, color, cdp, cndp, ccdn;
    uint32_t remaining;     /* elements left */
    hwaddr src, dst;
    bool active;
} SdmaChan;

struct Omap4Sdma {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[4];
    QEMUTimer *engine;
    uint32_t irqstatus[4], irqenable[4], ocp_sysconfig, gcr;
    SdmaChan ch[SDMA_NCH];
};

static void sdma_update_irq(Omap4Sdma *s)
{
    for (int i = 0; i < 4; i++) {
        qemu_set_irq(s->irq[i], (s->irqstatus[i] & s->irqenable[i]) != 0);
    }
}

static void sdma_complete(Omap4Sdma *s, int n)
{
    SdmaChan *c = &s->ch[n];

    c->active = false;
    c->ccr &= ~CCR_ENABLE;
    c->csr |= CSR_FRAME | CSR_BLOCK | CSR_LAST;
    if (c->cicr & (CSR_FRAME | CSR_BLOCK | CSR_LAST)) {
        for (int i = 0; i < 4; i++) {
            s->irqstatus[i] |= 1u << n;
        }
    }
    if (gpsmap_stub_log) {
        gpsmap_log("omap4.sdma: ch%d complete src=%08" HWADDR_PRIx
                   " dst=%08" HWADDR_PRIx "\n", n, c->src, c->dst);
    }
    sdma_update_irq(s);
}

static int sdma_elem_size(SdmaChan *c)
{
    return 1 << (c->csdp & 3);
}

/* Is addr the data port of one of the HSMMC controllers? */
static int sdma_mmc_port(hwaddr addr)
{
    for (int i = 0; i < 5; i++) {
        if (hsmmc_bases[i] && addr == hsmmc_bases[i] + 0x220) {
            return i;
        }
    }
    return -1;
}

static bool sdma_mmc_ready(int idx, bool read)
{
    uint32_t pstate = 0;

    address_space_read(&address_space_memory, hsmmc_bases[idx] + 0x224,
                       MEMTXATTRS_UNSPECIFIED, &pstate, 4);
    return pstate & (read ? (1 << 11) : (1 << 10));     /* BRE / BWE */
}

/* Move up to `max` elements; returns number moved */
static int sdma_step(Omap4Sdma *s, int n, int max)
{
    SdmaChan *c = &s->ch[n];
    int es = sdma_elem_size(c);
    int mmc_src = sdma_mmc_port(c->src), mmc_dst = sdma_mmc_port(c->dst);
    int moved = 0;

    while (c->remaining > 0 && moved < max) {
        uint32_t v = 0;

        if (mmc_src >= 0 && !sdma_mmc_ready(mmc_src, true)) {
            break;
        }
        if (mmc_dst >= 0 && !sdma_mmc_ready(mmc_dst, false)) {
            break;
        }
        address_space_read(&address_space_memory, c->src,
                           MEMTXATTRS_UNSPECIFIED, &v, es);
        address_space_write(&address_space_memory, c->dst,
                            MEMTXATTRS_UNSPECIFIED, &v, es);
        if (CCR_SRC_AMODE(c->ccr) != 0) {
            c->src += es;
        }
        if (CCR_DST_AMODE(c->ccr) != 0) {
            c->dst += es;
        }
        c->remaining--;
        moved++;
    }
    c->csac = c->src;
    c->cdac = c->dst;
    if (c->remaining == 0) {
        sdma_complete(s, n);
    }
    return moved;
}

static void sdma_engine(void *opaque)
{
    Omap4Sdma *s = opaque;
    bool pending = false;

    for (int n = 0; n < SDMA_NCH; n++) {
        if (s->ch[n].active) {
            sdma_step(s, n, 4096);
            if (s->ch[n].active) {
                pending = true;
            }
        }
    }
    if (pending) {
        timer_mod(s->engine, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 50000);
    }
}

static void sdma_start(Omap4Sdma *s, int n)
{
    SdmaChan *c = &s->ch[n];

    c->src = c->cssa;
    c->dst = c->cdsa;
    c->remaining = (c->cen & 0xffffff) * (c->cfn & 0xffff);
    c->active = true;
    c->csr &= ~(CSR_FRAME | CSR_BLOCK | CSR_LAST | CSR_HALF);
    if (gpsmap_stub_log) {
        gpsmap_log("omap4.sdma: ch%d start ccr=%08x csdp=%08x cen=%u cfn=%u "
                   "src=%08x dst=%08x sync=%u\n", n, c->ccr, c->csdp, c->cen,
                   c->cfn, c->cssa, c->cdsa, CCR_SYNC(c->ccr));
    }
    if (c->remaining == 0) {
        sdma_complete(s, n);
        return;
    }
    if (CCR_SYNC(c->ccr) == 0 && sdma_mmc_port(c->src) < 0 &&
        sdma_mmc_port(c->dst) < 0) {
        sdma_step(s, n, INT32_MAX);         /* mem-to-mem: instant */
        return;
    }
    sdma_step(s, n, 4096);
    if (c->active) {
        timer_mod(s->engine, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 20000);
    }
}

static uint64_t sdma_read(void *opaque, hwaddr addr, unsigned size)
{
    Omap4Sdma *s = opaque;

    if (addr >= 0x80 && addr < 0x80 + SDMA_NCH * 0x60) {
        int n = (addr - 0x80) / 0x60;
        SdmaChan *c = &s->ch[n];
        switch ((addr - 0x80) % 0x60) {
        case 0x00: return c->ccr;
        case 0x04: return c->clnk;
        case 0x08: return c->cicr;
        case 0x0c: return c->csr;
        case 0x10: return c->csdp;
        case 0x14: return c->cen;
        case 0x18: return c->cfn;
        case 0x1c: return c->cssa;
        case 0x20: return c->cdsa;
        case 0x24: return c->csei;
        case 0x28: return c->csfi;
        case 0x2c: return c->cdei;
        case 0x30: return c->cdfi;
        case 0x34: return c->csac;
        case 0x38: return c->cdac;
        case 0x3c: return c->active ? (c->remaining % (c->cen ? c->cen : 1)) : 0;
        case 0x40: return c->active ? (c->cen ? c->remaining / c->cen : 0) : 0;
        case 0x44: return c->color;
        case 0x50: return c->cdp;
        case 0x54: return c->cndp;
        case 0x58: return c->ccdn;
        default: return 0;
        }
    }
    switch (addr) {
    case 0x00: return 0x00010900;               /* REVISION */
    case 0x08: case 0x0c: case 0x10: case 0x14:
        return s->irqstatus[(addr - 8) / 4];
    case 0x18: case 0x1c: case 0x20: case 0x24:
        return s->irqenable[(addr - 0x18) / 4];
    case 0x28: return 1;                        /* SYSSTATUS RESETDONE */
    case 0x2c: return s->ocp_sysconfig & ~2u;
    case 0x64: return 0x000c00ff;               /* CAPS_0 */
    case 0x6c: return 0x000001ff;               /* CAPS_2 */
    case 0x70: return 0x000000f3;               /* CAPS_3 */
    case 0x74: return 0x00003ffe;               /* CAPS_4 */
    case 0x78: return s->gcr;
    default:
        qemu_log_mask(LOG_UNIMP, "omap4.sdma: unimplemented read @0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void sdma_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Omap4Sdma *s = opaque;

    if (addr >= 0x80 && addr < 0x80 + SDMA_NCH * 0x60) {
        int n = (addr - 0x80) / 0x60;
        SdmaChan *c = &s->ch[n];
        switch ((addr - 0x80) % 0x60) {
        case 0x00: {
            bool was = c->ccr & CCR_ENABLE;
            c->ccr = val;
            if (!was && (val & CCR_ENABLE)) {
                sdma_start(s, n);
            } else if (was && !(val & CCR_ENABLE)) {
                c->active = false;
            }
            break;
        }
        case 0x04: c->clnk = val; break;
        case 0x08: c->cicr = val; break;
        case 0x0c: c->csr &= ~val; break;       /* W1C */
        case 0x10: c->csdp = val; break;
        case 0x14: c->cen = val; break;
        case 0x18: c->cfn = val; break;
        case 0x1c: c->cssa = val; break;
        case 0x20: c->cdsa = val; break;
        case 0x24: c->csei = val; break;
        case 0x28: c->csfi = val; break;
        case 0x2c: c->cdei = val; break;
        case 0x30: c->cdfi = val; break;
        case 0x44: c->color = val; break;
        case 0x50: c->cdp = val; break;
        case 0x54: c->cndp = val; break;
        case 0x58: c->ccdn = val; break;
        default: break;
        }
        return;
    }
    switch (addr) {
    case 0x08: case 0x0c: case 0x10: case 0x14:
        s->irqstatus[(addr - 8) / 4] &= ~val;          /* W1C */
        sdma_update_irq(s);
        break;
    case 0x18: case 0x1c: case 0x20: case 0x24:
        s->irqenable[(addr - 0x18) / 4] = val;
        sdma_update_irq(s);
        break;
    case 0x2c: s->ocp_sysconfig = val; break;
    case 0x78: s->gcr = val; break;
    default:
        break;
    }
}

static const MemoryRegionOps sdma_ops = {
    .read = sdma_read,
    .write = sdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void sdma_reset(DeviceState *dev)
{
    Omap4Sdma *s = GARMIN_OMAP4_SDMA(dev);

    memset(s->ch, 0, sizeof(s->ch));
    memset(s->irqstatus, 0, sizeof(s->irqstatus));
    memset(s->irqenable, 0, sizeof(s->irqenable));
    s->ocp_sysconfig = 0; s->gcr = 0;
    timer_del(s->engine);
}

static void sdma_realize(DeviceState *dev, Error **errp)
{
    Omap4Sdma *s = GARMIN_OMAP4_SDMA(dev);

    s->engine = timer_new_ns(QEMU_CLOCK_VIRTUAL, sdma_engine, s);
    memory_region_init_io(&s->iomem, OBJECT(s), &sdma_ops, s, "omap4.sdma",
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (int i = 0; i < 4; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }
}

static const VMStateDescription vmstate_sdma_chan = {
    .name = "omap4.sdma.chan",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ccr, SdmaChan), VMSTATE_UINT32(clnk, SdmaChan),
        VMSTATE_UINT32(cicr, SdmaChan), VMSTATE_UINT32(csr, SdmaChan),
        VMSTATE_UINT32(csdp, SdmaChan), VMSTATE_UINT32(cen, SdmaChan),
        VMSTATE_UINT32(cfn, SdmaChan), VMSTATE_UINT32(cssa, SdmaChan),
        VMSTATE_UINT32(cdsa, SdmaChan), VMSTATE_UINT32(csei, SdmaChan),
        VMSTATE_UINT32(csfi, SdmaChan), VMSTATE_UINT32(cdei, SdmaChan),
        VMSTATE_UINT32(cdfi, SdmaChan), VMSTATE_UINT32(csac, SdmaChan),
        VMSTATE_UINT32(cdac, SdmaChan), VMSTATE_UINT32(color, SdmaChan),
        VMSTATE_UINT32(cdp, SdmaChan), VMSTATE_UINT32(cndp, SdmaChan),
        VMSTATE_UINT32(ccdn, SdmaChan), VMSTATE_UINT32(remaining, SdmaChan),
        VMSTATE_UINT64(src, SdmaChan), VMSTATE_UINT64(dst, SdmaChan),
        VMSTATE_BOOL(active, SdmaChan),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sdma = {
    .name = "omap4.sdma",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(engine, Omap4Sdma),
        VMSTATE_UINT32_ARRAY(irqstatus, Omap4Sdma, 4),
        VMSTATE_UINT32_ARRAY(irqenable, Omap4Sdma, 4),
        VMSTATE_UINT32(ocp_sysconfig, Omap4Sdma),
        VMSTATE_UINT32(gcr, Omap4Sdma),
        VMSTATE_STRUCT_ARRAY(ch, Omap4Sdma, SDMA_NCH, 1, vmstate_sdma_chan, SdmaChan),
        VMSTATE_END_OF_LIST()
    }
};

static void sdma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_sdma;

    dc->realize = sdma_realize;
    device_class_set_legacy_reset(dc, sdma_reset);
}

static const TypeInfo sdma_info = {
    .name = TYPE_GARMIN_OMAP4_SDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Omap4Sdma),
    .class_init = sdma_class_init,
};

/* ------------------------------------------------------------------ */
/* OMAP4 system mailbox + Ducati (Cortex-M3) responder                    */
/*                                                                        */
/* 8 mailboxes x 4-deep FIFOs, 4 users.  The firmware (MPU = user 0)      */
/* sends 4-word requests on mailbox 3 and expects a 4-word reply on       */
/* mailbox 2, delivered by the NEWMSG interrupt of user 0.  The Ducati    */
/* image is not run; we act as the M3 and answer every request by        */
/* echoing it back.                                                       */
/* ------------------------------------------------------------------ */

#define TYPE_GARMIN_OMAP4_MBOX "garmin.omap4-mailbox"
OBJECT_DECLARE_SIMPLE_TYPE(Omap4Mbox, GARMIN_OMAP4_MBOX)

#define MBOX_N          8
#define MBOX_DEPTH      4
#define MBOX_USERS      4
#define MBOX_MPU_TX     3       /* MPU -> Ducati */
#define MBOX_MPU_RX     2       /* Ducati -> MPU */
#define MBOX_REPLY_NS   200000  /* 200 us pretend M3 latency */

struct Omap4Mbox {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[MBOX_USERS];
    QEMUTimer *reply_timer;
    uint32_t fifo[MBOX_N][MBOX_DEPTH];
    uint8_t count[MBOX_N];
    uint32_t irqstatus[MBOX_USERS];
    uint32_t irqenable[MBOX_USERS];
    uint32_t sysconfig;
    uint32_t req[MBOX_DEPTH];
    uint8_t req_n;
    uint32_t seq;
};

static void mbox_update_irq(Omap4Mbox *s)
{
    for (int u = 0; u < MBOX_USERS; u++) {
        qemu_set_irq(s->irq[u], (s->irqstatus[u] & s->irqenable[u]) != 0);
    }
}

static void mbox_push(Omap4Mbox *s, int m, uint32_t val)
{
    if (s->count[m] < MBOX_DEPTH) {
        s->fifo[m][s->count[m]++] = val;
    }
    for (int u = 0; u < MBOX_USERS; u++) {
        s->irqstatus[u] |= 1u << (2 * m);           /* NEWMSG */
    }
    mbox_update_irq(s);
}

static uint32_t mbox_pop(Omap4Mbox *s, int m)
{
    uint32_t v = 0;

    if (s->count[m]) {
        v = s->fifo[m][0];
        memmove(&s->fifo[m][0], &s->fifo[m][1], (MBOX_DEPTH - 1) * 4);
        s->count[m]--;
        if (s->count[m] == 0) {
            for (int u = 0; u < MBOX_USERS; u++) {
                s->irqstatus[u] &= ~(1u << (2 * m));
            }
        }
        for (int u = 0; u < MBOX_USERS; u++) {
            s->irqstatus[u] |= 1u << (2 * m + 1);   /* NOTFULL */
        }
        mbox_update_irq(s);
    }
    return v;
}

/* The pretend Ducati: consume the request, answer on the reply mailbox */
static void mbox_ducati_reply(void *opaque)
{
    Omap4Mbox *s = opaque;
    uint32_t rep[MBOX_DEPTH];

    while (s->count[MBOX_MPU_TX] && s->req_n < MBOX_DEPTH) {
        s->req[s->req_n++] = mbox_pop(s, MBOX_MPU_TX);
    }
    if (s->req_n < MBOX_DEPTH) {
        return;
    }
    s->req_n = 0;
    memcpy(rep, s->req, sizeof(rep));
    gpsmap_log("ducati: req %08x %08x %08x %08x -> reply (echo) #%u\n",
               rep[0], rep[1], rep[2], rep[3], ++s->seq);
    for (int i = 0; i < MBOX_DEPTH; i++) {
        mbox_push(s, MBOX_MPU_RX, rep[i]);
    }
}

static uint64_t mbox_read(void *opaque, hwaddr addr, unsigned size)
{
    Omap4Mbox *s = opaque;
    uint32_t val = 0;
    int idx;

    switch (addr) {
    case 0x00: val = 0x00000400; break;                 /* REVISION */
    case 0x10: val = s->sysconfig & ~1u; break;         /* SYSCONFIG */
    case 0x40 ... 0x5c:                                 /* MESSAGE_m */
        val = mbox_pop(s, (addr - 0x40) / 4);
        break;
    case 0x80 ... 0x9c:                                 /* FIFOSTATUS_m */
        val = s->count[(addr - 0x80) / 4] >= MBOX_DEPTH;
        break;
    case 0xc0 ... 0xdc:                                 /* MSGSTATUS_m */
        val = s->count[(addr - 0xc0) / 4];
        break;
    case 0x100 ... 0x13c:
        idx = (addr - 0x100) / 0x10;
        switch (addr & 0xc) {
        case 0x0: val = s->irqstatus[idx]; break;
        case 0x4: val = s->irqstatus[idx] & s->irqenable[idx]; break;
        default:  val = s->irqenable[idx]; break;
        }
        break;
    default:
        break;
    }
    if (gpsmap_stub_log && addr >= 0x40) {
        gpsmap_log("omap4.mailbox: rd  %08" HWADDR_PRIx " -> %08x [%s]\n",
                   (hwaddr)(0x4a0f4000 + addr), val, gpsmap_ctx());
    }
    return val;
}

static void mbox_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Omap4Mbox *s = opaque;
    int idx;

    if (gpsmap_stub_log && addr >= 0x40) {
        gpsmap_log("omap4.mailbox: wr  %08" HWADDR_PRIx " <- %08" PRIx64 " [%s]\n",
                   (hwaddr)(0x4a0f4000 + addr), val, gpsmap_ctx());
    }
    switch (addr) {
    case 0x10:
        s->sysconfig = val;
        if (val & 1) {                                  /* SOFTRESET */
            memset(s->count, 0, sizeof(s->count));
            memset(s->irqstatus, 0, sizeof(s->irqstatus));
            memset(s->irqenable, 0, sizeof(s->irqenable));
            s->req_n = 0;
            mbox_update_irq(s);
        }
        break;
    case 0x40 ... 0x5c:
        idx = (addr - 0x40) / 4;
        mbox_push(s, idx, val);
        if (idx == MBOX_MPU_TX) {
            timer_mod(s->reply_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MBOX_REPLY_NS);
        }
        break;
    case 0x100 ... 0x13c:
        idx = (addr - 0x100) / 0x10;
        switch (addr & 0xc) {
        case 0x0:
            break;
        case 0x4:                                       /* IRQSTATUS_CLR: W1C */
            for (int m = 0; m < MBOX_N; m++) {
                if (s->count[m]) {                      /* NEWMSG is level */
                    val &= ~(1ull << (2 * m));
                }
            }
            s->irqstatus[idx] &= ~val;
            break;
        case 0x8:
            s->irqenable[idx] |= val;
            break;
        default:
            s->irqenable[idx] &= ~val;
            break;
        }
        mbox_update_irq(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps mbox_ops = {
    .read = mbox_read,
    .write = mbox_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void mbox_reset(DeviceState *dev)
{
    Omap4Mbox *s = GARMIN_OMAP4_MBOX(dev);

    memset(s->fifo, 0, sizeof(s->fifo));
    memset(s->count, 0, sizeof(s->count));
    memset(s->irqstatus, 0, sizeof(s->irqstatus));
    memset(s->irqenable, 0, sizeof(s->irqenable));
    s->sysconfig = 0;
    s->req_n = 0;
    s->seq = 0;
    timer_del(s->reply_timer);
}

static void mbox_realize(DeviceState *dev, Error **errp)
{
    Omap4Mbox *s = GARMIN_OMAP4_MBOX(dev);

    s->reply_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mbox_ducati_reply, s);
    memory_region_init_io(&s->iomem, OBJECT(s), &mbox_ops, s, "omap4.mailbox",
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (int u = 0; u < MBOX_USERS; u++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[u]);
    }
}

static const VMStateDescription vmstate_mbox = {
    .name = "omap4.mailbox",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(reply_timer, Omap4Mbox),
        VMSTATE_UINT32_2DARRAY(fifo, Omap4Mbox, MBOX_N, MBOX_DEPTH),
        VMSTATE_UINT8_ARRAY(count, Omap4Mbox, MBOX_N),
        VMSTATE_UINT32_ARRAY(irqstatus, Omap4Mbox, MBOX_USERS),
        VMSTATE_UINT32_ARRAY(irqenable, Omap4Mbox, MBOX_USERS),
        VMSTATE_UINT32(sysconfig, Omap4Mbox),
        VMSTATE_UINT32_ARRAY(req, Omap4Mbox, MBOX_DEPTH),
        VMSTATE_UINT8(req_n, Omap4Mbox),
        VMSTATE_UINT32(seq, Omap4Mbox),
        VMSTATE_END_OF_LIST()
    }
};

static void mbox_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_mbox;

    dc->realize = mbox_realize;
    device_class_set_legacy_reset(dc, mbox_reset);
}

static const TypeInfo mbox_info = {
    .name = TYPE_GARMIN_OMAP4_MBOX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Omap4Mbox),
    .class_init = mbox_class_init,
};

/* ------------------------------------------------------------------ */
/* SGX540 (PowerVR) shim                                                */
/*                                                                        */
/* The firmware embeds PowerVR services.  SGXInitialise resets the core,  */
/* loads the microkernel and writes EUR_CR_EVENT_KICK, then polls the     */
/* host-control block for the ukernel's "init complete" flag (bit 0) with */
/* a 500 ms timeout.  Without that flag the SGX device fails, the display */
/* class never creates a swap chain and DISPC is never touched.  We play  */
/* the microkernel: on KICK we set the flag.  The block is reached from   */
/* the SGX device node (0xa226a480) -> devinfo (+0xac) -> hostctl (+0x7d8).*/
/* ------------------------------------------------------------------ */

#define SGX_DEVNODE_PTR   0xa226a480
#define SGX_DEVINFO_OFF   0xac
#define SGX_HOSTCTL_OFF   0x7d8
#define EUR_CR_EVENT_KICK 0x0ac0
#define EUR_CR_SOFT_RESET 0x0080

/* Virtual read/write through the current CPU's page tables: the PowerVR
 * device structures live in a kernel mapping (0xc0000000+), not identity. */
static uint32_t sgx_ldr(uint32_t va)
{
    uint32_t v = 0;

    if (current_cpu) {
        cpu_memory_rw_debug(current_cpu, va, &v, 4, false);
    }
    return v;
}

static void sgx_str(uint32_t va, uint32_t v)
{
    if (current_cpu) {
        cpu_memory_rw_debug(current_cpu, va, &v, 4, true);
    }
}

static bool sgx_ptr_ok(uint32_t p)
{
    return p >= 0x80000000 && p != 0xffffffff;
}

/* ---- fake microkernel state ------------------------------------------ */
#define EUR_CR_EVENT_HOST_ENABLE2   0x0110
#define EUR_CR_EVENT_HOST_CLEAR2    0x0114
#define EUR_CR_EVENT_STATUS2        0x0118
#define EUR_CR_EVENT_STATUS         0x012c
#define EUR_CR_EVENT_HOST_ENABLE    0x0130
#define EUR_CR_EVENT_HOST_CLEAR     0x0134
#define EUR_CR_BIF_DIR_LIST_BASE0   0x0c84
#define SGX_EVENT_SW_EVENT          0x00004000  /* the only bit the host enables */
#define SGX_EVENT_MASTER            0x80000000
#define SGX_UKERNEL_LATENCY_NS      200000

static qemu_irq sgx_irq;
static QEMUTimer *sgx_event_timer;
static GarminRegStub *sgx_stub;
static uint32_t sgx_pending_events;

static void sgx_update_irq(GarminRegStub *s)
{
    uint32_t st1 = s->regs[EUR_CR_EVENT_STATUS / 4] & s->regs[EUR_CR_EVENT_HOST_ENABLE / 4];
    uint32_t st2 = s->regs[EUR_CR_EVENT_STATUS2 / 4] & s->regs[EUR_CR_EVENT_HOST_ENABLE2 / 4];

    if (sgx_irq) {
        qemu_set_irq(sgx_irq, (st1 | st2) != 0);
    }
}

/* Deliver a ukernel -> host event a little later, like real hardware */
static void sgx_event_cb(void *opaque)
{
    GarminRegStub *s = opaque;

    s->regs[EUR_CR_EVENT_STATUS / 4] |= sgx_pending_events | SGX_EVENT_MASTER;
    sgx_pending_events = 0;
    sgx_update_irq(s);
}

static void sgx_raise_event(GarminRegStub *s, uint32_t mask)
{
    sgx_pending_events |= mask;
    if (!sgx_event_timer) {
        sgx_event_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sgx_event_cb, s);
    }
    timer_mod(sgx_event_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SGX_UKERNEL_LATENCY_NS);
}

/*
 * SGX540 MMU walk: DIR_LIST_BASE0 points at a page directory of 1024 PDEs
 * (device VA bits 31:22); each PDE points at a page table of 1024 PTEs
 * (bits 21:12); bit 0 = valid; 4 KiB pages.  Returns physical address or -1.
 */
static hwaddr sgx_dev_to_phys(GarminRegStub *s, uint32_t dva)
{
    uint32_t pd = s->regs[EUR_CR_BIF_DIR_LIST_BASE0 / 4] & ~0xfffu;
    uint32_t pde = 0, pte = 0;

    if (!pd) {
        return -1;
    }
    address_space_read(&address_space_memory, pd + ((dva >> 22) & 0x3ff) * 4,
                       MEMTXATTRS_UNSPECIFIED, &pde, 4);
    if (!(pde & 1)) {
        return -1;
    }
    address_space_read(&address_space_memory, (pde & ~0xfffu) + ((dva >> 12) & 0x3ff) * 4,
                       MEMTXATTRS_UNSPECIFIED, &pte, 4);
    if (!(pte & 1)) {
        return -1;
    }
    return (pte & ~0xfffu) | (dva & 0xfff);
}

static hwaddr sgx_mmu(uint32_t pd, uint32_t dva)
{
    uint32_t pde = 0, pte = 0;

    pd &= ~0xfffu;
    if (!pd) {
        return -1;
    }
    address_space_read(&address_space_memory, pd + ((dva >> 22) & 0x3ff) * 4,
                       MEMTXATTRS_UNSPECIFIED, &pde, 4);
    if (!(pde & 1)) {
        return -1;
    }
    address_space_read(&address_space_memory, (pde & ~0xfffu) + ((dva >> 12) & 0x3ff) * 4,
                       MEMTXATTRS_UNSPECIFIED, &pte, 4);
    if (!(pte & 1)) {
        return -1;
    }
    return (pte & ~0xfffu) | (dva & 0xfff);
}

static uint32_t rd32(uint32_t pd, uint32_t dva)
{
    hwaddr pa = sgx_mmu(pd, dva);
    uint32_t v = 0;

    if (pa != (hwaddr)-1) {
        address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &v, 4);
    }
    return v;
}

static void wr32(uint32_t pd, uint32_t dva, uint32_t v)
{
    hwaddr pa = sgx_mmu(pd, dva);

    if (pa != (hwaddr)-1) {
        address_space_write(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &v, 4);
    }
}

/* completion counters only ever move forward */
static void wr32_mono(uint32_t pd, uint32_t dva, uint32_t v)
{
    if (dva && (int32_t)(v - rd32(pd, dva)) > 0) {
        wr32(pd, dva, v);
    }
}

static uint32_t sgx_dev_ldr(GarminRegStub *s, uint32_t dva)
{
    hwaddr pa = sgx_dev_to_phys(s, dva);
    uint32_t v = 0;

    if (pa != (hwaddr)-1) {
        address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &v, 4);
    }
    return v;
}

static void sgx_dev_str(GarminRegStub *s, uint32_t dva, uint32_t v)
{
    hwaddr pa = sgx_dev_to_phys(s, dva);

    if (pa != (hwaddr)-1) {
        address_space_write(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &v, 4);
    }
}

/* SGXOSTimer XORs EUR_CR_USE0/1_DM_SLOT every tick and declares a lockup
 * (-> HW recovery, full re-init) if the value stays the same three times.
 * A live microkernel keeps those moving, so we do too. */
static uint64_t sgx_read_fixup(GarminRegStub *s, hwaddr addr, uint64_t val)
{
    static uint32_t edm_counter;

    switch (addr) {
    case 0x0b10:                                /* EUR_CR_USE0_DM_SLOT */
    case 0x0b1c:                                /* EUR_CR_USE1_DM_SLOT */
        return ++edm_counter;
    case 0x0008:                                /* EUR_CR_CLKGATESTATUS: idle */
        return 0;
    default:
        return val;
    }
}

/*
 * Consume the kernel CCB like the microkernel would: every entry between the
 * read and write offsets is logged (service address + data words, with the
 * device-virtual data pointer translated) and the read offset catches up.
 * Command-specific completion work is done by sgx_process_command().
 */
static void sgx_process_command(GarminRegStub *s, uint32_t devinfo, const uint32_t *e);

static void sgx_consume_ccb(GarminRegStub *s, uint32_t devinfo)
{
    uint32_t ccbinfo = sgx_ldr(devinfo + 0x40);
    uint32_t base, woff_p, roff_p, woff, roff;

    if (!sgx_ptr_ok(ccbinfo)) {
        return;
    }
    base = sgx_ldr(ccbinfo + 0x8);
    woff_p = sgx_ldr(ccbinfo + 0xc);
    roff_p = sgx_ldr(ccbinfo + 0x10);
    if (!sgx_ptr_ok(base) || !sgx_ptr_ok(woff_p) || !sgx_ptr_ok(roff_p)) {
        return;
    }
    woff = sgx_ldr(woff_p) & 0xff;
    roff = sgx_ldr(roff_p) & 0xff;
    while (roff != woff) {
        uint32_t e[8];

        for (int i = 0; i < 8; i++) {
            e[i] = sgx_ldr(base + roff * 32 + 4 * i);
        }
        gpsmap_log("omap4.sgx: ccb[%u] svc=%03x %08x %08x %08x %08x %08x %08x %08x (data pa %08" HWADDR_PRIx ")\n",
                   roff, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7],
                   sgx_dev_to_phys(s, e[3]));
        sgx_process_command(s, devinfo, e);
        roff = (roff + 1) & 0xff;
    }
    sgx_str(roff_p, roff);
}

static void sgx_write_hook(GarminRegStub *s, hwaddr addr, uint64_t val)
{
    if (!gpsmap_sgx_shim) {
        return;                                 /* dead GPU: no microkernel */
    }
    switch (addr) {
    case EUR_CR_SOFT_RESET:
        s->regs[EUR_CR_EVENT_KICK / 4] = 0;
        s->regs[EUR_CR_EVENT_STATUS / 4] = 0;
        s->regs[EUR_CR_EVENT_STATUS2 / 4] = 0;
        sgx_update_irq(s);
        return;
    case EUR_CR_EVENT_HOST_CLEAR:               /* W1C into STATUS */
        s->regs[EUR_CR_EVENT_STATUS / 4] &= ~(uint32_t)val;
        s->regs[EUR_CR_EVENT_HOST_CLEAR / 4] = 0;
        sgx_update_irq(s);
        return;
    case EUR_CR_EVENT_HOST_CLEAR2:
        s->regs[EUR_CR_EVENT_STATUS2 / 4] &= ~(uint32_t)val;
        s->regs[EUR_CR_EVENT_HOST_CLEAR2 / 4] = 0;
        sgx_update_irq(s);
        return;
    case EUR_CR_EVENT_HOST_ENABLE:
    case EUR_CR_EVENT_HOST_ENABLE2:
        sgx_update_irq(s);
        return;
    default:
        break;
    }
    /* EUR_CR_EVENT_KICK starts the ukernel; EUR_CR_EVENT_KICK2 (+8) is the
     * host->ukernel doorbell for kernel CCB commands.  Both self-clear. */
    if ((addr != EUR_CR_EVENT_KICK && addr != EUR_CR_EVENT_KICK + 8) || !(val & 1)) {
        return;
    }
    s->regs[addr / 4] = 0;

    uint32_t node = sgx_ldr(SGX_DEVNODE_PTR);
    uint32_t devinfo = sgx_ptr_ok(node) ? sgx_ldr(node + SGX_DEVINFO_OFF) : 0;
    uint32_t hostctl = sgx_ptr_ok(devinfo) ? sgx_ldr(devinfo + SGX_HOSTCTL_OFF) : 0;

    if (!sgx_ptr_ok(hostctl)) {
        gpsmap_log("omap4.sgx: KICK but host-ctl unresolvable (node=%08x devinfo=%08x hostctl=%08x)\n",
                   node, devinfo, hostctl);
        return;
    }
    if (!(sgx_ldr(hostctl) & 1)) {
        sgx_str(hostctl, sgx_ldr(hostctl) | 1);    /* PVRSRV_USSE_EDM_INIT_COMPLETE */
        gpsmap_log("omap4.sgx: KICK -> microkernel init complete at hostctl %08x [%s]\n",
                   hostctl, gpsmap_ctx());
        return;
    }

    /*
     * Later kicks carry kernel CCB commands.  The one that gates everything
     * is SGXMKIF_CMD_GETMISCINFO: the driver clears bit 0 of the misc-info
     * block (devinfo +0x54 -> list -> block), kicks, and polls that bit for
     * 500 ms; then it compares the block against its own build (DDK version
     * 0x10711 / build 0xd3e39 at +0xc/+0x10, build options +0x18 == +0x4,
     * core revision +0x1c == devinfo +0x98, structure sizes +0x20..+0x50 ==
     * devinfo +0x9c..+0xcc).  A mismatch or timeout aborts PVR services
     * init and the display class is never registered.  Answer as a
     * microkernel built from the same tree would.
     */
    uint32_t misc_list = sgx_ldr(devinfo + 0x54);
    uint32_t ms = sgx_ptr_ok(misc_list) ? sgx_ldr(misc_list) : 0;

    sgx_consume_ccb(s, devinfo);

    if (sgx_ptr_ok(ms) && !(sgx_ldr(ms) & 1)) {
        sgx_str(ms + 0x0c, 0x00010711);
        sgx_str(ms + 0x10, 0x000d3e39);
        sgx_str(ms + 0x18, sgx_ldr(ms + 0x04));
        sgx_str(ms + 0x1c, sgx_ldr(devinfo + 0x98));
        for (int i = 0; i <= 12; i++) {
            sgx_str(ms + 0x20 + 4 * i, sgx_ldr(devinfo + 0x9c + 4 * i));
        }
        sgx_str(ms, sgx_ldr(ms) | 1);
        gpsmap_log("omap4.sgx: KICK -> misc-info block %08x answered [%s]\n",
                   ms, gpsmap_ctx());
    } else {
        gpsmap_log("omap4.sgx: KICK (command) [%s]\n", gpsmap_ctx());
    }
}

/*
 * The microkernel's side of the SGXMKIF protocol (PowerVR DDK 1.7.17), as
 * documented in docs/sgx_fake_ukernel.md.  We never execute any GPU work:
 * every kick is "completed" immediately by writing the completion counters
 * and status values the host expects, then the client CCB read offset is
 * advanced and a SW event interrupt is raised.  Frames therefore stay black,
 * but the OpenGL client, EGL and the display class keep flowing.
 */
#define CCB_SIZE_MASK   0xffff

static uint32_t sgx_client_pd(GarminRegStub *s, uint32_t ctx_pd)
{
    return ctx_pd ? ctx_pd : (s->regs[EUR_CR_BIF_DIR_LIST_BASE0 / 4] & ~0xfffu);
}

static void sgx_cmd_ta(GarminRegStub *s, uint32_t hwrc)
{
    uint32_t kpd = s->regs[EUR_CR_BIF_DIR_LIST_BASE0 / 4];
    uint32_t flags = rd32(kpd, hwrc + 0x00);
    uint32_t pd = sgx_client_pd(s, rd32(kpd, hwrc + 0x08));
    uint32_t base = rd32(kpd, hwrc + 0x0c);
    uint32_t ctl = rd32(kpd, hwrc + 0x10);
    int guard = 64;

    while (guard-- > 0) {
        uint32_t wr = rd32(pd, ctl), rd = rd32(pd, ctl + 4);
        uint32_t cmd, size, cflags, sh, n, a;

        if (rd == wr) {
            break;
        }
        cmd = base + rd;
        size = rd32(pd, cmd);
        cflags = rd32(pd, cmd + 4);
        sh = cmd + 0x50;
        gpsmap_log("omap4.sgx: TA ctx %08x pd %08x: cmd %08x size %x flags %x | shared ctrl %x nTA %u n3D %u nSrc %u | TAsync %08x/%08x 3Dsync %08x/%08x | +3c %08x | dep %08x/%08x\n",
                   hwrc, pd, cmd, size, cflags, rd32(pd, sh), rd32(pd, sh + 4), rd32(pd, sh + 8),
                   rd32(pd, sh + 0x2c), rd32(pd, sh + 0x14), rd32(pd, sh + 0x18), rd32(pd, sh + 0x24),
                   rd32(pd, sh + 0x28), rd32(pd, cmd + 0x3c), rd32(pd, sh + 0xb8), rd32(pd, sh + 0xbc));
        for (uint32_t off = 0; off < 0x50; off += 16) {
            gpsmap_log("omap4.sgx:   hdr+%02x: %08x %08x %08x %08x\n", off,
                       rd32(pd, cmd + off), rd32(pd, cmd + off + 4),
                       rd32(pd, cmd + off + 8), rd32(pd, cmd + off + 12));
        }
        for (uint32_t i = 0; i < MIN(rd32(pd, sh + 4), 8u); i++) {
            gpsmap_log("omap4.sgx:   TA status[%u]: *%08x = %08x\n", i,
                       rd32(pd, sh + 0xc0 + 8 * i), rd32(pd, sh + 0xc4 + 8 * i));
        }
        if (!(rd32(pd, sh) & 1) || size == 0 || size > 0x10000) {
            break;                              /* not READY / garbage */
        }
        /* TA finished */
        n = MIN(rd32(pd, sh + 0x2c), 8u);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t so = sh + 0x30 + 16 * i;
            wr32_mono(pd, rd32(pd, so + 4), rd32(pd, so) + 1);
        }
        a = rd32(pd, sh + 0x18);
        wr32_mono(pd, a, rd32(pd, sh + 0x14) + 1);
        n = MIN(rd32(pd, sh + 0x4), 32u);
        for (uint32_t i = 0; i < n; i++) {
            a = rd32(pd, sh + 0xc0 + 8 * i);
            if (a) {
                wr32(pd, a, rd32(pd, sh + 0xc4 + 8 * i));
            }
        }
        /* 3D finished (renders) */
        if (cflags & 0x2) {
            a = rd32(pd, sh + 0x28);
            wr32_mono(pd, a, rd32(pd, sh + 0x24) + 1);
            if (cflags & 0x200) {
                wr32_mono(pd, rd32(pd, sh + 0xbc), rd32(pd, sh + 0xb8) + 1);
            }
            a = rd32(pd, cmd + 0x3c);           /* HW dst sync list (see spec 4.4) */
            if (a) {
                uint32_t m = MIN(rd32(pd, a + 4), 16u);
                uint32_t access = rd32(pd, a);  /* render target "in use" word */

                for (uint32_t i = 0; i < m; i++) {
                    uint32_t so = a + 8 + 16 * i;
                    wr32_mono(pd, rd32(pd, so + 0xc), rd32(pd, so + 8) + 1);
                }
                /* SGXKickTA sets *access = 1 when it takes the render target
                 * and waits for the ukernel to clear it after the render. */
                if (access) {
                    wr32(pd, access, 0);
                    /*
                     * HEURISTIC: SGXKickTA also marks a second object "in use"
                     * (word at access-8 on the same render-target status page,
                     * seen as dev 0x0d80301c vs 0x0d803024) and waits for the
                     * ukernel to clear it after the render.  Structure owner
                     * not identified yet; clear it when it reads 1.
                     */
                    if (rd32(pd, access - 8) == 1) {
                        wr32(pd, access - 8, 0);
                    }
                }
            }
            n = MIN(rd32(pd, sh + 0x8), 4u);
            for (uint32_t i = 0; i < n; i++) {
                a = rd32(pd, sh + 0x1c0 + 8 * i);
                if (a) {
                    wr32(pd, a, rd32(pd, sh + 0x1c4 + 8 * i));
                }
            }
        }
        wr32(pd, ctl + 4, (rd + size) & CCB_SIZE_MASK);
        gpsmap_log("omap4.sgx: TA kick done: cmd %08x size %x flags %04x (rd %x -> %x, wr %x)\n",
                   cmd, size, cflags, rd, (rd + size) & CCB_SIZE_MASK, wr);
    }
    if (flags & 1) {
        wr32(kpd, hwrc, flags & ~1u);           /* NEWCONTEXT handled */
    }
}

static void sgx_cmd_transfer(GarminRegStub *s, uint32_t hwtc)
{
    /*
     * Transfer-queue command as observed in memory (SGXMKIF_TRANSFERCMD,
     * 0x180 bytes on this build):
     *   +0x70 ui32Size, +0x74 flags, +0x78 SGXMKIF_TRANSFERCMD_SHARED:
     *     +0x00 NumSrcSyncs, +0x04 asSrcSyncs[8] {RdPend, RdCplAddr, WrPend, WrCplAddr}
     *     +0x84 NumDstSyncs, +0x88 asDstSyncs[]
     *     +0x98 TA sync {WrPend, WrCplAddr, RdPend, RdCplAddr}
     *     +0xa8 3D sync {WrPend, WrCplAddr, RdPend, RdCplAddr}
     *     +0xb8 NumStatusVals, +0xbc status[4] {devaddr, value}
     */
    uint32_t kpd = s->regs[EUR_CR_BIF_DIR_LIST_BASE0 / 4];
    uint32_t pd = sgx_client_pd(s, rd32(kpd, hwtc + 0x08));
    uint32_t base = rd32(kpd, hwtc + 0x0c);
    uint32_t ctl = rd32(kpd, hwtc + 0x10);
    int guard = 64;

    while (guard-- > 0) {
        uint32_t wr = rd32(pd, ctl), rd = rd32(pd, ctl + 4);
        uint32_t cmd, size, sh, n;

        if (rd == wr) {
            break;
        }
        cmd = base + rd;
        size = rd32(pd, cmd + 0x70);
        sh = cmd + 0x78;
        if (size == 0 || size > 0x10000 || (size & 63)) {
            gpsmap_log("omap4.sgx: transfer cmd %08x has odd size %x, assuming 0x180\n", cmd, size);
            size = 0x180;
        }
        n = MIN(rd32(pd, sh), 8u);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t so = sh + 0x4 + 16 * i;
            wr32_mono(pd, rd32(pd, so + 4), rd32(pd, so) + 1);          /* src: read ops */
        }
        n = MIN(rd32(pd, sh + 0x84), 1u);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t so = sh + 0x88 + 16 * i;
            wr32_mono(pd, rd32(pd, so + 0xc), rd32(pd, so + 8) + 1);    /* dst: write ops */
        }
        wr32_mono(pd, rd32(pd, sh + 0x9c), rd32(pd, sh + 0x98) + 1);    /* TA sync write ops */
        wr32_mono(pd, rd32(pd, sh + 0xac), rd32(pd, sh + 0xa8) + 1);    /* 3D sync write ops */
        n = MIN(rd32(pd, sh + 0xb8), 4u);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = rd32(pd, sh + 0xbc + 8 * i);
            if (a) {
                wr32(pd, a, rd32(pd, sh + 0xc0 + 8 * i));               /* status value */
            }
        }
        wr32(pd, ctl + 4, (rd + size) & CCB_SIZE_MASK);
        gpsmap_log("omap4.sgx: transfer kick done: cmd %08x size %x dst %u status %u (rd %x -> %x, wr %x)\n",
                   cmd, size, rd32(pd, sh + 0x84), rd32(pd, sh + 0xb8), rd,
                   (rd + size) & CCB_SIZE_MASK, wr);
    }
}

static void sgx_cmd_miscinfo(GarminRegStub *s, uint32_t devinfo, uint32_t block_dva)
{
    uint32_t kpd = s->regs[EUR_CR_BIF_DIR_LIST_BASE0 / 4];
    hwaddr pa = sgx_mmu(kpd, block_dva);
    uint32_t v;

    if (pa == (hwaddr)-1) {
        return;
    }
    /* same contents as the CPU-side answer in the kick hook */
    v = 0x00010711; address_space_write(&address_space_memory, pa + 0x0c, MEMTXATTRS_UNSPECIFIED, &v, 4);
    v = 0x000d3e39; address_space_write(&address_space_memory, pa + 0x10, MEMTXATTRS_UNSPECIFIED, &v, 4);
    address_space_read(&address_space_memory, pa + 0x04, MEMTXATTRS_UNSPECIFIED, &v, 4);
    address_space_write(&address_space_memory, pa + 0x18, MEMTXATTRS_UNSPECIFIED, &v, 4);
    v = sgx_ldr(devinfo + 0x98);
    address_space_write(&address_space_memory, pa + 0x1c, MEMTXATTRS_UNSPECIFIED, &v, 4);
    for (int i = 0; i <= 12; i++) {
        v = sgx_ldr(devinfo + 0x9c + 4 * i);
        address_space_write(&address_space_memory, pa + 0x20 + 4 * i, MEMTXATTRS_UNSPECIFIED, &v, 4);
    }
    address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &v, 4);
    v |= 1;
    address_space_write(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &v, 4);
}

static void sgx_process_command(GarminRegStub *s, uint32_t devinfo, const uint32_t *e)
{
    uint32_t hostctl = sgx_ldr(devinfo + 0x7d8);
    int type = -1;

    for (int i = 0; i < 11; i++) {
        if (e[0] == sgx_ldr(devinfo + 0x58 + 4 * i) && e[0]) {
            type = i;
            break;
        }
    }
    switch (type) {
    case 0:                                     /* TA / render kick */
        sgx_cmd_ta(s, e[3]);
        break;
    case 1:                                     /* transfer queue kick */
        sgx_cmd_transfer(s, e[3]);
        break;
    case 3:                                     /* POWER: 1 off, 2 idle, 3 resume */
        if (sgx_ptr_ok(hostctl)) {
            if (e[3] == 1) {
                sgx_str(hostctl + 4, sgx_ldr(hostctl + 4) | 0x8);
            } else if (e[3] == 2) {
                sgx_str(hostctl + 4, sgx_ldr(hostctl + 4) | 0x4);
            }
        }
        break;
    case 5:                                     /* CLEANUP: host waits for hostctl+8 bit0 */
        if (sgx_ptr_ok(hostctl)) {
            sgx_str(hostctl + 8, sgx_ldr(hostctl + 8) | 1);
        }
        break;
    case 6:                                     /* GETMISCINFO */
        sgx_cmd_miscinfo(s, devinfo, e[3]);
        break;
    default:
        break;
    }
    gpsmap_log("omap4.sgx: command type %d (svc %03x) done\n", type, e[0]);
    sgx_raise_event(s, SGX_EVENT_SW_EVENT);
}

/* ------------------------------------------------------------------ */
/* OMAP4 DISPC: graphics pipeline rendered to a QEMU console            */
/*                                                                        */
/* Register subset: SYSCONFIG/SYSSTATUS, IRQSTATUS/IRQENABLE, CONTROL1/2, */
/* CONFIG1/2, SIZE_LCD1/2, GFX_BA0/BA1, GFX_POSITION/SIZE/ATTRIBUTES/     */
/* ROW_INC/PIXEL_INC.  Everything else is stored and read back.           */
/* A 60 Hz timer raises VSYNC/FRAMEDONE/EVSYNC and clears the GO bits.    */
/* ------------------------------------------------------------------ */

#define TYPE_GARMIN_OMAP4_DISPC "garmin.omap4-dispc"
OBJECT_DECLARE_SIMPLE_TYPE(Omap4Dispc, GARMIN_OMAP4_DISPC)

#define DISPC_IRQ_FRAMEDONE     (1 << 0)
#define DISPC_IRQ_VSYNC         (1 << 1)
#define DISPC_IRQ_EVSYNC_EVEN   (1 << 2)
#define DISPC_IRQ_EVSYNC_ODD    (1 << 3)
#define DISPC_IRQ_FRAMEDONE2    (1 << 22)
#define DISPC_IRQ_VSYNC2        (1 << 18)
#define DISPC_IRQ_FRAMEDONETV   (1 << 24)

struct Omap4Dispc {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *vsync;
    QemuConsole *con;
    uint32_t regs[0x1000 / 4];
    bool invalidate;
    int cur_w, cur_h;
};

#define R(off) (s->regs[(off) / 4])

static void dispc_update_irq(Omap4Dispc *s)
{
    qemu_set_irq(s->irq, (R(0x18) & R(0x1c)) != 0);
}

static void dispc_vsync_cb(void *opaque)
{
    Omap4Dispc *s = opaque;
    uint32_t irq = 0;

    if (R(0x40) & 1) {                          /* CONTROL1.LCDENABLE */
        irq |= DISPC_IRQ_VSYNC | DISPC_IRQ_FRAMEDONE;
        R(0x40) &= ~(1u << 5);                  /* GOLCD self-clears */
    }
    if (R(0x40) & 2) {                          /* CONTROL1.TVENABLE */
        irq |= DISPC_IRQ_EVSYNC_EVEN | DISPC_IRQ_EVSYNC_ODD | DISPC_IRQ_FRAMEDONETV;
        R(0x40) &= ~(1u << 6);                  /* GOTV */
    }
    if (R(0x238) & 1) {                         /* CONTROL2.LCDENABLE (LCD2) */
        irq |= DISPC_IRQ_VSYNC2 | DISPC_IRQ_FRAMEDONE2;
        R(0x238) &= ~(1u << 5);
    }
    R(0x18) |= irq;
    dispc_update_irq(s);
    timer_mod(s->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
}

static uint64_t dispc_read(void *opaque, hwaddr addr, unsigned size)
{
    Omap4Dispc *s = opaque;
    uint32_t val;

    switch (addr) {
    case 0x00: val = 0x00000040; break;         /* REVISION */
    case 0x10: val = R(0x10) & ~2u; break;      /* SYSCONFIG SOFTRESET */
    case 0x14: val = 1; break;                  /* SYSSTATUS RESETDONE */
    case 0x5c: val = 0; break;                  /* LINE_STATUS */
    case 0xa8: val = 0x00000400; break;         /* GFX_BUF_SIZE_STATUS */
    default:   val = R(addr & ~3u);
    }
    if (addr != 0x18 && addr != 0x1c && addr != 0x238) {   /* rare traffic: always log */
        gpsmap_log("omap4.dispc: rd  %08" HWADDR_PRIx " -> %08x\n",
                   (hwaddr)(0x58001000 + addr), val);
    }
    return val;
}

static void dispc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Omap4Dispc *s = opaque;

    if (addr != 0x18 && addr != 0x1c) {                    /* skip per-frame IRQ acks */
        gpsmap_log("omap4.dispc: wr  %08" HWADDR_PRIx " <- %08" PRIx64 "\n",
                   (hwaddr)(0x58001000 + addr), val);
    }
    switch (addr) {
    case 0x18:                                  /* IRQSTATUS W1C */
        R(0x18) &= ~val;
        dispc_update_irq(s);
        return;
    case 0x1c:
        R(0x1c) = val;
        dispc_update_irq(s);
        return;
    case 0x40: case 0x238:                      /* CONTROL1/2 */
        R(addr) = val;
        s->invalidate = true;
        if (val & 3) {
            timer_mod(s->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      NANOSECONDS_PER_SECOND / 60);
        }
        return;
    case 0x80: case 0x84: case 0x88: case 0x8c: case 0xa0: case 0xac: case 0xb0:
    case 0x7c: case 0x3cc:
        R(addr) = val;
        if (addr == 0x80) {
            garmin_gl_set_scanout(val);         /* host renderer target */
        }
        s->invalidate = true;
        return;
    default:
        R(addr & ~3u) = val;
    }
}

static const MemoryRegionOps dispc_ops = {
    .read = dispc_read,
    .write = dispc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* Convert one source pixel to 32-bit xRGB for the display surface */
static void dispc_draw(Omap4Dispc *s)
{
    DisplaySurface *surf = qemu_console_surface(s->con);
    uint32_t attr = R(0xa0);
    uint32_t sz = R(0x8c);
    int w = (sz & 0x7ff) + 1, h = ((sz >> 16) & 0x7ff) + 1;
    int fmt = (attr >> 1) & 0xf;
    hwaddr ba = R(0x80);
    int32_t row_inc = (int32_t)R(0xac);
    int bpp;

    if (!(attr & 1) || !((R(0x40) | R(0x238)) & 1)) {
        return;                                 /* GFX pipeline or LCD off */
    }
    switch (fmt) {
    case 0x6: bpp = 2; break;                   /* RGB16 */
    case 0x8: bpp = 3; break;                   /* RGB24 packed */
    case 0x9: case 0xc: case 0xd: case 0x0: bpp = 4; break; /* xRGB32 / ARGB32 / RGBA32 */
    default:  bpp = 4;
    }
    if (w != s->cur_w || h != s->cur_h) {
        qemu_console_resize(s->con, w, h);
        surf = qemu_console_surface(s->con);
        s->cur_w = w; s->cur_h = h;
    }
    uint32_t *dst = surface_data(surf);
    int dst_stride = surface_stride(surf) / 4;
    g_autofree uint8_t *line = g_malloc(w * bpp);
    hwaddr src = ba;
    for (int y = 0; y < h; y++) {
        address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED,
                           line, w * bpp);
        uint32_t *d = dst + y * dst_stride;
        for (int x = 0; x < w; x++) {
            const uint8_t *px = line + x * bpp;
            uint32_t v;
            switch (bpp) {
            case 2: {
                uint16_t p = px[0] | (px[1] << 8);
                v = ((p >> 11) & 0x1f) << 19 | ((p >> 5) & 0x3f) << 10 | (p & 0x1f) << 3;
                break;
            }
            case 3: v = px[0] | (px[1] << 8) | (px[2] << 16); break;
            default: v = px[0] | (px[1] << 8) | (px[2] << 16); break;
            }
            d[x] = v;
        }
        src += w * bpp + (row_inc > 0 ? row_inc - 1 : 0);
    }
    dpy_gfx_update_full(s->con);
}

static void dispc_gfx_update(void *opaque)
{
    Omap4Dispc *s = opaque;

    dispc_draw(s);
    s->invalidate = false;
}

static void dispc_gfx_invalidate(void *opaque)
{
    Omap4Dispc *s = opaque;

    s->invalidate = true;
}

static const GraphicHwOps dispc_gfx_ops = {
    .invalidate = dispc_gfx_invalidate,
    .gfx_update = dispc_gfx_update,
};

static void dispc_reset(DeviceState *dev)
{
    Omap4Dispc *s = GARMIN_OMAP4_DISPC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /*
     * The main image never programs the panel itself: it inherits the
     * configuration left behind by the factory bootblock and only flips
     * GFX_BA0/BA1.  Seed that state: LCD2 channel enabled, GFX pipeline
     * 1024x600 RGB16 at the first framebuffer, stride = width.
     */
    R(0x238) = 0x00000001;                      /* CONTROL2 LCDENABLE */
    R(0x3cc) = (599 << 16) | 1023;              /* SIZE_LCD2 */
    R(0x80)  = 0xbed46000;                      /* GFX_BA0 */
    R(0x84)  = 0xbefc6000;                      /* GFX_BA1 */
    R(0x88)  = 0;                               /* GFX_POSITION */
    R(0x8c)  = (599 << 16) | 1023;              /* GFX_SIZE */
    R(0xa0)  = 0x4e00488d;                      /* GFX_ATTRIBUTES: enabled, RGB16, LCD2 */
    R(0xac)  = 1;                               /* GFX_ROW_INC */
    R(0xb0)  = 1;                               /* GFX_PIXEL_INC */
    R(0x620) = 0x00000004;                      /* CONFIG2 LOADMODE=2 */
    timer_mod(s->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
}

static void dispc_realize(DeviceState *dev, Error **errp)
{
    Omap4Dispc *s = GARMIN_OMAP4_DISPC(dev);

    s->vsync = timer_new_ns(QEMU_CLOCK_VIRTUAL, dispc_vsync_cb, s);
    memory_region_init_io(&s->iomem, OBJECT(s), &dispc_ops, s, "omap4.dispc",
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->con = graphic_console_init(dev, 0, &dispc_gfx_ops, s);
    qemu_console_resize(s->con, 1024, 600);
    s->cur_w = 1024; s->cur_h = 600;
}

static int dispc_post_load(void *opaque, int version_id)
{
    Omap4Dispc *s = opaque;

    s->invalidate = true;
    s->cur_w = s->cur_h = 0;                    /* force a console resize */
    return 0;
}

static const VMStateDescription vmstate_dispc = {
    .name = "omap4.dispc",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = dispc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(vsync, Omap4Dispc),
        VMSTATE_UINT32_ARRAY(regs, Omap4Dispc, 0x1000 / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void dispc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_dispc;

    dc->realize = dispc_realize;
    device_class_set_legacy_reset(dc, dispc_reset);
}

static const TypeInfo dispc_info = {
    .name = TYPE_GARMIN_OMAP4_DISPC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Omap4Dispc),
    .class_init = dispc_class_init,
};

/* ------------------------------------------------------------------ */
/* Boot ROM stub                                                          */
/*                                                                        */
/*  0x00  b   park          (reset: secondary CPUs park here)             */
/*  0x04  b   .             (undef)                                       */
/*  0x08  b   smc_handler   (monitor vector: SMC)                         */
/*  0x0c.. b  .                                                           */
/*  0x20  smc_handler: mov r0,#0 ; movs pc,lr                             */
/*  0x40  park: ldr r1,=WKUPGEN_AUX_CORE_BOOT_0                           */
/*        loop: ldr r0,[r1]; cmp r0,#0; bne go; wfe; b loop               */
/*        go:   ldr pc,[r1,#4]                                            */
/* ------------------------------------------------------------------ */

#define ARM_B(from, to)  (0xea000000u | ((((int32_t)(to) - (int32_t)(from) - 8) >> 2) & 0x00ffffffu))
#define ARM_BNE(from, to) (0x1a000000u | ((((int32_t)(to) - (int32_t)(from) - 8) >> 2) & 0x00ffffffu))

#define ROM_SMC_HANDLER  0x20
#define ROM_PARK         0x40
#define ROM_BOOT0        0x90
#define ROM_PARK_NS      0x140

/*
 * The factory bootblock (secure world) hands the loader/main image over in
 * the Non-secure world with every GIC interrupt assigned to group 1 and
 * Non-secure access to VFP/NEON and ACTLR.SMP granted.  Without that the
 * firmware's GIC enables are silently ignored (group-0 interrupts cannot be
 * enabled from Non-secure state) and no timer tick ever arrives.
 *
 * Common "drop to NS SVC" sequence, placed at `at`, continuing at `target`:
 *   cps #mon; NSACR |= cp10|cp11|NS_SMP; SCR |= NS|FW|AW;
 *   spsr = SVC(ARM, I/F masked); lr = target; movs pc, lr
 */
static void rom_emit_to_ns(uint32_t *rom, uint32_t at, uint32_t target)
{
    rom[(at + 0x00) / 4] = 0xe59f003c;          /* ldr r0, [pc, #0x3c] -> +0x44 */
    rom[(at + 0x04) / 4] = 0xe3a010ff;          /* mov r1, #0xff */
    rom[(at + 0x08) / 4] = 0xe5801000;          /* str r1, [r0]  GICC_PMR (secure) */
    rom[(at + 0x0c) / 4] = 0xf1020016;          /* cps #0x16 (monitor) */
    rom[(at + 0x10) / 4] = 0xee110f51;          /* mrc p15,0,r0,c1,c1,2 NSACR */
    rom[(at + 0x14) / 4] = 0xe3800b03;          /* orr r0, r0, #0xc00 */
    rom[(at + 0x18) / 4] = 0xe3800701;          /* orr r0, r0, #0x40000 */
    rom[(at + 0x1c) / 4] = 0xee010f51;          /* mcr p15,0,r0,c1,c1,2 */
    rom[(at + 0x20) / 4] = 0xee110f11;          /* mrc p15,0,r0,c1,c1,0 SCR */
    rom[(at + 0x24) / 4] = 0xe3800031;          /* orr r0, r0, #NS|FW|AW */
    rom[(at + 0x28) / 4] = 0xee010f11;          /* mcr p15,0,r0,c1,c1,0 */
    rom[(at + 0x2c) / 4] = 0xe3a010d3;          /* mov r1, #0xd3 */
    rom[(at + 0x30) / 4] = 0xe169f001;          /* msr spsr_cxsf, r1 */
    rom[(at + 0x34) / 4] = 0xe59fe004;          /* ldr lr, [pc, #4] -> +0x40 */
    rom[(at + 0x38) / 4] = 0xf57ff06f;          /* isb */
    rom[(at + 0x3c) / 4] = 0xe1b0f00e;          /* movs pc, lr */
    rom[(at + 0x40) / 4] = target;
    rom[(at + 0x44) / 4] = OMAP4_MPCORE_BASE + 0x104;   /* GICC_PMR */
}

static void build_boot_rom(uint32_t *rom, uint32_t entry)
{
    memset(rom, 0, 0x200);
    rom[0x00 / 4] = ARM_B(0x00, ROM_PARK);
    rom[0x04 / 4] = ARM_B(0x04, 0x04);
    rom[0x08 / 4] = ARM_B(0x08, ROM_SMC_HANDLER);
    rom[0x0c / 4] = ARM_B(0x0c, 0x0c);
    rom[0x10 / 4] = ARM_B(0x10, 0x10);
    rom[0x14 / 4] = ARM_B(0x14, 0x14);
    rom[0x18 / 4] = ARM_B(0x18, 0x18);
    rom[0x1c / 4] = ARM_B(0x1c, 0x1c);
    /* smc handler: ROM secure services (L2 enable etc.) return success */
    rom[0x20 / 4] = 0xe3a00000;                 /* mov r0, #0 */
    rom[0x24 / 4] = 0xe1b0f00e;                 /* movs pc, lr */

    /* secondary CPU entry (0x40..0x87): drop to NS, then park */
    rom_emit_to_ns(rom, ROM_PARK, OMAP4_ROM_BASE + ROM_PARK_NS);

    /* primary CPU entry (0x90): all interrupts to group 1, drop to NS, jump */
    rom[0x90 / 4] = 0xe59f0024;                 /* ldr r0, [pc, #0x24] -> 0xbc */
    rom[0x94 / 4] = 0xe3e01000;                 /* mvn r1, #0 */
    rom[0x98 / 4] = 0xe5801000;                 /* str r1, [r0]       IGROUPR0 */
    rom[0x9c / 4] = 0xe5801004;                 /* str r1, [r0, #4]   IGROUPR1 */
    rom[0xa0 / 4] = 0xe5801008;                 /* str r1, [r0, #8]   IGROUPR2 */
    rom[0xa4 / 4] = 0xe580100c;                 /* str r1, [r0, #12]  IGROUPR3 */
    rom[0xa8 / 4] = 0xe5801010;                 /* str r1, [r0, #16]  IGROUPR4 */
    rom[0xac / 4] = ARM_B(0xac, 0xc0);
    rom[0xbc / 4] = OMAP4_MPCORE_BASE + 0x1080; /* GICD_IGROUPR0 */
    rom_emit_to_ns(rom, 0xc0, entry);           /* 0xc0..0x107 */

    /* NS park loop for secondary CPUs (OMAP4 AUX_CORE_BOOT protocol) */
    rom[0x140 / 4] = 0xe59f1014;                /* ldr r1, [pc, #0x14] -> 0x15c */
    rom[0x144 / 4] = 0xe5910000;                /* loop: ldr r0, [r1] */
    rom[0x148 / 4] = 0xe3500000;                /* cmp r0, #0 */
    rom[0x14c / 4] = ARM_BNE(0x14c, 0x158);     /* bne go */
    rom[0x150 / 4] = 0xe320f002;                /* wfe */
    rom[0x154 / 4] = ARM_B(0x154, 0x144);       /* b loop */
    rom[0x158 / 4] = 0xe591f004;                /* go: ldr pc, [r1, #4] */
    rom[0x15c / 4] = OMAP4_WKUPGEN_BASE + OMAP4_WKUPGEN_AUX_BOOT0;
}

/* ------------------------------------------------------------------ */
/* Machine                                                                */
/* ------------------------------------------------------------------ */

#define TYPE_GPSMAP_MACHINE MACHINE_TYPE_NAME("gpsmap7x08")
OBJECT_DECLARE_SIMPLE_TYPE(GpsmapMachineState, GPSMAP_MACHINE)

struct GpsmapMachineState {
    MachineState parent_obj;

    ARMCPU *cpu[2];
    MemoryRegion rom;
    MemoryRegion rom_alias;
    MemoryRegion sram;
    MemoryRegion sar_ram;
    MemoryRegion gpmc_cs0;
    uint64_t entry;
    bool stub_log;
    bool no_sgx;
    char *gl_hooks;
    char *bootcfg;
};

static void gpsmap_write_bootcfg(GpsmapMachineState *s)
{
    /*
     * Default boot-config record: 0xA55A magic at both candidate copies,
     * record body zero.  A file given via -machine bootcfg=<file> is
     * loaded at GPMC_CS0_BASE instead (full window image).
     */
    uint8_t magic[4] = { 0x5a, 0xa5, 0x5a, 0xa5 };

    if (s->bootcfg) {
        if (load_image_targphys(s->bootcfg, GPMC_CS0_BASE, GPMC_CS0_SIZE,
                                NULL) < 0) {
            error_report("gpsmap7x08: could not load bootcfg image %s",
                         s->bootcfg);
            exit(1);
        }
        return;
    }
    rom_add_blob_fixed("gpsmap.bootcfg.magicA", magic, 4, GPMC_CS0_BASE + 0x8000);
    rom_add_blob_fixed("gpsmap.bootcfg.magicB", magic, 4, GPMC_CS0_BASE + 0xc000);
}

static void gpsmap_machine_reset(MachineState *machine, ResetType type)
{
    GpsmapMachineState *s = GPSMAP_MACHINE(machine);

    qemu_devices_reset(type);

    /* All CPUs start in the boot ROM stub (secure), which drops to the
     * Non-secure world; CPU0 then enters the firmware, others park. */
    cpu_set_pc(CPU(s->cpu[0]), OMAP4_ROM_BASE + ROM_BOOT0);
    for (int i = 1; i < machine->smp.cpus; i++) {
        cpu_set_pc(CPU(s->cpu[i]), OMAP4_ROM_BASE + ROM_PARK);
    }
}

static void gpsmap_init(MachineState *machine)
{
    GpsmapMachineState *s = GPSMAP_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *mpcore;
    GarminRegStub *st;
    uint32_t rom[0x80];
    int ncpu = machine->smp.cpus;

    gpsmap_stub_log = s->stub_log;
    gpsmap_sgx_shim = !s->no_sgx;

    if (ncpu > 2) {
        error_report("gpsmap7x08: at most 2 CPUs supported");
        exit(1);
    }

    /* --- memories --- */
    memory_region_add_subregion(sysmem, OMAP4_DDR_BASE, machine->ram);

    memory_region_init_ram(&s->sram, NULL, "omap4.sram", OMAP4_SRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, OMAP4_SRAM_BASE, &s->sram);

    memory_region_init_ram(&s->rom, NULL, "omap4.rom", OMAP4_ROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, OMAP4_ROM_BASE, &s->rom);
    memory_region_init_alias(&s->rom_alias, NULL, "omap4.rom.alias", &s->rom,
                             0, OMAP4_ROM_SIZE);
    memory_region_add_subregion(sysmem, 0x00000000, &s->rom_alias);

    memory_region_init_ram(&s->sar_ram, NULL, "omap4.sar_ram",
                           OMAP4_SAR_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, OMAP4_SAR_RAM_BASE, &s->sar_ram);

    memory_region_init_ram(&s->gpmc_cs0, NULL, "gpmc.cs0", GPMC_CS0_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, GPMC_CS0_BASE, &s->gpmc_cs0);

    gpsmap_write_bootcfg(s);

    /* --- CPUs --- */
    for (int i = 0; i < ncpu; i++) {
        Object *cpuobj = object_new(machine->cpu_type);

        object_property_set_int(cpuobj, "reset-cbar", OMAP4_MPCORE_BASE,
                                &error_fatal);
        /* Cortex-A9 boots in secure state; the firmware relies on SMC */
        object_property_set_bool(cpuobj, "has_el3", true, &error_abort);
        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        s->cpu[i] = ARM_CPU(cpuobj);
    }

    /* --- MPCore private peripherals: SCU, GIC, global/private timers --- */
    mpcore = qdev_new(TYPE_A9MPCORE_PRIV);
    qdev_prop_set_uint32(mpcore, "num-cpu", ncpu);
    qdev_prop_set_uint32(mpcore, "num-irq", OMAP4_GIC_NUM_IRQ);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(mpcore), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(mpcore), 0, OMAP4_MPCORE_BASE);
    for (int i = 0; i < ncpu; i++) {
        DeviceState *cpudev = DEVICE(s->cpu[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(mpcore), i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(mpcore), ncpu + i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
    }
#define SPI(n) qdev_get_gpio_in(mpcore, (n))

    /* --- L2 cache controller --- */
    sysbus_create_simple("l2x0", OMAP4_PL310_BASE, NULL);

    /* --- catch-all unimplemented regions (lowest priority) --- */
    create_unimplemented_device("omap4.l3", 0x44000000, 0x01000000);
    create_unimplemented_device("omap4.l4_per", 0x48000000, 0x00400000);
    create_unimplemented_device("omap4.l4_abe", 0x49000000, 0x00100000);
    create_unimplemented_device("omap4.l4_cfg", 0x4a000000, 0x01000000);
    create_unimplemented_device("omap4.emif_dmm", 0x4c000000, 0x04000000);
    create_unimplemented_device("omap4.gpmc_regs", 0x50000000, 0x01000000);
    create_unimplemented_device("omap4.sgx-mem", 0x56010000, 0x01ff0000);
    create_unimplemented_device("omap4.dss", 0x58000000, 0x01000000);
    create_unimplemented_device("omap4.iva", 0x5a000000, 0x02000000);
    create_unimplemented_device("omap4.abe_l3", 0x40100000, 0x00100000);
    create_unimplemented_device("omap4.ocmc_ram", 0x40400000, 0x00100000);

    /* --- control module & PRCM --- */
    st = make_stub("omap4.ctrl_core", 0x4a002000, 0x1000, 0);
    stub_set(st, 0x200, 0x12345678);           /* STD_FUSE_DIE_ID_0 */
    stub_set(st, 0x204, 0x2b94e02f);           /* ID_CODE: OMAP4460 ES1.1 */
    stub_set(st, 0x208, 0x9abcdef0);           /* STD_FUSE_DIE_ID_1 */
    stub_set(st, 0x20c, 0x13579bdf);           /* STD_FUSE_DIE_ID_2 */
    stub_set(st, 0x210, 0x2468ace0);           /* STD_FUSE_DIE_ID_3 */
    stub_set(st, 0x214, 0x00000000);           /* STD_FUSE_PROD_ID_0 */
    stub_set(st, 0x218, 0x00000000);           /* STD_FUSE_PROD_ID_1 */
    stub_set(st, 0x2c4, 0x00000000);           /* STATUS: sys_boot pins */

    /*
     * PRCM reset/boot-ROM state for an OMAP4460 on a 38.4 MHz sys_clk, as
     * a real bootblock would leave it (values follow U-Boot's dpll tables):
     *   MPU  1200 MHz: M=125 N=3  M2=1
     *   CORE 1600 MHz: M=125 N=5  M2=1 M3=5 M4=8 M5=4 M6=6 M7=6
     *   PER  1536 MHz: M=20  N=0  M2=8 M3=6 M4=12 M5=9 M6=4 M7=5
     *   IVA  1862 MHz: M=97  N=3  M4=5 M5=6
     *   ABE  196.6 MHz: M=64 N=24 M2=1 M3=1
     *   USB  1920 MHz: M=50  N=1  M2=2
     */
    st = make_stub("omap4.cm1", 0x4a004000, 0x2000, 0);
    st->read_fixup = cm1_read_fixup;
    stub_set(st, 0x100, 0x00000110);   /* CM_CLKSEL_CORE: L3=/2 L4=/2 */
    stub_set(st, 0x108, 0x00000501);   /* CM_CLKSEL_ABE */
    stub_set(st, 0x110, 0x00000000);   /* CM_DLL_CTRL */
    stub_set(st, 0x120, 0x00000007);   /* CM_CLKMODE_DPLL_CORE: lock */
    stub_set(st, 0x12c, 0x00007d05);   /* CM_CLKSEL_DPLL_CORE */
    stub_set(st, 0x130, 0x00000001);   /* CM_DIV_M2_DPLL_CORE */
    stub_set(st, 0x134, 0x00000005);
    stub_set(st, 0x138, 0x00000008);
    stub_set(st, 0x13c, 0x00000004);
    stub_set(st, 0x140, 0x00000006);
    stub_set(st, 0x144, 0x00000006);
    stub_set(st, 0x160, 0x00000007);   /* CM_CLKMODE_DPLL_MPU */
    stub_set(st, 0x16c, 0x00007d03);   /* CM_CLKSEL_DPLL_MPU */
    stub_set(st, 0x170, 0x00000001);   /* CM_DIV_M2_DPLL_MPU */
    stub_set(st, 0x1a0, 0x00000007);   /* CM_CLKMODE_DPLL_IVA */
    stub_set(st, 0x1ac, 0x00006103);   /* CM_CLKSEL_DPLL_IVA */
    stub_set(st, 0x1b8, 0x00000005);
    stub_set(st, 0x1bc, 0x00000006);
    stub_set(st, 0x1e0, 0x00000007);   /* CM_CLKMODE_DPLL_ABE */
    stub_set(st, 0x1ec, 0x00004018);   /* CM_CLKSEL_DPLL_ABE */
    stub_set(st, 0x1f0, 0x00000001);
    stub_set(st, 0x1f4, 0x00000001);
    stub_set(st, 0x220, 0x00000007);   /* CM_CLKMODE_DPLL_DDRPHY */
    stub_set(st, 0x22c, 0x00007d05);
    stub_set(st, 0x230, 0x00000001);
    st = make_stub("omap4.cm2", 0x4a008000, 0x2000, 0);
    st->read_fixup = cm2_read_fixup;
    stub_set(st, 0x140, 0x00000007);   /* CM_CLKMODE_DPLL_PER */
    stub_set(st, 0x14c, 0x00001400);   /* CM_CLKSEL_DPLL_PER */
    stub_set(st, 0x150, 0x00000008);
    stub_set(st, 0x154, 0x00000006);
    stub_set(st, 0x158, 0x0000000c);
    stub_set(st, 0x15c, 0x00000009);
    stub_set(st, 0x160, 0x00000004);
    stub_set(st, 0x164, 0x00000005);
    stub_set(st, 0x180, 0x00000007);   /* CM_CLKMODE_DPLL_USB */
    stub_set(st, 0x18c, 0x00003201);
    stub_set(st, 0x190, 0x00000002);
    make_stub("omap4.ctrl_core_pad", 0x4a100000, 0x1000, 0);
    st = make_stub("omap4.prm", 0x4a306000, 0x2000, 0);
    st->read_fixup = prm_read_fixup;
    stub_set(st, 0x110, 0x00000007);   /* CM_SYS_CLKSEL: 38.4 MHz */
    stub_set(st, 0x400, 0x00000003);   /* PM_CORE_PWRSTCTRL: ON */
    stub_set(st, 0x300, 0x00000003);   /* PM_MPU_PWRSTCTRL: ON */
    stub_set(st, 0xb00, 0x00000003);   /* PM_L4PER_PWRSTCTRL: ON */
    stub_set(st, 0x1b08, 0x00000001);  /* PRM_RSTST: global cold reset */
    make_stub("omap4.scrm", 0x4a30a000, 0x1000, 0);
    make_stub("omap4.ctrl_wkup", 0x4a30c000, 0x1000, 0);
    make_stub("omap4.ctrl_wkup_pad", 0x4a31e000, 0x1000, 0);
    st = make_stub("omap4.wkupgen", OMAP4_WKUPGEN_BASE, 0x1000, 0);
    st->quiet = true;

    st = make_stub("omap4.trng", 0x48090000, 0x2000, 0);
    st->read_fixup = trng_read_fixup;

    /* --- Imaging subsystem (ISS: video input path), logging stub --- */
    st = make_stub("omap4.iss", 0x52000000, 0x100000, 0);
    stub_set(st, 0x000, 0x00000000);           /* ISS_HL_REVISION */
    stub_set(st, 0x014, 0x00000001);           /* ISS_HL_SYSSTATUS? */
    stub_selfclear(st, 0x010, 0x1);            /* ISS_HL_SYSCONFIG SOFTRESET */

    /*
     * Ducati (dual Cortex-M3 IPU) subsystem used for HDMI/video: the M3
     * cores themselves are not emulated; their L2 RAM and MMU are provided
     * so the host-side driver can load firmware and configure the MMU.
     */
    {
        MemoryRegion *l2ram = g_new(MemoryRegion, 1);
        memory_region_init_ram(l2ram, NULL, "ducati.l2ram", 0x10000, &error_fatal);
        memory_region_add_subregion(sysmem, 0x55020000, l2ram);
    }
    st = make_stub("omap4.ducati_mmu", 0x55080000, 0x4000, 0);
    stub_set(st, 0x2000, 0x00000011);          /* MMU_REVISION */
    stub_set(st, 0x2014, 0x00000001);          /* MMU_SYSSTATUS RESETDONE */
    stub_selfclear(st, 0x2010, 0x2);           /* MMU_SYSCONFIG SOFTRESET */
    {
        DeviceState *mb = qdev_new(TYPE_GARMIN_OMAP4_MBOX);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(mb), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(mb), 0, 0x4a0f4000);
        sysbus_connect_irq(SYS_BUS_DEVICE(mb), 0, SPI(26));  /* MAIL_U0_MPU_IRQ */
    }

    /* --- SmartReflex (AVS) sensors --- */
    st = make_stub("omap4.sr_mpu", 0x4a0d9000, 0x1000, 0); st->read_fixup = sr_read_fixup;
    st = make_stub("omap4.sr_iva", 0x4a0db000, 0x1000, 0); st->read_fixup = sr_read_fixup;
    st = make_stub("omap4.sr_core", 0x4a0dd000, 0x1000, 0); st->read_fixup = sr_read_fixup;

    /* --- 32K sync counter, watchdog --- */
    sysbus_create_simple(TYPE_GARMIN_OMAP4_32K, 0x4a304000, NULL);
    st = make_stub("omap4.wdt2", 0x4a314000, 0x1000, 0);
    stub_set(st, 0x14, 1);                      /* WDSC.. WDST RESETDONE */

    /* --- GP timers --- */
    /*
     * Firmware delay constants (e.g. 0x65aa ticks between I2C retries) only
     * make sense with the timers fed from SYS_CLK (38.4 MHz), which is the
     * CLKSEL=0 selection the firmware programs in CM_L4PER_GPTIMERx_CLKCTRL.
     * GPTIMER1 lives in the always-on domain and defaults to the 32 kHz clock.
     */
#define OMAP4_SYS_CLK 38400000
    make_gpt("omap4.gpt1",  0x4a318000, true,  32768,         SPI(IRQ_GPT1));
    make_gpt("omap4.gpt2",  0x48032000, true,  OMAP4_SYS_CLK, SPI(IRQ_GPT2));
    make_gpt("omap4.gpt3",  0x48034000, false, OMAP4_SYS_CLK, SPI(IRQ_GPT3));
    make_gpt("omap4.gpt4",  0x48036000, false, OMAP4_SYS_CLK, SPI(IRQ_GPT4));
    make_gpt("omap4.gpt5",  0x49038000, false, OMAP4_SYS_CLK, SPI(IRQ_GPT5));
    make_gpt("omap4.gpt6",  0x4903a000, false, OMAP4_SYS_CLK, SPI(IRQ_GPT6));
    make_gpt("omap4.gpt7",  0x4903c000, false, OMAP4_SYS_CLK, SPI(IRQ_GPT7));
    make_gpt("omap4.gpt8",  0x4903e000, false, OMAP4_SYS_CLK, SPI(IRQ_GPT8));
    make_gpt("omap4.gpt9",  0x4803e000, false, OMAP4_SYS_CLK, SPI(IRQ_GPT9));
    make_gpt("omap4.gpt10", 0x48086000, true,  OMAP4_SYS_CLK, SPI(IRQ_GPT10));
    make_gpt("omap4.gpt11", 0x48088000, false, OMAP4_SYS_CLK, SPI(IRQ_GPT11));

    /* --- UARTs --- */
    make_uart(0x4806a000, SPI(IRQ_UART1), serial_hd(1));
    make_uart(0x4806c000, SPI(IRQ_UART2), serial_hd(2));
    make_uart(0x48020000, SPI(IRQ_UART3), serial_hd(0));   /* console */
    make_uart(0x4806e000, SPI(IRQ_UART4), serial_hd(3));

    /* --- GPIO banks --- */
    /*
     * Board identification: HAL signals 0x13, 0x06, 0x28, 0x1f are packed
     * (bits 3..0) into the hardware-type byte at SRAM 0x40300445.  With the
     * HAL pin table the firmware installs at runtime they read GPIO3 pin 22,
     * GPIO2 pin 22, GPIO4 pin 19 and GPIO4 pin 4.  All four low = type 0 =
     * GPSMAP 7x08 class (1024x600 panel).  A type above 9 leaves the display
     * type unset and the firmware divides by zero in the geometry setup.
     */
    make_gpio("omap4.gpio1", 0x4a310000, SPI(IRQ_GPIO1), 0xffffffff);
    /* GPIO2.22: board-ID input; GPIO2.21: LAN9221 interrupt (active low, handler 0x80d77d16) */
    DeviceState *gpio2 = make_gpio("omap4.gpio2", 0x48055000, SPI(IRQ_GPIO2),
                                   0xffffffff & ~(1u << 22));
    /*
     * GPIO3.22: board-ID input (low = 7x08).  GPIO3.24/25: active-high
     * interrupt requests from the external UARTs on GPMC CS2/CS3; they idle
     * low, otherwise the firmware services a phantom interrupt forever.
     */
    DeviceState *gpio3 = make_gpio("omap4.gpio3", 0x48057000, SPI(IRQ_GPIO3),
                                   0xffffffff & ~(1u << 22) & ~(1u << 24) & ~(1u << 25));
    make_gpio("omap4.gpio4", 0x48059000, SPI(IRQ_GPIO4),
              0xffffffff & ~((1u << 19) | (1u << 4) | (1u << 17)));
    /* GPIO4.17 = card detect of SD slot sd0 (HAL signal 0x27, active low):
     * a card is present on HSMMC4 (docs/sd_card.md). */
    /* GPIO5.12 = touch controller INT (HAL signal 0x30; descriptor = bank*32+pin) */
    DeviceState *gpio5 = make_gpio("omap4.gpio5", 0x4805b000, SPI(IRQ_GPIO5), 0xffffffff);
    make_gpio("omap4.gpio6", 0x4805d000, SPI(IRQ_GPIO6), 0xffffffff);

    /* --- I2C, McSPI --- */
    {
        Omap4I2C *bus[4];
        bus[0] = make_i2c("omap4.i2c1", 0x48070000, SPI(IRQ_I2C1));
        bus[1] = make_i2c("omap4.i2c2", 0x48072000, SPI(IRQ_I2C2));
        bus[2] = make_i2c("omap4.i2c3", 0x48060000, SPI(IRQ_I2C3));
        bus[3] = make_i2c("omap4.i2c4", 0x48350000, SPI(IRQ_I2C4));
        Omap4I2C *i2c1 = bus[0];
        /*
         * Every 7-bit address on the four buses gets a generic register-file
         * slave so probes never NACK: the main image dereferences NULL and
         * reboots on unanswered devices (board controller at I2C4 0x04,
         * touch controller at I2C4 0x38, sensor at I2C4 0x48).  Identified
         * devices replace these one by one (TWL6030 below on I2C1).
         */
        for (int b = 0; b < 4; b++) {
            for (int a = 0x02; a < 0x78; a++) {
                if (b == 0 && a >= 0x48 && a <= 0x4b) {
                    continue;
                }
                if (b == 3 && a == 0x04) {
                    continue;               /* eGalax touch controller below */
                }
                g_autofree char *nm = g_strdup_printf("i2c%d.slave-%02x", b + 1, a);
                make_i2c_regfile(bus[b]->bus, a, nm);
            }
        }
        /* EETI eGalax touch controller on I2C4, INT on GPIO5.12 (active low) */
        {
            DeviceState *tp = qdev_new(TYPE_GARMIN_EGALAX);

            qdev_prop_set_uint8(tp, "address", 0x04);
            qdev_realize_and_unref(tp, BUS(bus[3]->bus), &error_fatal);
            qdev_connect_gpio_out(tp, 0, qdev_get_gpio_in(gpio5, 12));
        }
        /* TWL6030 PMIC: four slave IDs on I2C1 */
        make_i2c_regfile(i2c1->bus, 0x48, "twl6030.id0");
        make_i2c_regfile(i2c1->bus, 0x49, "twl6030.id1");
        make_i2c_regfile(i2c1->bus, 0x4a, "twl6030.id2");
        make_i2c_regfile(i2c1->bus, 0x4b, "twl6030.id3");
    }
    st = make_stub("omap4.mcspi1", 0x48098000, 0x1000, 0); st->read_fixup = mcspi_read_fixup;
    st = make_stub("omap4.mcspi2", 0x4809a000, 0x1000, 0); st->read_fixup = mcspi_read_fixup;
    st = make_stub("omap4.mcspi3", 0x480b8000, 0x1000, 0); st->read_fixup = mcspi_read_fixup;
    st = make_stub("omap4.mcspi4", 0x480ba000, 0x1000, 0); st->read_fixup = mcspi_read_fixup;

    /* --- MMC/eMMC --- */
    /* eMMC ("mnand") is device 1 = HSMMC2 (fixed by the firmware's slot
     * table); HSMMC1 carries the user SD card slot. */
    make_hsmmc("omap4.hsmmc1", 0, 0x4809c000, SPI(IRQ_MMC1), 0, false);
    make_hsmmc("omap4.hsmmc2", 1, 0x480b4000, SPI(IRQ_MMC2), 1, true);
    make_hsmmc("omap4.hsmmc3", 2, 0x480ad000, SPI(IRQ_MMC3), 2, false);
    make_hsmmc("omap4.hsmmc4", 3, 0x480d1000, SPI(IRQ_MMC4), 3, false);
    make_hsmmc("omap4.hsmmc5", 4, 0x480d5000, SPI(IRQ_MMC5), 4, false);

    /*
     * GPMC CS2..CS7 windows (128 MiB each per the loader's GPMC_CONFIG7
     * programming: 0x10000000, 0x18000000, ... 0x38000000).  Board devices
     * behind them are still unidentified; logging stubs for discovery.
     */
    /*
     * GPMC CS2: dual 16550-compatible UART (NMEA 0183 ports), byte-wide
     * registers at a 4-byte stride, channel 1 at +0x100.  The channel
     * interrupts arrive on GPIO3 pins 24 and 25 (active-high level; the
     * firmware enables both with LEVELDETECT1).  DLL=163 gives 4800 baud
     * with the base clock below.
     */
    {
        static const hwaddr ext_uart_base[2] = { 0x10000000, 0x10000100 };
        static const int ext_uart_pin[2] = { 24, 25 };

        for (int i = 0; i < 2; i++) {
            DeviceState *u = qdev_new(TYPE_SERIAL_MM);

            qdev_prop_set_uint8(u, "regshift", 2);
            qdev_prop_set_uint32(u, "baudbase", 4800 * 163);
            qdev_prop_set_chr(u, "chardev", serial_hd(4 + i));
            qdev_prop_set_uint8(u, "endianness", DEVICE_LITTLE_ENDIAN);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(u), &error_fatal);
            sysbus_mmio_map_overlap(SYS_BUS_DEVICE(u), 0, ext_uart_base[i], 1);
            sysbus_connect_irq(SYS_BUS_DEVICE(u), 0,
                               qdev_get_gpio_in(gpio3, ext_uart_pin[i]));
        }
        make_stub("gpmc.cs2", 0x10000000, 0x100000, 0);
    }
    make_stub("gpmc.cs3", 0x18000000, 0x100000, 0);
    make_stub("gpmc.cs4", 0x20000000, 0x100000, 0);
    make_stub("gpmc.cs5", 0x28000000, 0x100000, 0);
    make_stub("gpmc.cs6", 0x30000000, 0x100000, 0);
    make_stub("gpmc.cs7", 0x38000000, 0x100000, 0);

    /* --- GPMC, EMIF, DMM, SDMA --- */
    st = make_stub("omap4.gpmc", 0x50000000, 0x1000, 0);
    stub_set(st, 0x14, 1);                      /* GPMC_SYSSTATUS RESETDONE */
    stub_selfclear(st, 0x10, 0x2);              /* GPMC_SYSCONFIG SOFTRESET */
    st = make_stub("omap4.emif1", 0x4c000000, 0x1000, 0);
    stub_set(st, 0x04, 0x4);                    /* EMIF_STATUS PHY_DLL_READY */
    st = make_stub("omap4.emif2", 0x4d000000, 0x1000, 0);
    stub_set(st, 0x04, 0x4);
    make_stub("omap4.dmm", 0x4e000000, 0x1000, 0);
    {
        DeviceState *dma = qdev_new(TYPE_GARMIN_OMAP4_SDMA);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dma), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dma), 0, 0x4a056000);
        for (int i = 0; i < 4; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(dma), i, SPI(12 + i));
        }
    }

    /* --- display subsystem (stub until a framebuffer model lands) --- */
    st = make_stub("omap4.sgx", 0x56000000, 0x10000, 0);
    stub_set(st, 0x10, 0x01140000);             /* EUR_CR_CORE_ID: SGX540 */
    stub_set(st, 0x14, 0x00010125);             /* EUR_CR_CORE_REVISION 1.2.5 */
    st->write_hook = sgx_write_hook;
    st->read_fixup = sgx_read_fixup;
    sgx_stub = st;
    sgx_irq = SPI(21);                          /* GFX_IRQ */
    st = make_stub("omap4.dss", 0x58000000, 0x1000, 0);
    stub_set(st, 0x00, 0x00000040);             /* DSS_REVISION */
    stub_set(st, 0x14, 1);                      /* DSS_SYSSTATUS RESETDONE */
    stub_selfclear(st, 0x10, 0x2);
    {
        DeviceState *dispc = qdev_new(TYPE_GARMIN_OMAP4_DISPC);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dispc), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dispc), 0, 0x58001000);
        sysbus_connect_irq(SYS_BUS_DEVICE(dispc), 0, SPI(25));   /* DSS_IRQ */
    }
    make_stub("omap4.rfbi", 0x58002000, 0x1000, 0);
    make_stub("omap4.venc", 0x58003000, 0x1000, 0);
    st = make_stub("omap4.dsi1", 0x58004000, 0x1000, 0);
    stub_set(st, 0x304, 0x1);                   /* DSI_PLL_STATUS RESET_DONE */
    stub_set(st, 0x54, 0x80000000);             /* DSI_CLK_CTRL PLL_PWR_STATUS */
    make_stub("omap4.dsi2", 0x58005000, 0x1000, 0);
    make_stub("omap4.hdmi", 0x58006000, 0x1000, 0);

    /* --- USB --- */
    make_stub("omap4.usbtll", 0x4a062000, 0x1000, 0);
    make_stub("omap4.usbhost", 0x4a064000, 0x1000, 0);
    make_stub("omap4.musb", 0x4a0ab000, 0x1000, 0);

    /*
     * Ethernet: SMSC LAN9221 on GPMC CS1 (0x0c000000, from GPMC_CONFIG7_1
     * programmed by the loader; first access is PMT_CTRL at +0x84).
     * Interrupt line: GPIO2.21 active low on the real board (the firmware
     * registers the LAN service routine 0x80d77d16 for that pin, level-low).
     * Connecting QEMU's LAN9118 interrupt there (inverted) storms: the model
     * keeps the line asserted after the firmware's acknowledge sequence.
     * Until that is understood the interrupt stays on an unused SPI; the
     * firmware's network tasks still run (polling), no traffic is expected.
     */
    (void)gpio2;
    lan9118_init(0x0c000000, SPI(35));

    /* --- firmware images --- */
    if (machine->kernel_filename) {
        if (load_image_targphys(machine->kernel_filename, LOADER_LINK_ADDR,
                                machine->ram_size - (LOADER_LINK_ADDR -
                                                     OMAP4_DDR_BASE),
                                NULL) < 0) {
            error_report("gpsmap7x08: could not load %s",
                         machine->kernel_filename);
            exit(1);
        }
    }
    if (machine->firmware) {
        if (load_image_targphys(machine->firmware, MAIN_LINK_ADDR,
                                machine->ram_size - (MAIN_LINK_ADDR -
                                                     OMAP4_DDR_BASE),
                                NULL) < 0) {
            error_report("gpsmap7x08: could not load %s", machine->firmware);
            exit(1);
        }
        if (!machine->kernel_filename && s->entry == LOADER_LINK_ADDR) {
            s->entry = MAIN_LINK_ADDR;
        }
    }

    /* boot ROM stub needs the final entry address */
    build_boot_rom(rom, s->entry);
    rom_add_blob_fixed("gpsmap.bootrom", rom, sizeof(rom), OMAP4_ROM_BASE);

    /* OpenGL ES 1.1 API interception (after the image ROM blobs so the
     * reset handler runs after rom_reset()). */
    if (s->gl_hooks) {
        int n = garmin_gl_load_table(s->gl_hooks, &error_fatal);

        garmin_gl_mmio_init(sysmem);
        qemu_register_reset(garmin_gl_reset, NULL);
        error_report("gpsmap7x08: %d GL hooks from %s", n, s->gl_hooks);
    }
}

static void gpsmap_instance_init(Object *obj)
{
    GpsmapMachineState *s = GPSMAP_MACHINE(obj);

    s->entry = LOADER_LINK_ADDR;
    object_property_add_uint64_ptr(obj, "entry", &s->entry,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "entry",
        "Reset PC for CPU0 (default: image link address 0x80050000)");
}

static bool gpsmap_get_stub_log(Object *obj, Error **errp)
{
    return GPSMAP_MACHINE(obj)->stub_log;
}

static void gpsmap_set_stub_log(Object *obj, bool value, Error **errp)
{
    GPSMAP_MACHINE(obj)->stub_log = value;
}

static char *gpsmap_get_gl_hooks(Object *obj, Error **errp)
{
    return g_strdup(GPSMAP_MACHINE(obj)->gl_hooks);
}

static void gpsmap_set_gl_hooks(Object *obj, const char *value, Error **errp)
{
    GpsmapMachineState *s = GPSMAP_MACHINE(obj);

    g_free(s->gl_hooks);
    s->gl_hooks = g_strdup(value);
}

static bool gpsmap_get_no_sgx(Object *obj, Error **errp)
{
    return GPSMAP_MACHINE(obj)->no_sgx;
}

static void gpsmap_set_no_sgx(Object *obj, bool value, Error **errp)
{
    GPSMAP_MACHINE(obj)->no_sgx = value;
}

static char *gpsmap_get_bootcfg(Object *obj, Error **errp)
{
    return g_strdup(GPSMAP_MACHINE(obj)->bootcfg);
}

static void gpsmap_set_bootcfg(Object *obj, const char *value, Error **errp)
{
    GpsmapMachineState *s = GPSMAP_MACHINE(obj);

    g_free(s->bootcfg);
    s->bootcfg = g_strdup(value);
}

static void gpsmap_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Garmin GPSMAP 7x08 chartplotter (TI OMAP4460, 2x Cortex-A9)";
    mc->init = gpsmap_init;
    mc->reset = gpsmap_machine_reset;
    mc->max_cpus = 2;
    mc->min_cpus = 1;
    mc->default_cpus = 2;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a9");
    mc->default_ram_size = 1 * GiB;   /* loader relocates itself to 0xA0000000 */
    mc->default_ram_id = "gpsmap.ram";
    mc->ignore_memory_transaction_failures = false;
    mc->block_default_type = IF_SD;
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;

    object_class_property_add_bool(oc, "stub-log", gpsmap_get_stub_log,
                                   gpsmap_set_stub_log);
    object_class_property_set_description(oc, "stub-log",
        "Log every access to stubbed OMAP4 register blocks");
    object_class_property_add_bool(oc, "no-sgx", gpsmap_get_no_sgx,
                                   gpsmap_set_no_sgx);
    object_class_property_add_str(oc, "gl-hooks", gpsmap_get_gl_hooks,
                                  gpsmap_set_gl_hooks);
    object_class_property_set_description(oc, "gl-hooks",
        "File with OpenGL ES entry points to intercept (name addr nargs)");
    object_class_property_set_description(oc, "no-sgx",
        "Do not answer the SGX540 microkernel handshakes (GPU appears dead)");
    object_class_property_add_str(oc, "bootcfg", gpsmap_get_bootcfg,
                                  gpsmap_set_bootcfg);
    object_class_property_set_description(oc, "bootcfg",
        "Image loaded into the GPMC CS0 window at 0x08000000 (boot config)");
}

static const TypeInfo gpsmap_machine_info = {
    .name = TYPE_GPSMAP_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(GpsmapMachineState),
    .instance_init = gpsmap_instance_init,
    .class_init = gpsmap_class_init,
    .interfaces = arm_machine_interfaces,
};

static void gpsmap_register_types(void)
{
    type_register_static(&regstub_info);
    type_register_static(&sync32k_info);
    type_register_static(&gpt_info);
    type_register_static(&omap4_uart_info);
    type_register_static(&omap4_gpio_info);
    type_register_static(&omap4_i2c_info);
    type_register_static(&regfile_info);
    type_register_static(&hsmmc_info);
    type_register_static(&sdma_info);
    type_register_static(&dispc_info);
    type_register_static(&mbox_info);
    type_register_static(&egalax_info);
    type_register_static(&gpsmap_machine_info);
}

type_init(gpsmap_register_types)
