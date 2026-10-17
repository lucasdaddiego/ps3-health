/* The Audio module: tones that the user answers. A beep on the left speaker,
 * then on the right; a walk over the eight channels of a 7.1 port, one
 * speaker at a time; and a sweep from 20 Hz to 20 kHz. Every sound plays at
 * -18 dBFS with a fade at each end (tone.c), and after each one the user
 * answers CROSS heard or CIRCLE not heard. The app opens one audio port of
 * the system's audio library (libaudio, an SPRX of the firmware): eight
 * channels when the port opens with eight, else two. The start (the module
 * load, the library, the port) and the end are one journaled call each. A
 * thread keeps the port's ring of 16 blocks filled about 43 ms ahead of the
 * block the system reads. */
#include <stdio.h>
#include <string.h>
#include <ppu-lv2.h>
#include <lv2/systime.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <audio/audio.h>
#include <sysmodule/sysmodule.h>
#include "app.h"
#include <ps3gfx/ui.h>
#include "report.h"
#include "tone.h"

#define LEVEL_DB -18                         /* every sound's peak */
#define BEEP_HZ 440
#define LFE_HZ 50                            /* the subwoofer channel: a receiver filters it under about 120 Hz */
#define SWEEP_S 12
#define BLOCKS AUDIO_BLOCK_16
#define AHEAD 8                              /* blocks written ahead of the read: 8 x 256 samples, about 43 ms */
#define CUT_MS 40                            /* the fade out of the sound that plays when the port ends */
#define CH_MAX 8

/* The eight channels of the port, in the order of the system's 8-channel
 * layout: L, R, C, LFE, then the surround and the back pairs. */
static const char *const spk_name[CH_MAX] = {"Front left", "Front right", "Centre", "Subwoofer (LFE)",
                                             "Surround left", "Surround right", "Back left", "Back right"};
static const char *const spk_short[CH_MAX] = {"FL", "FR", "C", "LFE", "SL", "SR", "BL", "BR"};
static const float spk_x[CH_MAX] = {-0.62f, 0.62f, 0, 0.36f, -0.95f, 0.95f, -0.5f, 0.5f};   /* the layout on screen, -1 to 1 */
static const float spk_y[CH_MAX] = {-0.72f, -0.72f, -0.86f, -0.5f, 0.12f, 0.12f, 0.86f, 0.86f};

enum { ANS_NONE, ANS_HEARD, ANS_NOT };
static int lr[2], walk[CH_MAX], sweep_ans;   /* the answers */
static int lr_ran, walk_ran, sweep_ran;      /* a test ran: its answers, some may be missing after TRIANGLE */

/* ---- the port --------------------------------------------------------------- */

static int a_mod, a_init, a_open, a_started, a_frozen;   /* the steps that succeeded: audio_end undoes only these; the start froze before */
static s32 a_rc, a_rc8;                      /* the start's rc; the 8-channel open's rc (0: it opened) */
static int a_ch;                             /* the port's channels: 0 before a start, 2 or 8 */
static u32 port;
static audioPortConfig cfg;

static volatile int fill_on;                 /* the fill thread runs */
static sys_ppu_thread_t fill_th;
static int fill_ok;                          /* the thread was created */
static tone cur, next;                       /* cur: the fill thread's own; next: the UI thread's, handed over by the counts below */
static volatile int next_seq;                /* the sounds played; the fill thread takes next when it changes */
static volatile int stop_upto;               /* the sounds up to this one fade out */
static volatile int now_done = 1;            /* the sound ended (the fill thread's tone_done) */
static volatile float now_hz;                /* its frequency now */
static volatile int quiet;                   /* the port ends: the fill thread cuts the sound (CUT_MS) and starts no other */

static float *block_at(u32 b) { return (float *)(uintptr_t)cfg.audioDataStart + b * (u32)a_ch * AUDIO_BLOCK_SAMPLES; }

/* Fills the blocks from the last written one up to AHEAD past the block the
 * system reads. The write is never more than AHEAD ahead, so a distance
 * over AHEAD (or 0) means the read passed the write (the thread stalled for
 * AHEAD blocks or more): the write starts again after the read. At each
 * block: a stop fades the sound out, and a new sound starts once the last
 * one ended, so an answer never cuts a beep (a click). */
