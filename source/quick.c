/* The quick check, R1 on the home screen: the short tests that need no
 * answer, in one run. A SMART read of the drive (no self-test), the
 * temperatures and the fan duty, one pass of each memory test (the XDR, the
 * RSX memory, the SPU local stores), the network data and five pings to the
 * router. Each step is the module's own code, with its own journal entries,
 * and sets the module's tile as a normal run does. The steps run in one
 * thread while this screen draws them and a counter; CIRCLE stops after the
 * step that runs. One verdict screen at the end. */
#include <stdio.h>
#include <string.h>
#include <ppu-lv2.h>
#include <lv2/systime.h>
#include <sys/thread.h>
#include "app.h"
#include <ps3gfx/ui.h>
#include "report.h"

#define STEPS 4

static const struct {
    const char *name, *what;
    void (*run)(void);
    int (*end)(char *line, int n);
} steps[STEPS] = {
    {"Drive", "A SMART read: the attributes and the health line. No self-test.", drive_quick, drive_quick_end},
    {"Cooling", "The Cell and RSX temperatures and the fan duty, at rest.", cool_quick, cool_quick_end},
    {"Memory", "One pattern over the XDR and the RSX memory, then the SPU local stores.", mem_quick, mem_quick_end},
    {"Network", "The connection data and five pings to the router.", network_quick, network_quick_end},
};

enum { S_WAIT, S_RUN, S_DONE, S_SKIP };
static volatile int state[STEPS];
static volatile int step_now = -1, all_done;
static int dot[STEPS];
static char result[STEPS][160];

static void quick_thread(void *arg)
{
    (void)arg;
    for (int k = 0; k < STEPS; k++) {
        if (job_cancel) { state[k] = S_SKIP; continue; }
        step_now = k;
        state[k] = S_RUN;
        job_set_percent(-1);
        steps[k].run();
        state[k] = S_DONE;
    }
    __sync_synchronize();
    all_done = 1;
    sysThreadExit(0);
}

static u32 dot_color(int d) { return d == DOT_OK ? GREEN : d == DOT_WARN ? YELLOW : d == DOT_BAD ? RED : GREY; }

/* The four steps as rows: a dot or the spinner, the name, what it does or its result. */
static void draw_steps(float y, double sec, int results)
{
    float w = SW - 2 * MG, rh = (BOTTOM - y - 8) / STEPS;
    if (rh > 120) rh = 120;
    for (int k = 0; k < STEPS; k++, y += rh) {
        card(MG, y, w, rh - 16);
        float cy = y + (rh - 16) / 2;
        if (state[k] == S_RUN) spinner(MG + 44, cy, 18, sec);
        else if (state[k] == S_DONE && results) disc(MG + 44, cy, 13, dot_color(dot[k]));
        else if (state[k] == S_DONE) disc(MG + 44, cy, 9, BLUE);
        else ring(MG + 44, cy, 11, 3, DIM);
        text(MG + 88, cy - line_h(F_MED) / 2, F_MED, state[k] == S_WAIT ? GREY : WHITE, "%s", steps[k].name);
        const char *s = results && state[k] == S_DONE ? result[k] : state[k] == S_SKIP ? "Not run: the check was stopped." : steps[k].what;
        text_fit(MG + 330, cy - line_h(F_BODY) / 2, F_BODY, results && state[k] == S_DONE ? WHITE : GREY, w - 370, "%s", s);
    }
}

/* Before anything runs: what the check does. 1 start, 0 back, -1 exit. */
static int intro(void)
{
    for (int k = 0; k < STEPS; k++) state[k] = S_WAIT;
    while (1) {
        read_pad();
        if (quit || (pressed & BTN_START)) return -1;
        if (pressed & BTN_CIRCLE) return 0;
        if (pressed & BTN_CROSS) return 1;
        begin_frame();
        title();
        text(MG, TOP, F_MED, WHITE, "Quick check");
        text_fit(MG, TOP + 52, F_BODY, GREY, SW - 2 * MG, "%s", "The short tests that need no answer, in one run: a few minutes, most of it the RSX memory.");
        draw_steps(TOP + 110, 0, 0);
        status_line("Each step writes to the journal before it runs, as in its module. The modules have the long tests.", GREY);
        footer("CROSS start  CIRCLE home  START exit");
        ui_flip();
    }
}

