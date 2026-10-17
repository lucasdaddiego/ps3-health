/* The Memory module: pattern tests on the user memory (XDR) the app can get,
 * on the RSX memory (GDDR3) through its mapping (read back by the RSX's own
 * DMA when it passes a self-check, MEM_DMA), and on the local store of
 * each of the six SPUs (spu_run.c runs ps3gfx's SPU kit: each SPU tests its
 * own 256 KB). The app writes only to memory it allocated itself and to the
 * local stores of its own SPU threads. RSX memory comes from gfx_vram
 * (ps3gfx), a bump pointer over the local memory that gives nothing back:
 * the blocks stay with the app until it exits, and a second test reuses
 * them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <lv2/systime.h>
#include <sys/memory.h>
#include <ps3gfx/gfx.h>
#include "app.h"
#include <ps3gfx/ui.h>
#include "report.h"

#define BLOCK (1024 * 1024)
#define MAX_BLOCKS 256
#define MAX_ERR 8

typedef struct {
    int done, blocks, passes, cancelled;
    long errors;
    uintptr_t err_addr[MAX_ERR];
    uint32_t err_want[MAX_ERR], err_got[MAX_ERR];
    long long w_bytes, r_bytes, w_us, r_us;  /* the writes and the reads with their compare, for the speeds in the report */
    long long dma_bytes, ppu_bytes;          /* the RSX test's reads: by the RSX's DMA, through the mapping */
    int rechecks;                            /* blocks whose DMA copy differed, read again through the mapping */
} mem_result;

static mem_result xdr, vram;
static void *blk[MAX_BLOCKS];
static int xdr_kernel;                       /* the XDR test's blocks were the kernel's pages, not malloc's */

static void note_error(mem_result *r, uintptr_t addr, uint32_t want, uint32_t got)
{
    if (r->errors < MAX_ERR) {
        r->err_addr[r->errors] = addr;
        r->err_want[r->errors] = want;
        r->err_got[r->errors] = got;
    }
    r->errors++;
}

/* The four patterns of a word at index i in block b: address based, its
 * inverse, an alternating word, and a linear congruential sequence. With
 * a = (b << 20) + 4 i: a ^ 0xA5A5A5A5, its inverse, 0xAAAAAAAA and
 * 0x55555555 by turns, (a + 0x9E3779B9) * 2654435761.
 * MEM_LINES 1: the patterns as running values, one loop for the four with
 * no switch per word, and dcbz on each whole 128-byte line (the PPU's cache
 * line) before its 32 words, so a store does not first read the line from
 * memory. 0: the loops of 2.1.0, pattern() per word and no dcbz, to compare
 * the speeds on a console: the report gives both in MB/s. */
#ifndef MEM_LINES
#define MEM_LINES 1
#endif
#define CACHE_LINE 128

#if MEM_LINES
/* Word i of block b in pattern p is x ^ m, then x += dx and m ^= t: the
 * address grows by 4, the alternating word flips, and the sequence's next
 * value adds 4 x its multiplier. */
typedef struct { uint32_t x, dx, m, t; } run;

static run run_at(int p, int b, unsigned i)
{
    uint32_t a = ((uint32_t)b << 20) + i * 4;
    run r = {a, 4, 0xA5A5A5A5u, 0};
    if (p == 1) r.m = ~0xA5A5A5A5u;
    else if (p == 2) r.x = r.dx = 0, r.m = (i & 1) ? 0x55555555u : 0xAAAAAAAAu, r.t = 0xFFFFFFFFu;
    else if (p == 3) r.x = (a + 0x9E3779B9u) * 2654435761u, r.dx = 4 * 2654435761u, r.m = 0;
    return r;
}

#ifdef __PPU__
#define ZERO_LINE(p) __asm__ volatile("dcbz 0,%0" : : "r"(p) : "memory")
#else
#define ZERO_LINE(p) ((void)(p))             /* the host preview */
#endif

/* n words of pattern p, from word i of block b, into w (user memory: dcbz
 * needs a cached line, so never the RSX mapping). Only a line that w holds
 * whole is zeroed: a block from malloc need not start on a line. */
static void put_words(uint32_t *w, unsigned n, int p, int b, unsigned i)
{
    run r = run_at(p, b, i);
    unsigned k = 0;
    while (k < n) {
        unsigned end = k + 1;
        if (!((uintptr_t)(w + k) & (CACHE_LINE - 1)) && n - k >= CACHE_LINE / 4) {
            ZERO_LINE(w + k);
            end = k + CACHE_LINE / 4;
        }
        for (; k < end; k++, r.x += r.dx, r.m ^= r.t) w[k] = r.x ^ r.m;
    }
}

