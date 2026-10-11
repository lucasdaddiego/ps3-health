/* PS3 Health's part of the preview: the drive from a console's dumps, 383 and
 * 409 (PV_DRIVE, PV_ST, PV_RISE, PV_TEMP_OFF, PV_383_RC, PV_383_FAIL_AT), and
 * the six SPUs of the SPU kit (PV_SPU_FAIL, PV_SPU_LATE, PV_SPU_STALL,
 * PV_LS_FAULT, PV_HEAT_RATE): README.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include "lv2/systime.h"
#include "ppu-lv2.h"
#include "sys/spu.h"

/* PV_LS_FAULT: one bit stuck at 1 in one word of one SPU's local store. The
 * kit's kernel writes through SPU_KIT_WR, defined here before the include. */
static volatile uint32_t *ls_fault;
#define LS_FAULT_BIT 0x10u
#define SPU_KIT_WR(w, i, v) ((w)[i] = &(w)[i] == ls_fault ? (v) | LS_FAULT_BIT : (v))
#include <ps3gfx/spu_kit.h>

/* ---- syscalls: the emulated drive ----------------------------------------- */

typedef struct __attribute__((packed)) {
    u16 features, sector_count, lba_low, lba_mid, lba_high;
    u8 device, command;
    u32 is_ext, proto, in_out, size, pad;
    u64 buffer;
    u32 arglen, pad2;
} ata_block;

static int load(const char *name, u8 *out)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", getenv("PV_DRIVE") ? getenv("PV_DRIVE") : ".", name);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, 512, f);
    fclose(f);
    return n == 512 ? 0 : -1;
}

static u64 ata(ata_block *b, u8 *out)
{
    switch (b->command) {
    case 0xEC: return load("identify.bin", out) ? 0x80010003 : 0;
    case 0xB0:
        if (b->features == 0xD0) {
            if (load("smart.bin", out)) return 0x80010003;
            if (getenv("PV_ST")) out[363] = (u8)strtol(getenv("PV_ST"), NULL, 16);
            return 0;
        }
        if (b->features == 0xD1) return load("thresh.bin", out) ? 0x80010003 : 0;
        if (b->features == 0xD5 && b->lba_low == 6) return load("selftest.bin", out) ? 0x80010003 : 0;
        if (b->features == 0xD5 && b->lba_low == 1) return load("errlog.bin", out) ? 0x80010003 : 0;
        if (b->features == 0xD4) return 0;
        return 0x80010003;
    case 0x2F:
        if (b->lba_low == 4 && b->lba_mid == 0) { memset(out, 0, 512); out[0] = 1; out[8] = 1; out[9] = 1; return 0; }
        if (b->lba_low == 4 && b->lba_mid == 1) return load("devstat.bin", out) ? 0x80010003 : 0;
        if (b->lba_low == 0x11) return load("phy.bin", out) ? 0x80010003 : 0;
        return 0x80010003;
    }
    return 0x80010003;
}

/* ---- syscalls: the six SPUs ------------------------------------------------ */

/* The kit's program: on the console the archive's member, here no bytes. The
 * image calls are SPRX imports on the console (the firmware's liblv2), not
 * syscalls. The threads: ls runs to its end at the start, heat counts in a
 * host thread on the preview's clock until the terminate. */
const uint8_t spu_kit_spu[1];
const uint32_t spu_kit_spu_size = 0;

s32 sysSpuImageImport(sysSpuImage *image, const void *elf, u32 type)
{
    (void)elf;
    (void)type;
    memset(image, 0, sizeof *image);
    return 0;
}

s32 sysSpuImageClose(sysSpuImage *image)
{
    (void)image;
    return 0;
}

#define GROUP 0x4000
#define THREAD 0x5000
#define LS_LO 0x1980u                /* lo of the kit's program at ps3gfx 296b172: its _end, 0x1910, rounded up to 128 */
#define LS_HI (SPU_KIT_LS_TOP - SPU_KIT_LS_STACK)
#define LS_FAULT_WORD 0x4000u        /* PV_LS_FAULT: the word at lo + 0x10000 */
#define LS_TICKS 7980000u            /* a model, 0.1 s of the 79.8 MHz timebase: the host's time is not an SPU's */
#define ESTAT 0x8001000fu            /* LV2's CELL_ESTAT: a thread that did not end by itself has no exit status */

static struct {
    u32 num, cause, status;
    sysSpuThreadArgument arg[SPU_KIT_SPUS];
    s32 exit_status[SPU_KIT_SPUS];
    int exited[SPU_KIT_SPUS];                    /* the thread ended by itself, with exit_status */
} G;
static uint32_t ls_area[(LS_HI - LS_LO) / 4];   /* the area of one SPU: the threads run one after the other */
static pthread_t counter;                        /* heat: the counts, until the terminate */
static int counting;
static volatile int count_stop;

