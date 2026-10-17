/* The Network module: the connection as the system's netctl library reports
 * it (cable or Wi-Fi, the link speed and duplex, the Wi-Fi signal and
 * channel, the MTU, the addresses of the console, the router and the DNS
 * servers; never the MAC address), 20 pings to the router, the time of one
 * DNS lookup, and a verdict (ping.c). A ping is an ICMP echo on a raw socket,
 * one journaled call for the whole run; when the system refuses the raw
 * socket, the router does not answer it, or there is no journal, the ping is
 * the time of a TCP connect to the router's port 80 (or 53): its answer, a
 * connection or a refusal, is one round trip too. The screen says which.
 * The network starts as in the Transfer module (net_init, mod_net.c). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ppu-lv2.h>
#include <lv2/systime.h>
#include <sys/systime.h>
#include <net/net.h>
#include <net/netctl.h>
#include <net/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "app.h"
#include <ps3gfx/ui.h>
#include "report.h"
#include "ping.h"

#define PINGS 20                             /* CROSS */
#define QUICK_PINGS 5                        /* the quick check */
#define PING_GAP_US 200000                   /* between two pings */
#define PING_WAIT_MS 1000                    /* a ping with no answer in this time is lost */
#define ICMP_TRIES 3                         /* no ICMP answer in the first three: the TCP connect instead */
#define PING_ID 0x5048                       /* "PH" */
#define DNS_NAME "example.com"               /* a name kept for examples by IANA (RFC 2606): stable, and on every DNS server */

static struct {
    int done;                                /* read once */
    int frozen;                              /* the reads froze this console before (net_info): not made */
    s32 rc;                                  /* the device read's rc: netctl not started or refused; -1 not started or frozen */
    int device, link, link_type, mtu, rssi, channel, ip_config;   /* -1: not read */
    char ip[16], mask[16], router[16], dns1[16], dns2[16];
} info;
enum { MODE_NONE, MODE_ICMP, MODE_TCP };
static struct {
    int done, mode, port, n, cancelled;
    int us[PINGS];                           /* microseconds, -1 lost */
    ping_stats st;
    char why_tcp[80];                        /* why the TCP connect and not ICMP */
} pg;
static struct { int done, ms; } dns;         /* ms: -2 failed */
static int level = -1;                       /* the verdict: 0 ok, 1 check, 2 problem, -1 none */
static char why[128];

/* ---- netctl --------------------------------------------------------------- */

static s32 get(s32 code, union net_ctl_info *i)
{
    memset(i, 0, sizeof *i);
    return netCtlGetInfo(code, i);
}

/* Each value on its own; a refused one stays -1 or empty. No read of the
 * MAC address (NET_CTL_INFO_ETHER_ADDR), the BSSID or the SSID. The reads
 * are one journaled call, net_info: reads that froze the console are not
 * made again. */
static void read_info(void)
{
    union net_ctl_info i;
    memset(&info, 0, sizeof info);
    info.device = info.link = info.link_type = info.mtu = info.rssi = info.channel = info.ip_config = -1;
    info.done = 1;
    info.rc = -1;
    if (!net_is_up()) return;
    if (!journal_start("net_info")) { info.frozen = 1; return; }   /* 0 for a frozen entry */
    info.rc = get(NET_CTL_INFO_DEVICE, &i);
    if (info.rc == 0) info.device = (int)i.device;
    if (get(NET_CTL_INFO_LINK, &i) == 0) info.link = (int)i.link;
    if (get(NET_CTL_INFO_LINK_TYPE, &i) == 0) info.link_type = (int)i.link_type;
    if (get(NET_CTL_INFO_MTU, &i) == 0) info.mtu = (int)i.mtu;
    if (info.device == NET_CTL_DEVICE_WIRELESS) {
        if (get(NET_CTL_INFO_RSSI, &i) == 0) info.rssi = i.rssi;
        if (get(NET_CTL_INFO_CHANNEL, &i) == 0) info.channel = i.channel;
    }
    if (get(NET_CTL_INFO_IP_CONFIG, &i) == 0) info.ip_config = (int)i.ip_config;
    if (get(NET_CTL_INFO_IP_ADDRESS, &i) == 0) snprintf(info.ip, sizeof info.ip, "%.15s", i.ip_address);
    if (get(NET_CTL_INFO_NETMASK, &i) == 0) snprintf(info.mask, sizeof info.mask, "%.15s", i.netmask);
    if (get(NET_CTL_INFO_DEFAULT_ROUTE, &i) == 0) snprintf(info.router, sizeof info.router, "%.15s", i.default_route);
    if (get(NET_CTL_INFO_PRIMARY_DNS, &i) == 0) snprintf(info.dns1, sizeof info.dns1, "%.15s", i.primary_dns);
    if (get(NET_CTL_INFO_SECONDARY_DNS, &i) == 0) snprintf(info.dns2, sizeof info.dns2, "%.15s", i.secondary_dns);
    journal_done("net_info", info.rc, info.rc == 0);
}