/* The compare of n words read at w with pattern p from word i of block b:
 * word k's address is at + 4 k. Each wrong one goes to res (NULL: counted
 * only), the first one's address to *first when first is not NULL. The
 * errors. Line by line: each line's start asks for the line 8 ahead
 * (dcbt), so the in-order PPU compares while the next lines come from
 * memory, instead of waiting for each one. Near the end the hint names
 * lines past w's last word: dcbt is a hint, it never faults (an address
 * that would fault makes it a no-op), and the address is an integer, so
 * no pointer goes past the array. */
#define PREFETCH_AHEAD (8 * CACHE_LINE)
static long check_words(const uint32_t *w, unsigned n, int p, int b, unsigned i, uintptr_t at, mem_result *res, unsigned long *first)
{
    run r = run_at(p, b, i);
    long errors = 0;
    unsigned k = 0;
    while (k < n) {
        uintptr_t a = (uintptr_t)(w + k);
        unsigned end = k + (CACHE_LINE - (a & (CACHE_LINE - 1))) / 4;   /* the next line's first word */
        if (end > n) end = n;
        __builtin_prefetch((const void *)(a + PREFETCH_AHEAD));
        for (; k < end; k++, r.x += r.dx, r.m ^= r.t) {   /* the offsets in 64 bits: no 32-bit k * 4 to keep in the loop */
            uint32_t want = r.x ^ r.m;
            if (w[k] == want) continue;
            if (res) note_error(res, at + (uintptr_t)k * 4, want, w[k]);
            if (first && !errors) *first = (unsigned long)(at + (uintptr_t)k * 4);
            errors++;
        }
    }
    return errors;
}
#else
static uint32_t pattern(int p, int b, unsigned i)
{
    uint32_t a = ((uint32_t)b << 20) + i * 4;
    switch (p) {
    case 0: return a ^ 0xA5A5A5A5u;
    case 1: return ~(a ^ 0xA5A5A5A5u);
    case 2: return (i & 1) ? 0x55555555u : 0xAAAAAAAAu;
    default: return (a + 0x9E3779B9u) * 2654435761u;
    }
}

static void put_words(uint32_t *w, unsigned n, int p, int b, unsigned i)
{
    for (unsigned k = 0; k < n; k++) w[k] = pattern(p, b, i + k);
}

static long check_words(const uint32_t *w, unsigned n, int p, int b, unsigned i, uintptr_t at, mem_result *res, unsigned long *first)
{
    long errors = 0;
    for (unsigned k = 0; k < n; k++) {
        uint32_t want = pattern(p, b, i + k);
        if (w[k] == want) continue;
        if (res) note_error(res, at + k * 4, want, w[k]);
        if (first && !errors) *first = (unsigned long)(at + k * 4);
        errors++;
    }
    return errors;
}
#endif

/* The user memory is written and compared in place. The RSX mapping goes
 * through 64 KB chunks: a pattern chunk is built in user memory and copied
 * in, and the block is copied out before the compare, because one word at a
 * time on that mapping is far slower than a memcpy. A read is about 0.5 us
 * there, so with MEM_DMA the RSX copies the blocks into main memory instead
 * (below). */
#define CHUNK 65536
static uint32_t chunk[CHUNK / 4] __attribute__((aligned(128)));
static uint32_t readback[CHUNK / 4] __attribute__((aligned(128)));

/* MEM_DMA 1: the RSX test reads its blocks back by the RSX's own DMA,
 * ps3gfx's gfx_copy_to_main_job (the copy runs in the main thread between
 * two frames), DMA_BLOCKS blocks a copy into dma_buf, main memory taken for
 * the test only. 16 blocks (GFX_COPY_MAX): each copy waits for a frame end,
 * about 8 ms, so a pass over 231 MB waits 15 times, not 58 as with 4
 * blocks (4 MB: about 33 ms of copy and compare). DMA_BLOCKS_MIN blocks
 * when 16 MB of main memory is not free. The first use in a session is a
 * self-check, journaled as rsx_dma, so a freeze skips the DMA at the next
 * start. A block whose copy differs from the pattern is read again through
 * the mapping, which decides. A refused or failed check, a failed copy, or
 * a copy that differed where the mapping did not: the 64 KB copies, as
 * before, for the rest of the session. 0: no DMA. */
#ifndef MEM_DMA
#define MEM_DMA 1
#endif

#if MEM_DMA
#define DMA_BLOCKS (GFX_COPY_MAX / BLOCK)
#define DMA_BLOCKS_MIN 4
enum { DMA_UNTRIED, DMA_ON, DMA_OFF };
static int dma_state;                        /* for the session */
static char dma_why[80];                     /* DMA_OFF: the reason */
static char dma_note[80];                    /* the last RSX test's reads not by DMA: the reason, for the report */
static uint8_t *dma_buf;
static int dma_blocks;                       /* the blocks dma_buf holds: DMA_BLOCKS or DMA_BLOCKS_MIN */
static int dma_first, dma_n;                 /* the blocks in dma_buf: dma_n from dma_first */
static int dma_keep;                         /* a copy timed out: the main thread may still write dma_buf, never freed */
#endif