/* The steps in their thread while this one draws; CIRCLE stops after the
 * step that runs. No thread: the steps run here, with one frame first. */
static void run_steps(void)
{
    sys_ppu_thread_t t;
    u64 ret;
    job_cancel = 0;
    all_done = 0;
    step_now = -1;
    s64 t0 = sysGetSystemTime();
    if (sysThreadCreate(&t, quick_thread, NULL, 1500, 0x20000, THREAD_JOINABLE, "quick") != 0) {
        begin_frame();
        title();
        text(MG, TOP, F_MED, WHITE, "Quick check ...");
        ui_flip();
        for (int k = 0; k < STEPS; k++) { state[k] = S_RUN; steps[k].run(); state[k] = S_DONE; }
        return;
    }
    while (!all_done) {
        read_pad();
        if (quit || (pressed & BTN_CIRCLE)) job_cancel = 1;
        double sec = (double)(sysGetSystemTime() - t0) / 1e6;
        begin_frame();
        title();
        float x = text(MG, TOP, F_MED, WHITE, "Quick check, step %d of %d", step_now + 1 > 0 ? step_now + 1 : 1, STEPS);
        text(x + 32, TOP, F_MED | TNUM, GREY, "%.1f s", sec);
        text_fit(MG, TOP + 52, F_BODY, GREY, SW - 2 * MG, "%s", "The counter runs while the console works. If it stops, the console froze: the next start skips that step.");
        draw_steps(TOP + 110, sec, 0);
        footer(job_cancel ? "Stopping after this step ..." : "CIRCLE stop after this step");
        ui_flip();
    }
    sysThreadJoin(t, &ret);
    pressed = 0;                             /* a button still down is not a new press */
}

/* The results: a row per step and the verdict. 0 home, -1 exit. */
static int verdict(void)
{
    char bad[96] = "", warn[96] = "", msg[200];
    int worst = DOT_OK;
    for (int k = 0; k < STEPS; k++) {
        if (state[k] != S_DONE) { dot[k] = DOT_NONE; continue; }
        dot[k] = steps[k].end(result[k], sizeof result[k]);
        char *list = dot[k] == DOT_BAD ? bad : dot[k] == DOT_WARN ? warn : NULL;
        if (list) snprintf(list + strlen(list), sizeof bad - strlen(list), "%s%s", list[0] ? ", " : "", steps[k].name);
        if (dot[k] > worst) worst = dot[k];
    }
    write_report();                          /* every step's result reaches the file */
    if (bad[0]) snprintf(msg, sizeof msg, "Problem found: %s. Open the module for the details and the long tests.", bad);
    else if (warn[0]) snprintf(msg, sizeof msg, "Check: %s. Open the module for the details and the long tests.", warn);
    else if (job_cancel) snprintf(msg, sizeof msg, "Stopped: no problem in the steps that ran.");
    else snprintf(msg, sizeof msg, "No problem found in the quick check. The modules have the long tests.");
    while (1) {
        read_pad();
        if (quit || (pressed & BTN_START)) return -1;
        if (pressed & (BTN_CIRCLE | BTN_CROSS)) { pressed = 0; return 0; }
        begin_frame();
        title();
        text(MG, TOP, F_MED, WHITE, "Quick check: result");
        draw_steps(TOP + 110 - 52, 0, 1);
        status_line(msg, bad[0] ? RED : warn[0] ? YELLOW : job_cancel ? GREY : GREEN);
        footer("CIRCLE home  START exit");
        report_footnote();
        ui_flip();
    }
}

int quick_screen(void)
{
    ui_module = "Quick check";
    int r = intro();
    if (r > 0 && (cool_quick_ask() < 0 || mem_quick_ask() < 0)) r = -1;   /* the first-run prompts, before the run */
    if (r > 0) {
        run_steps();
        r = verdict();
    }
    ui_module = NULL;
    armed = 0;
    pressed = 0;                             /* the press that leaves acts nowhere else */
    return r;
}
