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

#endif
