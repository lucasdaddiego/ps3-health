/* ping.h: the echo packet, the statistics and the verdict. */
#include <stdarg.h>
#include <stdio.h>
#include "ping.h"

uint16_t ping_csum(const uint8_t *p, int n)
{
    uint32_t sum = 0;
    for (int i = 0; i + 1 < n; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) sum += (uint32_t)(p[n - 1] << 8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

int ping_request(uint8_t *out, int n, uint16_t id, uint16_t seq)
{
    int len = 8 + PING_PAYLOAD;
    if (n < len) return -1;
    out[0] = 8;                              /* echo request */
    out[1] = 0;
    out[2] = out[3] = 0;                     /* the checksum, below */
    out[4] = (uint8_t)(id >> 8), out[5] = (uint8_t)id;
    out[6] = (uint8_t)(seq >> 8), out[7] = (uint8_t)seq;
    for (int i = 0; i < PING_PAYLOAD; i++) out[8 + i] = (uint8_t)('a' + i % 26);
    uint16_t c = ping_csum(out, len);
    out[2] = (uint8_t)(c >> 8), out[3] = (uint8_t)c;
    return len;
}

int ping_is_reply(const uint8_t *in, int n, uint16_t id, uint16_t seq)
{
    int off = 0;
    if (n >= 20 && (in[0] >> 4) == 4) {      /* an IPv4 header first: its length, then ICMP */
        off = (in[0] & 15) * 4;
        if (off < 20 || in[9] != 1) return 0;   /* protocol 1: ICMP */
    }
    if (n - off < 8) return 0;
    const uint8_t *p = in + off;
    return p[0] == 0 && p[1] == 0 && (p[4] << 8 | p[5]) == id && (p[6] << 8 | p[7]) == seq;
}

void ping_calc(const int *us, int n, ping_stats *s)
{
    long long sum = 0, jsum = 0;
    int prev = -1, pairs = 0;
    s->sent = n;
    s->got = s->min_us = s->max_us = s->avg_us = s->jitter_us = 0;
    for (int i = 0; i < n; i++) {
        if (us[i] < 0) { prev = -1; continue; }
        if (!s->got || us[i] < s->min_us) s->min_us = us[i];
        if (us[i] > s->max_us) s->max_us = us[i];
        sum += us[i];
        s->got++;
        if (prev >= 0) { jsum += us[i] > prev ? us[i] - prev : prev - us[i]; pairs++; }
        prev = us[i];
    }
    s->loss_pct = n ? (n - s->got) * 100 / n : 0;
    if (s->got) s->avg_us = (int)(sum / s->got);
    if (pairs) s->jitter_us = (int)(jsum / pairs);
}

const char *ping_link_text(int link_type)
{
    static const char *const t[] = {"auto", "10 Mbit/s half duplex", "10 Mbit/s full duplex", "100 Mbit/s half duplex",
                                    "100 Mbit/s full duplex", "1000 Mbit/s half duplex", "1000 Mbit/s full duplex"};
    return link_type >= 0 && link_type <= 6 ? t[link_type] : "unknown";
}

/* The reason into why, and the level. */
static int say(char *why, int n, int level, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int say(char *why, int n, int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
    return level;
}

int ping_judge(const net_facts *f, char *why, int n)
{
    const ping_stats *p = &f->ping;
    if (!f->have_ip) return say(why, n, 2, "no address (no cable or Wi-Fi link, or no DHCP answer)");
    if (p->sent && !p->got) return say(why, n, 2, "the router did not answer one ping");
    if (p->sent && p->loss_pct > PING_LOSS_CHECK) return say(why, n, 1, "%d %% of the pings to the router were lost", p->loss_pct);
    if (!f->wired && f->rssi >= 0 && f->rssi < PING_RSSI_CHECK) return say(why, n, 1, "weak Wi-Fi signal (%d %%)", f->rssi);
    if (f->wired && f->link_type >= 1 && f->link_type <= 5)
        return say(why, n, 1, "the cable link is %s, the PS3 port does 1000 Mbit/s full duplex", ping_link_text(f->link_type));
    if (p->got && p->avg_us > PING_AVG_CHECK_US) return say(why, n, 1, "slow router answers (%d ms on average)", p->avg_us / 1000);
    if (p->got > 1 && p->jitter_us > PING_JITTER_CHECK_US) return say(why, n, 1, "uneven router answers (%d ms of jitter)", p->jitter_us / 1000);
    if (f->dns_ms == -2) return say(why, n, 1, "the DNS lookup failed");
    if (f->dns_ms > PING_DNS_CHECK_MS) return say(why, n, 1, "slow DNS lookup (%d ms)", f->dns_ms);
    return say(why, n, 0, "%s", f->wired ? "the cable link and the router look fine" : "the Wi-Fi link and the router look fine");
}