static int env_int(const char *name, int dflt) { return getenv(name) ? atoi(getenv(name)) : dflt; }

static void thread_exit(u32 k, s32 status)
{
    G.exit_status[k] = status;
    G.exited[k] = 1;
}

/* heat: every report counts on the preview's clock, PV_HEAT_RATE loops per
 * microsecond (default 355, the design's estimate for the console), a report
 * every SPU_KIT_HEAT_BATCH loops. PV_SPU_STALL=i,n: SPU i reports no more
 * after n s. PV_SPU_LATE's SPU never counts. */
static void *heat_count(void *unused)
{
    (void)unused;
    u64 rate = (u64)env_int("PV_HEAT_RATE", 355), t0 = sysGetSystemTime();
    int stall = -1, stall_s = 0, late = -1;
    if (getenv("PV_SPU_STALL")) sscanf(getenv("PV_SPU_STALL"), "%d,%d", &stall, &stall_s);
    if (getenv("PV_SPU_LATE")) sscanf(getenv("PV_SPU_LATE"), "%d", &late);
    while (!count_stop) {
        u64 dt = sysGetSystemTime() - t0;
        for (u32 k = 0; k < G.num && k < SPU_KIT_SPUS; k++) {
            if ((int)k == late) continue;
            u64 t = (int)k == stall && dt > (u64)stall_s * 1000000 ? (u64)stall_s * 1000000 : dt;
            spu_kit_rec *r = (spu_kit_rec *)(uintptr_t)G.arg[k].arg0;
            __atomic_store_n(&r->count, t * rate / SPU_KIT_HEAT_BATCH * SPU_KIT_HEAT_BATCH, __ATOMIC_RELAXED);
        }
        sched_yield();
    }
    return NULL;
}

/* One thread from its start, as the kit's program runs it: the checks of the
 * mark and arg0, the start report; then ls to its end (the 16 passes with a
 * report after each, the ticks and done = 1, the exit status), while heat
 * goes on in heat_count. A report is the thread's whole 128 bytes, as its
 * DMA. PV_SPU_LATE=i: SPU i never reports and never ends (the late path);
 * PV_SPU_LATE=i,0x200: it ends at once with that status, before its first
 * report, as after a bad argument (0x300: a bad mode). */
static void spu_thread(u32 k)
{
    const sysSpuThreadArgument *a = &G.arg[k];
    spu_kit_rec *out = (spu_kit_rec *)(uintptr_t)a->arg0;
    spu_kit_rec rec;
    u32 mode = (u32)(a->arg2 >> 32), index = (u32)a->arg2 & 0xffff;
    int late = -1, late_status = 0;
    if (((u32)a->arg2 >> 16) != SPU_KIT_MARK || !a->arg0 || (a->arg0 & 127)) { thread_exit(k, SPU_KIT_EXIT_ARGS); return; }
    if (mode != SPU_KIT_LS && mode != SPU_KIT_HEAT) { thread_exit(k, SPU_KIT_EXIT_MODE); return; }
    if (getenv("PV_SPU_LATE")) sscanf(getenv("PV_SPU_LATE"), "%d,%i", &late, &late_status);
    if (late == (int)index) {                        /* no report: the late path */
        if (late_status) thread_exit(k, late_status);
        return;
    }
    memset(&rec, 0, sizeof rec);
    rec.magic = SPU_KIT_MAGIC;
    rec.index = index;
    rec.mode = mode;
    rec.arg0 = a->arg0;
    rec.arg1 = a->arg1;
    rec.arg2 = a->arg2;
    *out = rec;
    if (mode == SPU_KIT_HEAT) return;                /* it counts in heat_count until the terminate */
    rec.lo = LS_LO;
    rec.hi = LS_HI;
    ls_fault = env_int("PV_LS_FAULT", -1) == (int)index ? &ls_area[LS_FAULT_WORD] : NULL;
    int stall = -1, stall_at = 0;                    /* PV_SPU_STALL=i,n: SPU i stops after pass n */
    if (getenv("PV_SPU_STALL")) sscanf(getenv("PV_SPU_STALL"), "%d,%d", &stall, &stall_at);
    for (u32 p = 0; p < SPU_KIT_LS_PASSES; p++) {
        if (stall == (int)index && (int)p == stall_at) { ls_fault = NULL; return; }   /* no done and no end: the stop at 5 s */
        spu_kit_ls_pass(&rec, ls_area, (rec.hi - rec.lo) / 4, rec.lo, p);
        rec.count = p + 1;
        *out = rec;
    }
    ls_fault = NULL;
    rec.ticks = LS_TICKS;
    rec.done = 1;
    *out = rec;
    thread_exit(k, (s32)(SPU_KIT_EXIT_LS | index));
}

