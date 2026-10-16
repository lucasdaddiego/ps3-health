/* The pure C side of the Cooling module: the console families and their
 * ranges, the paste verdict, the rise in the first minute of a load test, and
 * the halving of a long curve. Pure C, no PS3 headers: test/test_smart.c
 * builds this file on the Mac. */
#ifndef HEAT_H
#define HEAT_H

/* The console family, asked once in Cooling and kept in APP_DIR/family.txt
 * as its key. FAMILY_UNKNOWN: the question was skipped. */
enum { FAMILY_NONE, FAMILY_FAT, FAMILY_SLIM, FAMILY_SUPER, FAMILY_UNKNOWN };
typedef struct {
    const char *key, *name, *models;
    int idle_max;                            /* C: the hotter chip before the load */
    int peak_max;                            /* C: the hotter chip at its peak */
    int rise_max;                            /* C a minute: the hotter chip's rise over the first 60 s */
    int fan_max;                             /* %: the fan duty at the end of the load */
} heat_family;
const heat_family *heat_family_get(int family);   /* NULL for FAMILY_NONE and FAMILY_UNKNOWN */
int heat_family_parse(const char *s);             /* "slim\n" -> FAMILY_SLIM; FAMILY_NONE when it is no key */

/* The rise of one curve over its first 60 s in C a minute: the first read
 * value against the value at 60 s (step_s seconds between two samples). -1
 * when the curve is shorter or a value is missing. */
int heat_rise(const short *v, int n, int step_s);

/* The paste verdict: the four values of a load test against the family's
 * ranges. -1 in a value: not measured, not counted. */
enum { PASTE_NONE, PASTE_OK, PASTE_CHECK, PASTE_LIKELY };
typedef struct {
    int family, idle, peak, rise, fan;       /* the inputs */
    int over;                                /* bit 0 idle, 1 peak, 2 rise, 3 fan: above the range */
    int measured;                            /* how many of the four were measured */
    int verdict;                             /* PASTE_*: NONE with no family, or without idle, peak and rise */
} heat_paste;
void heat_paste_judge(int family, int idle, int peak, int rise, int fan, heat_paste *p);
const char *heat_paste_text(const heat_paste *p);   /* one sentence for the screen and the report */

/* A curve over its buffer: each pair of samples becomes its larger value (a
 * peak stays), -1 only when both are -1. Returns the new count, (n + 1) / 2. */
int heat_halve(short *v, int n);

#endif