static void fill_thread(void *arg)
{
    (void)arg;
    u32 wr = 1;
    int cur_seq = next_seq, cur_stopped = 0, cur_cut = 0;
    while (fill_on) {
        u32 rd = (u32)(*(volatile u64 *)(uintptr_t)cfg.readIndex % BLOCKS);
        u32 dist = (wr + BLOCKS - rd) % BLOCKS;
        if (dist == 0 || dist > AHEAD) wr = (rd + 1) % BLOCKS, dist = 1;
        for (; dist < AHEAD; dist++, wr = (wr + 1) % BLOCKS) {
            if (quiet && !cur_cut) { tone_cut(&cur, TONE_RATE * CUT_MS / 1000); cur_cut = 1; }
            if (!cur_stopped && stop_upto - cur_seq >= 0) { tone_stop(&cur); cur_stopped = 1; }
            if (!quiet && cur_seq != next_seq && tone_done(&cur)) {
                __sync_synchronize();
                cur = next;
                cur_seq = next_seq;
                cur_stopped = 0;
            }
            tone_fill(&cur, block_at(wr), AUDIO_BLOCK_SAMPLES);
            now_hz = (float)tone_freq(&cur);
            now_done = (quiet || cur_seq == next_seq) && tone_done(&cur);
        }
        sysUsleep(2000);
    }
    sysThreadExit(0);
}

static void play(const tone *t)
{
    next = *t;
    now_done = 0;
    __sync_synchronize();
    next_seq++;
}

static void hush(void) { stop_upto = next_seq; }   /* the fade out of every sound played so far: 20 ms for a beep */

/* The start, in a job: the audio module, the library, the port (eight
 * channels, else two), its ring zeroed, the start. One journaled call:
 * a start that froze the console is never made again. Each step runs once
 * it succeeded, as the network's start. */
static void job_up(void)
{
    if (!journal_start("audio_start")) { a_frozen = 1; return; }
    s32 rc = 0;
    if (!a_mod) { rc = sysModuleLoad(SYSMODULE_AUDIO); a_mod = rc == 0; }
    if (rc == 0 && !a_init) { rc = audioInit(); a_init = rc == 0; }
    if (rc == 0 && !a_open) {
        audioPortParam pp;
        memset(&pp, 0, sizeof pp);
        pp.numChannels = AUDIO_PORT_8CH;
        pp.numBlocks = BLOCKS;
        pp.attrib = AUDIO_PORT_INITLEVEL;
        pp.level = 1.0f;                     /* the samples hold the level */
        a_rc8 = rc = audioPortOpen(&pp, &port);
        if (rc) {
            pp.numChannels = AUDIO_PORT_2CH;
            rc = audioPortOpen(&pp, &port);
        }
        if (rc == 0) { a_open = 1; a_ch = (int)pp.numChannels; }
    }
    if (rc == 0 && !a_started) {
        rc = audioGetPortConfig(port, &cfg);
        if (rc == 0) {
            memset((void *)(uintptr_t)cfg.audioDataStart, 0, (size_t)BLOCKS * (size_t)a_ch * AUDIO_BLOCK_SAMPLES * sizeof(float));
            rc = audioPortStart(port);
            a_started = rc == 0;
        }
    }
    a_rc = rc;
    journal_done("audio_start", rc, rc == 0);
}

/* 1 when the port plays: the start (behind the counter, once), then the fill
 * thread. A start that froze before shows no job screen and runs nothing;
 * after SQUARE twice it runs again. A port left open by an end that froze
 * before is played again as it is. */
static int audio_up(void)
{
    if (!a_started) {
        a_frozen = journal_state("audio_start") == J_FROZEN;
        if (!a_frozen) run_job("Starting the audio", job_up);
    }
    if (!a_started) return 0;
    if (!fill_ok) {
        memset(&cur, 0, sizeof cur);
        cur.channels = a_ch;                 /* silence until the first sound */
        cur.len = 0;
        cur.stop = cur.cut = -1;
        stop_upto = next_seq;
        quiet = 0;
        fill_on = 1;
        fill_ok = sysThreadCreate(&fill_th, fill_thread, NULL, 900, 0x4000, THREAD_JOINABLE, "audio") == 0;
        if (!fill_ok) fill_on = 0;
    }
    return fill_ok;
}

/* The end: the sound that plays fades out in CUT_MS (a sweep's own fade is
 * 0.5 s), the port plays the faded blocks and some silence (the fade written,
 * then 70 ms: AHEAD blocks are 43 ms; 400 ms at most), the thread ends and
 * the whole ring is zeroed; then the port stop, its close, the library's
 * end and the module unload, each only where its start succeeded. One
 * journaled call. An end that froze this console before is not made: the
 * port stays open and silent, the flags stay, and a later test in this
 * session plays on it (no second start, no second audioInit). No journal
 * (no writable app folder): the port never opened, the module stays closed. */