/* One emulated group. The start runs each thread's start at once, in the
 * caller's thread: ls to its end, so the reports are all there when it
 * returns; heat then counts in a host thread. A refused call (PV_SPU_FAIL)
 * changes nothing: a refused terminate leaves heat counting (the stuck
 * path). */
static u64 spu_call(int n, u64 a, u64 b, u64 c, u64 f)
{
    if (env_int("PV_SPU_FAIL", 0) == n) return 0x80010002;   /* a refused call, for the error paths */
    switch (n) {
    case 169:                                        /* initialize */
        return 0;
    case 170:                                        /* group create */
        memset(&G, 0, sizeof G);
        G.num = (u32)b;
        *(u32 *)(uintptr_t)a = GROUP;
        return 0;
    case 172:                                        /* a thread in the group */
        if (c >= SPU_KIT_SPUS) return 0x80010002;
        G.arg[c] = *(const sysSpuThreadArgument *)(uintptr_t)f;
        *(u32 *)(uintptr_t)a = THREAD + (u32)c;
        return 0;
    case 173:                                        /* start */
        for (u32 k = 0; k < G.num && k < SPU_KIT_SPUS; k++) spu_thread(k);
        if ((u32)(G.arg[0].arg2 >> 32) == SPU_KIT_HEAT) {   /* heat never ends by itself */
            count_stop = 0;
            counting = pthread_create(&counter, NULL, heat_count, NULL) == 0;
        } else G.cause = SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT;
        return 0;
    case 177:                                        /* terminate */
        if (counting) {
            count_stop = 1;
            pthread_join(counter, NULL);
            counting = 0;
        }
        G.cause = SPU_THREAD_GROUP_JOIN_TERMINATED;
        G.status = (u32)b;
        return 0;
    case 178:                                        /* join */
        *(u32 *)(uintptr_t)b = G.cause;
        *(u32 *)(uintptr_t)c = G.status;
        return 0;
    case 165:                                        /* a thread's exit status: only after its own end */
        if (a < THREAD || a >= THREAD + SPU_KIT_SPUS) return 0x80010002;
        if (!G.exited[a - THREAD]) return ESTAT;
        *(s32 *)(uintptr_t)b = G.exit_status[a - THREAD];
        return 0;
    case 171:                                        /* group destroy */
        return 0;
    }
    return 0x80010003;
}

u64 pv_syscall(int n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g)
{
    (void)d; (void)g;
    if (n == 165 || (n >= 169 && n <= 178)) return spu_call(n, a, b, c, f);
    switch (n) {
    case 600: *(u32 *)(uintptr_t)c = 1; return 0;
    case 601: return 0;
    case 609: {
        u8 *info = (u8 *)(uintptr_t)b;
        u64 sectors = 2000409264ull;
        u32 ss = 512;
        memcpy(info + 0x28, &sectors, 8);
        memcpy(info + 0x30, &ss, 4);
        return 0;
    }
    case 616: return ata((ata_block *)(uintptr_t)c, (u8 *)(uintptr_t)e);
    case 383: {
        if (getenv("PV_383_RC")) return (s32)strtoul(getenv("PV_383_RC"), NULL, 16);   /* a refused call */
        double t = (sysGetSystemTime() - 1000000) / 1e6;
        if (getenv("PV_383_FAIL_AT") && t >= atof(getenv("PV_383_FAIL_AT"))) return 0x80010003;   /* refused from second s on */
        int rise = getenv("PV_RISE") ? (int)(20 * (1 - exp(-t / 70))) : 0;
        int off = getenv("PV_TEMP_OFF") ? atoi(getenv("PV_TEMP_OFF")) : 0;            /* readings off the 30..90 C graph */
        *(u32 *)(uintptr_t)b = (u32)((a ? 55 + rise : 51 + rise) + off) << 24;
        return 0;
    }
    case 409:
        *(u8 *)(uintptr_t)b = 0;
        *(u8 *)(uintptr_t)c = 1;
        *(u8 *)(uintptr_t)d = getenv("PV_RISE") ? (u8)(0x55 + (sysGetSystemTime() / 1000000) / 3) : 0x55;
        *(u8 *)(uintptr_t)e = 0;
        return 0;
    }
    return 0x80010003;
}
