/* PS3 Health's part of the preview: the drive from a console's dumps, 383 and
 * 409 (PV_DRIVE, PV_ST, PV_RISE, PV_TEMP_OFF, PV_383_RC, PV_383_FAIL_AT), the
 * six SPUs of the SPU kit (PV_SPU_FAIL, PV_SPU_LATE, PV_SPU_STALL,
 * PV_LS_FAULT, PV_HEAT_RATE), the audio port (PV_AUDIO_CH, PV_WAV) and the
 * Network module's world (PV_NET, PV_PING, PV_ICMP, PV_DNS_MS), the Clock
 * module's time server (PV_NTP, PV_NTP_OFF_MS) and the TV's data (PV_TV_RC):
 * README.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>
#include "lv2/systime.h"
#include "sys/systime.h"
#include "ppu-lv2.h"
#include "sys/spu.h"
#include "audio/audio.h"
#include "net/net.h"
#include "net/netctl.h"

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

/* ---- the kernel's pages (348, 349) ----------------------------------------
 * sys_memory_allocate and sys_memory_free in 1 MB pages, for the memory
 * tests' blocks (mod_mem.c's mem_take), from a pool of PV_KMEM_MB (default
 * 256: the XDR test takes its cap of 256 blocks, as malloc gave it before).
 * PV_KMEM_MB=0 refuses the first page: the malloc fallback. With
 * PV_KMEM_STAT=1 the exit prints the pages still taken: every test gives
 * its pages back, so it must be 0. */
#define KMEM_MAX 512
static void *kmem[KMEM_MAX];
static int kmem_taken;

static void kmem_exit(void)
{
    if (getenv("PV_KMEM_STAT")) fprintf(stderr, "kmem: %d pages of 1 MB still taken\n", kmem_taken);
}

static u64 kmem_call(int n, u64 a, u64 b, u64 c)
{
    static int once;
    if (!once) { once = 1; atexit(kmem_exit); }
    if (n == 349) {
        for (int i = 0; i < KMEM_MAX; i++)
            if (kmem[i] && (u64)(uintptr_t)kmem[i] == a) { free(kmem[i]); kmem[i] = NULL; kmem_taken--; return 0; }
        return 0x80010002;                           /* not an allocated address */
    }
    if (a != 1 << 20 || !(b & 0x400)) return 0x80010002;   /* one 1 MB page, as mem_take asks */
    if (kmem_taken >= env_int("PV_KMEM_MB", 256)) return 0x80010004;
    for (int i = 0; i < KMEM_MAX; i++)
        if (!kmem[i]) {
            void *m = NULL;
            if (posix_memalign(&m, 1 << 20, a)) return 0x80010004;
            kmem[i] = m;
            kmem_taken++;
            *(u64 *)(uintptr_t)c = (u64)(uintptr_t)m;
            return 0;
        }
    return 0x80010004;
}