static void write_block(int p, int b, int direct)
{
#if MEM_DMA
    dma_n = 0;                               /* a copy made before this write is stale */
#endif
    if (direct) {
        put_words(blk[b], BLOCK / 4, p, b, 0);
        return;
    }
    for (unsigned off = 0; off < BLOCK; off += CHUNK) {
        put_words(chunk, CHUNK / 4, p, b, off / 4);
        memcpy((uint8_t *)blk[b] + off, chunk, CHUNK);
    }
}

static void check_mapped(mem_result *r, int p, int b)
{
    for (unsigned off = 0; off < BLOCK; off += CHUNK) {
        memcpy(readback, (uint8_t *)blk[b] + off, CHUNK);
        check_words(readback, CHUNK / 4, p, b, off / 4, (uintptr_t)blk[b] + off, r, NULL);
    }
}

#if MEM_DMA
static void dma_off(const char *why)
{
    dma_state = DMA_OFF;
    snprintf(dma_why, sizeof dma_why, "%s", why);
    snprintf(dma_note, sizeof dma_note, "%s", why);
}

static void dma_failed(const char *what, int rc)
{
    char why[80];
    if (rc == GFX_COPY_ETIME) dma_keep = 1;
    snprintf(why, sizeof why, "%s, rc %d", what, rc);
    dma_off(why);
}

/* The self-check, in the job thread: block 0 gets pattern 3 through the
 * mapping, dma_buf is filled with 0x5a, and the RSX's copy of the block
 * must equal the block read through the mapping (a bad word of the memory
 * itself does not turn the DMA off). No writable folder (no journal), or a
 * frozen rsx_dma, keeps the DMA off. */
static void dma_check(void)
{
    if (!fs_ok) { dma_off("the app folder is not writable: no journal"); return; }
    if (!journal_start("rsx_dma")) { dma_off("it froze this console before (journal: rsx_dma)"); return; }
    progress("Checking the RSX's DMA copy against the mapping");
    write_block(3, 0, 0);
    memset(dma_buf, 0x5a, BLOCK);
    int rc = gfx_copy_to_main_job(blk[0], dma_buf, BLOCK);
    for (unsigned off = 0; rc == 0 && off < BLOCK; off += CHUNK) {
        memcpy(readback, (uint8_t *)blk[0] + off, CHUNK);
        if (memcmp(readback, dma_buf + off, CHUNK)) rc = 1;
    }
    journal_done("rsx_dma", rc, rc == 0);
    if (rc == 1) dma_off("the self-check's copy differed from the mapping");
    else if (rc) dma_failed("the self-check's copy failed", rc);
    else dma_state = DMA_ON;
}

/* At the start of each RSX test: the main memory of the copies, and the
 * self-check at the first test of the session. */
static void dma_start(int blocks)
{
    dma_n = 0;
    dma_note[0] = 0;
    if (dma_state == DMA_OFF) { snprintf(dma_note, sizeof dma_note, "%s", dma_why); return; }
    if (!blocks) return;
    if (!dma_buf && (dma_buf = malloc(DMA_BLOCKS * BLOCK))) dma_blocks = DMA_BLOCKS;
    if (!dma_buf && (dma_buf = malloc(DMA_BLOCKS_MIN * BLOCK))) dma_blocks = DMA_BLOCKS_MIN;
    if (!dma_buf) {
        snprintf(dma_note, sizeof dma_note, "no %d MB of main memory for the copies", DMA_BLOCKS_MIN);
        return;
    }
    if (dma_state == DMA_UNTRIED) dma_check();
}

static void dma_end(void)
{
    if (!dma_keep) {
        free(dma_buf);
        dma_buf = NULL;
    }
}

/* Block b from the RSX's copy, up to dma_blocks blocks a copy while they
 * follow each other in RSX memory. 0 when the copy failed: the caller reads
 * the block through the mapping. A block whose copy differs from the
 * pattern is read again through the mapping, which records the errors; no
 * wrong word there turns the DMA off. */
static int dma_read(mem_result *r, int p, int b)
{
    if (b < dma_first || b >= dma_first + dma_n) {
        int n = 1;
        while (n < dma_blocks && b + n < r->blocks && blk[b + n] == (uint8_t *)blk[b] + n * BLOCK) n++;
        dma_n = 0;
        int rc = gfx_copy_to_main_job(blk[b], dma_buf, n * BLOCK);
        if (rc) { dma_failed("a copy failed", rc); return 0; }
        dma_first = b;
        dma_n = n;
    }
    r->dma_bytes += BLOCK;
    const uint32_t *w = (const uint32_t *)(dma_buf + (size_t)(b - dma_first) * BLOCK);
    if (!check_words(w, BLOCK / 4, p, b, 0, (uintptr_t)blk[b], NULL, NULL)) return 1;
    long before = r->errors;
    check_mapped(r, p, b);
    r->rechecks++;
    if (r->errors == before) dma_off("a copy differed where the mapping did not");
    return 1;
}
#endif

