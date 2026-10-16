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

/* Days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil),
 * with the time of day as a fraction. */
static double day_number(const trend_rec *r)
{
    int y = r->year - (r->month <= 2), era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400, mp = (r->month + 9) % 12;
    int doy = (153 * mp + 2) / 5 + r->day - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097.0 + doe - 719468 + (r->hour * 60 + r->minute) / 1440.0;
}

void trend_life_eta(const trend_rec *r, int n, trend_eta *e)
{
    int first = -1, last = -1, k = 0;
    double sx = 0, sy = 0, sxx = 0, sxy = 0, d0 = 0;
    e->state = TREND_ETA_NO_LIFE;
    e->months = e->days = e->drop = e->reads = 0;
    for (int i = 0; i < n; i++) {
        if (r[i].life < 0) continue;
        if (first < 0) { first = i; d0 = day_number(&r[i]); }
        last = i;
        double x = day_number(&r[i]) - d0, y = r[i].life;
        sx += x, sy += y, sxx += x * x, sxy += x * y;
        k++;
    }
    e->reads = k;
    if (k == 0) return;
    e->days = (int)(day_number(&r[last]) - d0);
    e->drop = r[first].life - r[last].life;
    if (r[last].life == 0) { e->state = TREND_ETA_ZERO; return; }
    if (e->days < TREND_ETA_DAYS) { e->state = TREND_ETA_SHORT; return; }
    double den = k * sxx - sx * sx, slope = den > 0 ? (k * sxy - sx * sy) / den : 0;   /* points a day */
    if (e->drop < 1 || slope >= 0) { e->state = TREND_ETA_NO_DROP; return; }
    double months = r[last].life / -slope / 30.44;
    if (months > TREND_ETA_CAP) { e->state = TREND_ETA_OVER; return; }
    e->state = TREND_ETA_OK;
    e->months = (int)(months + 0.5);
}

int trend_eta_text(const trend_eta *e, char *out, int n)
{
    switch (e->state) {
    case TREND_ETA_OK:
        if (e->months < 1) return snprintf(out, n, "under a month to 0 %% (%d points in %d days)", e->drop, e->days);
        return snprintf(out, n, "about %d month%s to 0 %% (%d point%s in %d days)", e->months, e->months == 1 ? "" : "s", e->drop,
                        e->drop == 1 ? "" : "s", e->days);
    case TREND_ETA_OVER: return snprintf(out, n, "over 10 years to 0 %% (%d point%s in %d days)", e->drop, e->drop == 1 ? "" : "s", e->days);
    case TREND_ETA_NO_LIFE: return snprintf(out, n, "no estimate: no read with a life value");
    case TREND_ETA_SHORT:
        if (e->reads == 1) return snprintf(out, n, "no estimate yet: one read, it needs %d days of reads", TREND_ETA_DAYS);
        return snprintf(out, n, "no estimate yet: %d day%s of reads, it needs %d", e->days, e->days == 1 ? "" : "s", TREND_ETA_DAYS);
    case TREND_ETA_NO_DROP: return snprintf(out, n, "no estimate yet: life left has not dropped in %d days", e->days);
    default: return snprintf(out, n, "no estimate: life left is at 0 %%");
    }
}
