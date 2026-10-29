/* The pure C side of the Clock module: the NTP client packet and the
 * server's reply (RFC 5905, modes 3 and 4), the offset and the round trip of
 * one exchange, the drift between two results, the lines of clock.txt and
 * the verdict. No PS3 headers: test/test_smart.c builds this file on the
 * Mac. */
#ifndef NTP_H
#define NTP_H

#include <stdint.h>

#define NTP_PACKET 48
#define NTP_PORT 123
#define NTP_ERA 2208988800ull                /* 1900 to 1970 in seconds: an NTP timestamp minus this is Unix time */

typedef int64_t ntp_us;                      /* a time as microseconds since 1970, UTC */

/* The client packet (no leap, version 4, mode 3) with t1 as its transmit
 * time, 48 bytes into out: 0, or -1 when n is short. The reply carries the
 * same 8 bytes as its originate time, which ntp_parse checks. */
int ntp_request(uint8_t *out, int n, ntp_us t1);

typedef struct {
    int version, mode, leap, stratum;
    int precision;                           /* log2 seconds, signed */
    ntp_us t2, t3;                           /* the server's receive and transmit times, Unix microseconds */
    char ref[5];                             /* the reference id as text: a stratum 1 source ("GPS "), or a kiss code at stratum 0 ("RATE") */
} ntp_reply;
/* The reply in in: 0 and r filled; -1 when in is not a server's answer to
 * the request sent (short, not mode 4, its originate time not the request's
 * transmit time, a zero transmit time); -2 for a kiss-o'-death (stratum 0:
 * r->ref has the code, the server wants no more packets). */
int ntp_parse(const uint8_t *in, int n, const uint8_t *sent, ntp_reply *r);

/* One exchange: offset = the server's clock minus the console's, round trip
 * = the time on the wire (RFC 5905 section 8). t1 send, t2 and t3 from the
 * reply, t4 receive. */
typedef struct { ntp_us offset_us, rtt_us; } ntp_sample;
void ntp_sample_calc(ntp_us t1, ntp_us t2, ntp_us t3, ntp_us t4, ntp_sample *s);
int ntp_best(const ntp_sample *s, int n);    /* the sample with the shortest round trip; -1 when n is 0 */

/* The drift between two results: the console's clock ran then_console to
 * now_console while the true time ran that span plus the change of the
 * offset. ppm > 0: the console's clock runs fast. 0 and nothing when the
 * span is under NTP_DRIFT_MIN_S or not forward. */
#define NTP_DRIFT_MIN_S 600
#define NTP_DRIFT_MAX_PPM 200.0              /* past it the clock was set between the runs (a quartz clock stays under 50 ppm) */
int ntp_drift(ntp_us then_console, ntp_us then_offset, ntp_us now_console, ntp_us now_offset, double *ppm, double *ms_per_day);

/* clock.txt, one line per result: "<console utc seconds> <offset ms> <round trip ms> <server>". */
typedef struct {
    int64_t console_s;
    int offset_ms, rtt_ms;
    char server[64];
} ntp_line;
int ntp_line_format(char *out, int n, const ntp_line *l);   /* the line with its newline: its length, or -1 when it does not fit */
int ntp_line_parse(const char *s, ntp_line *l);             /* one line (with or without its newline): 0, or -1 when it is not one */

/* The verdict's limits and the reset mark: no PS3 firmware is older than
 * 2010, so a clock before it was reset (a flat RTC battery starts it at
 * 2000). */
#define NTP_OFF_CHECK_US 2000000ll           /* 2 s: check */
#define NTP_OFF_BAD_US 60000000ll            /* 60 s: problem, set the clock */
#define NTP_RESET_S 1262304000ll             /* 2010-01-01 00:00 UTC */
/* 0 ok, 1 check, 2 problem, with the reason in why: the clock before
 * NTP_RESET_S (problem), no answer (have 0: check), the offset past a limit. */
int ntp_judge(int have, ntp_us offset_us, int64_t console_s, char *why, int n);

/* "+0.31 s", "-2 min 3 s", "+3 h 2 min", "+12 days": a signed offset, two
 * units at most. */
void ntp_offset_text(ntp_us us, char *out, int n);
/* "1 h 23 min", "3 days 2 h", "45 s": a span, two units at most. */
void ntp_span_text(int64_t sec, char *out, int n);
int64_t ntp_civil_s(int y, int m, int d);    /* 00:00 UTC of a date as Unix seconds (proleptic Gregorian) */
/* The target of a clock set: the console's clock now plus the offset, as the
 * seconds and nanoseconds sys_time_set_current_time takes; a target before
 * 1970 is 0. */
void ntp_set_target(ntp_us now_us, ntp_us offset_us, uint64_t *sec, uint64_t *nsec);

#endif
