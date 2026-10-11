/* The Cooling module: the Cell and RSX temperatures (syscall 383), the fan
 * duty (syscall 409, read-only, behind the first-run prompt), and a timed load
 * with a temperature curve: two PPU threads, the six SPUs (the SPU kit's heat
 * mode through spu_run.c, behind the SPU prompt) and the RSX. The test stops
 * by itself at 75 C, after two failed temperature reads in a row, or when an
 * SPU stops reporting. Nothing is written to the fan: the system keeps its
 * own policy. */
#include <stdio.h>
#include <string.h>
#include <ppu-lv2.h>
#include <lv2/systime.h>
#include <sys/thread.h>
#include "app.h"
#include <ps3gfx/ui.h>
#include "report.h"

#define MAX_SAMPLES 400                      /* every 2 s: 13 min */
#define STOP_C 75                            /* a load test stops when the Cell or the RSX reaches it */
#define STOP_S "75 C"                        /* STOP_C in the texts */
#define WARM_C 72                            /* the verdict: warm from here */

static int cpu_now = -1, rsx_now = -1, temps_rc, read_once;   /* read_once: the module read once, so the report has its section and the rc */
static int fan_pct = -1, fan_mode = -1, fan_rc;
enum { STOP_NONE, STOP_HOT, STOP_READ, STOP_SPU };   /* why a load test stopped by itself */
static struct {
    int done, n, seconds, cancelled;
    int cpu0, rsx0, fan0, cpu_max, rsx_max, fan_end;
    int stop, stop_s, stop_cpu, stop_rsx, stop_spu;   /* STOP_*, its second; the readings of a STOP_HOT; the SPU of a STOP_SPU */
    int spus;                                /* the six SPUs ran the heat load (spu_up returned 1) */
    u64 c0[SPU_KIT_SPUS], c1[SPU_KIT_SPUS];  /* their counts at the first and the last sample */
    s64 t0, t1;                              /* the times of those samples */
    short cpu[MAX_SAMPLES], rsx[MAX_SAMPLES], fan[MAX_SAMPLES];   /* -1 = not read */
} load;
static spu_result spu;                       /* the SPU leg of the last load test */

