/* The Clock module: the console's clock against a time server (one NTP
 * exchange over UDP: four samples, the one with the shortest round trip
 * counts), the drift of the clock between runs from clock.txt, the wall
 * clock against the system timer within the session, and a warning when the
 * clock was reset (a flat RTC battery starts it at 2000). The time server is
 * pool.ntp.org through the console's resolver, or the first line of ntp.txt.
 * The network starts as in the Transfer module (net_init, mod_net.c); the
 * exchange is one journaled call, clock_ntp. SQUARE sets the clock to the
 * server's time after a confirm card: one system call (146), journaled as
 * clock_set, the app's one write to the system. The packets and the numbers
 * are in ntp.c. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <lv2/systime.h>
#include <sys/systime.h>
#include <net/net.h>
#include <net/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "app.h"
#include <ps3gfx/ui.h>
#include "report.h"
#include "ntp.h"

#define SAMPLES 4
#define NTP_WAIT_MS 1500                     /* a sample with no reply in this time is lost */
#define NTP_GAP_US 200000                    /* between two samples */
#define NTP_DEFAULT "pool.ntp.org"
#define NTP_FILE APP_DIR "/clock.txt"
#define NTP_SERVER_FILE APP_DIR "/ntp.txt"
#define HIST 8                               /* the lines of clock.txt kept: the drift and the list */
#define HIST_BYTES 16384                     /* clock.txt is read up to here; past it, the file starts again from the HIST lines */
#define TIMER_MIN_US 10000000                /* the timer check shows after 10 s */

enum { RES_NONE, RES_NO_NET, RES_FROZEN, RES_DNS, RES_SOCKET, RES_SEND, RES_TIMEOUT, RES_KOD, RES_CANCEL };

static struct {
    int done;                                /* an exchange was tried in this session */
    int have;                                /* a reply came: s[best] counts */
    int rc, err;                             /* why there is no reply (RES_*), the socket errno for RES_SOCKET */
    char server[64], ip[20], kiss[8];
    int sent, got, best;
    ntp_sample s[SAMPLES];
    ntp_reply rep[SAMPLES];                  /* each sample's reply */
    ntp_us tc[SAMPLES];                      /* the console's clock at each sample's end */
    ntp_reply reply;                         /* the reply of the best sample */
    ntp_us t_console;                        /* the console's clock at the best sample's end */
} q;
static ntp_line hist[HIST];                  /* the last lines of clock.txt before this session's result, oldest first */
static int nhist, hist_rc, hist_capped;      /* hist_rc: -1 the file could not be read (missing counts as 0 lines); hist_capped: the file reached HIST_BYTES */
static int have_drift, drift_set;            /* drift_set: the rate is past NTP_DRIFT_MAX_PPM, so the clock was set between the runs */
static double drift_ppm, drift_ms_day;
static int64_t drift_since_s, drift_span_s;
static ntp_us drift_step_us;                 /* the offset's change since the last line: the step when the clock was set */
static int append_rc;                        /* the last append to clock.txt */
static int level = -1;                       /* the verdict: 0 ok, 1 check, 2 problem, -1 none */
static char why[160];
static s64 open_sys;                         /* the timer check: the system timer and the wall clock at the open */
static ntp_us open_wall;
static int visited;
static int set_done;                         /* the clock was set (or the set was refused) in this session */
static s32 set_rc;                           /* the set's result: 0, or the system's code */
static ntp_us set_delta_us;                  /* the clock's move */
static int set_frozen;                       /* journal_state("clock_set") == J_FROZEN at the open: no set */
static int set_skipped;                      /* the server did not answer the fresh exchange before the set: not set */

/* ---- the clocks ------------------------------------------------------------ */

static ntp_us wall_us(void)
{
    u64 sec = 0, nsec = 0;
    sysGetCurrentTime(&sec, &nsec);
    return (ntp_us)sec * 1000000 + (ntp_us)(nsec / 1000);
}

static void utc_tm(int64_t sec, struct tm *tm)
{
    time_t t = (time_t)sec;
    gmtime_r(&t, tm);
}