void audio_end(void)
{
    u64 ret;
    if (!a_mod && !a_init && !a_open) return;    /* never started, or ended already */
    if (fill_ok) {
        quiet = 1;
        for (int i = 0, after = 0; i < 40; i++) {   /* the sleep first: the li r11 stays before its sc for tools/syscalls.py */
            sysUsleep(10000);
            if (now_done && ++after >= 7) break;
        }
        fill_on = 0;
        sysThreadJoin(fill_th, &ret);
        fill_ok = 0;
    }
    if (a_started) memset((void *)(uintptr_t)cfg.audioDataStart, 0, (size_t)BLOCKS * (size_t)a_ch * AUDIO_BLOCK_SAMPLES * sizeof(float));
    if (!journal_start("audio_end")) return;   /* it froze this console at an exit before: the port stays as it is */
    s32 rc = 0, r;
    if (a_started && (r = audioPortStop(port)) != 0 && rc == 0) rc = r;
    if (a_open && (r = audioPortClose(port)) != 0 && rc == 0) rc = r;
    if (a_init && (r = audioQuit()) != 0 && rc == 0) rc = r;
    if (a_mod && (r = sysModuleUnload(SYSMODULE_AUDIO)) != 0 && rc == 0) rc = r;
    a_mod = a_init = a_open = a_started = 0;
    journal_done("audio_end", rc, rc == 0);
}

/* ---- the questions ------------------------------------------------------- */

/* The speakers on a plan, the TV at the top and the listener in the middle:
 * the one playing in white, an answer in green or red, the rest grey. n: 2
 * for the front pair alone, 8 for all. active -1: none, -2: the front pair. */
static void draw_plan(float x, float y, float w, float h, int n, int active, const int *ans)
{
    card(x, y, w, h);
    float cx = x + w / 2, cy = y + h / 2 + 10, sx = w * 0.36f, sy = h * 0.36f;
    round_rect(cx - w * 0.2f, y + 26, w * 0.4f, 12, 6, GREY);
    text_c(cx, y + 44, F_SMALL, DIM, "TV");
    ring(cx, cy, 20, 3, DIM);
    text_c(cx, cy + 26, F_SMALL, DIM, "you");
    for (int k = 0; k < n; k++) {
        float px = cx + spk_x[k] * sx, py = cy + spk_y[k] * sy;
        int on = active == k || (active == -2 && k < 2);
        u32 c = on ? WHITE : ans && ans[k] == ANS_HEARD ? GREEN : ans && ans[k] == ANS_NOT ? RED : DIM;
        if (on) disc(px, py, 26, PANEL2);
        if (k == 3) round_frame(px - 18, py - 18, 36, 36, 6, 4, c);   /* the subwoofer: a box */
        else ring(px, py, 18, 4, c);
        if (on || (ans && ans[k] != ANS_NONE)) disc(px, py, 8, c);
        text_c(px, py + 28, F_SMALL, on ? WHITE : GREY, "%s", spk_short[k]);
    }
}

/* One sound until the answer: CROSS heard (ANS_HEARD), CIRCLE not heard
 * (ANS_NOT), TRIANGLE stops the test (0), START or Quit Game exits (-1).
 * The sound fades out at every answer. A sweep plays once; the answer can
 * come during it or after it. */