static void check_block(mem_result *r, int p, int b, int direct)
{
    if (direct) {
        check_words(blk[b], BLOCK / 4, p, b, 0, (uintptr_t)blk[b], r, NULL);
        return;
    }
#if MEM_DMA
    if (dma_buf && dma_state == DMA_ON && dma_read(r, p, b)) return;
#endif
    check_mapped(r, p, b);
    r->ppu_bytes += BLOCK;
}

/* passes 1 (the quick check): patterns 0 and 1 by turns, block by block,
 * so every bit of a word is written as 0 and as 1 in the same time. Each
 * block's write and check is timed, for the speeds in the report. */
static void run_passes(mem_result *r, int passes, int direct)
{
    int total = passes * r->blocks * 2, step = 0;
    for (int p = 0; p < passes && !job_cancel; p++) {
        for (int b = 0; b < r->blocks && !job_cancel; b++, step++) {
            s64 t = sysGetSystemTime();
            write_block(passes == 1 ? b & 1 : p, b, direct);
            r->w_us += sysGetSystemTime() - t;
            r->w_bytes += BLOCK;
            job_set_percent(step * 100 / total);
        }
        for (int b = 0; b < r->blocks && !job_cancel; b++, step++) {
            s64 t = sysGetSystemTime();
            check_block(r, passes == 1 ? b & 1 : p, b, direct);
            r->r_us += sysGetSystemTime() - t;
            r->r_bytes += BLOCK;
            job_set_percent(step * 100 / total);
        }
        if (!job_cancel) r->passes = p + 1;
    }
    r->cancelled = job_cancel;
}

/* The blocks of a memory test: up to max of 1 MB from the kernel
 * (sysMemoryAllocate, 1 MB pages), each given back with sysMemoryFree.
 * newlib's heap keeps what it grows to: after a full XDR test by malloc the
 * kernel had no memory left for the SPU group (2.2.0 rc3: ls_group rc
 * 0x80010004 in a quick check that followed it). malloc only when the
 * kernel refuses the first block. Then the last keep blocks go back at once,
 * as headroom for the app's own heap. *kernel: where they came from, for
 * mem_give. The blocks kept. */
static __attribute__((noinline)) void *kernel_block(void)   /* noinline: the li r11 stays next to its sc, for tools/syscalls.py */
{
    sys_mem_addr_t a;
    return sysMemoryAllocate(BLOCK, SYS_MEMORY_PAGE_SIZE_1M, &a) ? NULL : (void *)(uintptr_t)a;
}

int mem_take(void **blocks, int max, int keep, int *kernel)
{
    int n = 0;
    *kernel = 1;
    while (n < max && (blocks[n] = kernel_block()) != NULL) n++;
    if (!n) {
        *kernel = 0;
        while (n < max && (blocks[n] = malloc(BLOCK)) != NULL) n++;
    }
    if (keep > n) keep = n;
    mem_give(blocks + n - keep, keep, *kernel);
    return n - keep;
}

void mem_give(void *const *blocks, int n, int kernel)
{
    for (int b = 0; b < n; b++) {
        if (kernel) sysMemoryFree((sys_mem_addr_t)(uintptr_t)blocks[b]);
        else free(blocks[b]);
    }
}

static void xdr_run(int passes)
{
    memset(&xdr, 0, sizeof xdr);
    progress("Allocating the user memory in 1 MB blocks");
    int n = mem_take(blk, MAX_BLOCKS, 8, &xdr_kernel);         /* 8 MB of headroom for the app itself */
    xdr.blocks = n;
    progress(passes == 4 ? "Testing the user memory (XDR): 4 patterns, each written then verified"
                         : "Testing the user memory (XDR): 1 pattern, written then verified");
    run_passes(&xdr, passes, 1);
    mem_give(blk, n, xdr_kernel);
    xdr.done = 1;
}

static void job_xdr(void) { xdr_run(4); }

static void *vblk[MAX_BLOCKS];
static int vn = -1;

static void vram_run(int passes)
{
    memset(&vram, 0, sizeof vram);
    progress("Taking the free RSX memory in 1 MB blocks (kept until the app exits)");
    if (vn < 0) {
        vn = 0;
        while (vn < MAX_BLOCKS && (vblk[vn] = gfx_vram(BLOCK, 128, NULL)) != NULL) vn++;
    }
    memcpy(blk, vblk, sizeof blk);
    vram.blocks = vn;
#if MEM_DMA
    dma_start(vn);
#endif
    progress(passes == 2 ? "Testing the RSX memory (GDDR3): 2 patterns, written through the mapping, read back by the RSX"
                         : "Testing the RSX memory (GDDR3): 1 pattern, written through the mapping, read back by the RSX");
    run_passes(&vram, passes, 0);
#if MEM_DMA
    dma_end();
#endif
    vram.done = 1;
}

