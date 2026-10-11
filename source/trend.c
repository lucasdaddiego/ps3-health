/* The line format of trend.txt (trend.h). Only %d and %lld: the PPU newlib
 * reads and writes long long (_WANT_IO_LONG_LONG), and its C99 formats are
 * off. */
#include <stdio.h>
#include "trend.h"

int trend_line(char *out, int n, const struct tm *when, const smart_data *s, const smart_summary *sum)
{
    return snprintf(out, n, "%04d-%02d-%02d %02d:%02d %d %d %lld %d %lld", when->tm_year + 1900, when->tm_mon + 1,
                    when->tm_mday, when->tm_hour, when->tm_min, sum->hours < 0 ? -1 : sum->hours, s->temperature,
                    sum->writes_id ? (long long)sum->writes_raw : -1LL, sum->life_left, smart_error_sum(s));
}

/* Ten values, and the line ends after the last one. The year must have its
 * four digits: a line cut inside the year ("26-10-07 ...") reads as a date
 * otherwise. */
int trend_parse(const char *line, trend_rec *rec)
{
    trend_rec r;
    int year_end = -1, end = -1;
    if (sscanf(line, "%4d%n-%2d-%2d %2d:%2d %d %d %lld %d %lld%n", &r.year, &year_end, &r.month, &r.day, &r.hour,
               &r.minute, &r.hours, &r.temp, &r.writes, &r.life, &r.errors, &end) != 10)
        return -1;
    if (year_end != 4 || end < 0) return -1;
    const char *rest = line + end;
    if (*rest == '\r') rest++;
    if (*rest) return -1;
    if (r.month < 1 || r.month > 12 || r.day < 1 || r.day > 31 || r.hour < 0 || r.hour > 23 || r.minute < 0 ||
        r.minute > 59 || r.hours < -1 || r.temp < -1 || r.temp > 255 || r.writes < -1 || r.life < -1 || r.life > 100 ||
        r.errors < 0)
        return -1;
    *rec = r;
    return 0;
}