static int have_ip(void) { return info.ip[0] && strcmp(info.ip, "0.0.0.0"); }

/* ---- the pings ------------------------------------------------------------ */

static s64 now_us(void) { return sysGetSystemTime(); }

/* The errno of a failed socket call: -1 and net_errno, or a small negative errno. */
static int sock_err(int rc) { return rc == -1 || rc < -255 ? net_errno : -rc; }

static void addr_of(struct sockaddr_in *a, in_addr_t ip, int port)
{
    memset(a, 0, sizeof *a);
    a->sin_len = sizeof *a;
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    a->sin_addr.s_addr = ip;
}

/* One echo on the raw socket s: the microseconds to its reply, -1 lost. A
 * poll with the time left comes before each receive, so no receive waits
 * longer than the ping (the socket's own timeout is a second guard); a
 * packet that is not this reply (another ICMP message) is read and skipped. */
static int icmp_once(int s, in_addr_t ip, int seq)
{
    static uint8_t out[64], in[1500];
    struct sockaddr_in a, from;
    int len = ping_request(out, sizeof out, PING_ID, (uint16_t)seq);
    addr_of(&a, ip, 0);
    s64 t0 = now_us();
    if (netSendTo(s, out, len, 0, (struct sockaddr *)&a, sizeof a) != len) return -1;
    for (;;) {
        int left = PING_WAIT_MS - (int)((now_us() - t0) / 1000);
        struct pollfd p = {s, POLLIN, 0};
        if (left <= 0 || netPoll(&p, 1, left) <= 0) return -1;
        socklen_t fl = sizeof from;
        ssize_t r = netRecvFrom(s, in, sizeof in, 0, (struct sockaddr *)&from, &fl);
        if (r < 0) return -1;
        if (ping_is_reply(in, (int)r, PING_ID, (uint16_t)seq)) return (int)(now_us() - t0);
    }
}

/* One TCP connect to the router: the microseconds to its answer, a
 * connection or a refusal (RST); -1 when nothing came back in time. A
 * non-blocking connect, then a poll for the answer. */
static int tcp_once(in_addr_t ip, int port)
{
    struct sockaddr_in a;
    int s = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP), us = -1, one = 1;
    if (s < 0) return -1;
    netSetSockOpt(s, SOL_SOCKET, SO_NBIO, &one, sizeof one);
    addr_of(&a, ip, port);
    s64 t0 = now_us();
    int rc = netConnect(s, (struct sockaddr *)&a, sizeof a), e = rc < 0 ? sock_err(rc) : 0;
    if (rc == 0 || e == NET_ECONNREFUSED) us = (int)(now_us() - t0);
    else if (e == NET_EINPROGRESS || e == NET_EWOULDBLOCK) {
        struct pollfd p = {s, POLLOUT, 0};
        if (netPoll(&p, 1, PING_WAIT_MS) > 0) {
            int err = 0;
            socklen_t l = sizeof err;
            s64 t1 = now_us();
            if (netGetSockOpt(s, SOL_SOCKET, SO_ERROR, &err, &l) == 0 && (err == 0 || err == NET_ECONNREFUSED)) us = (int)(t1 - t0);
        }
    }
    netClose(s);
    return us;
}