static void read_now(void)
{
    temps_rc = console_temps(&cpu_now, &rsx_now);
    read_once = 1;
    fan_rc = fan_policy_read(&fan_pct, &fan_mode);     /* -1 and no value when skipped or frozen */
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
 * that stops. */
static void job_spu_up(void) { load.spus = spu_up(&spu_cool, SPU_KIT_HEAT, &spu); }
static void job_spu_down(void) { spu_down(&spu); }

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

/* A sample every 2 s, then the stops, the hot one first: the Cell or the RSX
 * at 75 C, two failed reads in a row, an SPU whose count did not move since
 * the last sample (heat reports every 2^20 loops, hundreds of times a
 * second). The first sample only keeps the counts. A sample that catches up
 * after a slow frame, under 1 s after the last count, skips the counts. 1
 * when the test stops. */
static int sample(s64 now, int *misses)
{
    read_now();
    load.cpu[load.n] = (short)cpu_now;
    load.rsx[load.n] = (short)rsx_now;
    load.fan[load.n] = (short)fan_pct;
    int first = load.n++ == 0, still = -1;
    *misses = cpu_now < 0 || rsx_now < 0 ? *misses + 1 : 0;
    if (load.spus && (first || now - load.t1 >= 1000000)) {
        const volatile spu_kit_rec *rp = spu_reports();
        for (int i = 0; i < SPU_KIT_SPUS; i++) {
            u64 c = rp[i].count;
            if (first) load.c0[i] = c;
            else if (c == load.c1[i] && still < 0) still = i;
            load.c1[i] = c;
        }
        if (first) load.t0 = now;
        load.t1 = now;
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
    for (int t = 0; t <= span; t += span > 180 ? 60 : 30)
        text_c(gx + gw * t / span, gy + gh + 8, F_SMALL, DIM, "%d:%02d", t / 60, t % 60);
    float sx = gw / (span / 2.0f);
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

static void load_screen(int seconds)
{
    sys_ppu_thread_t th[2];
    int ok[2] = {0, 0}, misses = 0;
    u64 ret;
    memset(&load, 0, sizeof load);
    load.seconds = seconds;
    read_now();
    load.cpu0 = cpu_now;
    load.rsx0 = rsx_now;
    load.fan0 = fan_pct;
    if (spu_blocked(&spu_cool)) spu_up(&spu_cool, SPU_KIT_HEAT, &spu);   /* no call: only the reason, no job screen */
    else run_job("Starting the six SPUs", job_spu_up);
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
        float rx = SW - MG;
        if (fan_pct >= 0) rx = text_r(rx, TOP, F_MED | TNUM, WHITE, "fan %d%%", fan_pct) - 40;
        rx = text_r(rx, TOP, F_MED | TNUM, temp_color(rsx_now), "RSX %d C", rsx_now) - 40;
        text_r(rx, TOP, F_MED | TNUM, temp_color(cpu_now), "Cell %d C", cpu_now);
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
    s64 t_end = sysGetSystemTime();
    load_on = 0;
    for (int k = 0; k < 2; k++) if (ok[k]) sysThreadJoin(th[k], &ret);
    if (load.spus) run_job("Stopping the six SPUs", job_spu_down);
    load.cpu_max = load.rsx_max = -1;
    for (int k = 0; k < load.n; k++) {
        if (load.cpu[k] > load.cpu_max) load.cpu_max = load.cpu[k];
        if (load.rsx[k] > load.rsx_max) load.rsx_max = load.rsx[k];
    }
    load.fan_end = fan_pct;
    load.seconds = (int)((t_end - t0) / 1000000);
    load.done = 1;
    pressed = 0;                             /* a button still down is not a new press */
    write_report();
}

/* A test that stopped by itself (75 C, no readings, an SPU), SPUs that could
 * not be stopped, no readings, a short test and 72 C or more give a yellow
 * verdict. The test stops at 75 C, so no verdict is red. */
static int verdict_dot(void)
{
    int m = load.cpu_max > load.rsx_max ? load.cpu_max : load.rsx_max;
    return load.stop || spu.stuck || m < 0 || load.n < 10 || m >= WARM_C ? DOT_WARN : DOT_OK;
}

static const char *verdict_text(void)
{
    int m = load.cpu_max > load.rsx_max ? load.cpu_max : load.rsx_max;
    if (spu.stuck && !spu.ran) return "No verdict: the SPUs of an earlier test could not be stopped.";   /* spu_up made no call: the Memory module's group */
    if (spu.stuck && !load.spus) return "No verdict: the SPUs could not be stopped at their start.";   /* stuck in spu_up: the test did not run */
    if (spu.stuck) return "No verdict: the SPUs could not be stopped after the test.";
    if (load.stop == STOP_HOT) return "Warm: the test reached the app's limit of " STOP_S " and stopped before its end. Clean the dust and check the fan.";
    if (load.stop == STOP_READ) return "Stopped after two failed temperature reads in a row: no verdict.";
    if (load.stop == STOP_SPU) {
        static char t[64];
        snprintf(t, sizeof t, "Stopped because SPU %d stopped reporting: no verdict.", load.stop_spu);
        return t;
    }
    if (m < 0) return "No temperature readings during the test: no verdict.";
    if (load.n < 10) return "Too short for a verdict: run at least two minutes.";
    if (m >= WARM_C) return "Warm under load. The console copes, but dust or dry paste would push it further: clean it.";
    return "Within the usual range under load.";
}

/* The SPU leg of the last test in one line, and its colour. */
static u32 spu_line(char *s, int n)
{
    if (spu.stuck) {
        snprintf(s, n, "The SPUs did not stop: quit the app with START.");   /* one line at the smallest visible area */
        return RED;
    }
    if (!load.spus) {
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
 * maximum (" to ", ">", " -> max "). No sample (SPUs that could not be
 * stopped before the load, CIRCLE at once): the start values only, not the
 * -1 maxima. */
static void temps_line(char *s, int n, const char *to)
{
    if (load.n) snprintf(s, n, "Cell %d%s%d C, RSX %d%s%d C", load.cpu0, to, load.cpu_max, load.rsx0, to, load.rsx_max);
    else snprintf(s, n, "Cell %d C, RSX %d C at the start", load.cpu0, load.rsx0);
}

static void cool_tile(void)
{
    char l1[48], l2[48], tail[24];
    if (load.done) {
        temps_line(l1, sizeof l1, ">");
        if (spu.stuck) snprintf(tail, sizeof tail, ", SPUs stuck");
        else if (load.stop == STOP_HOT) snprintf(tail, sizeof tail, ", stop %d C", STOP_C);
        else if (load.stop == STOP_READ) snprintf(tail, sizeof tail, ", no reading");
        else if (load.stop == STOP_SPU) snprintf(tail, sizeof tail, ", SPU %d stopped", load.stop_spu);
        else if (load.spus) snprintf(tail, sizeof tail, ", %d SPUs", SPU_KIT_SPUS);
        else snprintf(tail, sizeof tail, ", no SPUs");
        if (load.fan0 >= 0) snprintf(l2, sizeof l2, "fan %d>%d%%, %d:%02d load%s", load.fan0, load.fan_end, load.seconds / 60, load.seconds % 60, tail);
        else snprintf(l2, sizeof l2, "%d:%02d load test%s", load.seconds / 60, load.seconds % 60, tail);
        state_set("cooling", verdict_dot(), l1, l2);
    } else if (cpu_now >= 0) {
        snprintf(l1, sizeof l1, "Cell %d C, RSX %d C idle", cpu_now, rsx_now);
        if (fan_pct >= 0) snprintf(l2, sizeof l2, "fan %d%%, no load test yet", fan_pct);
        else snprintf(l2, sizeof l2, "no load test yet");
        state_set("cooling", cpu_now >= 70 || rsx_now >= 70 ? DOT_WARN : DOT_OK, l1, l2);
    }
}

static void job_read(void) { read_now(); }

int mod_cool_open(void)
{
    if (!fs_ok) return 0;                    /* no journal, no system call: the home screen says why */
    if (journal_state("fan_policy") == J_NONE) {   /* the first time, and again after SQUARE twice */
        int r = first_run_prompt("sys_sm_get_fan_policy (syscall 409)", "Reads the fan policy: the duty the system set, 0 to 100 %.",
                                 "The read half of what webMAN uses for its fan control. Nothing is written.",
                                 "Read-only. Without it the module shows temperatures only.");
        if (r < 0) return -1;
        if (r == 0) journal_skip("fan_policy");
    }
    run_job("Reading the temperatures and the fan policy", job_read);   /* the journaled calls, with the counter */
    while (1) {
        read_pad();
        int back = ui_leave();
        if (back <= 0) { cool_tile(); return back; }
        if ((pressed & (BTN_CROSS | BTN_TRIANGLE)) && cpu_now >= 0 && !stuck_shown()) {
            int seconds = (pressed & BTN_CROSS) ? 120 : 300;
            if (spu_ask(&spu_cool) < 0) { cool_tile(); return -1; }   /* the SPU prompt, once; a skip runs the load without them */
            load_screen(seconds);
        }
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
        if (w > 1180)
            text_wrap(x + 760, y + 66, F_BODY, GREY, w - 820, 3,
                      cpu_now >= 0 ? "Idle values under 60 C are usual for a PS3 with clean cooling. SQUARE reads again."
                                   : "Syscall 383 did not answer; the report has its return code.");
        y += 200;
        float ow = (w - 32) / 2;
        option_card(MG, y, ow, 220, BTN_CROSS, cpu_now >= 0 && !stuck_shown(), "Load test, 2 minutes",
                    "Two PPU threads, the six SPUs and the RSX fill rate at full load, the temperatures every 2 s on a curve.");
        option_card(MG + ow + 32, y, ow, 220, BTN_TRIANGLE, cpu_now >= 0 && !stuck_shown(), "Load test, 5 minutes",
                    "Long enough for a steady temperature. The test stops by itself at " STOP_S ".");
        y += 244;
        float h = BOTTOM - y;
        card(MG, y, w, h);
        heading(x, y + 22, "LAST RESULT");
        if (load.done) {
            float tw = w * 0.45f, vy = y + 146;
            char tl[64];
            temps_line(tl, sizeof tl, " to ");
            text_fit(x, y + 62, F_BODY, WHITE, tw, "%d:%02d%s: %s", load.seconds / 60, load.seconds % 60, load.cancelled || load.stop ? " (stopped)" : "", tl);
            if (load.fan0 >= 0)
                text_fit(x, y + 104, F_BODY, WHITE, tw, "Fan %d%% to %d%%%s", load.fan0, load.fan_end, load.fan_end > load.fan0 ? ", it responded" : "");
            else text(x, y + 104, F_BODY, GREY, "Fan not read");
            if (h >= 230 || spu.stuck) {         /* the SPU line where it fits (not at the smallest visible area), or SPUs still up */
                char sl[112];
                u32 c = spu_line(sl, sizeof sl);
                text_fit(x, vy, F_BODY, c, tw, "%s", sl);
                vy += 42;
            }
            text_wrap(x, vy, F_BODY, verdict_dot() == DOT_OK ? GREEN : YELLOW, tw,
                      (int)((h - (vy - y) - 9) / wrap_step(F_BODY)), verdict_text());   /* n lines end 39n - 4 under vy: 13 px stay free */
            if (h >= 160) draw_graph(MG + w * 0.5f, y + 16, w * 0.5f - 16, h - 32, load.n, load.n * 2);
        } else text(x, y + 62, F_BODY, GREY, "No load test in this session.");
        footer("CROSS 2 min load  TRIANGLE 5 min load  SQUARE read again  CIRCLE home  START exit");
        report_footnote();
        ui_flip();
    }
}

/* The SPU leg in the report: the rate of each SPU, or why it was not used,
 * then the group's end (the terminate, the join). */
static void spu_report(void)
{
    const spu_result *r = &spu;
    if (load.spus) {
        rep_out("SPU leg: %d of %d", r->started, SPU_KIT_SPUS);
        if (load.t1 > load.t0) {
            rep_out(", M loops/s per SPU");
            for (int i = 0; i < SPU_KIT_SPUS; i++) rep_out(" %.1f", spu_rate(i));
        } else rep_out(", rate not measured (under two samples)");
    } else rep_out("SPU leg: not used, %s", r->why);
    if (r->ran) {
        if (!load.spus) rep_out(", SPUs started %d of %d", r->started, SPU_KIT_SPUS);
        if (r->terminated) rep_out(", terminate rc 0x%08x", (unsigned)r->term_rc);
        if (r->joined) rep_out(", join cause %u status 0x%x", (unsigned)r->cause, (unsigned)r->status);
        if (load.spus && r->why[0]) rep_out(", %s", r->why);
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
    if (!load.done) return;
    char tl[64];
    temps_line(tl, sizeof tl, " -> max ");
    rep_out("load test %d s%s: %s, fan %d -> %d%%\n", load.seconds, load.cancelled || load.stop ? " (stopped)" : "", tl, load.fan0, load.fan_end);
    spu_report();
    if (load.stop == STOP_HOT)
        rep_out("stop: reached the %d C limit at %d:%02d (Cell %d C, RSX %d C)\n", STOP_C, load.stop_s / 60, load.stop_s % 60, load.stop_cpu,
                load.stop_rsx);
    else if (load.stop == STOP_READ) rep_out("stop: two failed temperature reads in a row, at %d:%02d\n", load.stop_s / 60, load.stop_s % 60);
    else if (load.stop == STOP_SPU) rep_out("stop: SPU %d stopped reporting, at %d:%02d\n", load.stop_spu, load.stop_s / 60, load.stop_s % 60);
    rep_out("verdict: %s\n", verdict_text());
    rep_out("samples (every 2 s): t Cell RSX fan\n");
    for (int k = 0; k < load.n; k += 5)
        rep_out("  %4d %3d %3d %3d\n", k * 2, load.cpu[k], load.rsx[k], load.fan[k]);   /* -1 = not read */
}