static void zone_text(char *s, int n)
{
    if (console_tz_rc) { snprintf(s, n, "zone not read"); return; }
    int a = console_tz < 0 ? -console_tz : console_tz;
    snprintf(s, n, "UTC%c%d:%02d%s", console_tz < 0 ? '-' : '+', a / 60, a % 60, console_summer ? ", summer time" : "");
}

/* ---- clock.txt ------------------------------------------------------------- */

static void hist_load(void)
{
    static char buf[HIST_BYTES];
    nhist = hist_rc = hist_capped = 0;
    int n = fs_read_file(NTP_FILE, buf, sizeof buf - 1);
    if (n < 0) return;                       /* missing: no lines */
    buf[n] = 0;
    hist_capped = n >= (int)sizeof buf - 1;
    int total = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        ntp_line l;
        if (ntp_line_parse(line, &l)) continue;
        if (nhist == HIST) { memmove(hist, hist + 1, sizeof hist - sizeof hist[0]); nhist--; }
        hist[nhist++] = l;
        total++;
    }
    if (total == 0 && n > 0) hist_rc = -1;   /* a file with no line of ours */
}

/* The result as one more line; a file at the cap starts again from the kept lines. */
static void hist_append(const ntp_line *l)
{
    char text[HIST * 96 + 96];
    int j = 0;
    if (hist_capped)
        for (int k = 0; k < nhist; k++) {
            int m = ntp_line_format(text + j, (int)sizeof text - j, &hist[k]);
            if (m > 0) j += m;
        }
    int m = ntp_line_format(text + j, (int)sizeof text - j, l);
    if (m < 0) { append_rc = -1; return; }
    j += m;
    append_rc = fs_write_file(NTP_FILE, text, (uint64_t)j, !hist_capped);
    if (append_rc == 0) hist_capped = 0;
}

/* ---- the exchange ---------------------------------------------------------- */

static int sock_err(int rc) { return rc == -1 || rc < -255 ? net_errno : -rc; }

static void addr_of(struct sockaddr_in *a, in_addr_t ip, int port)
{
    memset(a, 0, sizeof *a);
    a->sin_len = sizeof *a;
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    a->sin_addr.s_addr = ip;
}

static void server_name(char *s, int n)
{
    if (fs_read_line(NTP_SERVER_FILE, s, n) <= 0) snprintf(s, n, "%s", NTP_DEFAULT);
}

static int resolve(const char *host, in_addr_t *out)
{
    *out = inet_addr(host);
    if (*out != (in_addr_t)-1) return 0;
    struct net_hostent *he = netGetHostByName(host);
    if (!he || !he->h_addr_list) return -1;
    uint32_t *list = (uint32_t *)(uintptr_t)he->h_addr_list;     /* 32-bit pointers in the network library */
    if (!list[0]) return -1;
    *out = *(uint32_t *)(uintptr_t)list[0];
    return 0;
}

/* One sample on the socket s: 0 and the sample, -1 and q.rc. t1 is the wall
 * clock; the round trip comes from the system timer, so t4 = t1 + the timer's
 * step (the wall clock's read at t4 would add its own resolution). A packet
 * that is not the reply to ours is read and skipped. */
static int exchange(int s, in_addr_t ip, ntp_sample *out, ntp_reply *rep_out, ntp_us *tc_out)
{
    static uint8_t req[NTP_PACKET], in[128];
    struct sockaddr_in a, from;
    ntp_us t1 = wall_us();
    s64 s1 = sysGetSystemTime();
    ntp_request(req, sizeof req, t1);
    addr_of(&a, ip, NTP_PORT);
    if (netSendTo(s, req, NTP_PACKET, 0, (struct sockaddr *)&a, sizeof a) != NTP_PACKET) { q.rc = RES_SEND; return -1; }
    for (;;) {
        int left = NTP_WAIT_MS - (int)((sysGetSystemTime() - s1) / 1000);
        struct pollfd p = {s, POLLIN, 0};
        if (left <= 0 || netPoll(&p, 1, left) <= 0) { q.rc = RES_TIMEOUT; return -1; }
        socklen_t fl = sizeof from;
        ssize_t r = netRecvFrom(s, in, sizeof in, 0, (struct sockaddr *)&from, &fl);
        if (r < 0) { q.rc = RES_TIMEOUT; return -1; }
        s64 s4 = sysGetSystemTime();
        ntp_reply rep;
        int pr = ntp_parse(in, (int)r, req, &rep);
        if (pr == -2) { q.rc = RES_KOD; snprintf(q.kiss, sizeof q.kiss, "%s", rep.ref); return -1; }
        if (pr < 0) continue;
        ntp_sample_calc(t1, rep.t2, rep.t3, t1 + (s4 - s1), out);
        *rep_out = rep;
        *tc_out = t1 + (s4 - s1);
        return 0;
    }
}

