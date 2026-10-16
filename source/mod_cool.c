/* The Cooling module: the Cell and RSX temperatures (syscall 383), the fan
 * duty (syscall 409, read-only, behind the first-run prompt), and a timed load
 * with a temperature curve: two PPU threads, the six SPUs (the SPU kit's heat
 * mode through spu_run.c, behind the SPU prompt) and the RSX. The test stops
 * by itself at 75 C, after two failed temperature reads in a row, or when an
 * SPU stops reporting. Nothing is written to the fan: the system keeps its
 * own policy.
 * Also: the console family, asked once and kept in family.txt, for a paste
 * estimate after each test (heat.c: provisional ranges), and the soak test:
 * 30 or 60 minutes of load turns and memory turns by turns, one SPU group at
 * a time, with the same stops. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ppu-lv2.h>
#include <lv2/systime.h>
#include <sys/thread.h>
#include "app.h"
#include <ps3gfx/ui.h>
#include "report.h"
#include "heat.h"

#define MAX_SAMPLES 400                      /* every 2 s: 13 min; a soak halves the curve when it is full */
#define STOP_C 75                            /* a load test stops when the Cell or the RSX reaches it */
#define STOP_S "75 C"                        /* STOP_C in the texts */
#define WARM_C 72                            /* the verdict: warm from here */
#define FAMILY_FILE APP_DIR "/family.txt"
#define SOAK_LOAD_S 240                      /* a soak's load turn */
#define SOAK_MEM_S 60                        /* a soak's memory turn, right after a load turn: the chips are warm */
#define SOAK_BLOCKS 64                       /* the XDR of the memory turns, in 1 MB blocks */

static int cpu_now = -1, rsx_now = -1, temps_rc, read_once;   /* read_once: the module read once, so the report has its section and the rc */
static int fan_pct = -1, fan_mode = -1, fan_rc;
static int family;                           /* FAMILY_*: read from FAMILY_FILE at the first open */
enum { STOP_NONE, STOP_HOT, STOP_READ, STOP_SPU };   /* why a load test stopped by itself */
static struct {
    int done, n, seconds, cancelled;
    int step;                                /* seconds between two samples: 2, doubled at each halving of a soak */
    int cpu0, rsx0, fan0, cpu_max, rsx_max, fan_end;
    int stop, stop_s, stop_cpu, stop_rsx, stop_spu;   /* STOP_*, its second; the readings of a STOP_HOT; the SPU of a STOP_SPU */
    int spus;                                /* the six SPUs run the heat load now (spu_up returned 1) */
    int spus_ran;                            /* they ran in at least one load turn */
    u64 c0[SPU_KIT_SPUS], c1[SPU_KIT_SPUS];  /* their counts at the first and the last sample of the heat */
    s64 t0, t1;                              /* the times of those samples */
    short cpu[MAX_SAMPLES], rsx[MAX_SAMPLES], fan[MAX_SAMPLES];   /* -1 = not read */
    /* a soak */
    int soak;                                /* the minutes chosen; 0 for a load test */
    int turns;                               /* turns begun, load and memory */
    long mem_errors, mem_passes;             /* the XDR patterns of the memory turns */
    int mem_blocks;
    unsigned long mem_first;                 /* the first wrong word's address */
    int ls_runs, ls_ok;                      /* SPU local-store tests begun, and ended with the six */
    unsigned ls_errors;
    char ls_why[64];                         /* the last local-store test that did not end */
} load;
static spu_result spu;                       /* the SPU leg of the last load test (of a soak: its last load turn) */
static heat_paste paste;                     /* the paste estimate of the last test */

static void read_now(void)
{
    temps_rc = console_temps(&cpu_now, &rsx_now);
    read_once = 1;
    fan_rc = fan_policy_read(&fan_pct, &fan_mode);     /* -1 and no value when skipped or frozen */
}

/* ---- the console family ---------------------------------------------------- */

static void family_load(void)
{
    char t[24];
    int n = fs_read_file(FAMILY_FILE, t, sizeof t - 1);
    if (n <= 0) return;
    t[n] = 0;
    family = heat_family_parse(t);
}

static void family_save(int f)
{
    const heat_family *h = heat_family_get(f);
    char t[16];
    int n = snprintf(t, sizeof t, "%s\n", h ? h->key : "unknown");
    family = f;
    fs_write_file(FAMILY_FILE, t, (u64)n, 0);   /* a failed write: the answer holds for this session */
}

static const char *family_name(void)
{
    const heat_family *h = heat_family_get(family);
    return h ? h->name : "Model not set";
}

/* The question, once at the first open (CIRCLE: not now, kept as unknown)
 * and again on L1. -1 on exit. */
static int family_screen(void)
{
    while (1) {
        read_pad();
        if (quit || (pressed & BTN_START)) return -1;
        int f = pressed & BTN_CROSS ? FAMILY_FAT : pressed & BTN_TRIANGLE ? FAMILY_SLIM : pressed & BTN_SQUARE ? FAMILY_SUPER : 0;
        if (f) { family_save(f); break; }
        if (pressed & BTN_CIRCLE) {
            if (family == FAMILY_NONE) family_save(FAMILY_UNKNOWN);
            break;
        }
        begin_frame();
        title();
        text(MG, TOP, F_MED, WHITE, "Which PS3 is this?");
        text_wrap(MG, TOP + 52, F_BODY, GREY, SW - 2 * MG, 2,
                  "The model number is on the label at the back. The paste estimate compares a load test with the usual "
                  "range of this family. Asked once: L1 in Cooling changes it.");
        float w = (SW - 2 * MG - 64) / 3, y = TOP + 150, h = 250;
        for (int k = 0; k < 3; k++) {
            const heat_family *hf = heat_family_get(FAMILY_FAT + k);
            static const unsigned btn[3] = {BTN_CROSS, BTN_TRIANGLE, BTN_SQUARE};
            char desc[96];
            snprintf(desc, sizeof desc, "%s. %s", hf->models, k == 0 ? "The first, big model, 2006 to 2009." : k == 1 ? "The slim model, 2009 to 2012." : "The top-loading model, from 2012.");
            option_card(MG + k * (w + 32), y, w, h, btn[k], 1, hf->name, desc);
            if (family == FAMILY_FAT + k) round_frame(MG + k * (w + 32) - 4, y - 4, w + 8, h + 8, 22, 3, WHITE);
        }
        footer("CROSS fat  TRIANGLE slim  SQUARE super slim  CIRCLE not now  START exit");
        ui_flip();
    }
    pressed = 0;                             /* the answer acts nowhere else */
    return 0;
}