static void job_vram(void) { vram_run(2); }

/* The soak test of the Cooling module: one pattern over the caller's blocks,
 * written then verified in place, as the XDR test. The errors, and the first
 * wrong word's address in *first (untouched when none); stop ends it early. */
long mem_soak_pass(void *const *blocks, int n, int p, volatile int *stop, unsigned long *first)
{
    long errors = 0;
    for (int b = 0; b < n && !*stop; b++) put_words(blocks[b], BLOCK / 4, p, b, 0);
    for (int b = 0; b < n && !*stop; b++)
        errors += check_words(blocks[b], BLOCK / 4, p, b, 0, (uintptr_t)blocks[b], NULL, errors ? NULL : first);
    return errors;
}

/* A wrong word on two lines, so that it fits a third of the screen: where,
 * then the word written and the word read. The y of the next. */
static float err_lines(float x, float ey, float w, const char *where, uint32_t want, uint32_t got)
{
    text_fit(x + 32, ey, F_SMALL, RED, w - 64, "%s", where);
    text_fit(x + 32, ey + 32, F_SMALL, RED, w - 64, "wrote 0x%08x, read 0x%08x", (unsigned)want, (unsigned)got);
    return ey + 64;
}

/* ---- the SPU local stores ------------------------------------------------- */

#define LS_US 5000000                        /* the six SPUs end their test within 5 s, or they are stopped */
#define TB_HZ 79800000.0                     /* the timebase, the rate of the SPU decrementer on every PS3 */

/* The test on SQUARE: the driver's result and the verdict on it. */
enum { LS_NONE, LS_PASS, LS_ERRORS, LS_FAILED, LS_NOTRUN, LS_STUCK };
static struct {
    int state;
    spu_result r;
    u32 lo, hi, errors;                      /* the area of the reports; the wrong reads of the six, saturating */
    u32 ticks;                               /* of the slowest SPU, 0 when not measured */
    char why[96];                            /* failed or not run: the reason */
} ls;

static void job_ls(void)
{
    progress("Starting the six SPUs");
    if (!spu_up(&spu_ls, SPU_KIT_LS, &ls.r)) return;
    progress("Each SPU tests its own local store: 16 passes of march patterns");
    spu_wait_done(LS_US);
    progress("Stopping the six SPUs");
    spu_down(&ls.r);
}

/* An SPU passes when its last report has done = 1, its exit status is
 * 0x500 | index and it read no wrong word, over the same area as the others.
 * The done flags of all six come first: after the stop at 5 s the late SPU
 * has no exit status (its read fails, -1), so a status check first would
 * give "the exit status ... was not read", not the late SPU's pass count. */
static void ls_judge(void)
{
    const spu_result *r = &ls.r;
    ls.lo = r->rec[0].lo;
    ls.hi = r->rec[0].hi;
    if (r->stuck) { ls.state = LS_STUCK; return; }
    if (r->why[0]) {
        ls.state = r->skipped ? LS_NOTRUN : LS_FAILED;
        snprintf(ls.why, sizeof ls.why, "%s", r->why);
        return;
    }
    ls.state = LS_FAILED;
    for (int i = 0; i < SPU_KIT_SPUS; i++)
        if (r->rec[i].done != 1) {
            snprintf(ls.why, sizeof ls.why, "SPU %d did not finish in %d s (%u of %d passes)", i, LS_US / 1000000,
                     (unsigned)r->rec[i].count, SPU_KIT_LS_PASSES);
            return;
        }
    if (r->terminated) { snprintf(ls.why, sizeof ls.why, "the SPUs took over %d s", LS_US / 1000000); return; }   /* the last one finished after the stop */
    for (int i = 0; i < SPU_KIT_SPUS; i++) {
        const spu_kit_rec *e = &r->rec[i];
        s32 st = r->exit_status[i];
        if (st == (s32)(SPU_KIT_EXIT_ROOM | i)) { snprintf(ls.why, sizeof ls.why, "SPU %d: no room for the test", i); return; }
        if (st == -1) { snprintf(ls.why, sizeof ls.why, "the exit status of SPU %d was not read", i); return; }
        if (st != (s32)(SPU_KIT_EXIT_LS | i)) { snprintf(ls.why, sizeof ls.why, "SPU %d ended with status 0x%x", i, (unsigned)st); return; }
        if (e->lo != ls.lo || e->hi != ls.hi) { snprintf(ls.why, sizeof ls.why, "SPU %d tested another area", i); return; }
        ls.errors = e->errors > 0xffffffffu - ls.errors ? 0xffffffffu : ls.errors + e->errors;
        if (e->ticks > ls.ticks) ls.ticks = e->ticks;
    }
    ls.state = ls.errors ? LS_ERRORS : LS_PASS;
}

