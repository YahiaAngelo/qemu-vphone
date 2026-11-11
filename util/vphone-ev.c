/*
 * vphone diagnostic event rates (VPHONE_EVENT_STATS=<seconds>); see include/qemu/vphone-ev.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/vphone-ev.h"
#include "qemu/thread.h"

int vphone_ev_on;
__thread unsigned long long *vphone_ev_tls;

#define VEV_THREADS 256
static unsigned long long *vev_arrays[VEV_THREADS];
static int vev_nthreads;
static QemuMutex vev_lock;
static int vev_period;

static const char *const vev_names[VEV_MAX] = {
    [VEV_SVC_EL0] = "svc_el0", [VEV_SVC_OTHER] = "svc_hvc_smc_other",
    [VEV_IRQ] = "irq", [VEV_FIQ] = "fiq",
    [VEV_DABT_EL0] = "dabt_el0", [VEV_DABT_EL1] = "dabt_el1",
    [VEV_IABT] = "iabt", [VEV_UDEF] = "udef_trap", [VEV_GENTER] = "genter",
    [VEV_GXF_ABORT] = "gxf_abort", [VEV_EXC_OTHER] = "exc_other",
    [VEV_ERET] = "eret", [VEV_GEXIT] = "gexit",
    [VEV_TLB_FILL] = "tlb_fill",
    [VEV_FLUSH_MMUIDX] = "flush_mmuidx", [VEV_FLUSH_MMUIDX_ALL] = "flush_mmuidx_allcpu",
    [VEV_FLUSH_ASID] = "flush_asid_switch",
    [VEV_FLUSH_PAGE] = "flush_page", [VEV_FLUSH_PAGE_ALL] = "flush_page_allcpu",
    [VEV_FLUSH_RANGE] = "flush_range", [VEV_FLUSH_RANGE_ALL] = "flush_range_allcpu",
    [VEV_TB_GEN] = "tb_gen", [VEV_TB_GEN_KCYC] = "tb_gen_kcycles",
    [VEV_NOTDIRTY] = "notdirty_write", [VEV_MMIO] = "mmio",
    [VEV_RANGE_FULL_SIZE] = "range_full_size", [VEV_RANGE_FULL_LARGE] = "range_full_large",
    [VEV_PAGE_FULL_LARGE] = "page_full_large", [VEV_FLUSHIDX_ALL] = "flushidx_all",
    [VEV_FLUSHIDX_ONE] = "flushidx_one", [VEV_FLUSHIDX_OTHER] = "flushidx_other",
    [VEV_RANGE_BY_LOG] = "range_by_log",
    [VEV_ASID_RECENT8] = "asid_recent8", [VEV_ASID_KEY_RECENT8] = "asid_sprr_recent8",
    [VEV_SPRR_EL0_FLUSH] = "sprr_el0_flush", [VEV_SPRR_EL1_FLUSH] = "sprr_el1_flush",
    [VEV_GXF_ERET_FLUSH] = "gxf_eret_flush",
};

void vphone_ev_register(void)
{
    unsigned long long *a = g_new0(unsigned long long, VEV_MAX);

    qemu_mutex_lock(&vev_lock);
    if (vev_nthreads < VEV_THREADS) {
        vev_arrays[vev_nthreads++] = a;
    }
    qemu_mutex_unlock(&vev_lock);
    vphone_ev_tls = a;
}

static void *vev_logger(void *opaque)
{
    unsigned long long prev[VEV_MAX] = { 0 }, sum[VEV_MAX];
    struct timespec t0;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct timespec now;
        GString *line = g_string_new(NULL);

        g_usleep(vev_period * G_USEC_PER_SEC);
        clock_gettime(CLOCK_MONOTONIC, &now);
        memset(sum, 0, sizeof(sum));
        qemu_mutex_lock(&vev_lock);
        for (int t = 0; t < vev_nthreads; t++) {
            for (int k = 0; k < VEV_MAX; k++) {
                sum[k] += qatomic_read(&vev_arrays[t][k]);
            }
        }
        qemu_mutex_unlock(&vev_lock);
        g_string_printf(line, "vphone-ev t=%ld", (long)(now.tv_sec - t0.tv_sec));
        for (int k = 0; k < VEV_MAX; k++) {
            unsigned long long r = (sum[k] - prev[k]) / vev_period;

            if (k < VEV_FILLX) {
                g_string_append_printf(line, " %s=%llu", vev_names[k], r);
            } else if (r) {
                g_string_append_printf(line, " fill_i%d_%s=%llu", (k - VEV_FILLX) / 2,
                                       (k - VEV_FILLX) & 1 ? "hi" : "lo", r);
            }
            prev[k] = sum[k];
        }
        fprintf(stderr, "%s\n", line->str);
        g_string_free(line, true);
    }
    return NULL;
}

static void __attribute__((constructor)) vev_init(void)
{
    const char *e = getenv("VPHONE_EVENT_STATS");
    QemuThread th;

    if (!e || !*e || atoi(e) <= 0) {
        return;
    }
    vev_period = atoi(e);
    qemu_mutex_init(&vev_lock);
    vphone_ev_on = 1;
    qemu_thread_create(&th, "vphone-ev", vev_logger, NULL, QEMU_THREAD_DETACHED);
}