/* ---- the load ------------------------------------------------------------- */

static volatile int load_on;

static void load_thread(void *arg)
{
    (void)arg;
    volatile double x = 1.0;
    while (load_on) {
        for (int i = 0; i < 200000; i++) x = x * 1.0000001 + 0.5;
    }
    sysThreadExit(0);
}

/* Fill-rate work before anything else is drawn: the opaque background,
 * again and again, so the picture does not change. (A layer under the alpha
 * test's 0x10 would be discarded and cost the RSX nothing.) 160 layers, twice
 * the 80 of 2.0.0: without a depth buffer each layer moves half the bytes. */
static void rsx_load(void)
{
    for (int k = 0; k < 160; k++) background();
}

/* The SPU leg: the start and the end each in a job (a second thread with the
 * counter): every SPU call is journaled, and a freeze shows as a counter
 * that stops. A family that the journal refuses gets no job screen: spu_up
 * makes no call then, it only fills the reason. */
static int counts_fresh;                     /* the next sample takes the counts as the start of the rate */
static void job_spu_up(void) { load.spus = spu_up(&spu_cool, SPU_KIT_HEAT, &spu); }
static void job_spu_down(void) { spu_down(&spu); }

static void heat_up(void)
{
    if (spu_blocked(&spu_cool)) spu_up(&spu_cool, SPU_KIT_HEAT, &spu);   /* no call: only the reason, no job screen */
    else run_job("Starting the six SPUs", job_spu_up);
    if (load.spus) load.spus_ran = 1;
    counts_fresh = 1;
}

static void heat_down(void)
{
    if (load.spus) run_job("Stopping the six SPUs", job_spu_down);
    load.spus = 0;
}

/* The loops of SPU i per microsecond (millions a second) from the first to
 * the last sample; 0 before two samples. */
static double spu_rate(int i)
{
    return load.t1 > load.t0 ? (double)(load.c1[i] - load.c0[i]) / (double)(load.t1 - load.t0) : 0;
}

static int spu_mean(void)
{
    double m = 0;
    for (int i = 0; i < SPU_KIT_SPUS; i++) m += spu_rate(i);
    return (int)(m / SPU_KIT_SPUS + 0.5);
}

/* A full curve (a soak): each pair of samples becomes one, twice the step. */
static void halve(void)
{
    heat_halve(load.cpu, load.n);
    heat_halve(load.rsx, load.n);
    load.n = heat_halve(load.fan, load.n);
    load.step *= 2;
}

/* A sample every step, then the stops, the hot one first: the Cell or the
 * RSX at 75 C, two failed reads in a row, an SPU whose count did not move
 * since the last sample (heat reports every 2^20 loops, hundreds of times a
 * second). The first sample of a heat only keeps the counts. A sample that
 * catches up after a slow frame, under 1 s after the last count, skips the
 * counts. 1 when the test stops. */
static int sample(s64 now, int *misses)
{
    read_now();
    if (load.n == MAX_SAMPLES) halve();
    load.cpu[load.n] = (short)cpu_now;
    load.rsx[load.n] = (short)rsx_now;
    load.fan[load.n] = (short)fan_pct;
    load.n++;
    int still = -1;
    *misses = cpu_now < 0 || rsx_now < 0 ? *misses + 1 : 0;
    if (load.spus && (counts_fresh || now - load.t1 >= 1000000)) {
        const volatile spu_kit_rec *rp = spu_reports();
        for (int i = 0; i < SPU_KIT_SPUS; i++) {
            u64 c = rp[i].count;
            if (counts_fresh) load.c0[i] = c;
            else if (c == load.c1[i] && still < 0) still = i;
            load.c1[i] = c;
        }
        if (counts_fresh) load.t0 = now;
        load.t1 = now;
        counts_fresh = 0;
    }
    int m = cpu_now > rsx_now ? cpu_now : rsx_now;
    if (m >= STOP_C) {
        load.stop = STOP_HOT;
        load.stop_cpu = cpu_now;
        load.stop_rsx = rsx_now;
    } else if (*misses >= 2) load.stop = STOP_READ;
    else if (still >= 0) {
        load.stop = STOP_SPU;
        load.stop_spu = still;
    }
    return load.stop != STOP_NONE;
}

static u32 temp_color(int t) { return t < 0 ? GREY : t < WARM_C ? GREEN : t < 80 ? YELLOW : RED; }

/* The y of a temperature on a graph from 30 to 90 C; outside that range it stays on the edge. */
static float graph_y(int t, float gy, float gh)
{
    t = t < 30 ? 30 : t > 90 ? 90 : t;
    return gy + gh - (t - 30) * gh / 60;
}

/* The time ticks of an axis that covers span seconds. */
static int tick_s(int span) { return span > 1800 ? 600 : span > 600 ? 300 : span > 180 ? 60 : 30; }

/* The curve: 30 to 90 C, the Cell in white, the RSX in blue, the warm line
 * and the stop as thin lines; span is the time the x axis covers. */