static void gap(void)
{
    for (int k = 0; k < PING_GAP_US / 20000 && !job_cancel; k++) sysUsleep(20000);
}

static void ping_progress(int k, int count)
{
    static char msg[64];
    snprintf(msg, sizeof msg, "Ping %d of %d to the router %s", k + 1, count, info.router);
    progress(msg);
    job_set_percent(k * 100 / count);
}

/* The ICMP run: 1 when it ran to its end (or a cancel), 0 when TCP must take
 * over (why_tcp says why). One journaled call from the socket to its close. */
static int icmp_run(in_addr_t ip, int count)
{
    int js = journal_state("ping_icmp");
    if (!fs_ok) { snprintf(pg.why_tcp, sizeof pg.why_tcp, "no journal for ICMP"); return 0; }
    if (js == J_FROZEN) { snprintf(pg.why_tcp, sizeof pg.why_tcp, "ICMP froze this console before"); return 0; }
    if (!journal_start("ping_icmp")) { snprintf(pg.why_tcp, sizeof pg.why_tcp, "ICMP skipped by the journal"); return 0; }
    int s = netSocket(AF_INET, SOCK_RAW, IPPROTO_ICMP), got = 0, k;
    if (s < 0) {
        int e = sock_err(s);
        journal_done("ping_icmp", e, 0);
        snprintf(pg.why_tcp, sizeof pg.why_tcp, "ICMP socket refused (errno %d)", e);
        return 0;
    }
    struct timeval tv = {PING_WAIT_MS / 1000, 0};
    netSetSockOpt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    pg.mode = MODE_ICMP;
    for (k = 0; k < count && !job_cancel; k++) {
        ping_progress(k, count);
        pg.us[k] = icmp_once(s, ip, k + 1);
        if (pg.us[k] >= 0) got++;
        pg.n = k + 1;
        if (!got && k + 1 == ICMP_TRIES) break;
        if (k + 1 < count) gap();
    }
    netClose(s);
    journal_done("ping_icmp", 0, 1);
    if (got || job_cancel || k == count) return 1;
    snprintf(pg.why_tcp, sizeof pg.why_tcp, "no ICMP answer from the router");
    return 0;
}

/* The TCP run: port 80, else 53 when 80 gives nothing at the first ping. */
static void tcp_run(in_addr_t ip, int count)
{
    pg.mode = MODE_TCP;
    pg.port = 80;
    pg.n = 0;
    for (int k = 0; k < count && !job_cancel; k++) {
        ping_progress(k, count);
        pg.us[k] = tcp_once(ip, pg.port);
        if (k == 0 && pg.us[k] < 0 && (pg.us[k] = tcp_once(ip, 53)) >= 0) pg.port = 53;
        pg.n = k + 1;
        if (k + 1 < count) gap();
    }
}

static void ping_run(int count)
{
    memset(&pg, 0, sizeof pg);
    in_addr_t ip = info.router[0] ? inet_addr(info.router) : (in_addr_t)-1;
    if (ip == (in_addr_t)-1 || ip == 0) return;   /* no router address: no ping (pg.done stays 0) */
    if (!icmp_run(ip, count)) tcp_run(ip, count);
    pg.cancelled = job_cancel;
    ping_calc(pg.us, pg.n, &pg.st);
    pg.done = 1;
    job_set_percent(-1);
}

/* One lookup of DNS_NAME through the console's resolver, timed. Only the
 * answer's presence counts: the address list is not read. */
