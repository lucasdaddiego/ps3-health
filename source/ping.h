/* The pure C side of the Network module: the ICMP echo packet, the ping
 * statistics and the verdict. Pure C, no PS3 headers: test/test_smart.c
 * builds this file on the Mac. */
#ifndef PING_H
#define PING_H

#include <stdint.h>

#define PING_PAYLOAD 32                      /* bytes after the 8-byte ICMP header */

uint16_t ping_csum(const uint8_t *p, int n);   /* the Internet checksum (RFC 1071), as it goes on the wire */
/* An echo request (type 8) with id, seq and a payload of PING_PAYLOAD bytes:
 * its length, or -1 when out is too short. The fields are written byte by
 * byte in network order, so the host's order does not matter. */
int ping_request(uint8_t *out, int n, uint16_t id, uint16_t seq);
/* 1 when in is the echo reply (type 0) to id and seq, with or without the
 * IPv4 header in front (a raw socket gives it on BSD stacks). */
int ping_is_reply(const uint8_t *in, int n, uint16_t id, uint16_t seq);

/* The times of a run in microseconds, -1 for a lost one. Jitter: the mean
 * difference between two answers in a row (RFC 3550's idea, without its
 * smoothing). The values are 0 when nothing came back. */
typedef struct {
    int sent, got, loss_pct;
    int min_us, max_us, avg_us, jitter_us;
} ping_stats;
void ping_calc(const int *us, int n, ping_stats *s);

/* What the verdict reads. -1: not known (rssi on a cable, dns not run). */
typedef struct {
    int have_ip;                             /* the console has an address */
    int wired;                               /* 1 cable, 0 Wi-Fi */
    int link_type;                           /* netctl's link type on a cable: 1 to 6 (10 half to 1000 full), 0 auto */
    int rssi;                                /* Wi-Fi signal in % (netctl), -1 */
    int dns_ms;                              /* the lookup time, -1 not run, -2 failed */
    ping_stats ping;                         /* sent 0: no ping ran */
} net_facts;
/* 0 ok, 1 check, 2 problem, and the reason (the worst first) in why. */
int ping_judge(const net_facts *f, char *why, int n);
const char *ping_link_text(int link_type);   /* "1000 Mbit/s full duplex", "auto" */

#endif