static void draw_graph(float x, float y, float w, float h, int n, int span)
{
    card(x, y, w, h);
    float gx = x + 76, gw = w - 100, gy = y + 20, gh = h - 60;
    for (int t = 40; t <= 90; t += gh < 240 ? 20 : 10) {     /* a short graph: every 20 C */
        float ly = graph_y(t, gy, gh);
        rect(gx, ly, gw, 1, LINE);
        text_r(gx - 12, ly - line_h(F_SMALL) / 2, F_SMALL, DIM, "%d", t);
    }
    rect(gx, graph_y(WARM_C, gy, gh), gw, 2, 0xffcc4060);       /* warm */
    rect(gx, graph_y(STOP_C, gy, gh), gw, 2, 0xff646470);       /* the stop */
    if (span < 2) span = 2;
    for (int t = 0; t <= span; t += tick_s(span))
        text_c(gx + gw * t / span, gy + gh + 8, F_SMALL, DIM, "%d:%02d", t / 60, t % 60);
    float sx = gw * (load.step ? load.step : 2) / span;
    for (int t = 0; t < 2; t++) {           /* one batch per curve, the Cell over the RSX */
        const short *v = t ? load.cpu : load.rsx;
        batch_begin(t ? WHITE : BLUE);
        for (int k = 1; k < n; k++)
            if (v[k] >= 0 && v[k - 1] >= 0)  /* a failed read leaves a gap */
                batch_line(gx + (k - 1) * sx, graph_y(v[k - 1], gy, gh), gx + k * sx, graph_y(v[k], gy, gh), 4);
        batch_end();
    }
    text_r(x + w - 24, y + 16, F_SMALL, WHITE, "Cell");
    text_r(x + w - 90, y + 16, F_SMALL, BLUE, "RSX");
}

/* The temperatures and the fan on the right of the title line. */
static void now_values(void)
{
    float rx = SW - MG;
    if (fan_pct >= 0) rx = text_r(rx, TOP, F_MED | TNUM, WHITE, "fan %d%%", fan_pct) - 40;
    rx = text_r(rx, TOP, F_MED | TNUM, temp_color(rsx_now), "RSX %d C", rsx_now) - 40;
    text_r(rx, TOP, F_MED | TNUM, temp_color(cpu_now), "Cell %d C", cpu_now);
}

static void start_test(int seconds, int soak)
{
    memset(&load, 0, sizeof load);
    load.seconds = seconds;
    load.soak = soak;
    load.step = 2;
    read_now();
    load.cpu0 = cpu_now;
    load.rsx0 = rsx_now;
    load.fan0 = fan_pct;
}

/* The end of every test: the maxima, the fan, the time, the paste estimate. */
static void end_test(s64 t0)
{
    load.cpu_max = load.rsx_max = -1;
    for (int k = 0; k < load.n; k++) {
        if (load.cpu[k] > load.cpu_max) load.cpu_max = load.cpu[k];
        if (load.rsx[k] > load.rsx_max) load.rsx_max = load.rsx[k];
    }
    load.fan_end = fan_pct;
    load.seconds = (int)((sysGetSystemTime() - t0) / 1000000);
    load.done = 1;
    int idle = load.cpu0 > load.rsx0 ? load.cpu0 : load.rsx0, peak = load.cpu_max > load.rsx_max ? load.cpu_max : load.rsx_max;
    int rc = heat_rise(load.cpu, load.n, load.step), rr = heat_rise(load.rsx, load.n, load.step);
    if (spu.stuck || load.stop == STOP_READ) rc = rr = -1;   /* no estimate on readings the test could not finish */
    heat_paste_judge(family, idle, peak, rc > rr ? rc : rr, load.fan_end, &paste);
    pressed = 0;                             /* a button still down is not a new press */
    write_report();
}

static void load_screen(int seconds)
{
    sys_ppu_thread_t th[2];
    int ok[2] = {0, 0}, misses = 0;
    u64 ret;
    start_test(seconds, 0);
    heat_up();
    if (spu_stuck()) spu.stuck = 1;          /* the SPUs of an earlier test (the Memory module's) could not be stopped: the red line */
    load_on = 1;
    int started = 0;
    for (int k = 0; k < 2; k++)
        if (sysThreadCreate(&th[k], load_thread, NULL, 2500, 0x4000, THREAD_JOINABLE, "load") == 0) ok[k] = 1, started++;
    s64 t0 = sysGetSystemTime(), next = t0;
    while (!spu.stuck) {                     /* SPUs that could not be stopped, at their start or in an earlier test: no load on top */
        read_pad();
        s64 now = sysGetSystemTime();
        if (quit || (pressed & BTN_CIRCLE)) { load.cancelled = 1; break; }
        if (now - t0 >= (s64)seconds * 1000000) break;
        if (now >= next && load.n < MAX_SAMPLES) {
            next += 2000000;
            if (sample(now, &misses)) { load.stop_s = (int)((now - t0) / 1000000); break; }
        }
        begin_frame();
        rsx_load();
        title();
        double sec = (double)(now - t0) / 1e6;
        float x = text(MG, TOP, F_MED | TNUM, WHITE, "Load test  %d:%02d of %d:%02d", (int)sec / 60, (int)sec % 60, seconds / 60, seconds % 60);
        spinner(x + 40, TOP + 20, 16, sec);
        now_values();
        bar(MG, TOP + 56, SW - 2 * MG, 10, (float)(sec / seconds), GREEN);
        draw_graph(MG, TOP + 90, SW - 2 * MG, BOTTOM - TOP - 184, load.n, seconds);
        if (load.spus)
            text_fit(MG, BOTTOM - 80, F_BODY, GREY, SW - 2 * MG, "%d PPU threads, the six SPUs and the RSX fill rate at full load. Started at Cell %d C, RSX %d C.",
                     started, load.cpu0, load.rsx0);
        else
            text_fit(MG, BOTTOM - 80, F_BODY, GREY, SW - 2 * MG, "%d PPU threads and the RSX fill rate at full load. Started at Cell %d C, RSX %d C.",
                     started, load.cpu0, load.rsx0);
        if (!load.spus) text_fit(MG, BOTTOM - 36, F_BODY, GREY, SW - 2 * MG, "SPUs not used: %s.", spu.why);
        else if (load.t1 > load.t0) text_fit(MG, BOTTOM - 36, F_BODY, GREY, SW - 2 * MG, "%d SPUs: about %d million loops a second each.", SPU_KIT_SPUS, spu_mean());
        else text_fit(MG, BOTTOM - 36, F_BODY, GREY, SW - 2 * MG, "%d SPUs at full load: their rate after the second sample.", SPU_KIT_SPUS);
        status_line("The counter runs while the console works. The test stops by itself at " STOP_S ". CIRCLE stops it and keeps the curve.", GREY);
        footer("CIRCLE stop");
        ui_flip();
    }
    load_on = 0;
    for (int k = 0; k < 2; k++) if (ok[k]) sysThreadJoin(th[k], &ret);
    heat_down();
    end_test(t0);
}