u64 pv_syscall(int n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g)
{
    if (n == 348 || n == 349) return kmem_call(n, a, b, c);
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

/* ---- the netctl module ---------------------------------------------------- */

/* net_end's unload: the library's stubs have sysModuleLoad and no unload
 * (stub/pv_health.h declares it). Nothing to unload here. */
s32 sysModuleUnload(u32 id)
{
    (void)id;
    return 0;
}

/* ---- boot.txt at the cap ------------------------------------------------- */

/* boot_mark's rename: the library's stubs have no sysLv2FsRename
 * (stub/pv_health.h declares it). The paths go to the sandbox as stubs.c maps
 * them ($PV_ROOT, else /tmp/pv). An existing target is refused, as lv2 does
 * (webMAN unlinks the target before each cellFsRename): a boot.c that stops
 * removing boot.old.txt first empties boot.txt here, as on the console. */
s32 sysLv2FsRename(const char *from, const char *to)
{
    char a[1024], b[1024];
    struct stat st;
    const char *root = getenv("PV_ROOT") ? getenv("PV_ROOT") : "/tmp/pv";
    snprintf(a, sizeof a, "%s%s", root, from);
    snprintf(b, sizeof b, "%s%s", root, to);
    if (stat(b, &st) == 0) return (s32)0x80010014;                   /* EEXIST */
    return rename(a, b) ? (s32)0x80010006 : 0;
}

/* ---- the audio port ------------------------------------------------------- */

/* One port: its ring in host memory and a reader thread that moves the read
 * index on the preview's clock, a block of 256 samples every 5333 us.
 * PV_AUDIO_CH=2: the 8-channel open is refused, as a console whose system
 * gives two. PV_WAV=<file>: the blocks as the reader passes them, raw 32-bit
 * float, interleaved: a fill that falls behind shows as a repeated block. */
static float aring[AUDIO_BLOCK_32 * 8 * AUDIO_BLOCK_SAMPLES];
static volatile u64 aread;
static int a_ch, a_blocks;
static pthread_t a_th;
static volatile int a_stop, a_on;

static void *audio_reader(void *unused)
{
    (void)unused;
    u64 t0 = sysGetSystemTime(), done = 0;
    FILE *wav = getenv("PV_WAV") ? fopen(getenv("PV_WAV"), "wb") : NULL;
    while (!a_stop) {
        u64 b = (sysGetSystemTime() - t0) * 48000 / 1000000 / AUDIO_BLOCK_SAMPLES;
        for (; done < b; done++) {
            if (wav) fwrite(&aring[(done % a_blocks) * a_ch * AUDIO_BLOCK_SAMPLES], sizeof(float), (size_t)a_ch * AUDIO_BLOCK_SAMPLES, wav);
            aread = (done + 1) % a_blocks;
        }
        usleep(200);
    }
    if (wav) fclose(wav);
    return NULL;
}

s32 audioInit(void) { return 0; }
s32 audioQuit(void) { return 0; }

s32 audioPortOpen(audioPortParam *p, u32 *port)
{
    if (p->numChannels == 8 && env_int("PV_AUDIO_CH", 8) == 2) return (s32)0x80310705;   /* refused: the 2-channel open follows */
    if ((p->numChannels != 2 && p->numChannels != 8) || p->numBlocks > AUDIO_BLOCK_32) return (s32)0x80310704;
    a_ch = (int)p->numChannels;
    a_blocks = (int)p->numBlocks;
    *port = 1;
    return 0;
}

s32 audioGetPortConfig(u32 port, audioPortConfig *c)
{
    (void)port;
    memset(c, 0, sizeof *c);
    c->readIndex = (u64)(uintptr_t)&aread;
    c->status = a_on ? AUDIO_STATUS_RUN : AUDIO_STATUS_READY;
    c->channelCount = (u64)a_ch;
    c->numBlocks = (u64)a_blocks;
    c->portSize = (u32)(a_ch * a_blocks * AUDIO_BLOCK_SAMPLES * 4);
    c->audioDataStart = (u64)(uintptr_t)aring;
    return 0;
}

s32 audioPortStart(u32 port)
{
    (void)port;
    a_stop = 0;
    aread = 0;
    a_on = pthread_create(&a_th, NULL, audio_reader, NULL) == 0;
    return 0;
}

s32 audioPortStop(u32 port)
{
    (void)port;
    if (a_on) {
        a_stop = 1;
        pthread_join(a_th, NULL);
        a_on = 0;
    }
    return 0;
}

s32 audioPortClose(u32 port)
{
    (void)port;
    return 0;
}

/* ---- the Network module's world -------------------------------------------- */

/* netctl as PV_NET sets it: wired (the default: 1000 Mbit/s full duplex),
 * wired100, wifi (signal 78 % on channel 6), wifiweak (31 %), down (no
 * address). The MAC address is never asked: its code is refused. */
s32 netCtlGetInfo(s32 code, union net_ctl_info *i)
{
    const char *m = getenv("PV_NET") ? getenv("PV_NET") : "wired";
    int wifi = !strncmp(m, "wifi", 4), down = !strcmp(m, "down");
    memset(i, 0, sizeof *i);
    switch (code) {
    case NET_CTL_INFO_DEVICE: i->device = wifi ? NET_CTL_DEVICE_WIRELESS : NET_CTL_DEVICE_WIRED; return 0;
    case NET_CTL_INFO_LINK: i->link = down ? NET_CTL_LINK_DISCONNECTED : NET_CTL_LINK_CONNECTED; return 0;
    case NET_CTL_INFO_LINK_TYPE: i->link_type = wifi ? 0 : !strcmp(m, "wired100") ? 4 : 6; return 0;
    case NET_CTL_INFO_MTU: i->mtu = 1500; return 0;
    case NET_CTL_INFO_RSSI: if (!wifi) return (s32)0x80130106; i->rssi = !strcmp(m, "wifiweak") ? 31 : 78; return 0;
    case NET_CTL_INFO_CHANNEL: if (!wifi) return (s32)0x80130106; i->channel = 6; return 0;
    case NET_CTL_INFO_IP_CONFIG: i->ip_config = NET_CTL_IP_DHCP; return 0;
    case NET_CTL_INFO_IP_ADDRESS: snprintf(i->ip_address, sizeof i->ip_address, "%s", down ? "0.0.0.0" : "192.168.1.20"); return 0;
    case NET_CTL_INFO_NETMASK: snprintf(i->netmask, sizeof i->netmask, "255.255.255.0"); return 0;
    case NET_CTL_INFO_DEFAULT_ROUTE: if (!down) snprintf(i->default_route, sizeof i->default_route, "192.168.1.1"); return 0;
    case NET_CTL_INFO_PRIMARY_DNS: snprintf(i->primary_dns, sizeof i->primary_dns, "192.168.1.1"); return 0;
    case NET_CTL_INFO_SECONDARY_DNS: snprintf(i->secondary_dns, sizeof i->secondary_dns, "1.1.1.1"); return 0;
    }
    return (s32)0x80130106;                  /* NET_CTL_ERROR_INVALID_CODE */
}

/* The router: PV_PING=frames[,n] answers each ping after that many frames
 * of the preview's clock (16.7 ms each: the clock moves by frames, so the
 * times are frame steps), every nth lost; PV_PING=none answers nothing.
 * PV_ICMP=refuse: the raw socket is refused, so the module times TCP
 * connects (refused at once by the router, as a closed port 80). No PV_PING:
 * offline, as the library's world. PV_DNS_MS=n: a lookup of example.com,
 * the Network module's name, answers after n ms; any other name (Transfer's
 * download host) fails, as it does with no PV_DNS_MS (offline). */
#define FD_RAW 0x701
#define FD_TCP 0x702
#define FD_UDP 0x703                         /* the Clock module's socket to the time server */
static uint8_t ntp_req[48];
static u64 ntp_sent_at;
static s32 pv_errno = NET_ENETDOWN;
static u64 sent_at;
static int pings;
static uint8_t last_req[64];
static int last_len;

s32 *netErrnoLoc(void) { return &pv_errno; }

static int ping_frames(void) { return getenv("PV_PING") ? atoi(getenv("PV_PING")) : 0; }
static int ping_lost(void)
{
    const char *p = getenv("PV_PING"), *c = p ? strchr(p, ',') : NULL;
    if (p && !strcmp(p, "none")) return 1;
    int every = c ? atoi(c + 1) : 0;
    return every > 0 && pings % every == 0;
}
static void wait_until(u64 t) { while (sysGetSystemTime() < t) usleep(200); }

s32 netSocket(s32 d, s32 t, s32 p)
{
    (void)d; (void)p;
    if (t == SOCK_DGRAM) {
        if (!getenv("PV_NTP")) { pv_errno = NET_ENETDOWN; return -1; }
        return FD_UDP;
    }
    if (!getenv("PV_PING")) { pv_errno = NET_ENETDOWN; return -1; }
    if (t == SOCK_RAW) {
        if (getenv("PV_ICMP") && !strcmp(getenv("PV_ICMP"), "refuse")) { pv_errno = NET_EPROTONOSUPPORT; return -1; }
        return FD_RAW;
    }
    return t == SOCK_STREAM ? FD_TCP : -1;
}

ssize_t netSendTo(s32 s, const void *b, size_t l, s32 f, const struct sockaddr *a, socklen_t al)
{
    (void)f; (void)a; (void)al;
    if (s == FD_UDP) {
        if (l != sizeof ntp_req) return -1;
        memcpy(ntp_req, b, l);
        ntp_sent_at = sysGetSystemTime();
        return (ssize_t)l;
    }
    if (s != FD_RAW || l > sizeof last_req) return -1;
    memcpy(last_req, b, l);
    last_len = (int)l;
    sent_at = sysGetSystemTime();
    pings++;
    return (ssize_t)l;
}

/* The reply after an IPv4 header, as a BSD raw socket gives it, once the
 * poll said it came. */
/* The time server: PV_NTP=frames answers the Clock module's packet after
 * that many frames with the preview's clock plus PV_NTP_OFF_MS (the
 * server's clock minus the console's, default 0), stratum 2; PV_NTP=none
 * answers nothing (the module's timeout); PV_NTP=kod answers a
 * kiss-o'-death (RATE). Unset: no UDP socket. The server must be an address
 * in ntp.txt: the resolver gives no list on the host (netGetHostByName). */
static int ntp_frames(void) { return getenv("PV_NTP") ? atoi(getenv("PV_NTP")) : 0; }

/* The clock set (the Clock module's SQUARE): PV_CLOCK_SET=refused answers
 * EPERM; else the set is kept as a shift of the console's clock, which the
 * time server's replies take into account (the library's clock itself does
 * not move, so the set shows as the server's offset going to 0). */
static long long pv_clock_shift_us;
s32 sysSetCurrentTime(u64 sec, u64 nsec)
{
    if (getenv("PV_CLOCK_SET") && !strcmp(getenv("PV_CLOCK_SET"), "refused")) return (s32)0x80010009;
    u64 s = 0, ns = 0;
    sysGetCurrentTime(&s, &ns);
    pv_clock_shift_us += (long long)(sec * 1000000 + nsec / 1000) - (long long)(s * 1000000 + ns / 1000);
    return 0;
}
static int ntp_none(void) { return getenv("PV_NTP") && !strcmp(getenv("PV_NTP"), "none"); }

static void ntp_stamp(uint8_t *p, u64 us)
{
    u64 sec = us / 1000000 + 2208988800ull, frac = ((us % 1000000) << 32) / 1000000;
    p[0] = (uint8_t)(sec >> 24); p[1] = (uint8_t)(sec >> 16); p[2] = (uint8_t)(sec >> 8); p[3] = (uint8_t)sec;
    p[4] = (uint8_t)(frac >> 24); p[5] = (uint8_t)(frac >> 16); p[6] = (uint8_t)(frac >> 8); p[7] = (uint8_t)frac;
}

static ssize_t ntp_reply(uint8_t *o, size_t l)
{
    if (l < 48 || ntp_none()) { pv_errno = NET_EAGAIN; return -1; }
    wait_until(ntp_sent_at + (u64)ntp_frames() * 16667);
    u64 sec = 0, nsec = 0;
    sysGetCurrentTime(&sec, &nsec);
    u64 now = sec * 1000000 + nsec / 1000 + (u64)((long long)(getenv("PV_NTP_OFF_MS") ? atoll(getenv("PV_NTP_OFF_MS")) : 0) * 1000 - pv_clock_shift_us);
    memset(o, 0, 48);
    o[0] = 0x24;                             /* version 4, mode 4 (server) */
    o[1] = 2;                                /* stratum 2 */
    o[2] = 6;                                /* poll */
    o[3] = (uint8_t)-20;                     /* precision 2^-20 s */
    if (getenv("PV_NTP") && !strcmp(getenv("PV_NTP"), "kod")) { o[1] = 0; memcpy(o + 12, "RATE", 4); }
    memcpy(o + 24, ntp_req + 40, 8);         /* originate: the request's transmit time */
    ntp_stamp(o + 32, now);
    ntp_stamp(o + 40, now);
    return 48;
}

ssize_t netRecvFrom(s32 s, void *b, size_t l, s32 f, struct sockaddr *from, socklen_t *fl)
{
    (void)f; (void)from; (void)fl;
    if (s == FD_UDP) return ntp_reply(b, l);
    if (s != FD_RAW || l < (size_t)(20 + last_len) || ping_lost()) { pv_errno = NET_EAGAIN; return -1; }
    wait_until(sent_at + (u64)ping_frames() * 16667);
    uint8_t *o = b;
    memset(o, 0, 20);
    o[0] = 0x45;
    o[9] = 1;
    memcpy(o + 20, last_req, (size_t)last_len);
    o[20] = 0;                               /* echo reply */
    return 20 + last_len;
}

s32 netConnect(s32 s, const struct sockaddr *a, socklen_t l)
{
    (void)a; (void)l;
    if (s != FD_TCP) return -1;
    sent_at = sysGetSystemTime();
    pings++;
    pv_errno = NET_EINPROGRESS;
    return -1;
}

/* The answer of the ping on either socket after its frames; a lost one at
 * the poll's timeout. */
s32 netPoll(struct pollfd *p, nfds_t n, s32 timeout)
{
    (void)n;
    if (p->fd == FD_UDP) {
        if (ntp_none()) { wait_until(sysGetSystemTime() + (u64)timeout * 1000); return 0; }
        wait_until(ntp_sent_at + (u64)ntp_frames() * 16667);
        p->revents = POLLIN;
        return 1;
    }
    if (p->fd != FD_TCP && p->fd != FD_RAW) return -1;
    if (ping_lost()) { wait_until(sysGetSystemTime() + (u64)timeout * 1000); return 0; }
    wait_until(sent_at + (u64)ping_frames() * 16667);
    p->revents = p->fd == FD_TCP ? POLLOUT : POLLIN;
    return 1;
}

s32 netGetSockOpt(s32 s, s32 lv, s32 o, void *v, socklen_t *l)
{
    (void)lv; (void)l;
    if (s != FD_TCP || o != SO_ERROR) return -1;
    *(int *)v = NET_ECONNREFUSED;            /* port 80 closed: the router's RST is the answer */
    return 0;
}

/* The address list is a 32-bit pointer, as in the network library, and the
 * host has no memory such a pointer reaches (a 64-bit macOS process maps
 * nothing in its low 4 GB). So only the Network module's name gets an
 * answer: that module reads no address, h_addr_list only has to be set.
 * Transfer's resolve() reads the address: its host gets NULL. */
struct net_hostent *netGetHostByName(const char *name)
{
    static struct net_hostent he = {0, 0, 2, 4, 1};   /* AF_INET, 4-byte addresses, a list pointer that is set */
    if (!getenv("PV_DNS_MS") || strcmp(name, "example.com")) return NULL;
    wait_until(sysGetSystemTime() + (u64)atoi(getenv("PV_DNS_MS")) * 1000);
    return &he;
}

/* ---- the TV --------------------------------------------------------------- */

/* An HDMI TV at 1080p: full-range RGB, no EDID latency, Rec. 709 primaries
 * (fractions of 1024), gamma 2.20, one HDCP key, and the modes a 1080p TV
 * accepts. PV_TV_RC=hex: the read is refused with that rc. */
s32 videoGetNumberOfDevice(u32 out) { (void)out; return 1; }

s32 videoGetDeviceInfo(u32 out, u32 dev, videoDeviceInfo *info)
{
    (void)out; (void)dev;
    if (getenv("PV_TV_RC")) return (s32)strtoul(getenv("PV_TV_RC"), NULL, 16);
    static const videoDisplayMode modes[] = {
        {VIDEO_RESOLUTION_1080, VIDEO_SCANMODE_PROGRESSIVE, 0, VIDEO_ASPECT_16_9, {0, 0}, 0x05},
        {VIDEO_RESOLUTION_1080, VIDEO_SCANMODE_INTERLACE, 0, VIDEO_ASPECT_16_9, {0, 0}, 0x05},
        {VIDEO_RESOLUTION_720, VIDEO_SCANMODE_PROGRESSIVE, 0, VIDEO_ASPECT_16_9, {0, 0}, 0x05},
        {VIDEO_RESOLUTION_480, VIDEO_SCANMODE_PROGRESSIVE, 0, VIDEO_ASPECT_16_9, {0, 0}, 0x05},
        {VIDEO_RESOLUTION_480, VIDEO_SCANMODE_PROGRESSIVE, 1, VIDEO_ASPECT_16_9, {0, 0}, 0x05},   /* the same mode with another conversion flag: shown once */
        {VIDEO_RESOLUTION_480, VIDEO_SCANMODE_INTERLACE, 0, VIDEO_ASPECT_4_3, {0, 0}, 0x05},
        {VIDEO_RESOLUTION_576, VIDEO_SCANMODE_PROGRESSIVE, 0, VIDEO_ASPECT_16_9, {0, 0}, 0x02},
        {VIDEO_RESOLUTION_576, VIDEO_SCANMODE_INTERLACE, 0, VIDEO_ASPECT_4_3, {0, 0}, 0x02},
    };
    memset(info, 0, sizeof *info);
    info->portType = VIDEO_PORT_HDMI;
    info->colorSpace = 7;                    /* RGB, YUV and xvYCC, as the test TV */

    info->latency = 0;
    info->state = VIDEO_STATE_ENABLED;
    info->rgbOutputRange = 1;
    info->colorInfo = (videoColorInfo){655, 338, 307, 614, 154, 61, 320, 337, 220};
    info->availableModeCount = sizeof modes / sizeof modes[0];
    memcpy(info->availableModes, modes, sizeof modes);
    info->ksvList.count = 1;
    return 0;
}