/* The quick check's SPU leg, in its job thread: the prompt was asked before
 * (mem_quick_ask). A start that the journal refuses makes no call. */
static void ls_quick(void)
{
    memset(&ls, 0, sizeof ls);
    if (spu_blocked(&spu_ls)) spu_up(&spu_ls, SPU_KIT_LS, &ls.r);
    else job_ls();
    ls_judge();
}

/* SQUARE, in the UI thread: -1 on exit. No journal, no SPU call. Then the
 * prompt, once. A start that the journal refuses runs nothing and shows no
 * job screen: spu_up makes no call then, it only fills the reason. */
static int ls_open(void)
{
    if (fs_ok && spu_ask(&spu_ls) < 0) return -1;   /* the last result stays for the tile and the report */
    memset(&ls, 0, sizeof ls);
    if (!fs_ok) {
        ls.state = LS_NOTRUN;
        snprintf(ls.why, sizeof ls.why, "the app folder is not writable: no journal");
        return 0;
    }
    if (spu_blocked(&spu_ls)) spu_up(&spu_ls, SPU_KIT_LS, &ls.r);
    else run_job("Testing the SPU local stores", job_ls);
    ls_judge();
    return 0;
}

static void ls_card(float x, float y, float w, float h)
{
    test_card(x, y, w, h, BTN_SQUARE, "SPU local store",
              "The six SPUs, the Cell's helper cores: each tests its own 256 KB local store, 16 march passes. Under a second.");
    float ry = y + TEST_RESULT_Y;
    if (ls.state == LS_NONE) { text(x + 32, ry, F_MED, DIM, "Not tested"); return; }
    if (ls.state == LS_STUCK) {
        text(x + 32, ry, F_MED, RED, "Failed");
        text_wrap(x + 32, ry + 52, F_BODY, RED, w - 64, 2, "The SPUs could not be stopped: quit the app with START.");
        return;
    }
    if (ls.state == LS_NOTRUN || ls.state == LS_FAILED) {
        text(x + 32, ry, F_MED, ls.r.frozen ? RED : YELLOW, "%s", ls.state == LS_NOTRUN ? "Not run" : "Failed");
        text_wrap(x + 32, ry + 52, F_BODY, GREY, w - 64, 2, ls.why);
        return;
    }
    if (ls.errors) text(x + 32, ry, F_MED, RED, "%u error%s", (unsigned)ls.errors, ls.errors == 1 ? "" : "s");
    else text(x + 32, ry, F_MED, GREEN, "No errors");
    char t[24] = "";
    if (ls.ticks) snprintf(t, sizeof t, ", %.2f s", ls.ticks / TB_HZ);
    text_fit(x + 32, ry + 52, F_BODY, GREY, w - 64, "%d SPUs x %u KB%s", SPU_KIT_SPUS, (unsigned)((ls.hi - ls.lo + 512) / 1024), t);
    float ey = ry + 96;                      /* the first wrong read of each SPU with errors, while lines fit */
    for (int i = 0; i < SPU_KIT_SPUS && ey + 32 < y + h - 30; i++) {
        const spu_kit_rec *e = &ls.r.rec[i];
        if (!e->errors) continue;
        char at[32];
        snprintf(at, sizeof at, "SPU %d at 0x%05x:", i, (unsigned)e->err_addr);
        ey = err_lines(x, ey, w, at, e->err_want, e->err_got);
    }
}

static void ls_report(void)
{
    const spu_result *r = &ls.r;
    if (ls.state == LS_NONE) return;
    if (ls.state == LS_PASS || ls.state == LS_ERRORS) {
        rep_out("SPU local store: %d SPUs, %u bytes each (0x%05x to 0x%05x), %d passes, %u errors, %.3f s (%u ticks)\n", SPU_KIT_SPUS,
                (unsigned)(ls.hi - ls.lo), (unsigned)ls.lo, (unsigned)ls.hi, SPU_KIT_LS_PASSES, (unsigned)ls.errors, ls.ticks / TB_HZ,
                (unsigned)ls.ticks);
        for (int i = 0; i < SPU_KIT_SPUS; i++) {
            const spu_kit_rec *e = &r->rec[i];
            if (e->errors)
                rep_out("  SPU %d: %u errors, the first at 0x%05x: wrote 0x%08x, read 0x%08x\n", i, (unsigned)e->errors,
                        (unsigned)e->err_addr, (unsigned)e->err_want, (unsigned)e->err_got);
        }
    } else if (ls.state == LS_STUCK) rep_out("SPU local store: failed, the SPUs could not be stopped\n");
    else rep_out("SPU local store: %s, %s\n", ls.state == LS_NOTRUN ? "not run" : "failed", ls.why);
    if (!r->ran) return;
    rep_out("  SPUs started %d of %d", r->started, SPU_KIT_SPUS);
    if (r->terminated) rep_out(", terminate rc 0x%08x", (unsigned)r->term_rc);
    if (r->joined) rep_out(", join cause %u status 0x%x", (unsigned)r->cause, (unsigned)r->status);
    rep_out(", exit status");
    for (int i = 0; i < SPU_KIT_SPUS; i++) {
        if (r->exit_status[i] == -1) rep_out(" -");
        else rep_out(" 0x%x", (unsigned)r->exit_status[i]);
    }
    rep_out(", %llu ms in all\n", (unsigned long long)(r->us / 1000));
}

