/* trend.txt in the app folder: one line per full SMART read, oldest first,
 * under a first line "drive <model>" (the IDENTIFY model: another drive
 * starts a new file). A line holds the local date and time, the power-on
 * hours, the drive temperature in C, the raw value of the host-writes
 * attribute, the life left in % and the sum of the error counters
 * (smart_error_sum), apart by one space; -1 is "not reported":
 *     2026-10-07 19:52 934 36 72607 100 0
 * Pure C, no PS3 headers: test/test_smart.c builds this file on the Mac. */
#ifndef TREND_H
#define TREND_H

#include <time.h>
#include "smart.h"

typedef struct {
    int year, month, day, hour, minute;      /* the local time of the read */
    int hours, temp, life;                   /* power-on hours, drive temperature in C, life left in %; -1 not reported */
    long long writes, errors;                /* the raw host-writes value (-1: no such attribute), the error counter sum */
} trend_rec;

/* The line of one read, without the line end: its length, as snprintf. */
int trend_line(char *out, int n, const struct tm *when, const smart_data *s, const smart_summary *sum);
/* A line back into rec: 0, or -1 when it is no trend line (the header, a
 * blank or cut line, two lines joined, an extra field, a value out of
 * range). One CR at the end is allowed. */
int trend_parse(const char *line, trend_rec *rec);

/* SSD life left as a time: a straight line through the life left of the
 * reads (least squares over the days, one point a day: the last read of
 * each day), once the reads span TREND_ETA_DAYS and life left dropped by a
 * point or more from the first read to the last. The months to 0 % from the
 * last read at that rate, at most TREND_ETA_CAP (then "over 10 years"). The
 * dates come from the console clock: a read dated before an earlier one
 * (the clock was set back) is left out, and when the last read is such a
 * read there is no estimate (TREND_ETA_CLOCK). A rough guide: wear is not
 * linear, and the life attribute counts in whole points. */
#define TREND_ETA_DAYS 14
#define TREND_ETA_CAP 120                    /* months */
enum { TREND_ETA_OK, TREND_ETA_OVER, TREND_ETA_NO_LIFE, TREND_ETA_SHORT, TREND_ETA_NO_DROP, TREND_ETA_ZERO, TREND_ETA_CLOCK };
typedef struct {
    int state;                               /* TREND_ETA_* */
    int months;                              /* OK: the months to 0 % */
    int days, drop, reads;                   /* the days from the first read with a life value to the last, the points lost, those reads */
} trend_eta;
void trend_life_eta(const trend_rec *r, int n, trend_eta *e);
/* The estimate, or why there is none, in one line: its length, as snprintf.
 *     about 31 months to 0 % (3 points in 92 days)
 *     no estimate yet: 9 days of reads, it needs 14
 *     no estimate: the last read is dated before an earlier one (clock set back?) */
int trend_eta_text(const trend_eta *e, char *out, int n);

#endif