static int ask(const tone *t, const char *head, const char *name, const char *question, int plan_n, int active, const int *ans)
{
    int r = 0;
    play(t);
    s64 t0 = sysGetSystemTime();
    while (1) {
        read_pad();
        if (quit || (pressed & BTN_START)) { r = -1; break; }
        if (pressed & BTN_CROSS) { r = ANS_HEARD; break; }
        if (pressed & BTN_CIRCLE) { r = ANS_NOT; break; }
        if (pressed & BTN_TRIANGLE) break;
        begin_frame();
        title();
        float lw = (SW - 2 * MG) * 0.5f, y = TOP;
        text(MG, y, F_MED, GREY, "%s", head);
        text_fit(MG, y + 70, F_LARGE, WHITE, lw - 32, "%s", name);
        double sec = (double)(sysGetSystemTime() - t0) / 1e6;
        if (t->len > 0) {                    /* the sweep: its frequency and its progress */
            float hz = now_hz;
            if (now_done) text(MG, y + 160, F_MED, GREY, "Done: did you hear it all the way?");
            else if (hz >= 1000) text(MG, y + 160, F_MED | TNUM, BLUE, "%.1f kHz", hz / 1000);
            else text(MG, y + 160, F_MED | TNUM, BLUE, "%.0f Hz", hz);
            bar(MG, y + 220, lw - 32, 10, (float)(sec / SWEEP_S), BLUE);
            text_wrap(MG, y + 260, F_BODY, GREY, lw - 32, 4,
                      "From 20 Hz to 20 kHz in 12 s, low to high. Most ears stop near 15 kHz: that is the ear, not the TV. "
                      "A rattle or a buzz on the way is a speaker or a cable.");
        } else {
            text(MG, y + 160, F_MED, BLUE, "%d Hz beeps at %d dBFS", (int)(t->f0 + 0.5), LEVEL_DB);
            spinner(MG + 20, y + 250, 16, sec);
            text_wrap(MG, y + 290, F_BODY, GREY, lw - 32, 4,
                      plan_n == 8 ? "On a stereo TV the console mixes the surround channels into its two speakers, and may "
                                    "drop the subwoofer: \"not heard\" there is no fault."
                                  : "The beeps repeat until you answer.");
        }
        draw_plan(MG + lw, TOP, lw, BOTTOM - TOP, plan_n, active, ans);
        status_line(question, WHITE);
        footer("CROSS heard  CIRCLE not heard  TRIANGLE stop the test  START exit");
        ui_flip();
    }
    hush();
    pressed = 0;                             /* the answer acts nowhere else */
    return r;
}

/* CROSS: the left speaker, then the right; each beeps until its answer. */
static int test_lr(void)
{
    static const char *const side[2] = {"Left speaker", "Right speaker"};
    static const char *const q[2] = {"Do the beeps come from the left speaker? CROSS heard, CIRCLE not heard.",
                                     "Do the beeps come from the right speaker? CROSS heard, CIRCLE not heard."};
    tone t;
    char head[48];
    int plan[CH_MAX] = {0};
    lr[0] = lr[1] = ANS_NONE;
    lr_ran = 1;
    for (int k = 0; k < 2; k++) {
        tone_beeps(&t, a_ch, 1u << k, BEEP_HZ, LEVEL_DB);
        snprintf(head, sizeof head, "Left and right  %d of 2", k + 1);
        memcpy(plan, lr, sizeof lr);
        int r = ask(&t, head, side[k], q[k], 2, k, plan);
        if (r < 0) return -1;
        if (r == 0) break;
        lr[k] = r;
    }
    return 0;
}

/* TRIANGLE: the eight channels one after the other. */
static int test_walk(void)
{
    tone t;
    char head[48], q[128], lower[24];
    memset(walk, 0, sizeof walk);
    walk_ran = 1;
    for (int k = 0; k < CH_MAX; k++) {
        tone_beeps(&t, a_ch, 1u << k, k == 3 ? LFE_HZ : BEEP_HZ, LEVEL_DB);
        snprintf(head, sizeof head, "Speaker walk  %d of %d", k + 1, CH_MAX);
        snprintf(lower, sizeof lower, "%s", k == 3 ? "subwoofer" : spk_name[k]);
        if (lower[0] >= 'A' && lower[0] <= 'Z') lower[0] = (char)(lower[0] - 'A' + 'a');   /* "the front left speaker" */
        snprintf(q, sizeof q, "Do the beeps come from the %s speaker? CROSS heard, CIRCLE not heard.", lower);
        int r = ask(&t, head, spk_name[k], q, CH_MAX, k, walk);
        if (r < 0) return -1;
        if (r == 0) break;
        walk[k] = r;
    }
    return 0;
}

/* SQUARE: the sweep on the front pair. */
static int test_sweep(void)
{
    tone t;
    sweep_ans = ANS_NONE;
    sweep_ran = 1;
    tone_sweep(&t, a_ch, 3, 20, 20000, SWEEP_S, LEVEL_DB);
    int r = ask(&t, "Sweep", "20 Hz to 20 kHz", "Did you hear the sweep, clean, on both speakers? CROSS heard, CIRCLE not heard.", 2, -2, NULL);
    if (r < 0) return -1;
    sweep_ans = r;
    return 0;
}

/* ---- screen --------------------------------------------------------------- */

static int heard_count(const int *a, int n, int *asked)
{
    int h = 0;
    *asked = 0;
    for (int k = 0; k < n; k++) {
        if (a[k] != ANS_NONE) (*asked)++;
        if (a[k] == ANS_HEARD) h++;
    }
    return h;
}