/* ---- the soak test ---------------------------------------------------------- */

/* The memory turn's thread: the Memory module's four patterns, one after
 * the other, over the soak's blocks until the turn ends. */
static void *soak_blk[SOAK_BLOCKS];
static volatile int mem_stop;
static volatile long mem_errors, mem_passes;
static volatile unsigned long mem_first;

static void mem_thread(void *arg)
{
    (void)arg;
    for (int p = 0; !mem_stop; p = (p + 1) % 4) {
        unsigned long first = 0;
        long e = mem_soak_pass(soak_blk, load.mem_blocks, p, &mem_stop, &first);
        if (e && !mem_errors) mem_first = first;
        mem_errors += e;
        if (!mem_stop) mem_passes++;
    }
    sysThreadExit(0);
}

static spu_result soak_ls;                   /* the last local-store test of a memory turn */
static unsigned soak_ls_err;
static int soak_ls_done;
static void job_soak_ls(void) { soak_ls_done = mem_soak_ls(&soak_ls, &soak_ls_err); }

/* The SPU check at the start of a memory turn: the heat group is down, so
 * one group at a time. */
static void soak_ls_run(void)
{
    if (spu_blocked(&spu_ls)) {              /* no call, no job screen, no test counted: only the reason */
        mem_soak_ls(&soak_ls, &soak_ls_err);
        snprintf(load.ls_why, sizeof load.ls_why, "%s", soak_ls.why);
        return;
    }
    run_job("Testing the SPU local stores", job_soak_ls);
    load.ls_runs++;
    if (soak_ls_done) {
        load.ls_ok++;
        load.ls_errors = soak_ls_err > 0xffffffffu - load.ls_errors ? 0xffffffffu : load.ls_errors + soak_ls_err;
    } else snprintf(load.ls_why, sizeof load.ls_why, "%s", soak_ls.why[0] ? soak_ls.why : "an SPU did not finish in 5 s");
}

/* The turns: even ones load (the heat on the SPUs), odd ones memory (the SPU
 * check, then the patterns). Each turn is one journaled entry, soak_load or
 * soak_mem, from its start to its end: a freeze inside is recorded, and the
 * soak does not run again until the journal is cleared. */
static sys_ppu_thread_t mem_th;
static int mem_th_ok, in_turn;

static void turn_begin(int turn)
{
    load.turns = turn + 1;
    in_turn = 1;
    if (turn % 2 == 0) {
        journal_start("soak_load");
        heat_up();
        return;
    }
    journal_start("soak_mem");
    if (!spu.stuck && !spu_stuck()) soak_ls_run();
    mem_stop = 0;
    mem_th_ok = load.mem_blocks && sysThreadCreate(&mem_th, mem_thread, NULL, 1500, 0x4000, THREAD_JOINABLE, "soak_mem") == 0;
}

static void turn_end(int turn)
{
    u64 ret;
    in_turn = 0;
    if (turn % 2 == 0) {
        heat_down();
        journal_done("soak_load", 0, !spu.stuck);
        return;
    }
    mem_stop = 1;
    if (mem_th_ok) sysThreadJoin(mem_th, &ret);
    mem_th_ok = 0;
    journal_done("soak_mem", 0, 1);
}

static const char *soak_blocked(void)
{
    if (journal_state("soak_load") == J_FROZEN) return "a load turn of the soak froze this console before";
    if (journal_state("soak_mem") == J_FROZEN) return "a memory turn of the soak froze this console before";
    return NULL;
}

static void soak_screen(int minutes)
{
    sys_ppu_thread_t th[2];
    int ok[2] = {0, 0}, misses = 0, seconds = minutes * 60, nturns = seconds / (SOAK_LOAD_S + SOAK_MEM_S) * 2;
    u64 ret;
    start_test(seconds, minutes);
    mem_errors = mem_passes = 0;
    mem_first = 0;
    while (load.mem_blocks < SOAK_BLOCKS && (soak_blk[load.mem_blocks] = malloc(1024 * 1024)) != NULL) load.mem_blocks++;
    if (spu_stuck()) spu.stuck = 1;
    load_on = 1;
    int started = 0, turn = 0;
    for (int k = 0; k < 2; k++)
        if (sysThreadCreate(&th[k], load_thread, NULL, 2500, 0x4000, THREAD_JOINABLE, "load") == 0) ok[k] = 1, started++;
    if (!spu.stuck) turn_begin(0);
    s64 t0 = sysGetSystemTime(), next = t0, turn_to = t0 + SOAK_LOAD_S * 1000000LL;
    while (!spu.stuck) {
        read_pad();
        s64 now = sysGetSystemTime();
        if (quit || (pressed & BTN_CIRCLE)) { load.cancelled = 1; break; }
        if (now - t0 >= (s64)seconds * 1000000) break;
        if (now >= turn_to && turn + 1 < nturns) {   /* the next turn; its jobs stop the clock of the curve for a moment */
            turn_end(turn);
            if (spu.stuck) break;
            turn_begin(++turn);
            now = sysGetSystemTime();
            turn_to = now + (turn % 2 ? SOAK_MEM_S : SOAK_LOAD_S) * 1000000LL;
            if (next < now) next = now;
            continue;
        }
        if (now >= next) {
            next += load.step * 1000000LL;
            if (sample(now, &misses)) { load.stop_s = (int)((now - t0) / 1000000); break; }
        }
        begin_frame();
        rsx_load();
        title();
        double sec = (double)(now - t0) / 1e6;
        float x = text(MG, TOP, F_MED | TNUM, WHITE, "Soak test  %d:%02d of %d:00", (int)sec / 60, (int)sec % 60, minutes);
        spinner(x + 40, TOP + 20, 16, sec);
        now_values();
        bar(MG, TOP + 56, SW - 2 * MG, 10, (float)(sec / seconds), GREEN);
        draw_graph(MG, TOP + 90, SW - 2 * MG, BOTTOM - TOP - 184, load.n, seconds);
        if (turn % 2 == 0)
            text_fit(MG, BOTTOM - 80, F_BODY, WHITE, SW - 2 * MG, "Load turn, %d of %d: %d PPU threads, %s and the RSX fill rate at full load.", turn + 1, nturns,
                     started, load.spus ? "the six SPUs" : "no SPUs");
        else
            text_fit(MG, BOTTOM - 80, F_BODY, WHITE, SW - 2 * MG, "Memory turn, %d of %d: patterns over %d MB of XDR, the RSX fill rate, the SPU local stores checked.",
                     turn + 1, nturns, load.mem_blocks);
        if (!load.ls_runs && load.ls_why[0])   /* the SPU check refused: the reason */
            text_fit(MG, BOTTOM - 36, F_BODY, mem_errors ? RED : GREY, SW - 2 * MG, "So far: %ld memory passes, %ld errors. SPU local stores not tested: %s.",
                     (long)mem_passes, (long)mem_errors, load.ls_why);
        else
            text_fit(MG, BOTTOM - 36, F_BODY, mem_errors || load.ls_errors ? RED : GREY, SW - 2 * MG,
                     "So far: %ld memory passes, %ld errors. SPU local stores: %d of %d tests ended, %u errors.", (long)mem_passes, (long)mem_errors,
                     load.ls_ok, load.ls_runs, load.ls_errors);
        status_line("The soak stops by itself at " STOP_S ", after two failed reads or a stalled SPU. CIRCLE stops it and keeps the curve.", GREY);
        footer("CIRCLE stop");
        ui_flip();
    }
    if (in_turn) turn_end(turn);
    load_on = 0;
    for (int k = 0; k < 2; k++) if (ok[k]) sysThreadJoin(th[k], &ret);
    for (int b = 0; b < load.mem_blocks; b++) free(soak_blk[b]);
    load.mem_errors = mem_errors;
    load.mem_passes = mem_passes;
    load.mem_first = mem_first;
    end_test(t0);
}