/* ---- screen --------------------------------------------------------------- */

/* One test: its button, what it does, and its result. */
static void mem_card(float x, float y, float w, float h, unsigned btn, const char *name, const char *desc, const mem_result *r)
{
    test_card(x, y, w, h, btn, name, desc);
    float ry = y + TEST_RESULT_Y;
    if (!r->done) { text(x + 32, ry, F_MED, DIM, "Not tested"); return; }
    if (r->errors) text(x + 32, ry, F_MED, RED, "%ld error%s", r->errors, r->errors == 1 ? "" : "s");
    else text(x + 32, ry, F_MED, r->cancelled ? YELLOW : GREEN, "%s", r->cancelled ? "Stopped, no error so far" : "No errors");
    text(x + 32, ry + 52, F_BODY, GREY, "%d MB in 1 MB blocks, %d pass%s", r->blocks, r->passes, r->passes == 1 ? "" : "es");
    float ey = ry + 96;
    for (int k = 0; k < r->errors && k < MAX_ERR && ey + 32 < y + h - 30; k++) {
        char at[32];
        snprintf(at, sizeof at, "at 0x%08lx:", (unsigned long)r->err_addr[k]);
        ey = err_lines(x, ey, w, at, r->err_want[k], r->err_got[k]);
    }
}

static void tile_line(char *out, int n, const char *name, const mem_result *r)
{
    if (!r->done) snprintf(out, n, "%s not tested", name);
    else if (r->cancelled) snprintf(out, n, "%s stopped: %ld errors", name, r->errors);   /* short: line 1 of a tile at the smallest visible area */
    else snprintf(out, n, "%s %d MB: %ld errors", name, r->blocks, r->errors);
}

/* An error is red; a stopped test is no pass, yellow, and so is an SPU test
 * that failed or did not run (as Transfer's tests that did not run). */
static void mem_tile(void)
{
    char l1[48], l2[48];
    if (!xdr.done && !vram.done && ls.state == LS_NONE) return;
    int bad = (xdr.done && xdr.errors) || (vram.done && vram.errors) || ls.state == LS_ERRORS;
    int stopped = (xdr.done && xdr.cancelled) || (vram.done && vram.cancelled) || (ls.state != LS_NONE && ls.state != LS_PASS);
    tile_line(l1, sizeof l1, "XDR", &xdr);
    tile_line(l2, sizeof l2, "RSX", &vram);
    size_t n = strlen(l2);
    if (ls.state == LS_PASS) snprintf(l2 + n, sizeof l2 - n, ", SPU ok");
    else if (ls.state == LS_ERRORS) snprintf(l2 + n, sizeof l2 - n, ", SPU %u err", (unsigned)ls.errors);
    else if (ls.state == LS_NOTRUN) snprintf(l2 + n, sizeof l2 - n, ", SPU not run");
    else if (ls.state != LS_NONE) snprintf(l2 + n, sizeof l2 - n, ", SPU failed");
    state_set("memory", bad ? DOT_BAD : stopped ? DOT_WARN : DOT_OK, l1, l2);
}

int mod_mem_open(void)
{
    while (1) {
        read_pad();
        int back = ui_leave();
        if (back <= 0) { mem_tile(); return back; }
        if (pressed & BTN_CROSS) run_job_cancelable("Memory test", job_xdr);
        if (pressed & BTN_TRIANGLE) run_job_cancelable("Memory test", job_vram);
        if ((pressed & BTN_SQUARE) && ls_open() < 0) { mem_tile(); return -1; }
        begin_frame();
        title();
        float w = (SW - 2 * MG - 64) / 3, h = BOTTOM - TOP - 56 > 560 ? 560 : BOTTOM - TOP - 56;   /* two wrong words fit */
        mem_card(MG, TOP, w, h, BTN_CROSS, "User memory (XDR)",
                 "All the user memory the app can get, about 200 MB of 256: four patterns written and read back. A few seconds.",
                 &xdr);
        mem_card(MG + w + 32, TOP, w, h, BTN_TRIANGLE, "RSX memory (GDDR3)",
                 "The free graphics memory: two patterns, read back by the RSX. Kept until the app exits.", &vram);
        ls_card(MG + 2 * (w + 32), TOP, w, h);
        text_fit(MG, TOP + h + 24, F_BODY, GREY, SW - 2 * MG, "%s",
                 "Each test writes only to memory the app allocated or to an SPU's own local store. A wrong word is a memory error.");
        footer("CROSS test the user memory  TRIANGLE test the RSX memory  SQUARE test the SPUs  CIRCLE home  START exit");
        ui_flip();
    }
}