static void gap(void)
{
    for (int k = 0; k < NTP_GAP_US / 20000 && !job_cancel; k++) sysUsleep(20000);
}

/* The whole exchange: the name, the address, the socket, the samples. */
static void query_run(void)
{
    char msg[96];
    memset(&q, 0, sizeof q);
    q.done = 1;
    have_drift = drift_set = 0;
    server_name(q.server, sizeof q.server);
    if (!net_is_up()) { q.rc = RES_NO_NET; return; }
    snprintf(msg, sizeof msg, "Looking up %s", q.server);
    progress(msg);
    in_addr_t ip;
    if (resolve(q.server, &ip)) { q.rc = RES_DNS; return; }
    uint32_t h = ntohl(ip);
    snprintf(q.ip, sizeof q.ip, "%u.%u.%u.%u", h >> 24, (h >> 16) & 255, (h >> 8) & 255, h & 255);
    if (fs_ok && !journal_start("clock_ntp")) { q.rc = RES_FROZEN; return; }   /* 0 for a frozen or skipped entry */
    int s = netSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        q.rc = RES_SOCKET;
        q.err = sock_err(s);
        if (fs_ok) journal_done("clock_ntp", q.err, 0);
        return;
    }
    struct timeval tv = {NTP_WAIT_MS / 1000 + 1, 0};
    netSetSockOpt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (int k = 0; k < SAMPLES && !job_cancel; k++) {
        snprintf(msg, sizeof msg, "Sample %d of %d from %s", k + 1, SAMPLES, q.server);
        progress(msg);
        job_set_percent(k * 100 / SAMPLES);
        q.sent++;
        if (exchange(s, ip, &q.s[q.got], &q.rep[q.got], &q.tc[q.got]) == 0) q.got++;
        else if (q.rc == RES_KOD) break;     /* the server wants no more */
        if (k + 1 < SAMPLES) gap();
    }
    netClose(s);
    if (fs_ok) journal_done("clock_ntp", 0, 1);
    job_set_percent(-1);
    if (job_cancel && !q.got) { q.rc = RES_CANCEL; return; }
    if (!q.got) return;                      /* q.rc says why */
    q.have = 1;
    q.rc = RES_NONE;
    q.best = ntp_best(q.s, q.got);
    q.reply = q.rep[q.best];
    q.t_console = q.tc[q.best];
    if (nhist) {
        const ntp_line *last = &hist[nhist - 1];
        have_drift = ntp_drift(last->console_s * 1000000, (ntp_us)last->offset_ms * 1000, q.t_console, q.s[q.best].offset_us, &drift_ppm, &drift_ms_day);
        drift_set = have_drift && (drift_ppm > NTP_DRIFT_MAX_PPM || drift_ppm < -NTP_DRIFT_MAX_PPM);
        drift_since_s = last->console_s;
        drift_span_s = q.t_console / 1000000 - last->console_s;
        drift_step_us = q.s[q.best].offset_us - (ntp_us)last->offset_ms * 1000;
    }
    if (fs_ok) {
        ntp_line l;
        l.console_s = q.t_console / 1000000;
        l.offset_ms = (int)(q.s[q.best].offset_us / 1000);
        l.rtt_ms = (int)(q.s[q.best].rtt_us / 1000);
        snprintf(l.server, sizeof l.server, "%s", q.server);
        hist_append(&l);
    }
}