/* R1: 30 or 60 minutes. -1 on exit. */
static int soak_choice(void)
{
    const char *why = soak_blocked();
    while (1) {
        read_pad();
        if (quit || (pressed & BTN_START)) return -1;
        if (pressed & BTN_CIRCLE) break;
        if ((pressed & (BTN_CROSS | BTN_TRIANGLE)) && !why) {
            int minutes = pressed & BTN_CROSS ? 30 : 60;
            if (spu_ask(&spu_cool) < 0) return -1;   /* the SPU prompt, once; a skip runs the soak without them */
            soak_screen(minutes);
            break;
        }
        begin_frame();
        title();
        text(MG, TOP, F_MED, WHITE, "Soak test");
        float w = (SW - 2 * MG - 32) / 2, y = TOP + 64, h = 250;
        option_card(MG, y, w, h, BTN_CROSS, !why, "30 minutes", "Six load turns of 4 minutes, each followed by a memory turn of 1 minute.");
        option_card(MG + w + 32, y, w, h, BTN_TRIANGLE, !why, "60 minutes", "Twelve of each. Long enough to warm the whole console through.");
        text_wrap(MG, y + h + 36, F_BODY, why ? RED : GREY, SW - 2 * MG, 4,
                  why ? why : "A load turn: two PPU threads, the six SPUs and the RSX at full load. A memory turn: the six SPUs test their "
                              "local stores, then the PPU writes and checks patterns over 64 MB while the RSX keeps its load. One SPU group "
                              "at a time. The soak stops by itself at 75 C, after two failed reads or a stalled SPU.");
        footer(why ? "CIRCLE back  START exit" : "CROSS 30 minutes  TRIANGLE 60 minutes  CIRCLE back  START exit");
        ui_flip();
    }
    pressed = 0;
    return 0;
}

/* ---- results ---------------------------------------------------------------- */

static int soak_bad(void) { return load.soak && (load.mem_errors || load.ls_errors); }

/* A test that stopped by itself (75 C, no readings, an SPU), SPUs that could
 * not be stopped, no readings, a short test and 72 C or more give a yellow
 * verdict. The test stops at 75 C, so no temperature is red; a memory error
 * in a soak is. */
static int verdict_dot(void)
{
    int m = load.cpu_max > load.rsx_max ? load.cpu_max : load.rsx_max;
    if (soak_bad()) return DOT_BAD;
    return load.stop || spu.stuck || m < 0 || load.n < 10 || m >= WARM_C ? DOT_WARN : DOT_OK;
}

static const char *verdict_text(void)
{
    static char t[128];
    int m = load.cpu_max > load.rsx_max ? load.cpu_max : load.rsx_max;
    if (soak_bad()) {
        snprintf(t, sizeof t, "Memory errors under heat: %ld in the XDR, %u in the SPU local stores. Run the Memory module's tests.", load.mem_errors,
                 load.ls_errors);
        return t;
    }
    if (spu.stuck && !spu.ran) return "No verdict: the SPUs of an earlier test could not be stopped.";   /* spu_up made no call: the Memory module's group */
    if (spu.stuck && !load.spus_ran) return "No verdict: the SPUs could not be stopped at their start.";   /* stuck in spu_up: the test did not run */
    if (spu.stuck) return "No verdict: the SPUs could not be stopped after the test.";
    if (load.stop == STOP_HOT) return "Warm: the test reached the app's limit of " STOP_S " and stopped before its end. Clean the dust and check the fan.";
    if (load.stop == STOP_READ) return "Stopped after two failed temperature reads in a row: no verdict.";
    if (load.stop == STOP_SPU) {
        snprintf(t, sizeof t, "Stopped because SPU %d stopped reporting: no verdict.", load.stop_spu);
        return t;
    }
    if (m < 0) return "No temperature readings during the test: no verdict.";
    if (load.n < 10) return "Too short for a verdict: run at least two minutes.";
    if (m >= WARM_C) return "Warm under load. The console copes, but dust or dry paste would push it further: clean it.";
    if (load.soak && !load.cancelled) {
        snprintf(t, sizeof t, "Stable: %d minutes of load and memory turns, no memory error.", load.soak);
        return t;
    }
    if (load.soak) return "No error so far, under the usual peak: the soak was stopped before its end.";
    return "Within the usual range under load.";
}