static int any_not(void)
{
    if (lr[0] == ANS_NOT || lr[1] == ANS_NOT || sweep_ans == ANS_NOT) return 1;
    for (int k = 0; k < CH_MAX; k++) if (walk[k] == ANS_NOT) return 1;
    return 0;
}

static const char *ans_text(int a) { return a == ANS_HEARD ? "heard" : a == ANS_NOT ? "not heard" : "not answered"; }

/* A test stopped with TRIANGLE before its last answer: no pass, yellow, as
 * Memory's stopped test. */
static int lr_open(void) { return lr_ran && (lr[0] == ANS_NONE || lr[1] == ANS_NONE); }
static int walk_open(void)
{
    int asked;
    heard_count(walk, CH_MAX, &asked);
    return walk_ran && asked < CH_MAX;
}
static int sweep_open(void) { return sweep_ran && sweep_ans == ANS_NONE; }

/* "Left and right heard", "Right not heard", "Neither heard" */
static void lr_line(char *s, int n)
{
    if (!lr_ran) snprintf(s, n, "Left and right not tested");
    else if (lr[0] == ANS_HEARD && lr[1] == ANS_HEARD) snprintf(s, n, "Left and right heard");
    else if (lr[0] == ANS_NOT && lr[1] == ANS_NOT) snprintf(s, n, "Neither side heard");
    else if (lr[0] == ANS_NOT) snprintf(s, n, "Left not heard");
    else if (lr[1] == ANS_NOT) snprintf(s, n, "Right not heard");
    else snprintf(s, n, "Left and right: not answered");
}

static void audio_tile(void)
{
    char l1[48], l2[48];
    int asked, j = 0;
    if (!lr_ran && !walk_ran && !sweep_ran) return;
    lr_line(l1, sizeof l1);
    l2[0] = 0;
    if (walk_ran) {
        int h = heard_count(walk, CH_MAX, &asked);
        if (asked < CH_MAX) j += snprintf(l2 + j, sizeof l2 - j, "walk %d of %d answered", asked, CH_MAX);
        else j += snprintf(l2 + j, sizeof l2 - j, "walk %d of %d heard", h, asked);
    }
    if (sweep_ran && j < (int)sizeof l2) j += snprintf(l2 + j, sizeof l2 - j, "%ssweep %s", j ? ", " : "", ans_text(sweep_ans));
    if (!l2[0]) snprintf(l2, sizeof l2, "walk and sweep not tested");
    state_set("audio", any_not() || lr_open() || walk_open() || sweep_open() ? DOT_WARN : DOT_OK, l1, l2);
}

/* The note under the cards: why the port did not play, or its channels. */
static void port_note(void)
{
    if (a_frozen) status_line("The audio start froze this console before: no sound. SQUARE twice on the home screen clears the journal.", RED);
    else if (!a_started && a_rc) status_linef(YELLOW, "The audio did not start: rc 0x%08x", (unsigned)a_rc);
    else if (a_ch == 8) status_line("The port plays 8 channels at 48 kHz. Set the TV to a normal volume: every sound plays at -18 dBFS.", GREY);
    else if (a_ch == 2) status_line("The port plays 2 channels at 48 kHz: no speaker walk. Every sound plays at -18 dBFS.", GREY);
    else status_line("Set the TV to a normal volume first: every sound plays at -18 dBFS, with a fade at each end.", GREY);
}

static void result_line(float x, float y, float w, u32 c, const char *s) { text_fit(x + 32, y + TEST_RESULT_Y, F_MED, c, w - 64, "%s", s); }