static void dns_run(void)
{
    progress("DNS lookup of " DNS_NAME);
    s64 t0 = now_us();
    struct net_hostent *he = netGetHostByName(DNS_NAME);
    dns.ms = he && he->h_addr_list ? (int)((now_us() - t0) / 1000) : -2;
    dns.done = 1;
}

static void judge(void)
{
    net_facts f;
    memset(&f, 0, sizeof f);
    f.have_ip = have_ip();
    f.wired = info.device != NET_CTL_DEVICE_WIRELESS;
    f.link_type = info.link_type;
    f.rssi = f.wired ? -1 : info.rssi;
    f.dns_ms = dns.done ? dns.ms : -1;
    if (pg.done) f.ping = pg.st;
    level = ping_judge(&f, why, sizeof why);
}

static void job_open(void)
{
    net_init();
    read_info();
}

static void job_test(void)
{
    net_init();
    read_info();
    memset(&pg, 0, sizeof pg);
    dns.done = 0;
    if (have_ip()) {
        ping_run(PINGS);
        if (!job_cancel) dns_run();
    }
    judge();
}

/* ---- tile, quick check ---------------------------------------------------- */

/* Why there is no connection data, for the tile and the quick check. */
static const char *no_info(void)
{
    return net_start_frozen() ? "Network start froze before" : info.frozen ? "Network info froze before" : !net_is_up() ? "Network not started"
                                                                                                         : "Connection not read";
}

static void link_line(char *s, int n)
{
    if (!info.done || info.rc) snprintf(s, n, "%s", no_info());
    else if (info.device == NET_CTL_DEVICE_WIRELESS) {
        if (info.channel >= 0 && info.rssi >= 0) snprintf(s, n, "Wi-Fi ch %d, signal %d %%", info.channel, info.rssi);
        else snprintf(s, n, "Wi-Fi");
    } else if (info.link == NET_CTL_LINK_DISCONNECTED) snprintf(s, n, "Wired, no link");
    else if (info.link_type >= 1 && info.link_type <= 6)   /* short: line 1 of a tile, "Wired, 1000 Mbit/s full" */
        snprintf(s, n, "Wired, %d Mbit/s %s", info.link_type <= 2 ? 10 : info.link_type <= 4 ? 100 : 1000, info.link_type % 2 ? "half" : "full");
    else snprintf(s, n, "Wired");
}

/* "router 0.6 ms, no loss, DNS 23 ms" */
static void ping_line(char *s, int n)
{
    int j;
    if (!have_ip()) { snprintf(s, n, "no address"); return; }
    if (!pg.done) j = snprintf(s, n, "no router ping");
    else if (!pg.st.got) j = snprintf(s, n, "router: no answer");
    else if (pg.st.loss_pct) j = snprintf(s, n, "router %.1f ms, %d%% loss", pg.st.avg_us / 1000.0, pg.st.loss_pct);
    else j = snprintf(s, n, "router %.1f ms, no loss", pg.st.avg_us / 1000.0);
    if (dns.done && j < n) {
        if (dns.ms >= 0) snprintf(s + j, n - j, ", DNS %d ms", dns.ms);
        else snprintf(s + j, n - j, ", DNS failed");
    }
}

static int level_dot(void) { return level == 0 ? DOT_OK : level == 1 ? DOT_WARN : DOT_BAD; }

static void network_tile(void)
{
    char l1[48], l2[48];
    if (level < 0) return;                   /* no test ran: the open's read alone sets no tile */
    link_line(l1, sizeof l1);
    ping_line(l2, sizeof l2);
    state_set("network", level_dot(), l1, l2);
}

/* A network that did not start (or froze before), or reads that froze
 * before: the step runs no test and gives no verdict, as the module. */
void network_quick(void)
{
    net_init();
    read_info();
    memset(&pg, 0, sizeof pg);
    dns.done = 0;
    if (!net_is_up() || info.frozen) { level = -1; return; }
    if (have_ip()) ping_run(QUICK_PINGS);
    judge();
}