/* The SPU leg of the last test in one line, and its colour. */
static u32 spu_line(char *s, int n)
{
    if (spu.stuck) {
        snprintf(s, n, "The SPUs did not stop: quit the app with START.");   /* one line at the smallest visible area */
        return RED;
    }
    if (load.soak) {                         /* short: half the card at the smallest visible area */
        int k = snprintf(s, n, "Memory: %ld errors (%ld passes), SPUs: %u errors", load.mem_errors, load.mem_passes, load.ls_errors);
        if (load.ls_ok < load.ls_runs && k < n) snprintf(s + k, n - k, " (%d of %d ran)", load.ls_ok, load.ls_runs);
        return soak_bad() ? RED : load.ls_ok < load.ls_runs ? YELLOW : WHITE;
    }
    if (!load.spus_ran) {
        snprintf(s, n, "SPUs not used: %s", spu.why);
        return spu.frozen ? RED : spu.declined ? GREY : YELLOW;
    }
    int k = snprintf(s, n, "SPUs: %d of %d", spu.started, SPU_KIT_SPUS);
    if (load.t1 > load.t0 && k < n) k += snprintf(s + k, n - k, ", about %d M loops/s each", spu_mean());
    if (spu.why[0] && k < n) {               /* a call after the run failed */
        snprintf(s + k, n - k, ", then %s", spu.why);
        return YELLOW;
    }
    return WHITE;
}

/* The last test left SPUs that could not be stopped: its result keeps the
 * red line, and no other load test runs on top of them. */
static int stuck_shown(void) { return load.done && spu.stuck; }

/* The temperatures of the last test; to: the text between each start and
 * maximum (" to ", " -> max "). No sample (SPUs that could not be stopped
 * before the load, CIRCLE at once): the start values only, not the -1
 * maxima. */
static void temps_line(char *s, int n, const char *to)
{
    if (load.n) snprintf(s, n, "Cell %d%s%d C, RSX %d%s%d C", load.cpu0, to, load.cpu_max, load.rsx0, to, load.rsx_max);
    else snprintf(s, n, "Cell %d C, RSX %d C at the start", load.cpu0, load.rsx0);
}

static const char *paste_word(void)
{
    return paste.verdict == PASTE_OK ? ", paste ok" : paste.verdict == PASTE_CHECK ? ", check paste" : paste.verdict == PASTE_LIKELY ? ", dry paste?" : "";
}

static void cool_tile(void)
{
    char l1[48], l2[64], tail[24];
    if (load.done) {
        if (load.n) snprintf(l1, sizeof l1, "Cell %d>%d, RSX %d>%d C", load.cpu0, load.cpu_max, load.rsx0, load.rsx_max);   /* short: line 1 of a tile */
        else snprintf(l1, sizeof l1, "Cell %d C, RSX %d C", load.cpu0, load.rsx0);
        if (spu.stuck) snprintf(tail, sizeof tail, ", SPUs stuck");
        else if (load.stop == STOP_HOT) snprintf(tail, sizeof tail, ", stop %d C", STOP_C);
        else if (load.stop == STOP_READ) snprintf(tail, sizeof tail, ", no reading");
        else if (load.stop == STOP_SPU) snprintf(tail, sizeof tail, ", SPU %d stopped", load.stop_spu);
        else if (load.soak) snprintf(tail, sizeof tail, ", %ld mem errors", load.mem_errors + (long)load.ls_errors);
        else if (load.spus_ran) snprintf(tail, sizeof tail, ", %d SPUs", SPU_KIT_SPUS);
        else snprintf(tail, sizeof tail, ", no SPUs");
        if (load.soak) snprintf(l2, sizeof l2, "soak %d:%02d%s", load.seconds / 60, load.seconds % 60, tail);
        else if (load.fan0 >= 0) snprintf(l2, sizeof l2, "fan %d>%d%%, %d:%02d load%s", load.fan0, load.fan_end, load.seconds / 60, load.seconds % 60, tail);
        else snprintf(l2, sizeof l2, "%d:%02d load test%s", load.seconds / 60, load.seconds % 60, tail);
        if (strlen(l2) + strlen(paste_word()) <= 44) strcat(l2, paste_word());   /* state.txt keeps 44 characters */
        state_set("cooling", verdict_dot(), l1, l2);
    } else if (cpu_now >= 0) {
        snprintf(l1, sizeof l1, "Cell %d C, RSX %d C idle", cpu_now, rsx_now);
        if (fan_pct >= 0) snprintf(l2, sizeof l2, "fan %d%%, no load test yet", fan_pct);
        else snprintf(l2, sizeof l2, "no load test yet");
        state_set("cooling", cpu_now >= 70 || rsx_now >= 70 ? DOT_WARN : DOT_OK, l1, l2);
    }
}

static void job_read(void) { read_now(); }

/* The fan's first-run prompt, the first time and again after SQUARE twice: -1 on exit. */
static int fan_prompt(void)
{
    if (journal_state("fan_policy") != J_NONE) return 0;
    int r = first_run_prompt("sys_sm_get_fan_policy (syscall 409)", "Reads the fan policy: the duty the system set, 0 to 100 %.",
                             "The read half of what webMAN uses for its fan control. Nothing is written.",
                             "Read-only. Without it the module shows temperatures only.");
    if (r < 0) return -1;
    if (r == 0) journal_skip("fan_policy");
    return 0;
}

/* A choice card for a button the controller draws as a label (R1): the
 * label in a pill, then the title and the description as option_card. */
static void label_card(float x, float y, float w, float h, const char *label, int on, const char *title_s, const char *desc)
{
    card(x, y, w, h);
    float lw = text_w(F_SMALL, label) + 24;
    round_rect(x + 52 - lw / 2, y + 38, lw, 32, 8, PANEL2);
    text_c(x + 52, y + 40, F_SMALL, WHITE, "%s", label);
    text_fit(x + 100, y + 34, F_MED, on ? WHITE : DIM, w - 128, "%s", title_s);
    text_wrap(x + 32, y + 110, F_BODY, on ? GREY : DIM, w - 64, (int)((h - 130) / wrap_step(F_BODY)), desc);
}