static const char *no_reply(void)
{
    static char s[96];
    switch (q.rc) {
    case RES_NO_NET: return net_start_frozen() ? "the network start froze this console before" : "the network did not start";
    case RES_FROZEN: return "the exchange froze this console before: not made again";
    case RES_DNS: snprintf(s, sizeof s, "%.40s did not resolve (no DNS, or no internet)", q.server); return s;
    case RES_SOCKET: snprintf(s, sizeof s, "the UDP socket was refused (errno %d)", q.err); return s;
    case RES_SEND: return "the packet could not be sent";
    case RES_TIMEOUT: snprintf(s, sizeof s, "no reply from %s in %d s (port 123 blocked, or no internet)", q.ip, NTP_WAIT_MS / 1000 + (NTP_WAIT_MS % 1000 ? 1 : 0)); return s;
    case RES_KOD: snprintf(s, sizeof s, "the server refused with the kiss code %s", q.kiss); return s;
    case RES_CANCEL: return "stopped";
    }
    return "";
}

/* The verdict: the reset mark counts even without an exchange; without
 * one, nothing else does (the tile keeps its last result). */
static void judge(void)
{
    int64_t now_s = wall_us() / 1000000;
    int tried = q.done && q.rc != RES_NO_NET && q.rc != RES_FROZEN && q.rc != RES_CANCEL;
    if (!tried && now_s >= NTP_RESET_S) { level = -1; return; }
    level = ntp_judge(q.have, q.have ? q.s[q.best].offset_us : 0, now_s, why, sizeof why);
    if (!q.have && level == 1 && tried) snprintf(why, sizeof why, "the clock could not be checked: %s", no_reply());
}

static void job_query(void)
{
    net_init();
    query_run();
    judge();
}

/* ---- the set ------------------------------------------------------------- */

/* The confirm card of SQUARE: 1 set, 0 keep the clock, -1 exit. */
static int confirm_set(void)
{
    char t[32], msg[320];
    ntp_offset_text(q.s[q.best].offset_us, t, sizeof t);
    snprintf(msg, sizeof msg, "The console's clock moves to the time of %.40s: the server is asked again and the clock moved by the fresh offset (%s at the last reply). "
             "One system call (146, the clock set), journaled as clock_set; then the server is asked once more, and the drift starts from that result.", q.server, t);
    while (1) {
        read_pad();
        int back = ui_leave();
        if (back <= 0) return back;
        if (pressed & BTN_CROSS) return 1;
        begin_frame();
        title();
        float x = MG + 40, w = SW - 2 * MG - 80, y = TOP + 10;
        card(MG, y, SW - 2 * MG, 430);
        float ty = heading(x, y + 22, "SET THE CONSOLE CLOCK");
        text(x, ty + 6, F_LARGE | TNUM, WHITE, "%s", t);
        int lines = text_wrap(x, ty + 110, F_BODY, GREY, w, 4, msg);
        text_wrap(x, ty + 110 + lines * wrap_step(F_BODY) + 16, F_BODY, DIM, w, 2,
                  "The system's own Set Automatically corrects the clock only at a PlayStation Network sign-in. Games and the system see the new time at once.");
        footer("CROSS set the clock  CIRCLE keep it  START exit");
        ui_flip();
    }
}

/* The set: a fresh exchange (the open's offset may be minutes old), its
 * best offset added to the clock, then the server asked once more. The
 * target is taken right before the call: the journal's synced write before
 * it takes milliseconds (rc1 on the console: a set 5.6 ms short). A refused
 * call leaves the clock as it was. */
static void job_set(void)
{
    u64 sec, nsec;
    set_skipped = 0;
    query_run();
    if (!q.have) { set_skipped = 1; judge(); return; }
    ntp_us delta = q.s[q.best].offset_us;
    progress("Setting the clock");
    if (fs_ok && !journal_start("clock_set")) { set_frozen = 1; judge(); return; }
    ntp_set_target(wall_us(), delta, &sec, &nsec);
    set_rc = sysSetCurrentTime(sec, nsec);
    if (fs_ok) journal_done("clock_set", set_rc, set_rc == 0);
    set_done = 1;
    if (set_rc) { judge(); return; }
    set_delta_us = delta;
    open_wall += delta;                      /* the timer check: the wall clock moved by the set, not by itself */
    gap();
    query_run();
    judge();
}

/* ---- tile, quick check ---------------------------------------------------- */