int mod_audio_open(void)
{
    if (!fs_ok) return 0;                    /* no journal, no audio call: the home screen says why */
    if (!a_started) a_frozen = journal_state("audio_start") == J_FROZEN;   /* SQUARE twice on the home screen clears it */
    int r = 0;
    while (1) {
        read_pad();
        int back = ui_leave();
        if (back <= 0) { r = back; break; }
        unsigned go = pressed & (BTN_CROSS | BTN_TRIANGLE | BTN_SQUARE);
        if (go && (go != BTN_TRIANGLE || a_ch != 2) && audio_up()) {
            int t = go & BTN_CROSS ? test_lr() : go & BTN_TRIANGLE ? (a_ch == 8 ? test_walk() : 0) : test_sweep();
            write_report();
            if (t < 0) { r = -1; break; }
        }
        begin_frame();
        title();
        float w = (SW - 2 * MG - 64) / 3, h = BOTTOM - TOP - 56 > 500 ? 500 : BOTTOM - TOP - 56, x = MG, y = TOP;
        char s[64];
        int asked, n;
        test_card(x, y, w, h, BTN_CROSS, "Left and right", "A beep on the left speaker, then on the right. Each beeps until you answer.");
        if (lr_ran) {
            lr_line(s, sizeof s);
            result_line(x, y, w, lr[0] == ANS_HEARD && lr[1] == ANS_HEARD ? GREEN : YELLOW, s);
            text_fit(x + 32, y + TEST_RESULT_Y + 52, F_BODY, GREY, w - 64, "left %s, right %s", ans_text(lr[0]), ans_text(lr[1]));
        } else result_line(x, y, w, DIM, "Not tested");
        x += w + 32;
        test_card(x, y, w, h, BTN_TRIANGLE, "Speaker walk", "The eight channels of 7.1 one by one, each speaker named as it plays. Needs an 8-channel port.");
        if (a_ch == 2) {
            result_line(x, y, w, GREY, "Not available");
            text_fit(x + 32, y + TEST_RESULT_Y + 52, F_BODY, GREY, w - 64, "the port opened 2 channels");
        } else if (walk_ran) {
            n = heard_count(walk, CH_MAX, &asked);
            snprintf(s, sizeof s, "%d of %d heard", n, asked);
            result_line(x, y, w, n < asked || asked < CH_MAX ? YELLOW : GREEN, s);
            int line = 0;
            for (int k = 0; k < CH_MAX && line < 3; k++)
                if (walk[k] == ANS_NOT) text_fit(x + 32, y + TEST_RESULT_Y + 52 + 38 * line++, F_BODY, GREY, w - 64, "%s not heard", spk_name[k]);
            if (asked < CH_MAX && line < 3) text_fit(x + 32, y + TEST_RESULT_Y + 52 + 38 * line, F_BODY, GREY, w - 64, "%d not answered", CH_MAX - asked);
        } else result_line(x, y, w, DIM, "Not tested");
        x += w + 32;
        test_card(x, y, w, h, BTN_SQUARE, "Sweep", "One tone from 20 Hz to 20 kHz in 12 s on both front speakers, low to high.");
        if (sweep_ran) {
            result_line(x, y, w, sweep_ans == ANS_HEARD ? GREEN : YELLOW, sweep_ans == ANS_HEARD ? "Heard" : sweep_ans == ANS_NOT ? "Not heard" : "Not answered");
            text_fit(x + 32, y + TEST_RESULT_Y + 52, F_BODY, GREY, w - 64, "at %d dBFS, fade in and out", LEVEL_DB);
        } else result_line(x, y, w, DIM, "Not tested");
        port_note();
        footer(a_ch == 2 ? "CROSS left and right  SQUARE sweep  CIRCLE home  START exit"
                         : "CROSS left and right  TRIANGLE speaker walk  SQUARE sweep  CIRCLE home  START exit");
        report_footnote();
        ui_flip();
    }
    audio_end();                             /* every way out of the module: no port stays open behind the home screen */
    audio_tile();
    return r;
}

void mod_audio_report(void)
{
    if (!a_frozen && !a_rc && !a_ch && !lr_ran && !walk_ran && !sweep_ran) return;
    rep_out("--- audio ---\n");
    if (a_frozen) rep_out("port: not started, the start froze this console before\n");
    else if (a_rc && !a_started) rep_out("port: the start failed, rc 0x%08x\n", (unsigned)a_rc);
    else if (a_ch) rep_out("port: %d channels, 48 kHz, %d blocks of %d samples\n", a_ch, BLOCKS, AUDIO_BLOCK_SAMPLES);
    if (a_ch == 2 && a_rc8) rep_out("8-channel port refused: rc 0x%08x\n", (unsigned)a_rc8);
    if (lr_ran) rep_out("left: %s, right: %s (%d Hz beeps at %d dBFS)\n", ans_text(lr[0]), ans_text(lr[1]), BEEP_HZ, LEVEL_DB);
    if (walk_ran) {
        rep_out("speaker walk:");
        for (int k = 0; k < CH_MAX; k++) rep_out("%s %s %s", k ? "," : "", spk_short[k], ans_text(walk[k]));
        rep_out("\n");
    }
    if (sweep_ran) rep_out("sweep 20 Hz to 20 kHz in %d s at %d dBFS: %s\n", SWEEP_S, LEVEL_DB, ans_text(sweep_ans));
}