int mod_cool_open(void)
{
    if (!fs_ok) return 0;                    /* no journal, no system call: the home screen says why */
    if (fan_prompt() < 0) return -1;
    run_job("Reading the temperatures and the fan policy", job_read);   /* the journaled calls, with the counter */
    if (family == FAMILY_NONE) family_load();
    if (family == FAMILY_NONE && family_screen() < 0) { cool_tile(); return -1; }   /* asked once */
    while (1) {
        read_pad();
        int back = ui_leave();
        if (back <= 0) { cool_tile(); return back; }
        int can = cpu_now >= 0 && !stuck_shown();
        if ((pressed & (BTN_CROSS | BTN_TRIANGLE)) && can) {
            int seconds = (pressed & BTN_CROSS) ? 120 : 300;
            if (spu_ask(&spu_cool) < 0) { cool_tile(); return -1; }   /* the SPU prompt, once; a skip runs the load without them */
            load_screen(seconds);
        }
        if ((pressed & BTN_R1) && can && soak_choice() < 0) { cool_tile(); return -1; }
        if ((pressed & BTN_L1) && family_screen() < 0) { cool_tile(); return -1; }
        if (pressed & BTN_SQUARE) read_now();
        begin_frame();
        title();
        float w = SW - 2 * MG, y = TOP, x = MG + 32;
        card(MG, y, w, 176);
        heading(x, y + 22, "NOW");
        if (cpu_now >= 0) {
            text(x, y + 62, F_SMALL, GREY, "Cell");
            text(x, y + 88, F_LARGE, temp_color(cpu_now), "%d C", cpu_now);
            text(x + 250, y + 62, F_SMALL, GREY, "RSX");
            text(x + 250, y + 88, F_LARGE, temp_color(rsx_now), "%d C", rsx_now);
        } else text_wrap(x, y + 70, F_MED, YELLOW, 460, 2, "Temperatures not readable: no load test.");
        text(x + 500, y + 62, F_SMALL, GREY, "Fan");
        if (fan_pct >= 0) text(x + 500, y + 88, F_LARGE, WHITE, "%d%%", fan_pct);
        else text(x + 500, y + 100, F_BODY, journal_state("fan_policy") == J_FROZEN ? RED : GREY, "%s",
                  journal_state("fan_policy") == J_SKIPPED ? "read skipped" : journal_state("fan_policy") == J_FROZEN ? "read froze before"
                                                                                                          : "not read");
        {                                    /* the console family and the paste estimate of the last test */
            float px = x + 760, pw = w - 820;
            heading(px, y + 22, "PASTE ESTIMATE");
            float ex = text(px, y + 58, F_BODY, heat_family_get(family) ? WHITE : GREY, "%s", family_name());
            if (heat_family_get(family)) text_fit(ex + 16, y + 58, F_BODY, DIM, px + pw - ex - 16, "%s", heat_family_get(family)->models);
            const char *pt = load.done ? heat_paste_text(&paste) : heat_family_get(family) ? "After a load test: idle, peak, rise in the first minute and fan duty against this model's usual range."
                                                                                           : "L1 sets the console model: the estimate compares a load test with its usual range.";
            text_wrap(px, y + 100, F_SMALL, load.done && paste.verdict >= PASTE_CHECK ? YELLOW : GREY, pw, 2, pt);
        }
        y += 200;
        float ow = (w - 64) / 3;
        option_card(MG, y, ow, 220, BTN_CROSS, can, "Load test, 2 minutes", "PPU, SPUs and RSX at full load, a sample every 2 s on a curve.");
        option_card(MG + ow + 32, y, ow, 220, BTN_TRIANGLE, can, "Load test, 5 minutes", "Long enough for a steady temperature. It stops at " STOP_S ".");
        label_card(MG + 2 * (ow + 32), y, ow, 220, "R1", can, "Soak test, 30 or 60 min", "Load and memory turns: errors under heat. It stops at " STOP_S ".");
        y += 244;
        float h = BOTTOM - y;
        card(MG, y, w, h);
        heading(x, y + 22, load.done && load.soak ? "LAST RESULT: SOAK TEST" : "LAST RESULT");
        if (load.done) {
            float tw = w * 0.45f, vy = y + 146;
            char tl[64];
            temps_line(tl, sizeof tl, " to ");
            text_fit(x, y + 62, F_BODY, WHITE, tw, "%d:%02d%s: %s", load.seconds / 60, load.seconds % 60, load.cancelled || load.stop ? " (stopped)" : "", tl);
            char sl[112];
            u32 c = spu_line(sl, sizeof sl);
            if (load.soak) text_fit(x, y + 104, F_BODY, c, tw, "%s", sl);   /* a soak: the memory line second, it is the soak's point */
            else if (load.fan0 >= 0)
                text_fit(x, y + 104, F_BODY, WHITE, tw, "Fan %d%% to %d%%%s", load.fan0, load.fan_end, load.fan_end > load.fan0 ? ", it responded" : "");
            else text(x, y + 104, F_BODY, GREY, "Fan not read");
            if (!load.soak && (h >= 230 || spu.stuck)) {   /* the SPU line where it fits (not at the smallest visible area), or SPUs still up */
                text_fit(x, vy, F_BODY, c, tw, "%s", sl);
                vy += 42;
            }
            text_wrap(x, vy, F_BODY, verdict_dot() == DOT_OK ? GREEN : verdict_dot() == DOT_BAD ? RED : YELLOW, tw,
                      (int)((h - (vy - y) - 9) / wrap_step(F_BODY)), verdict_text());   /* n lines end 39n - 4 under vy: 13 px stay free */
            if (h >= 160) draw_graph(MG + w * 0.5f, y + 16, w * 0.5f - 16, h - 32, load.n, load.n * load.step);
        } else text(x, y + 62, F_BODY, GREY, "No load test in this session.");
        footer("CROSS 2 min  TRIANGLE 5 min  R1 soak  L1 console model  SQUARE read again  CIRCLE home  START exit");
        report_footnote();
        ui_flip();
    }
}

/* ---- the quick check, the compat text ----------------------------------- */

int cool_quick_ask(void) { return fan_prompt() < 0 ? -1 : 1; }

void cool_quick(void)
{
    progress("Reading the temperatures and the fan policy");
    read_now();
}

