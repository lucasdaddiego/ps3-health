/* The pure C side of the Cooling module: the console families and their
 * ranges, the idle warning, the warm start, the paste verdict, the rise in
 * the first minute of a load test, the halving of a long curve and the
 * Cooling line of the compatibility report. Pure C, no PS3 headers:
 * test/test_smart.c builds this file on the Mac. */
#ifndef HEAT_H
#define HEAT_H

/* The console family, asked once in Cooling and kept in APP_DIR/family.txt
 * as its key. FAMILY_UNKNOWN: the question was skipped. */
enum { FAMILY_NONE, FAMILY_FAT, FAMILY_SLIM, FAMILY_SUPER, FAMILY_UNKNOWN };
typedef struct {
    const char *key, *name, *models;
    int idle_max;                            /* C: the hotter chip before the load */
    int peak_max;                            /* C: the hotter chip at its peak in the first HEAT_PEAK_S */
    int rise_max;                            /* C a minute: the hotter chip's rise over the first 60 s */
    int fan_max;                             /* %: the fan duty at the end of the load */
} heat_family;
const heat_family *heat_family_get(int family);   /* NULL for FAMILY_NONE and FAMILY_UNKNOWN */
int heat_family_parse(const char *s);             /* "slim\n" -> FAMILY_SLIM; FAMILY_NONE when it is no key */

/* The idle warning: the hotter chip more than HEAT_IDLE_MARGIN above the
 * family's idle range, or at HEAT_IDLE_WARN and more with no family set. 1
 * when t is warm at rest; t -1 (not read) is not. */
#define HEAT_IDLE_MARGIN 3
#define HEAT_IDLE_WARN 70
int heat_idle_hot(int family, int t);

/* A warm start: a load test that begins less than HEAT_REST_S after the end
 * of the last load in this session (since_s; -1: no load yet), or more than
 * HEAT_WARM_C above the reading taken when the module opened (open_c and
 * start_c: the hotter chip; -1 not read). Its idle and rise say little about
 * the paste: they leave the verdict. */
#define HEAT_REST_S 600
#define HEAT_WARM_C 3
int heat_warm_start(long since_s, int open_c, int start_c);

/* The rise of one curve over its first 60 s in C a minute: the first read
 * value against the value at 60 s (step_s seconds between two samples). -1
 * when the curve is shorter or a value is missing. */
int heat_rise(const short *v, int n, int step_s);

/* The paste verdict: the four values of a load test against the family's
 * ranges. -1 in a value: not measured, not counted. peak: the hotter chip's
 * peak in the first HEAT_PEAK_S, as the ranges were measured. flags:
 * HEAT_WARM, a warm start (idle and rise not counted: the verdict on the
 * peak and the fan); HEAT_HOT_EARLY, the test reached its stop before 60 s
 * (likely, whatever the other values). */
#define HEAT_PEAK_S 120
#define HEAT_WARM 1
#define HEAT_HOT_EARLY 2
enum { PASTE_NONE, PASTE_OK, PASTE_CHECK, PASTE_LIKELY };
typedef struct {
    int family, idle, peak, rise, fan, flags;   /* the inputs */
    int over;                                /* bit 0 idle, 1 peak, 2 rise, 3 fan: above the range (a warm start: never idle and rise) */
    int verdict;                             /* PASTE_*: NONE with no family, or without idle, peak and rise (a minute of test) */
} heat_paste;
void heat_paste_judge(int family, int idle, int peak, int rise, int fan, int flags, heat_paste *p);
const char *heat_paste_text(const heat_paste *p);   /* one sentence for the screen and the report */

/* A curve over its buffer: each pair of samples becomes its larger value (a
 * peak stays), -1 only when both are -1. Returns the new count, (n + 1) / 2. */
int heat_halve(short *v, int n);

/* The Cooling line of the compatibility report (the QR code on Help): the
 * readings that can set the ranges, no identifier. detail 1: every field;
 * 0: the family, idle, peak and rise alone, for a link near the QR code's
 * limit. A value of -1 is not read: left out. Its length, cut at n - 1:
 *     Super slim, Cell/RSX C, idle 58/55, peak 67/62, rise 9/min, fan 25>33%, load 120 s, 6 SPUs */
typedef struct {
    int family;
    int cpu0, rsx0;                          /* C at the start */
    int cpu_peak, rsx_peak;                  /* C: the peak in the first HEAT_PEAK_S */
    int cpu_max, rsx_max;                    /* C: the peak of the whole test */
    int rise;                                /* C a minute, the hotter chip */
    int fan0, fan_end;                       /* % */
    int soak, seconds, stopped, spus, warm;  /* a soak, the test's seconds, it stopped before its end, the six SPUs ran, a warm start */
} heat_report;
int heat_compat_line(const heat_report *r, int detail, char *out, int n);

#endif
