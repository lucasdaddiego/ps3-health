/* The Memory module: pattern tests on the user memory (XDR) the app can get,
 * on the RSX memory (GDDR3) through its mapping, and on the local store of
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
} mem_result;

static mem_result xdr, vram;
static void *blk[MAX_BLOCKS];

static void note_error(mem_result *r, uintptr_t addr, uint32_t want, uint32_t got)
{
    if (r->errors < MAX_ERR) {
        r->err_addr[r->errors] = addr;
        r->err_want[r->errors] = want;
        r->err_got[r->errors] = got;
    }
    r->errors++;
}

/* Pattern p of a word at index i in block b: address based, its inverse, an
 * alternating word, and a linear congruential sequence. */
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

/* The user memory is written and compared in place. The RSX mapping goes
 * through 64 KB chunks: a pattern chunk is built in user memory and copied
 * in, and the block is copied out before the compare, because one word at a
 * time on that mapping is far slower than a memcpy. */
#define CHUNK 65536
static uint32_t chunk[CHUNK / 4] __attribute__((aligned(128)));
static uint32_t readback[CHUNK / 4] __attribute__((aligned(128)));

static void write_block(int p, int b, int direct)
{
    if (direct) {
        uint32_t *w = blk[b];
        for (unsigned i = 0; i < BLOCK / 4; i++) w[i] = pattern(p, b, i);
        return;
    }
    for (unsigned off = 0; off < BLOCK; off += CHUNK) {
        for (unsigned i = 0; i < CHUNK / 4; i++) chunk[i] = pattern(p, b, off / 4 + i);
        memcpy((uint8_t *)blk[b] + off, chunk, CHUNK);
    }
}

static void check_block(mem_result *r, int p, int b, int direct)
{
    for (unsigned off = 0; off < BLOCK; off += CHUNK) {
        const uint32_t *w = direct ? (const uint32_t *)((uint8_t *)blk[b] + off) : readback;
        if (!direct) memcpy(readback, (uint8_t *)blk[b] + off, CHUNK);
        for (unsigned i = 0; i < CHUNK / 4; i++) {
            uint32_t want = pattern(p, b, off / 4 + i);
            if (w[i] != want) note_error(r, (uintptr_t)blk[b] + off + i * 4, want, w[i]);
        }
    }
}

static void run_passes(mem_result *r, int passes, int direct)
{
    int total = passes * r->blocks * 2, step = 0;
    for (int p = 0; p < passes && !job_cancel; p++) {
        for (int b = 0; b < r->blocks && !job_cancel; b++, step++) {
            write_block(p, b, direct);
            job_set_percent(step * 100 / total);
        }
        for (int b = 0; b < r->blocks && !job_cancel; b++, step++) {
            check_block(r, p, b, direct);
            job_set_percent(step * 100 / total);
        }
        if (!job_cancel) r->passes = p + 1;
    }
    r->cancelled = job_cancel;
}

static void xdr_run(int passes)
{
    memset(&xdr, 0, sizeof xdr);
    progress("Allocating the user memory in 1 MB blocks");
    int n = 0;
    while (n < MAX_BLOCKS && (blk[n] = malloc(BLOCK)) != NULL) n++;
    for (int k = 0; k < 8 && n > 0; k++) free(blk[--n]);     /* headroom for the app itself */
    xdr.blocks = n;
    progress(passes == 4 ? "Testing the user memory (XDR): 4 patterns, each written then verified"
                         : "Testing the user memory (XDR): 1 pattern, written then verified");
    run_passes(&xdr, passes, 1);
    for (int b = 0; b < n; b++) free(blk[b]);
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
    progress(passes == 2 ? "Testing the RSX memory (GDDR3) through its mapping: 2 patterns, slow by design"
                         : "Testing the RSX memory (GDDR3) through its mapping: 1 pattern, slow by design");
    run_passes(&vram, passes, 0);
    vram.done = 1;
}

static void job_vram(void) { vram_run(2); }

/* The soak test of the Cooling module: one pattern over the caller's blocks,
 * written then verified in place, as the XDR test. The errors, and the first
 * wrong word's address in *first (untouched when none); stop ends it early. */
long mem_soak_pass(void *const *blocks, int n, int p, volatile int *stop, unsigned long *first)
{
    long errors = 0;
    for (int b = 0; b < n && !*stop; b++) {
        uint32_t *w = blocks[b];
        for (unsigned i = 0; i < BLOCK / 4; i++) w[i] = pattern(p, b, i);
    }
    for (int b = 0; b < n && !*stop; b++) {
        const uint32_t *w = blocks[b];
        for (unsigned i = 0; i < BLOCK / 4; i++)
            if (w[i] != pattern(p, b, i)) {
                if (!errors) *first = (unsigned long)(uintptr_t)&w[i];
                errors++;
            }
    }
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
                 "All the user memory the app can get, about 200 MB of 256: four patterns written and read back. About a minute.",
                 &xdr);
        mem_card(MG + w + 32, TOP, w, h, BTN_TRIANGLE, "RSX memory (GDDR3)",
                 "The free graphics memory, through its slow mapping: two patterns, a few minutes. Kept until the app exits.", &vram);
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

static void report_result(const mem_result *r, const char *name)
{
    if (!r->done) return;
    rep_out("%s: %d MB in 1 MB blocks, %d passes%s, %ld errors\n", name, r->blocks, r->passes, r->cancelled ? " (stopped)" : "",
            r->errors);
    for (int k = 0; k < r->errors && k < MAX_ERR; k++)
        rep_out("  error at 0x%08lx: wrote 0x%08x, read 0x%08x\n", (unsigned long)r->err_addr[k], r->err_want[k], r->err_got[k]);
}

void mod_mem_report(void)
{
    if (!xdr.done && !vram.done && ls.state == LS_NONE) return;
    rep_out("--- memory ---\n");
    report_result(&xdr, "user memory (XDR)");
    report_result(&vram, "RSX memory (GDDR3)");
    ls_report();
}