int cool_quick_end(char *line, int n)
{
    cool_tile();
    if (cpu_now < 0) {
        snprintf(line, n, "temperatures not readable (383 rc 0x%08x)", (unsigned)temps_rc);
        return DOT_WARN;
    }
    if (fan_pct >= 0) snprintf(line, n, "Cell %d C, RSX %d C, fan %d%% at rest", cpu_now, rsx_now, fan_pct);
    else snprintf(line, n, "Cell %d C, RSX %d C at rest, fan not read", cpu_now, rsx_now);
    return cpu_now >= 70 || rsx_now >= 70 ? DOT_WARN : DOT_OK;
}

int cool_compat(char *out, int n)
{
    if (!load.done) return 0;
    char rise[16] = "?";
    int rc = heat_rise(load.cpu, load.n, load.step), rr = heat_rise(load.rsx, load.n, load.step);
    if (rc >= 0 || rr >= 0) snprintf(rise, sizeof rise, "%d", rc > rr ? rc : rr);
    return snprintf(out, n, "%s; idle Cell %d RSX %d C; peak Cell %d RSX %d C; rise %s C/min; fan %d>%d%%; %s %d s%s, %s", family_name(), load.cpu0,
                    load.rsx0, load.cpu_max, load.rsx_max, rise, load.fan0, load.fan_end, load.soak ? "soak" : "load", load.seconds,
                    load.cancelled || load.stop ? " (stopped)" : "", load.spus_ran ? "6 SPUs" : "no SPUs");
}

/* ---- report ----------------------------------------------------------------- */

/* The SPU leg in the report: the rate of each SPU, or why it was not used,
 * then the group's end (the terminate, the join). */
static void spu_report(void)
{
    const spu_result *r = &spu;
    if (load.spus_ran) {
        rep_out("SPU leg: %d of %d", r->started, SPU_KIT_SPUS);
        if (load.t1 > load.t0) {
            rep_out(", M loops/s per SPU");
            for (int i = 0; i < SPU_KIT_SPUS; i++) rep_out(" %.1f", spu_rate(i));
        } else rep_out(", rate not measured (under two samples)");
    } else rep_out("SPU leg: not used, %s", r->why);
    if (r->ran) {
        if (!load.spus_ran) rep_out(", SPUs started %d of %d", r->started, SPU_KIT_SPUS);
        if (r->terminated) rep_out(", terminate rc 0x%08x", (unsigned)r->term_rc);
        if (r->joined) rep_out(", join cause %u status 0x%x", (unsigned)r->cause, (unsigned)r->status);
        if (load.spus_ran && r->why[0]) rep_out(", %s", r->why);
        rep_out(", %llu ms in all", (unsigned long long)(r->us / 1000));
    }
    rep_out("\n");
}

void mod_cool_report(void)
{
    if (!read_once) return;
    rep_out("--- cooling ---\n");
    if (cpu_now >= 0) rep_out("now: Cell %d C, RSX %d C (383 rc 0x%08x)", cpu_now, rsx_now, (unsigned)temps_rc);
    else rep_out("now: temperatures not read (383 rc 0x%08x)", (unsigned)temps_rc);
    if (fan_pct >= 0) rep_out(", fan %d%% (policy mode %d)", fan_pct, fan_mode);
    else rep_out(", fan not read (409 rc 0x%08x)", (unsigned)fan_rc);
    rep_out("\n");
    const heat_family *hf = heat_family_get(family);
    rep_out("console model: %s%s%s\n", family_name(), hf ? ", " : "", hf ? hf->models : "");
    if (!load.done) return;
    char tl[64];
    temps_line(tl, sizeof tl, " -> max ");
    if (load.soak) rep_out("soak test %d min, %d s%s: %s, fan %d -> %d%%\n", load.soak, load.seconds, load.cancelled || load.stop ? " (stopped)" : "", tl,
                           load.fan0, load.fan_end);
    else rep_out("load test %d s%s: %s, fan %d -> %d%%\n", load.seconds, load.cancelled || load.stop ? " (stopped)" : "", tl, load.fan0, load.fan_end);
    spu_report();
    if (load.soak) {
        rep_out("soak: %d turns of %d s load and %d s memory; memory %ld passes over %d MB, %ld errors", load.turns, SOAK_LOAD_S, SOAK_MEM_S,
                load.mem_passes, load.mem_blocks, load.mem_errors);
        if (load.mem_errors) rep_out(" (the first at 0x%08lx)", load.mem_first);
        rep_out("; SPU local stores %d of %d tests ended, %u errors%s%s\n", load.ls_ok, load.ls_runs, load.ls_errors, load.ls_why[0] ? ", last: " : "",
                load.ls_why);
    }
    if (load.stop == STOP_HOT)
        rep_out("stop: reached the %d C limit at %d:%02d (Cell %d C, RSX %d C)\n", STOP_C, load.stop_s / 60, load.stop_s % 60, load.stop_cpu,
                load.stop_rsx);
    else if (load.stop == STOP_READ) rep_out("stop: two failed temperature reads in a row, at %d:%02d\n", load.stop_s / 60, load.stop_s % 60);
    else if (load.stop == STOP_SPU) rep_out("stop: SPU %d stopped reporting, at %d:%02d\n", load.stop_spu, load.stop_s / 60, load.stop_s % 60);
    rep_out("verdict: %s\n", verdict_text());
    rep_out("paste: %s (idle %d C, peak %d C, rise %d C/min, fan %d%%; over the range:%s%s%s%s%s)\n", heat_paste_text(&paste), paste.idle, paste.peak,
            paste.rise, paste.fan, paste.over & 1 ? " idle" : "", paste.over & 2 ? " peak" : "", paste.over & 4 ? " rise" : "",
            paste.over & 8 ? " fan" : "", paste.over ? "" : " none");
    rep_out("samples (every %d s): t Cell RSX fan\n", load.step);
    int every = load.step >= 10 ? 1 : 10 / load.step;   /* about one line per 10 s */
    for (int k = 0; k < load.n; k += every)
        rep_out("  %4d %3d %3d %3d\n", k * load.step, load.cpu[k], load.rsx[k], load.fan[k]);   /* -1 = not read */
}
