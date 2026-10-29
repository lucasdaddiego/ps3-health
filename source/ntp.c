/* The NTP client of the Clock module, pure C: see ntp.h. */
#include <stdio.h>
#include <string.h>
#include "ntp.h"

/* ---- timestamps ------------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Unix microseconds to the 64-bit NTP timestamp: seconds since 1900, then
 * a 32-bit fraction. Era 0 only (1900 to 2036): the console's clock is
 * there. */
static void put_ts(uint8_t *p, ntp_us us)
{
    int64_t sec = us / 1000000, frac = us % 1000000;
    if (frac < 0) { frac += 1000000; sec--; }
    put32(p, (uint32_t)(sec + (int64_t)NTP_ERA));
    put32(p + 4, (uint32_t)((frac << 32) / 1000000));
}

/* An NTP timestamp to Unix microseconds. Seconds under NTP_ERA are era 1
 * (after 2036-02-07): 2^32 is added. 0 stays 0: "not set". */
static ntp_us get_ts(const uint8_t *p)
{
    uint32_t sec = get32(p), frac = get32(p + 4);
    if (!sec && !frac) return 0;
    int64_t s = (int64_t)sec - (int64_t)NTP_ERA;
    if (sec < NTP_ERA) s += 1ll << 32;
    return s * 1000000 + (int64_t)(((uint64_t)frac * 1000000) >> 32);
}

/* ---- the packets ----------------------------------------------------------- */

int ntp_request(uint8_t *out, int n, ntp_us t1)
{
    if (n < NTP_PACKET) return -1;
    memset(out, 0, NTP_PACKET);
    out[0] = 0x23;                           /* leap 0, version 4, mode 3 (client) */
    put_ts(out + 40, t1);
    return 0;
}

int ntp_parse(const uint8_t *in, int n, const uint8_t *sent, ntp_reply *r)
{
    memset(r, 0, sizeof *r);
    if (n < NTP_PACKET) return -1;
    r->leap = in[0] >> 6;
    r->version = (in[0] >> 3) & 7;
    r->mode = in[0] & 7;
    r->stratum = in[1];
    r->precision = (int8_t)in[3];
    if (r->mode != 4 || r->version < 1 || r->version > 4) return -1;
    if (memcmp(in + 24, sent + 40, 8)) return -1;   /* the originate time must be our transmit time */
    if (r->stratum == 0 || r->stratum == 1) {        /* a kiss code, or the name of a stratum 1 source */
        for (int k = 0; k < 4; k++) {
            char c = (char)in[12 + k];
            r->ref[k] = c >= 32 && c < 127 ? c : c == 0 ? 0 : '?';
        }
        r->ref[4] = 0;
    }
    if (r->stratum == 0) return -2;
    r->t2 = get_ts(in + 32);
    r->t3 = get_ts(in + 40);
    if (!r->t3) return -1;
    return 0;
}

/* ---- the numbers ----------------------------------------------------------- */

void ntp_sample_calc(ntp_us t1, ntp_us t2, ntp_us t3, ntp_us t4, ntp_sample *s)
{
    s->offset_us = ((t2 - t1) + (t3 - t4)) / 2;
    s->rtt_us = (t4 - t1) - (t3 - t2);
    if (s->rtt_us < 0) s->rtt_us = 0;
}

int ntp_best(const ntp_sample *s, int n)
{
    int b = -1;
    for (int k = 0; k < n; k++)
        if (b < 0 || s[k].rtt_us < s[b].rtt_us) b = k;
    return b;
}

int ntp_drift(ntp_us then_console, ntp_us then_offset, ntp_us now_console, ntp_us now_offset, double *ppm, double *ms_per_day)
{
    *ppm = *ms_per_day = 0;
    int64_t span = now_console - then_console;
    if (span < (int64_t)NTP_DRIFT_MIN_S * 1000000) return 0;
    double truth = (double)span + (double)(now_offset - then_offset);   /* the true time that passed */
    if (truth <= 0) return 0;
    double fast = -(double)(now_offset - then_offset) / truth;          /* the offset grows when the console runs slow */
    *ppm = fast * 1e6;
    *ms_per_day = fast * 86400000.0;
    return 1;
}

/* ---- clock.txt ------------------------------------------------------------- */

int ntp_line_format(char *out, int n, const ntp_line *l)
{
    int j = snprintf(out, n, "%lld %d %d %s\n", (long long)l->console_s, l->offset_ms, l->rtt_ms, l->server);
    return j < 0 || j >= n ? -1 : j;
}

int ntp_line_parse(const char *s, ntp_line *l)
{
    long long sec;
    int off, rtt, used = 0;
    memset(l, 0, sizeof *l);
    if (sscanf(s, "%lld %d %d %63s%n", &sec, &off, &rtt, l->server, &used) != 4 || used == 0) return -1;
    if (sec < 0 || rtt < 0) return -1;
    for (const char *p = s + used; *p; p++)
        if (*p != ' ' && *p != '\r' && *p != '\n') return -1;
    l->console_s = sec;
    l->offset_ms = off;
    l->rtt_ms = rtt;
    return 0;
}

/* ---- the verdict and the texts --------------------------------------------- */

int ntp_judge(int have, ntp_us offset_us, int64_t console_s, char *why, int n)
{
    char t[32];
    if (console_s < NTP_RESET_S) {
        snprintf(why, n, "the clock was reset, it is before 2010: set the date and time in the system settings");
        return 2;
    }
    if (!have) {
        snprintf(why, n, "no answer from the time server, so the clock could not be checked");
        return 1;
    }
    ntp_offset_text(offset_us, t, sizeof t);
    int64_t a = offset_us < 0 ? -offset_us : offset_us;
    if (a > NTP_OFF_BAD_US) {
        snprintf(why, n, "the clock is %s off: set the date and time in the system settings", t + 1);
        return 2;
    }
    if (a > NTP_OFF_CHECK_US) {
        snprintf(why, n, "the clock is %s off the time server", t + 1);
        return 1;
    }
    snprintf(why, n, "the clock is within %s of the time server", t + 1);
    return 0;
}

void ntp_offset_text(ntp_us us, char *out, int n)
{
    char sign = us < 0 ? '-' : '+';
    int64_t a = us < 0 ? -us : us;
    if (a < 60000000) snprintf(out, n, "%c%.2f s", sign, (double)a / 1e6);
    else if (a < 3600000000ll) snprintf(out, n, "%c%lld min %lld s", sign, (long long)(a / 60000000), (long long)(a / 1000000 % 60));
    else if (a < 86400000000ll) snprintf(out, n, "%c%lld h %lld min", sign, (long long)(a / 3600000000ll), (long long)(a / 60000000 % 60));
    else snprintf(out, n, "%c%lld days", sign, (long long)(a / 86400000000ll));
}

void ntp_span_text(int64_t sec, char *out, int n)
{
    if (sec < 0) sec = 0;
    if (sec < 60) snprintf(out, n, "%lld s", (long long)sec);
    else if (sec < 3600) snprintf(out, n, "%lld min %lld s", (long long)(sec / 60), (long long)(sec % 60));
    else if (sec < 86400) snprintf(out, n, "%lld h %lld min", (long long)(sec / 3600), (long long)(sec / 60 % 60));
    else snprintf(out, n, "%lld day%s %lld h", (long long)(sec / 86400), sec / 86400 == 1 ? "" : "s", (long long)(sec / 3600 % 24));
}

/* Days from 1970-01-01 of a date, the civil algorithm of Howard Hinnant. */
int64_t ntp_civil_s(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400;
}