static void drift_line(char *s, int n)
{
    char t[32];
    if (set_done && set_rc == 0) {
        ntp_offset_text(set_delta_us, t, sizeof t);
        snprintf(s, n, "Clock set in this session (%s)", t);
    } else if (drift_set) {
        ntp_offset_text(-drift_step_us, t, sizeof t);
        snprintf(s, n, "Clock set since the last run (%s)", t);
    } else if (have_drift) {
        struct tm tm;
        utc_tm(drift_since_s + console_utc_offset(), &tm);
        snprintf(s, n, "Drift %+.1f s/day since %04d-%02d-%02d", drift_ms_day / 1000, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    } else if (!q.have) snprintf(s, n, "%s", q.done ? no_reply() : "Set the date and time in the system settings");
    else if (!nhist) snprintf(s, n, "First result: the next run shows the drift");
    else snprintf(s, n, "Drift: needs 10 min between runs");
}

static int level_dot(void) { return level == 0 ? DOT_OK : level == 1 ? DOT_WARN : DOT_BAD; }

static void clock_tile(void)
{
    char l1[48], l2[48], t[32];
    if (level < 0) return;
    if (level == 2 && !q.have) snprintf(l1, sizeof l1, "Clock reset: before 2010");
    else if (q.have) {
        ntp_offset_text(q.s[q.best].offset_us, t, sizeof t);
        snprintf(l1, sizeof l1, "%s vs %.30s", t, q.server);
    } else snprintf(l1, sizeof l1, "No answer from %.30s", q.server);
    drift_line(l2, sizeof l2);
    state_set("clock", level_dot(), l1, l2);
}

void clock_quick(void)
{
    net_init();
    hist_load();
    query_run();
    judge();
}

int clock_quick_end(char *line, int n)
{
    char t[32], d[48];
    if (level < 0) {
        snprintf(line, n, "Not run: %s.", no_reply());
        return DOT_NONE;
    }
    if (q.have) {
        ntp_offset_text(q.s[q.best].offset_us, t, sizeof t);
        drift_line(d, sizeof d);
        snprintf(line, n, "%s off %s, round trip %lld ms; %s", t, q.server, (long long)(q.s[q.best].rtt_us / 1000), d);
    } else snprintf(line, n, "%s", why);
    clock_tile();
    return level_dot();
}

/* ---- screen --------------------------------------------------------------- */

static float row(float x, float y, float kw, float w, const char *key, u32 c, const char *fmt, ...) __attribute__((format(printf, 7, 8)));
static float row(float x, float y, float kw, float w, const char *key, u32 c, const char *fmt, ...)
{
    va_list ap;
    text(x, y, F_BODY, GREY, "%s", key);
    va_start(ap, fmt);
    vtext_fit(x + kw, y, F_BODY, c, w - kw, fmt, ap);
    va_end(ap);
    return y + 44;
}

static const char *const day_names[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

static void draw_clock(float x, float y, float w, float h)
{
    char s[64], span[32];
    struct tm lt, ut;
    ntp_us now = wall_us();
    s64 sys = sysGetSystemTime();
    local_tm(&lt);
    utc_tm(now / 1000000, &ut);
    card(x, y, w, h);
    float cx = x + 32, cw = w - 64, kw = 190;
    float ty = heading(cx, y + 22, "CONSOLE CLOCK");
    text(cx, ty + 6, F_LARGE | TNUM, now / 1000000 < NTP_RESET_S ? RED : WHITE, "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
    text_fit(cx, ty + 96, F_MED, WHITE, cw, "%s %04d-%02d-%02d", day_names[lt.tm_wday % 7], lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    ty += 150;
    zone_text(s, sizeof s);
    ty = row(cx, ty, kw, cw, "Time zone", console_tz_rc ? GREY : WHITE, "%s", s);
    ty = row(cx, ty, kw, cw, "UTC", WHITE, "%02d:%02d:%02d, %04d-%02d-%02d", ut.tm_hour, ut.tm_min, ut.tm_sec, ut.tm_year + 1900, ut.tm_mon + 1, ut.tm_mday);
    ntp_span_text(sys / 1000000, span, sizeof span);
    ty = row(cx, ty, kw, cw, "On for", WHITE, "%s", span);
    s64 run = sys - open_sys;
    if (run >= TIMER_MIN_US) {
        s64 d = (s64)(now - open_wall) - run;
        ntp_span_text(run / 1000000, span, sizeof span);
        row(cx, ty, kw, cw, "Timer check", d > 1000 || d < -1000 ? YELLOW : WHITE, "wall clock %+lld us vs the system timer in %s", (long long)d, span);
    } else row(cx, ty, kw, cw, "Timer check", GREY, "wall clock vs the system timer, after 10 s");
}

static u32 level_color(void) { return level == 0 ? GREEN : level == 1 ? YELLOW : level == 2 ? RED : GREY; }

static void draw_server(float x, float y, float w, float h)
{
    char t[32];
    card(x, y, w, h);
    float cx = x + 32, cw = w - 64, kw = 190;
    float ty = heading(cx, y + 22, "TIME SERVER");
    if (!q.done) {
        text_wrap(cx, ty + 8, F_BODY, YELLOW, cw, 4,
                  net_start_frozen() ? "The network start froze this console before: no network call runs. SQUARE twice on the home screen clears the journal."
                  : !net_is_up()     ? "The network did not start: the time server cannot be asked."
                                     : "The exchange froze this console before: not made again. SQUARE twice on the home screen clears the journal.");
        return;
    }
    if (q.have) {
        const ntp_sample *b = &q.s[q.best];
        ntp_offset_text(b->offset_us, t, sizeof t);
        float ex = text(cx, ty + 6, F_LARGE | TNUM, level_color(), "%s", t);
        text(ex + 16, ty + 6 + base_dy(F_LARGE, F_BODY), F_BODY, GREY, "%s", b->offset_us < 0 ? "console ahead" : "console behind");
        ty += 96;
        ty = row(cx, ty, kw, cw, "Server", WHITE, "%s (%s)", q.server, q.ip);
        ty = row(cx, ty, kw, cw, "Round trip", WHITE, "%lld ms, the best of %d of %d replies", (long long)(b->rtt_us / 1000), q.got, q.sent);
        if (q.reply.stratum == 1 && q.reply.ref[0]) ty = row(cx, ty, kw, cw, "Stratum", WHITE, "1, source %s", q.reply.ref);
        else ty = row(cx, ty, kw, cw, "Stratum", WHITE, "%d", q.reply.stratum);
        char list[128] = "";
        for (int k = 0; k < q.got; k++) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%+.3f/%lld", k ? "  " : "", (double)q.s[k].offset_us / 1e6, (long long)(q.s[k].rtt_us / 1000));
        row(cx, ty, kw, cw, "Samples", GREY, "%s", list);
        return;
    }
    text(cx, ty + 6, F_LARGE, q.rc == RES_CANCEL ? GREY : YELLOW, "%s", q.rc == RES_CANCEL ? "Stopped" : "No answer");
    text_wrap(cx, ty + 96, F_BODY, GREY, cw, 3, no_reply());
    if (q.rc == RES_DNS || q.rc == RES_TIMEOUT)
        text_wrap(cx, ty + 96 + 3 * wrap_step(F_BODY), F_SMALL, DIM, cw, 2, "Another server: write its name or address as the first line of " NTP_SERVER_FILE ".");
}

static void draw_drift(float x, float y, float w, float h)
{
    char s[64], span[32];
    struct tm tm;
    card(x, y, w, h);
    float cx = x + 32, cw = w - 64, bottom = y + h - 16;
    float ty = heading(cx, y + 22, "DRIFT BETWEEN RUNS");
    int extra = 0;                           /* a second line of the set's message */
    if (set_done && set_rc == 0) {
        char t[32], m[200];
        ntp_offset_text(set_delta_us, t, sizeof t);
        snprintf(m, sizeof m, "The clock was set in this session (%s, to the server's time), then the server asked again. The drift starts again from that result.", t);
        extra = (text_wrap(cx, ty + 6, F_MED, GREEN, cw, 2, m) - 1) * wrap_step(F_MED);
    } else if (set_done) {
        char m[200];
        snprintf(m, sizeof m, "The clock could not be set: the system returned 0x%08x. Set it in the system settings (Date and Time Settings).", (unsigned)set_rc);
        extra = (text_wrap(cx, ty + 6, F_MED, YELLOW, cw, 2, m) - 1) * wrap_step(F_MED);
    } else if (set_frozen) {
        extra = (text_wrap(cx, ty + 6, F_MED, YELLOW, cw, 2, "The clock set froze this console before: not made again. SQUARE twice on the home screen clears the journal.") - 1) * wrap_step(F_MED);
    } else if (set_skipped) {
        extra = (text_wrap(cx, ty + 6, F_MED, YELLOW, cw, 2, "The server did not answer the exchange before the set: the clock was not set.") - 1) * wrap_step(F_MED);
    } else if (drift_set) {
        char t[32];
        utc_tm(drift_since_s + console_utc_offset(), &tm);
        ntp_offset_text(-drift_step_us, t, sizeof t);
        text_fit(cx, ty + 6, F_MED, WHITE, cw, "The clock was set since the last result (%s, %04d-%02d-%02d %02d:%02d): the drift starts again from this one.", t,
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    } else if (have_drift) {
        utc_tm(drift_since_s + console_utc_offset(), &tm);
        ntp_span_text(drift_span_s, span, sizeof span);
        text_fit(cx, ty + 6, F_MED, drift_ms_day > 2000 || drift_ms_day < -2000 ? YELLOW : WHITE, cw, "%+.2f s per day (%+.1f ppm) over %s, since %04d-%02d-%02d %02d:%02d",
                 drift_ms_day / 1000, drift_ppm, span, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    } else if (hist_rc < 0) text_fit(cx, ty + 6, F_MED, YELLOW, cw, "clock.txt has no line this app wrote: delete it over FTP to start again");
    else if (!nhist) text_fit(cx, ty + 6, F_MED, GREY, cw, "%s", q.have ? "First result kept: the next run shows the drift." : "No result kept yet: a quartz clock drifts a few seconds a month.");
    else if (q.have) text_fit(cx, ty + 6, F_MED, GREY, cw, "Two results under 10 min apart: no drift yet.");
    else text_fit(cx, ty + 6, F_MED, GREY, cw, "%d result%s kept: the drift shows after an answer.", nhist, nhist == 1 ? "" : "s");
    ty += 52 + extra;
    if (append_rc) { text_fit(cx, ty, F_SMALL, YELLOW, cw, "The result could not be written to clock.txt (rc 0x%08x).", (unsigned)append_rc); ty += 32; }
    int fit = (int)((bottom - ty) / 36);
    if (fit <= 0 || !nhist) return;
    int first = nhist > fit ? nhist - fit : 0;
    for (int k = nhist - 1; k >= first; k--, ty += 36) {
        utc_tm(hist[k].console_s + console_utc_offset(), &tm);
        ntp_offset_text((ntp_us)hist[k].offset_ms * 1000, s, sizeof s);
        text(cx, ty, F_BODY | TNUM, GREY, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        text(cx + 260, ty, F_BODY | TNUM, WHITE, "%s", s);
        text_fit(cx + 440, ty, F_BODY, DIM, cw - 440, "round trip %d ms, %s", hist[k].rtt_ms, hist[k].server);
    }
}

int mod_clock_open(void)
{
    /* As Network: the start behind the counter, backed by the journal; a
     * start that froze before shows no job screen. */
    int js = journal_state("net_start");
    if (!net_is_up() && fs_ok && js != J_FROZEN && js != J_SKIPPED) run_job("Starting the network", net_init);
    else net_init();
    visited = 1;
    hist_load();
    open_sys = sysGetSystemTime();
    open_wall = wall_us();
    int can = net_is_up() && journal_state("clock_ntp") != J_FROZEN;
    set_frozen = journal_state("clock_set") == J_FROZEN;
    if (can) run_job_cancelable("Asking the time server", job_query);
    else judge();
    clock_tile();
    while (1) {
        read_pad();
        int back = ui_leave();
        if (back <= 0) { clock_tile(); return back; }
        if ((pressed & BTN_CROSS) && can) {
            run_job_cancelable("Asking the time server", job_query);
            clock_tile();
            write_report();
        }
        if ((pressed & BTN_SQUARE) && can && q.have && !set_frozen) {
            int r = confirm_set();
            if (r < 0) { clock_tile(); return -1; }
            if (r == 1) {
                run_job("Setting the clock", job_set);
                clock_tile();
                write_report();
            }
        }
        begin_frame();
        title();
        float w = SW - 2 * MG, cw = (w - 32) / 2, h1 = 372, y2 = TOP + h1 + 24;
        draw_clock(MG, TOP, cw, h1);
        draw_server(MG + cw + 32, TOP, cw, h1);
        if (y2 + 120 <= BOTTOM) draw_drift(MG, y2, w, BOTTOM - 4 - y2);
        if (level >= 0) status_linef(level_color(), "%s%s.", level == 0 ? "No problem found: " : level == 1 ? "Check: " : "Problem: ", why);
        else status_line(q.done ? "No verdict: the exchange did not run." : "The console keeps its clock on a small battery: this checks it against the internet.", GREY);
        footer(!can ? "CIRCLE home  START exit" : q.have && !set_frozen ? "CROSS ask again  SQUARE set the clock  CIRCLE home  START exit"
                                                                      : "CROSS ask the time server again  CIRCLE home  START exit");
        report_footnote();
        ui_flip();
    }
}

void mod_clock_report(void)
{
    if (!visited && level < 0) return;
    char s[64], span[32];
    struct tm ut;
    ntp_us now = wall_us();
    s64 sys = sysGetSystemTime();
    utc_tm(now / 1000000, &ut);
    zone_text(s, sizeof s);
    ntp_span_text(sys / 1000000, span, sizeof span);
    rep_out("--- clock ---\nconsole clock %04d-%02d-%02d %02d:%02d:%02d UTC (%s), on for %s\n", ut.tm_year + 1900, ut.tm_mon + 1, ut.tm_mday, ut.tm_hour,
            ut.tm_min, ut.tm_sec, s, span);
    if (q.done && q.have) {
        const ntp_sample *b = &q.s[q.best];
        rep_out("time server %s (%s): offset %+.3f s, round trip %lld ms, stratum %d%s%s, %d of %d replies\n  samples (offset s / round trip ms):", q.server, q.ip,
                (double)b->offset_us / 1e6, (long long)(b->rtt_us / 1000), q.reply.stratum, q.reply.ref[0] ? " " : "", q.reply.ref, q.got, q.sent);
        for (int k = 0; k < q.got; k++) rep_out(" %+.3f/%lld", (double)q.s[k].offset_us / 1e6, (long long)(q.s[k].rtt_us / 1000));
        rep_out("\n");
    } else if (q.done) rep_out("time server %s: no answer, %s\n", q.server, no_reply());
    if (set_done && set_rc == 0) rep_out("clock set: moved %+.3f s to the server's time (sys_time_set_current_time), then asked again\n", (double)set_delta_us / 1e6);
    else if (set_done) rep_out("clock set refused: rc 0x%08x, the clock as it was\n", (unsigned)set_rc);
    else if (set_skipped) rep_out("clock set skipped: no answer from the server before it\n");
    if (have_drift && !(set_done && set_rc == 0)) {    /* a drift across this session's set is not one (the screen hides it too) */
        utc_tm(drift_since_s, &ut);
        ntp_span_text(drift_span_s, span, sizeof span);
        if (drift_set) rep_out("clock set since the last result: the console's clock moved %+.3f s against the server over %s, since %04d-%02d-%02d %02d:%02d UTC (no drift rate)\n",
                               (double)-drift_step_us / 1e6, span, ut.tm_year + 1900, ut.tm_mon + 1, ut.tm_mday, ut.tm_hour, ut.tm_min);
        else rep_out("drift %+.3f s per day (%+.2f ppm) over %s, since %04d-%02d-%02d %02d:%02d UTC\n", drift_ms_day / 1000, drift_ppm, span, ut.tm_year + 1900, ut.tm_mon + 1,
                     ut.tm_mday, ut.tm_hour, ut.tm_min);
    }
    rep_out("clock.txt: %d line%s kept%s%s\n", nhist, nhist == 1 ? "" : "s", hist_rc < 0 ? ", no line of this app" : "", append_rc ? ", the append failed" : "");
    if (visited && sys - open_sys >= TIMER_MIN_US) {
        ntp_span_text((sys - open_sys) / 1000000, span, sizeof span);
        rep_out("timer check: the wall clock moved %+lld us against the system timer in %s\n", (long long)((now - open_wall) - (sys - open_sys)), span);
    }
    if (level >= 0) rep_out("verdict: %s\n", why);
}
