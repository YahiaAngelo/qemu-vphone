/*
 * vphone-linux A/B switch (include/qemu/vphone-ab.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/vphone-ab.h"
#include "qemu/thread.h"
#include "hw/core/cpu.h"
#include "exec/tb-flush.h"
#include "exec/cputlb.h"

int vphone_ab_arm;
const char *vphone_ab_what;
static char *vab_file;

static void vab_tlb_flush(CPUState *cpu, run_on_cpu_data d)
{
    tlb_flush(cpu);
}

static void *vab_thread(void *opaque)
{
    for (;;) {
        char c = 0;
        FILE *f;

        g_usleep(500 * 1000);
        f = fopen(vab_file, "r");
        if (!f) {
            continue;
        }
        if (fread(&c, 1, 1, f) != 1) {
            c = 0;
        }
        fclose(f);
        if ((c == '0' || c == '1') && c - '0' != qatomic_read(&vphone_ab_arm)) {
            CPUState *cpu;
            struct timespec ts;

            qatomic_set(&vphone_ab_arm, c - '0');
            if (first_cpu) {
                queue_tb_flush(first_cpu);
                CPU_FOREACH(cpu) {
                    async_run_on_cpu(cpu, vab_tlb_flush, RUN_ON_CPU_NULL);
                }
            }
            clock_gettime(CLOCK_MONOTONIC, &ts);
            fprintf(stderr, "vphone-ab arm=%d t=%ld.%03ld\n", c - '0', (long)ts.tv_sec,
                    ts.tv_nsec / 1000000);
        }
    }
    return NULL;
}

static void __attribute__((constructor)) vab_init(void)
{
    const char *e = getenv("VPHONE_AB_FILE");
    const char *a = getenv("VPHONE_AB");
    QemuThread th;

    vphone_ab_arm = a && *a == '1';
    vphone_ab_what = getenv("VPHONE_AB_WHAT");
    if (!e || !*e) {
        return;
    }
    vab_file = g_strdup(e);
    qemu_thread_create(&th, "vphone-ab", vab_thread, NULL, QEMU_THREAD_DETACHED);
}