int network_quick_end(char *line, int n)
{
    char a[48], b[64];
    if (level < 0) {                         /* no tile change, no dot */
        snprintf(line, n, "Not run: %s.", net_start_frozen() ? "the network start froze this console before"
                                          : info.frozen      ? "reading the connection data froze this console before"
                                                             : "the network did not start");
        return DOT_NONE;
    }
    link_line(a, sizeof a);
    ping_line(b, sizeof b);
    snprintf(line, n, "%s; %s", a, b);
    network_tile();
    return level_dot();
}

/* ---- screen --------------------------------------------------------------- */

static float row(float x, float y, float kw, float w, const char *key, u32 c, const char *fmt, ...) __attribute__((format(printf, 7, 8)));
static float row(float x, float y, float kw, float w, const char *key, u32 c, const char *fmt, ...)
{
    char v[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    text(x, y, F_BODY, GREY, "%s", key);
    text_fit(x + kw, y, F_BODY, c, w - kw, "%s", v);
    return y + 44;
}

static void draw_info(float x, float y, float w, float h)
{
    card(x, y, w, h);
    float cx = x + 32, cw = w - 64, kw = 200, bottom = y + h - 16;
    y = heading(cx, y + 22, "CONNECTION");
    if (!info.done || info.rc) {
        text_wrap(cx, y + 8, F_BODY, YELLOW, cw, 3,
                  net_start_frozen() ? "The network start froze this console before: no network call runs. SQUARE twice on the home screen clears the journal."
                  : info.frozen      ? "Reading the connection data froze this console before: not read again. SQUARE twice on the home screen clears the journal."
                  : net_is_up()      ? "The system did not give the connection data."
                                     : "The network did not start: no connection data.");
        return;
    }
    char s[64];
    y += 6;
    y = row(cx, y, kw, cw, "Device", WHITE, "%s", info.device == NET_CTL_DEVICE_WIRELESS ? "Wi-Fi (802.11b/g)" : info.device == NET_CTL_DEVICE_WIRED ? "Cable (Ethernet)" : "not read");
    if (info.device == NET_CTL_DEVICE_WIRELESS) {
        if (info.rssi >= 0) y = row(cx, y, kw, cw, "Signal", info.rssi < PING_RSSI_CHECK ? YELLOW : WHITE, "%d %% on channel %d", info.rssi, info.channel);
        else y = row(cx, y, kw, cw, "Signal", GREY, "not read");
    } else {
        if (info.link == NET_CTL_LINK_DISCONNECTED) y = row(cx, y, kw, cw, "Link", YELLOW, "no link: check the cable");
        else y = row(cx, y, kw, cw, "Link", info.link_type >= 1 && info.link_type <= 5 ? YELLOW : WHITE, "%s", ping_link_text(info.link_type));
    }
    if (info.mtu > 0) y = row(cx, y, kw, cw, "MTU", WHITE, "%d bytes", info.mtu);
    else y = row(cx, y, kw, cw, "MTU", GREY, "not read");
    y = row(cx, y, kw, cw, "Address", have_ip() ? WHITE : YELLOW, "%s%s", have_ip() ? info.ip : "none",
            info.ip_config == NET_CTL_IP_DHCP ? " (DHCP)" : info.ip_config == NET_CTL_IP_STATIC ? " (static)" : info.ip_config == NET_CTL_IP_PPPOE ? " (PPPoE)" : "");
    y = row(cx, y, kw, cw, "Router", info.router[0] ? WHITE : YELLOW, "%s", info.router[0] ? info.router : "none");
    if (info.dns2[0] && strcmp(info.dns2, "0.0.0.0")) snprintf(s, sizeof s, "%s, %s", info.dns1, info.dns2);
    else snprintf(s, sizeof s, "%s", info.dns1[0] ? info.dns1 : "none");
    y = row(cx, y, kw, cw, "DNS servers", WHITE, "%s", s);
    if (dns.done && y + line_h(F_BODY) <= bottom) {
        if (dns.ms >= 0) row(cx, y, kw, cw, "DNS lookup", dns.ms > PING_DNS_CHECK_MS ? YELLOW : WHITE, "%d ms for " DNS_NAME, dns.ms);
        else row(cx, y, kw, cw, "DNS lookup", YELLOW, "failed for " DNS_NAME);
    }
}

/* The router card: the mean time large, then min, max, jitter and loss, the
 * method, and a bar per ping (a lost one as a red mark). */
static void draw_ping(float x, float y, float w, float h)
{
    card(x, y, w, h);
    float cx = x + 32, cw = w - 64;
    float top = heading(cx, y + 22, "ROUTER PING");
    if (!pg.done) {
        text_wrap(cx, top + 8, F_BODY, GREY, cw, 3,
                  have_ip() || !info.done ? "CROSS sends 20 pings to the router and looks up " DNS_NAME "."
                  : info.rc               ? "No connection data: no router to ping."
                                          : "No address: no router to ping.");
        return;
    }
    const ping_stats *p = &pg.st;
    if (p->got) {
        float ex = text(cx, top + 6, F_LARGE | TNUM, p->avg_us > PING_AVG_CHECK_US ? YELLOW : GREEN, "%.1f ms", p->avg_us / 1000.0);
        text(ex + 16, top + 6 + base_dy(F_LARGE, F_BODY), F_BODY, GREY, "on average");
        text_fit(cx, top + 82, F_BODY, WHITE, cw, "min %.1f, max %.1f, jitter %.1f ms", p->min_us / 1000.0, p->max_us / 1000.0, p->jitter_us / 1000.0);
    } else text(cx, top + 6, F_LARGE, RED, "No answer");
    text_fit(cx, top + 126, F_BODY, p->loss_pct > PING_LOSS_CHECK ? YELLOW : WHITE, cw, "%d of %d answered, %d %% lost%s", p->got, p->sent, p->loss_pct,
             pg.cancelled ? " (stopped)" : "");
    if (pg.mode == MODE_ICMP) text_fit(cx, top + 170, F_SMALL, GREY, cw, "ICMP echo to %s", info.router);
    else text_fit(cx, top + 170, F_SMALL, YELLOW, cw, "TCP connect to port %d (%s)", pg.port, pg.why_tcp);
    float by = top + 250, bh = y + h - 32 - by;   /* the bars: the slowest answer at 4/5 of the height, the scale at least 2 ms */
    if (bh < 40 || !pg.n) return;
    int mx = p->max_us * 5 / 4 > 2000 ? p->max_us * 5 / 4 : 2000;
    text(cx, by - 34, F_SMALL, DIM, "each ping, 0 to %.1f ms", mx / 1000.0);
    float bw = cw / PINGS;
    for (int k = 0; k < pg.n; k++) {
        float bx = cx + k * bw;
        if (pg.us[k] < 0) { rect(bx + 3, by + bh - 6, bw - 6, 6, RED); continue; }
        float v = bh * pg.us[k] / mx;
        if (v < 4) v = 4;
        rect(bx + 3, by + bh - v, bw - 6, v, pg.us[k] > PING_AVG_CHECK_US ? YELLOW : BLUE);
    }
    rect(cx, by + bh + 2, cw, 2, LINE);
}

int mod_network_open(void)
{
    /* As Transfer: the start behind the counter, backed by the journal; a
     * start that froze before shows no job screen. */
    int js = journal_state("net_start");
    if (!net_is_up() && fs_ok && js != J_FROZEN && js != J_SKIPPED) run_job("Starting the network", job_open);
    else job_open();
    while (1) {
        read_pad();
        int back = ui_leave();
        if (back <= 0) { network_tile(); return back; }
        int can = net_is_up() && !info.frozen;   /* reads that froze before: no test on top */
        if ((pressed & BTN_CROSS) && can) {
            run_job_cancelable("Network test", job_test);
            network_tile();
            write_report();
        }
        begin_frame();
        title();
        float w = SW - 2 * MG, cw = (w - 32) / 2, h = BOTTOM - TOP - 4;
        draw_info(MG, TOP, cw, h);
        draw_ping(MG + cw + 32, TOP, cw, h);
        if (level >= 0) status_linef(level == 0 ? GREEN : level == 1 ? YELLOW : RED, "%s%s.", level == 0 ? "No problem found: " : level == 1 ? "Check: " : "Problem: ", why);
        else status_line("The router answers within a few milliseconds on a cable, a little more on Wi-Fi.", GREY);
        footer(can ? "CROSS ping the router and time a DNS lookup  CIRCLE home  START exit" : "CIRCLE home  START exit");
        report_footnote();
        ui_flip();
    }
}

void mod_network_report(void)
{
    if (level < 0 && !info.done) return;
    rep_out("--- network ---\n");
    if (info.rc) {
        if (net_start_frozen()) rep_out("connection: not read, the network start froze this console before\n");
        else if (info.frozen) rep_out("connection: not read, the reads froze this console before (net_info)\n");
        else if (!net_is_up()) rep_out("connection: not read, the network did not start\n");
        else rep_out("connection: not read, netctl refused (rc 0x%08x)\n", (unsigned)info.rc);
        return;
    }
    char lt[12] = "not read", ls[12] = "not read";   /* a value netctl refused: "not read", never -1 */
    if (info.link_type >= 0) snprintf(lt, sizeof lt, "%d", info.link_type);
    if (info.link >= 0) snprintf(ls, sizeof ls, "%d", info.link);
    rep_out("device %s, link %s (type %s, state %s)", info.device == NET_CTL_DEVICE_WIRELESS ? "Wi-Fi" : info.device == NET_CTL_DEVICE_WIRED ? "cable" : "not read",
            info.link_type >= 0 ? ping_link_text(info.link_type) : "not read", lt, ls);
    if (info.mtu > 0) rep_out(", MTU %d", info.mtu);
    else rep_out(", MTU not read");
    if (info.device == NET_CTL_DEVICE_WIRELESS) {
        if (info.rssi >= 0) rep_out(", signal %d %%", info.rssi);
        else rep_out(", signal not read");
        if (info.channel >= 0) rep_out(", channel %d", info.channel);
        else rep_out(", channel not read");
    }
    rep_out("\naddress %s", info.ip[0] ? info.ip : "none");
    if (info.ip_config >= 0) rep_out(" (config %d)", info.ip_config);
    rep_out(", netmask %s, router %s, DNS %s %s\n", info.mask[0] ? info.mask : "none", info.router[0] ? info.router : "none",
            info.dns1[0] ? info.dns1 : "none", info.dns2);
    if (pg.done) {
        if (pg.mode == MODE_ICMP) rep_out("ping: ICMP echo to the router, %d sent", pg.st.sent);
        else rep_out("ping: TCP connect to the router's port %d (%s), %d sent", pg.port, pg.why_tcp, pg.st.sent);
        rep_out(", %d answered, %d%% lost%s", pg.st.got, pg.st.loss_pct, pg.cancelled ? " (stopped)" : "");
        if (pg.st.got) rep_out(", avg %.2f ms, min %.2f, max %.2f, jitter %.2f", pg.st.avg_us / 1000.0, pg.st.min_us / 1000.0, pg.st.max_us / 1000.0,
                               pg.st.jitter_us / 1000.0);
        rep_out("\n  times (ms):");
        for (int k = 0; k < pg.n; k++) {
            if (pg.us[k] < 0) rep_out(" -");
            else rep_out(" %.2f", pg.us[k] / 1000.0);
        }
        rep_out("\n");
    }
    if (dns.done) {
        if (dns.ms >= 0) rep_out("DNS lookup of %s: %d ms\n", DNS_NAME, dns.ms);
        else rep_out("DNS lookup of %s: failed\n", DNS_NAME);
    }
    if (level >= 0) rep_out("verdict: %s\n", why);
}