/* The soak test's SPU check (mod_cool.c), in a job or, when the journal
 * refuses the family, with no call: the local-store test once, into its own
 * result r. The wrong reads of the six in *errors (saturating); 1 when the
 * six ended their test, 0 when not (r->why, or a late SPU). */
int mem_soak_ls(spu_result *r, unsigned *errors)
{
    *errors = 0;
    if (!spu_up(&spu_ls, SPU_KIT_LS, r)) return 0;
    int done = spu_wait_done(LS_US);
    spu_down(r);
    if (!done || r->why[0] || r->terminated) return 0;
    for (int i = 0; i < SPU_KIT_SPUS; i++) {
        if (r->rec[i].done != 1) return 0;
        *errors = r->rec[i].errors > 0xffffffffu - *errors ? 0xffffffffu : *errors + r->rec[i].errors;
    }
    return 1;
}

/* ---- the quick check ----------------------------------------------------- */

int mem_quick_ask(void) { return spu_ask(&spu_ls) < 0 ? -1 : 1; }

/* One pass of each test: the XDR, the RSX memory, the SPU local stores. */
void mem_quick(void)
{
    xdr_run(1);
    if (!job_cancel) vram_run(1);
    if (!job_cancel) ls_quick();
}

int mem_quick_end(char *line, int n)
{
    char a[48], b[48];
    mem_tile();
    tile_line(a, sizeof a, "XDR", &xdr);
    tile_line(b, sizeof b, "RSX", &vram);
    snprintf(line, n, "%s, %s, SPU %s", a, b, ls.state == LS_PASS ? "ok" : ls.state == LS_ERRORS ? "errors" : ls.state == LS_NONE ? "not tested" : ls.state == LS_NOTRUN ? "not run" : "failed");
    int bad = (xdr.done && xdr.errors) || (vram.done && vram.errors) || ls.state == LS_ERRORS;
    int stopped = (xdr.done && xdr.cancelled) || (vram.done && vram.cancelled) || (ls.state != LS_NONE && ls.state != LS_PASS) || !xdr.done || !vram.done;
    return bad ? DOT_BAD : stopped ? DOT_WARN : DOT_OK;
}

/* The speeds: MB/s over the time of the writes and of the reads with their
 * compare (MEM_LINES says which loops), for a console run to compare. */
static void report_result(const mem_result *r, const char *name, const char *how)
{
    if (!r->done) return;
    rep_out("%s: %d MB in 1 MB blocks, %d passes%s, %ld errors", name, r->blocks, r->passes, r->cancelled ? " (stopped)" : "", r->errors);
    if (r->w_us > 0 && r->r_us > 0)
        rep_out("; write %.0f MB/s, read and compare %.0f MB/s (%s)", (double)r->w_bytes / r->w_us, (double)r->r_bytes / r->r_us, how);
    rep_out("\n");
    for (int k = 0; k < r->errors && k < MAX_ERR; k++)
        rep_out("  error at 0x%08lx: wrote 0x%08x, read 0x%08x\n", (unsigned long)r->err_addr[k], r->err_want[k], r->err_got[k]);
}

/* The RSX test's read path: dma (the RSX's copies), ppu (64 KB copies
 * through the mapping), or both when the DMA turned off during the test. */
static void report_vram(void)
{
    char how[96];
    const char *path = !vram.dma_bytes ? "read: ppu, 64 KB copies through the mapping"
                     : !vram.ppu_bytes ? "read: dma, the RSX's copies into main memory" : "read: dma, then ppu";
    snprintf(how, sizeof how, "%s, %s", MEM_LINES ? "running patterns" : "pattern() per word", path);
    report_result(&vram, "RSX memory (GDDR3)", how);
    if (!vram.done) return;
    if (vram.rechecks)
        rep_out("  read again through the mapping after their DMA copy differed: %d block%s\n", vram.rechecks, vram.rechecks == 1 ? "" : "s");
#if MEM_DMA
    if (dma_note[0]) rep_out("  RSX DMA off: %s\n", dma_note);
#endif
}

void mod_mem_report(void)
{
    if (!xdr.done && !vram.done && ls.state == LS_NONE) return;
    rep_out("--- memory ---\n");
    char how[96];
    snprintf(how, sizeof how, "%s, %s", MEM_LINES ? "running patterns, dcbz per line" : "pattern() per word",
             xdr_kernel ? "1 MB kernel pages" : "malloc blocks");
    report_result(&xdr, "user memory (XDR)", how);
    report_vram();
    ls_report();
}
