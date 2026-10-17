/* Mac-side test of the pure C layer (smart.c, vendor.c, trend.c, compat.c,
 * qrcodegen.c, heat.c, ping.c, tone.c) with synthetic sectors laid out as
 * ACS-3 says. build.sh and CI run it. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../source/smart.h"
#include "../source/trend.h"
#include "../source/vendor.h"
#include "../source/compat.h"
#include "../source/version.h"
#include "../source/qrcodegen.h"
#include "../source/heat.h"
#include "../source/ping.h"
#include "../source/tone.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void put_word(uint8_t *b, int i, unsigned w) { b[2 * i] = w & 0xff; b[2 * i + 1] = w >> 8; }

static void put_string(uint8_t *b, int first, int nwords, const char *s)
{
    char pad[64];
    memset(pad, ' ', sizeof pad);
    memcpy(pad, s, strlen(s));
    for (int k = 0; k < nwords; k++) put_word(b, first + k, (uint8_t)pad[2 * k] << 8 | (uint8_t)pad[2 * k + 1]);
}

/* A Phy event counter at off: the id, then the value in 2 x its size code bytes, little endian. The next offset. */
static int put_phy(uint8_t *b, int off, unsigned id, unsigned long long v)
{
    b[off] = id & 0xff;
    b[off + 1] = id >> 8;
    for (unsigned i = 0; i < 2 * ((id >> 12) & 7); i++) b[off + 2 + i] = i < 8 ? (uint8_t)(v >> (8 * i)) : 0;
    return off + 2 + 2 * ((id >> 12) & 7);
}

static void seal(uint8_t *b)            /* make the byte sum 0, as the drive does */
{
    unsigned sum = 0;
    for (int i = 0; i < 511; i++) sum += b[i];
    b[511] = (uint8_t)(0x100 - (sum & 0xff));
}

static void identify(uint8_t *b)
{
    memset(b, 0, 512);
    put_string(b, 10, 10, "        AB12345678");
    put_string(b, 23, 4, "V0.1");
    put_string(b, 27, 20, "DHI-SSD-V800S1TB");
    put_word(b, 69, 1 << 14 | 1 << 5);
    put_word(b, 76, 0x000E);             /* Gen1-3 */
    put_word(b, 77, 3 << 1);             /* running at Gen3 */
    put_word(b, 82, 1);
    put_word(b, 83, 0x4000 | 1 << 10);
    put_word(b, 84, 0x4000 | 2);
    put_word(b, 85, 1);
    put_word(b, 87, 0x4000 | 2);
    put_word(b, 100, 0x6DB0); put_word(b, 101, 0x7470); put_word(b, 102, 0); put_word(b, 103, 0);  /* 1953525168 */
    put_word(b, 105, 8);
    put_word(b, 106, 0x4000);
    put_word(b, 169, 1);
    put_word(b, 217, 1);
    b[510] = 0xA5;
    seal(b);
}

static void attr(uint8_t *b, int slot, int id, int value, int worst, unsigned long long raw)
{
    uint8_t *e = b + 2 + slot * 12;
    e[0] = id; e[1] = 0x32; e[3] = value; e[4] = worst;
    for (int k = 0; k < 6; k++) e[5 + k] = raw >> (8 * k);
}

int main(void)
{
    uint8_t id[512], sw[512], data[512], th[512], log[512];

    identify(id);
    ata_identity o;
    CHECK(ata_parse_identify(id, NULL, &o) == 0);
    CHECK(!strcmp(o.model, "DHI-SSD-V800S1TB"));
    CHECK(!strcmp(o.serial, "AB12345678"));
    CHECK(!strcmp(o.firmware, "V0.1"));
    CHECK(o.sectors == 1953525168ull);
    CHECK(o.lba48 && o.smart_supported && o.smart_enabled && o.selftest_supported);
    CHECK(o.trim && o.drat && o.rzat && o.dsm_max_blocks == 8);
    CHECK(o.sata_max_gen == 3 && o.sata_cur_gen == 3);
    CHECK(o.rotation == 1 && o.logical_size == 512 && o.checksum == 1 && o.swapped == 0);

    memcpy(sw, id, 512);                 /* the same sector with every word swapped */
    ata_unswap(sw, 512);
    CHECK(ata_parse_identify(sw, NULL, &o) == 0 && o.swapped == 1);
    CHECK(!strcmp(o.model, "DHI-SSD-V800S1TB") && o.sectors == 1953525168ull);
    sw[511] = sw[510] = 0;               /* no signature: the fcntl model decides */
    CHECK(ata_identify_order(sw, "DHI-SSD-V800S1TB    ") == 1);
    CHECK(ata_identify_order(id, "dhi ssd v800s1tb") == 0);

    memset(data, 0, 512); memset(th, 0, 512);
    data[0] = 0x10;
    attr(data, 0, 1, 100, 100, 0);
    attr(data, 1, 5, 100, 100, 0);
    attr(data, 2, 9, 100, 100, 1234);
    attr(data, 3, 194, 67, 50, 0x0032001E0021ull);   /* 33 C now, min/max packed above */
    attr(data, 4, 241, 100, 100, 0x123456789Aull);
    data[363] = 0x00; data[367] = 0x5B; data[372] = 2;
    seal(data);
    th[2] = 1; th[3] = 50; th[14] = 5; th[15] = 10; th[26] = 9; th[27] = 0;
    seal(th);
    smart_data s;
    CHECK(smart_parse(data, th, &s) == 0);
    CHECK(s.count == 5 && s.temperature == 33 && s.short_minutes == 2);
    CHECK(s.a[2].raw == 1234 && s.a[4].raw == 0x123456789Aull);
    CHECK(s.a[0].thresh == 50 && s.a[1].thresh == 10 && s.a[2].thresh == 0);
    CHECK(s.checksum == 1 && s.thresh_checksum == 1 && (s.offline_caps & 0x10));

    char why[256];
    CHECK(smart_health(&s, NULL, 0, why, sizeof why) == HEALTH_OK);
    data[2 + 12 + 5] = 3; seal(data);  /* 3 reallocated sectors */
    smart_parse(data, th, &s);
    CHECK(smart_health(&s, NULL, 0, why, sizeof why) == HEALTH_WARN && strstr(why, "Reallocated sectors = 3"));
    data[2 + 3] = 40; seal(data);      /* attribute 1 falls to 40, threshold 50 */
    smart_parse(data, th, &s);
    CHECK(smart_health(&s, NULL, 0, why, sizeof why) == HEALTH_FAIL && strstr(why, "1 Raw read error rate at threshold"));

    memset(log, 0, 512);
    log[0] = 1;
    uint8_t *d = log + 2;                /* descriptor 1: short, passed at 1200 h */
    d[0] = 0x01; d[1] = 0x00; d[2] = 1200 & 0xff; d[3] = 1200 >> 8;
    d = log + 2 + 24;                    /* descriptor 2 (newest): short, read failure at LBA 0x1000 */
    d[0] = 0x01; d[1] = 0x70; d[2] = 1234 & 0xff; d[3] = 1234 >> 8; d[5] = 0x00; d[6] = 0x10;
    log[508] = 2;
    seal(log);
    selftest_log l;
    CHECK(selftest_parse(log, &l) == 2);
    CHECK(l.e[0].hours == 1234 && l.e[0].lba == 0x1000 && (l.e[0].status >> 4) == 7);
    CHECK(l.e[1].hours == 1200 && l.checksum == 1);
    CHECK(!strcmp(selftest_status_text(l.e[0].status), "FAILED (read)"));
    data[2 + 3] = 100; data[2 + 12 + 5] = 0; seal(data);
    smart_parse(data, th, &s);
    CHECK(smart_health(&s, &l, 0, why, sizeof why) == HEALTH_WARN && strstr(why, "last self-test failed"));

    char masked[21];
    serial_mask("AB12345678", masked, sizeof masked);
    CHECK(!strcmp(masked, "AB******78"));
    serial_mask("S1234", masked, sizeof masked);          /* too short to keep any character */
    CHECK(!strcmp(masked, "*****"));
    serial_mask("", masked, sizeof masked);
    CHECK(!strcmp(masked, ""));
    serial_mask("WD-WCC4N1234567", masked, 8);           /* fits the buffer */
    CHECK(strlen(masked) == 7);
    CHECK(smart_counter(10) && smart_counter(199) && !smart_counter(9));
    CHECK(!strcmp(smart_attr_name(193), "Load cycles"));

    /* temperature warning: 33 C is fine for an SSD, a 65 C limit trips at 65 */
    CHECK(smart_health(&s, NULL, TEMP_LIMIT_SSD, why, sizeof why) == HEALTH_OK);
    attr(data, 3, 194, 67, 50, 0x0032001E0041ull); seal(data);   /* 65 C now */
    smart_parse(data, th, &s);
    CHECK(s.temperature == 65);
    CHECK(smart_health(&s, NULL, TEMP_LIMIT_SSD, why, sizeof why) == HEALTH_WARN && strstr(why, "temperature 65 C"));
    CHECK(smart_health(&s, NULL, 0, why, sizeof why) == HEALTH_OK);

    /* vendors */
    CHECK(vendor_detect("Samsung SSD 870 EVO 1TB") == VENDOR_SAMSUNG);
    CHECK(vendor_detect("MZ7LN512HMJP-000L7") == VENDOR_SAMSUNG);
    CHECK(vendor_detect("CT1000MX500SSD1") == VENDOR_MICRON);
    CHECK(vendor_detect("KINGSTON SA400S37480G") == VENDOR_KINGSTON);
    CHECK(vendor_detect("WDC WD10JPVX-22JC3T0") == VENDOR_WD);
    CHECK(vendor_detect("WDS100T2B0A-00SM50") == VENDOR_SANDISK);
    CHECK(vendor_detect("ST1000LM035-1RK172") == VENDOR_SEAGATE);
    CHECK(vendor_detect("INTEL SSDSC2KW512G8") == VENDOR_INTEL);
    CHECK(vendor_detect("TOSHIBA MQ01ABD100") == VENDOR_TOSHIBA);
    CHECK(vendor_detect("MK1655GSX") == VENDOR_TOSHIBA && vendor_detect("TOSHIBA MK2555GSX") == VENDOR_TOSHIBA);
    CHECK(vendor_detect("MKNSSDRE1TB") == VENDOR_UNKNOWN);          /* Mushkin: no Toshiba table, whose 241 is in LBAs */
    CHECK(vendor_detect("MKNSSDCR240GB-DX") == VENDOR_UNKNOWN && vendor_detect("MK") == VENDOR_UNKNOWN);
    CHECK(vendor_detect("Dahua V800 2.5 inch SATA 1TB SSD") == VENDOR_MAXIO);
    CHECK(vendor_detect("DHI-SSD-V800S1TB") == VENDOR_MAXIO);
    CHECK(vendor_detect("Some Drive") == VENDOR_UNKNOWN && vendor_detect("") == VENDOR_UNKNOWN);
    CHECK(!strcmp(vendor_attr_name(VENDOR_MAXIO, 161), "Valid spare blocks"));
    CHECK(!strcmp(vendor_attr_name(VENDOR_MAXIO, 5), "Reallocated sectors"));      /* generic fallback */
    CHECK(!strcmp(vendor_attr_name(VENDOR_UNKNOWN, 161), "Vendor attribute"));
    CHECK(!strcmp(vendor_attr_name(VENDOR_SEAGATE, 1), "Raw read error rate (packed)"));
    CHECK(vendor_get(VENDOR_SAMSUNG)->writes_unit == UNIT_LBA && vendor_get(VENDOR_INTEL)->writes_unit == UNIT_MIB32);
    CHECK(vendor_get(99)->vendor == VENDOR_UNKNOWN);

    /* summary: Samsung layout, 241 in LBAs, 177 = life */
    memset(data, 0, 512);
    attr(data, 0, 9, 100, 100, 0x0001000003E8ull);   /* 1000 h, junk above the low 32 bits */
    attr(data, 1, 12, 100, 100, 250);
    attr(data, 2, 177, 97, 97, 40);
    attr(data, 3, 241, 100, 100, 4000000000ull);    /* 4e9 LBAs = 2.048 TB */
    seal(data);
    smart_parse(data, NULL, &s);
    smart_summary m;
    smart_summarize(&s, 1, VENDOR_SAMSUNG, &m);
    CHECK(m.hours == 1000 && m.cycles == 250 && m.life_left == 97 && m.life_id == 177);
    CHECK(m.writes_id == 241 && m.writes_unit == UNIT_LBA && m.tb_written > 2.047 && m.tb_written < 2.049);
    CHECK(m.temp_limit == TEMP_LIMIT_SSD);
    smart_summarize(&s, 0, VENDOR_UNKNOWN, &m);
    CHECK(m.life_left == -1 && m.writes_id == 0 && m.tb_written < 0 && m.temp_limit == TEMP_LIMIT_HDD);
    smart_summarize(&s, 1, VENDOR_MAXIO, &m);
    CHECK(m.writes_id == 241 && m.writes_unit == UNIT_MIB32 && m.writes_raw == 4000000000ull);
    smart_summarize(&s, 1, VENDOR_KINGSTON, &m);
    CHECK(m.writes_id == 241 && m.writes_unit == UNIT_NONE && m.tb_written < 0);
    /* the Dahua test drive: no 169, life in 202, 241 in 32 MiB units */
    memset(data, 0, 512);
    attr(data, 0, 9, 96, 100, 921); attr(data, 1, 202, 100, 100, 100); attr(data, 2, 241, 100, 100, 72132); seal(data);
    smart_parse(data, NULL, &s);
    smart_summarize(&s, 1, VENDOR_MAXIO, &m);
    CHECK(m.life_left == 100 && m.life_id == 202 && m.tb_written > 2.41 && m.tb_written < 2.43);
    memset(data, 0, 512); seal(data);
    smart_parse(data, NULL, &s);
    smart_summarize(&s, 1, VENDOR_SAMSUNG, &m);
    CHECK(m.hours == -1 && m.cycles == -1 && m.life_left == -1);

    /* delta: raw now - raw before, never for temperatures */
    smart_data before, now;
    memset(data, 0, 512);
    attr(data, 0, 9, 100, 100, 1000); attr(data, 1, 5, 100, 100, 0); attr(data, 2, 194, 60, 50, 40); seal(data);
    smart_parse(data, NULL, &before);
    attr(data, 0, 9, 100, 100, 1012); attr(data, 1, 5, 100, 100, 2); attr(data, 2, 194, 60, 50, 45);
    attr(data, 3, 12, 100, 100, 7); seal(data);
    smart_parse(data, NULL, &now);
    long long dl = 0;
    CHECK(smart_delta(&now, &before, 9, &dl) && dl == 12);
    CHECK(smart_delta(&now, &before, 5, &dl) && dl == 2);
    CHECK(!smart_delta(&now, &before, 194, &dl));      /* temperature packs min/max */
    CHECK(!smart_delta(&now, &before, 12, &dl));       /* only in one side */
    CHECK(smart_delta(&before, &now, 9, &dl) && dl == -12);

    /* SMART error log 01h */
    uint8_t el[512];
    memset(el, 0, sizeof el);
    el[0] = 1; el[1] = 2; el[452] = 2;
    uint8_t *e2 = el + 2 + 90;
    e2[4 * 12 + 7] = 0x25; e2[60 + 1] = 0x40; e2[60 + 8] = 0x51;   /* READ DMA EXT, UNC, ERR */
    el[511] = (uint8_t)(256 - (sector_checksum(el) & 0xff));
    error_log eo;
    CHECK(errorlog_parse(el, &eo) == 0 && eo.error_count == 2 && eo.index == 2 && eo.checksum);
    CHECK(eo.last_command == 0x25 && eo.last_error == 0x40 && eo.last_status == 0x51);
    memset(el, 0, sizeof el);
    CHECK(errorlog_parse(el, &eo) == -1 && eo.error_count == 0);

    /* device statistics (GPL 04h) and Phy counters (GPL 11h) */
    uint8_t pg[512];
    memset(pg, 0, sizeof pg);
    pg[0] = 1; pg[2] = 0; pg[8] = 3; pg[9] = 1; pg[10] = 5; pg[11] = 7;
    uint8_t pages[16];
    CHECK(devstat_pages(pg, pages, 16) == 3 && pages[1] == 5 && pages[2] == 7);
    dev_stats ds;
    memset(&ds, 0, sizeof ds);
    memset(pg, 0, sizeof pg); pg[0] = 1; pg[2] = 1;
    /* qword at 24: sectors written = 4750000000, supported + valid */
    unsigned long long swq = 4750000000ull | (3ull << 62);
    for (int i = 0; i < 8; i++) pg[24 + i] = (uint8_t)(swq >> (8 * i));
    unsigned long long hrs = 931ull | (3ull << 62);
    for (int i = 0; i < 8; i++) pg[16 + i] = (uint8_t)(hrs >> (8 * i));
    devstat_general(pg, &ds);
    CHECK(ds.have_general && ds.sectors_written == 4750000000ll && ds.power_on_hours == 931 && ds.resets == -1);
    memset(pg, 0, sizeof pg); pg[0] = 1; pg[2] = 5;
    unsigned long long tmax = 48ull | (3ull << 62), tmin = (unsigned long long)(uint8_t)(int8_t)-3 | (3ull << 62);
    for (int i = 0; i < 8; i++) { pg[32 + i] = (uint8_t)(tmax >> (8 * i)); pg[40 + i] = (uint8_t)(tmin >> (8 * i)); }
    devstat_temperature(pg, &ds);
    CHECK(ds.have_temp && ds.temp_max == 48 && ds.temp_min == -3 && ds.temp_now == -999);
    memset(pg, 0, sizeof pg); pg[0] = 1; pg[2] = 7;
    unsigned long long eu = 1ull | (3ull << 62);
    for (int i = 0; i < 8; i++) pg[8 + i] = (uint8_t)(eu >> (8 * i));
    devstat_ssd(pg, &ds);
    CHECK(ds.have_ssd && ds.endurance_used == 1);
    devstat_general(pg, &ds);                       /* wrong page: ignored */
    CHECK(ds.sectors_written == 4750000000ll);
    uint8_t ph[512];
    memset(ph, 0, sizeof ph);
    /* id 0x0001 (16-bit) = 7, id 0x200A (32-bit) = 3, id 0x100B = 2 */
    ph[4] = 0x01; ph[5] = 0x10; ph[6] = 7; ph[7] = 0;
    ph[8] = 0x0A; ph[9] = 0x20; ph[10] = 3;
    ph[14] = 0x0B; ph[15] = 0x10; ph[16] = 2;
    phy_counters pc;
    CHECK(phy_parse(ph, &pc) == 0 && pc.count == 3 && pc.icrc == 7 && pc.comreset == 3 && pc.crc_h2d == 2 && pc.phy_nrdy == -1);
    memset(ph, 0, sizeof ph);
    CHECK(phy_parse(ph, &pc) == -1);
    put_phy(ph, put_phy(ph, 4, 0x300A, 0x123456789ABCull), 0x1001, 5);    /* a 48-bit counter: 6 bytes, then a 16-bit one */
    CHECK(phy_parse(ph, &pc) == 0 && pc.count == 2 && pc.comreset == 0x123456789ABCll && pc.icrc == 5);
    memset(ph, 0, sizeof ph);
    put_phy(ph, put_phy(ph, 4, 0x4009, 0x0102030405060708ull), 0x100B, 4);   /* a 64-bit counter first */
    CHECK(phy_parse(ph, &pc) == 0 && pc.count == 2 && pc.phy_nrdy == 0x0102030405060708ll && pc.crc_h2d == 4);
    memset(ph, 0, sizeof ph);
    put_phy(ph, 4, 0x4002, ~0ull);                                         /* over LLONG_MAX: kept at it, not negative */
    CHECK(phy_parse(ph, &pc) == 0 && pc.rerr_data == 0x7FFFFFFFFFFFFFFFll);
    memset(ph, 0, sizeof ph);
    put_phy(ph, put_phy(ph, 4, 0x1001, 5), 0x9001, 9);                    /* vendor 0x8001 is not the ICRC counter 0x001 */
    CHECK(phy_parse(ph, &pc) == 0 && pc.count == 2 && pc.icrc == 5);
    memset(ph, 0, sizeof ph);
    put_phy(ph, put_phy(ph, 4, 0x1001, 5), 0x5009, 1);                    /* size code 5: the list stops there */
    CHECK(phy_parse(ph, &pc) == 0 && pc.count == 1 && pc.phy_nrdy == -1);

    /* the host-writes unit math, and the error counters added up */
    CHECK(writes_bytes(3, UNIT_LBA) == 1536.0 && writes_bytes(2, UNIT_GIB) == 2147483648.0);
    CHECK(writes_bytes(72607, UNIT_MIB32) == 72607.0 * 33554432.0 && writes_bytes(72607, UNIT_NONE) < 0);
    memset(data, 0, 512);
    attr(data, 0, 9, 100, 100, 934); attr(data, 1, 5, 100, 100, 2);
    attr(data, 2, 199, 100, 100, 0x00FF00000003ull);   /* 3 in the low 32 bits */
    attr(data, 3, 12, 100, 100, 40); seal(data);       /* 12 is no error counter */
    smart_parse(data, NULL, &s);
    CHECK(smart_error_sum(&s) == 5);
    memset(data, 0, 512); attr(data, 0, 9, 100, 100, 934); seal(data);
    smart_parse(data, NULL, &s);
    CHECK(smart_error_sum(&s) == 0);

    /* trend.txt: a line, and the same values back */
    struct tm when;
    memset(&when, 0, sizeof when);
    when.tm_year = 2026 - 1900; when.tm_mon = 9; when.tm_mday = 7; when.tm_hour = 19; when.tm_min = 52;
    memset(data, 0, 512);
    attr(data, 0, 9, 96, 100, 934); attr(data, 1, 194, 64, 50, 0x0032001E0024ull);   /* 36 C */
    attr(data, 2, 202, 100, 100, 100); attr(data, 3, 241, 100, 100, 72607);
    attr(data, 4, 5, 100, 100, 1); attr(data, 5, 199, 100, 100, 2); seal(data);
    smart_parse(data, NULL, &s);
    smart_summarize(&s, 1, VENDOR_MAXIO, &m);
    char tl[96];
    trend_rec tr;
    int len = trend_line(tl, sizeof tl, &when, &s, &m);
    CHECK(len == (int)strlen(tl) && !strcmp(tl, "2026-10-07 19:52 934 36 72607 100 3"));
    CHECK(trend_parse(tl, &tr) == 0 && tr.year == 2026 && tr.month == 10 && tr.day == 7 && tr.hour == 19 && tr.minute == 52);
    CHECK(tr.hours == 934 && tr.temp == 36 && tr.writes == 72607 && tr.life == 100 && tr.errors == 3);
    /* not reported: no 194/190, no life or writes attribute, hours with bit 31 set */
    memset(data, 0, 512); attr(data, 0, 9, 100, 100, 0x80000001ull); seal(data);
    smart_parse(data, NULL, &s);
    smart_summarize(&s, 0, VENDOR_UNKNOWN, &m);
    trend_line(tl, sizeof tl, &when, &s, &m);
    CHECK(!strcmp(tl, "2026-10-07 19:52 -1 -1 -1 -1 0"));
    CHECK(trend_parse(tl, &tr) == 0 && tr.hours == -1 && tr.temp == -1 && tr.writes == -1 && tr.life == -1 && tr.errors == 0);
    /* the longest line: 63 characters, 64 bytes with the line end */
    static const int ctr[] = {5, 10, 184, 187, 196, 197, 198, 199};
    memset(data, 0, 512);
    for (int k = 0; k < 8; k++) attr(data, k, ctr[k], 100, 100, 0xFFFFFFFFull);
    attr(data, 8, 9, 100, 100, 0x7FFFFFFFull); attr(data, 9, 194, 100, 100, 0xFF);
    attr(data, 10, 202, 100, 100, 0); attr(data, 11, 241, 100, 100, 0xFFFFFFFFFFFFull); seal(data);
    smart_parse(data, NULL, &s);
    smart_summarize(&s, 1, VENDOR_MAXIO, &m);
    len = trend_line(tl, sizeof tl, &when, &s, &m);
    CHECK(len == 63 && trend_parse(tl, &tr) == 0 && tr.hours == 2147483647 && tr.temp == 255);
    CHECK(tr.writes == 281474976710655ll && tr.life == 100 && tr.errors == 8 * 4294967295ll);
    static const char *const bad[] = {
        "drive Dahua V800 2.5 inch SATA 1TB SSD",          /* the header */
        "",                                                /* a blank line */
        "2026-10-07 19:52 934 36 72607 100",               /* cut at the end */
        "26-10-07 19:52 934 36 72607 100 3",               /* cut inside the year */
        "2026-10-07 19:52 934 36 72607 100 32026-10-08 09:00 950 37 72700 100 3",   /* two lines joined */
        "2026-10-07 19:52 934 36 72607 100 3 7",           /* an extra field */
        "2026-10-07 19:52 934 36 72607 100 3 ",            /* a space at the end */
        " 2026-10-07 19:52 934 36 72607 100 3",            /* a space before */
        "2026-10-07 19:52 934 36 72607 100 3\r\r",         /* two CRs */
        "2026-00-07 19:52 934 36 72607 100 3", "2026-13-07 19:52 934 36 72607 100 3",    /* month */
        "2026-10-00 19:52 934 36 72607 100 3", "2026-10-32 19:52 934 36 72607 100 3",    /* day */
        "2026-10-07 -1:52 934 36 72607 100 3", "2026-10-07 24:52 934 36 72607 100 3",    /* hour */
        "2026-10-07 19:-1 934 36 72607 100 3", "2026-10-07 19:60 934 36 72607 100 3",    /* minute */
        "2026-10-07 19:52 -2 36 72607 100 3",                                          /* power-on hours */
        "2026-10-07 19:52 934 -2 72607 100 3", "2026-10-07 19:52 934 256 72607 100 3",  /* temperature */
        "2026-10-07 19:52 934 36 -2 100 3",                                            /* host writes */
        "2026-10-07 19:52 934 36 72607 -2 3", "2026-10-07 19:52 934 36 72607 101 3",    /* life */
        "2026-10-07 19:52 934 36 72607 100 -1",                                        /* errors */
    };
    for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++)
        if (trend_parse(bad[k], &tr) != -1) { printf("FAIL: trend_parse took \"%s\"\n", bad[k]); fails++; }
    CHECK(trend_parse("2026-10-07 19:52 934 36 72607 100 3\r", &tr) == 0 && tr.errors == 3);   /* one CR */
    CHECK(trend_parse("2026-12-31 23:59 -1 255 -1 0 0", &tr) == 0 && tr.day == 31 && tr.temp == 255 && tr.life == 0);
    CHECK(trend_parse("2026-01-01 00:00 0 -1 0 -1 0", &tr) == 0 && tr.month == 1 && tr.hour == 0 && tr.minute == 0);

    /* the issue link: a blank issue, title first, then the fields (body is what the mobile app fills) */
    char enc[64], url[2400];
    CHECK(url_encode("a b/c=d~", enc, sizeof enc) == 14 && !strcmp(enc, "a%20b%2Fc%3Dd~"));
    CHECK(url_encode("abcdefgh", enc, 6) == 2 && !strcmp(enc, "ab"));   /* bounded: room for one %XX and the NUL */
    const form_field f[] = {{"firmware", "4.93 (Europe)"}, {"health", "OK\nhours 921"}};
    int n = issue_form_url("Compat: X on 4.93", f, 2, url, sizeof url);
    CHECK(n > 0 && n == (int)strlen(url));
    CHECK(!strcmp(url, APP_REPO "/issues/new?title=Compat%3A%20X%20on%204.93&firmware=4.93%20%28Europe%29&health=OK%0Ahours%20921"));
    CHECK(issue_form_url("title", f, 2, url, 40) == -1);   /* does not fit */
    const form_field b[] = {{"body", "console: x\nfirmware: 4.93"}};
    n = issue_form_url("Compat: X on 4.93", b, 1, url, sizeof url);
    CHECK(n > 0 && !strcmp(url, APP_REPO "/issues/new?title=Compat%3A%20X%20on%204.93&body=console%3A%20x%0Afirmware%3A%204.93"));
    /* a real-size link fits a QR code at version 25 */
    char big[700];
    memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
    const form_field g[] = {{"body", big}};
    n = issue_form_url("t", g, 1, url, sizeof url);
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(25)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(25)];
    CHECK(n > 700 && qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_LOW, 1, 25, qrcodegen_Mask_AUTO, true));
    CHECK(qrcodegen_getSize(qr) >= 21 && qrcodegen_getSize(qr) <= 117);
    {                                        /* the limit itself: 1273 bytes encode, 1274 do not */
        static char lim[COMPAT_QR_BYTES + 2];
        memset(lim, 'a', COMPAT_QR_BYTES); lim[COMPAT_QR_BYTES] = 0;
        CHECK(qrcodegen_encodeText(lim, tmp, qr, qrcodegen_Ecc_LOW, 1, 25, qrcodegen_Mask_AUTO, true));
        lim[COMPAT_QR_BYTES] = 'a'; lim[COMPAT_QR_BYTES + 1] = 0;
        CHECK(!qrcodegen_encodeText(lim, tmp, qr, qrcodegen_Ecc_LOW, 1, 25, qrcodegen_Mask_AUTO, true));
    }
    /* Help's link with the Cooling line (compat_fit_url): the line in full, its short form, or none, so that the
     * link stays within the QR code; at a long health reason (smart_health's why holds up to 255 characters) */
    {
        static char body[1024], why[256], full[272], brief[272], u2[2400];   /* "cooling: ", the reason, the line end */
        heat_report hr = {FAMILY_SUPER, 58, 55, 67, 62, 70, 65, 9, 25, 33, 30, 1805, 1, 1, 1};
        heat_compat_line(&hr, 1, why, sizeof why);
        snprintf(full, sizeof full, "cooling: %s\n", why);
        heat_compat_line(&hr, 0, why, sizeof why);
        snprintf(brief, sizeof brief, "cooling: %s\n", why);
        const char *const alt[2] = {full, brief};
        for (int len = 40; len <= 255; len += 215) {   /* a short reason, then the longest */
            why[0] = 0;
            while ((int)strlen(why) + 42 <= len) strcat(why, "197 Current Pending Sector Count = 65535, ");
            while ((int)strlen(why) < len) strcat(why, "x");
            snprintf(body, sizeof body,
                     "console: CECH-____, HEN/CFW ____ (you fill these in)\nfirmware: 4.93, app %s\n"
                     "drive: Samsung SSD 870 EVO 1TB, fw SVT02B6Q, 1000 GB, SSD, SATA 3 (max 3)\n"
                     "result: 609 rc 0x0, 600 rc 0x0, IDENTIFY ok, SMART ok, log ok, last self-test completed without error at 12345 h, "
                     "file write 25 / read 63 MB/s, error log 0, GPL logs stats ok/phy ok\n"
                     "health: WARN (%s)\nhours 1 year 152 days, cycles 12345, temp 45 C\n"
                     "5=4294967295 187=4294967295 196=4294967295 197=4294967295 198=4294967295 199=4294967295 \n", APP_VERSION, why);
            int base = (int)strlen(body);
            n = compat_fit_url("Compat: Samsung SSD 870 EVO 1TB on 4.93", body, sizeof body, alt, 2, url, sizeof url, COMPAT_QR_BYTES);
            CHECK(n > 0 && n <= COMPAT_QR_BYTES && qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_LOW, 1, 25, qrcodegen_Mask_AUTO, true));
            int used = !strcmp(body + base, full) ? 0 : !strcmp(body + base, brief) ? 1 : 2;
            for (int k = 0; k < used; k++) {     /* each richer line left out would not have fitted */
                const form_field fb[] = {{"body", body}};
                body[base] = 0; strcat(body, alt[k]);
                CHECK(issue_form_url("Compat: Samsung SSD 870 EVO 1TB on 4.93", fb, 1, u2, sizeof u2) > COMPAT_QR_BYTES);
            }
            if (len == 40) CHECK(used == 0);  /* a short reason: the full line */
            else CHECK(used > 0);             /* the longest: the line shrinks or goes */
        }
        memset(body, ' ', sizeof body - 1);  /* even the body alone does not fit: every space takes 3 bytes */
        body[sizeof body - 1] = 0;
        CHECK(compat_fit_url("t", body, sizeof body, alt, 2, url, sizeof url, COMPAT_QR_BYTES) == -1);
    }

    /* life left as a time (trend_life_eta) */
    {
        trend_rec r[6];
        trend_eta e;
        char et[96];
        memset(r, 0, sizeof r);
        const int ymd[6][3] = {{2026, 1, 1}, {2026, 1, 20}, {2026, 2, 10}, {2026, 3, 1}, {2026, 3, 20}, {2026, 4, 1}};
        const int life[6] = {100, 100, 99, 99, 98, 98};
        for (int i = 0; i < 6; i++) { r[i].year = ymd[i][0]; r[i].month = ymd[i][1]; r[i].day = ymd[i][2]; r[i].life = life[i]; }
        trend_life_eta(r, 6, &e);       /* 2 points in 90 days: 98 / (2/90) days = about 145 months, over the cap */
        CHECK(e.state == TREND_ETA_OVER && e.days == 90 && e.drop == 2 && e.reads == 6);
        trend_eta_text(&e, et, sizeof et);
        CHECK(!strcmp(et, "over 10 years to 0 % (2 points in 90 days)"));
        const int fast[6] = {60, 55, 50, 45, 40, 36};   /* the least-squares line: 36 left in about 5 months */
        for (int i = 0; i < 6; i++) r[i].life = fast[i];
        trend_life_eta(r, 6, &e);
        CHECK(e.state == TREND_ETA_OK && e.months == 5 && e.drop == 24);
        trend_eta_text(&e, et, sizeof et);
        CHECK(!strcmp(et, "about 5 months to 0 % (24 points in 90 days)"));
        r[5].life = 1;                  /* a steep line: under a month */
        trend_life_eta(r, 6, &e);
        CHECK(e.state == TREND_ETA_OK && e.months == 0);
        trend_eta_text(&e, et, sizeof et);
        CHECK(!strncmp(et, "under a month to 0 %", 20));
        r[5].life = 0;
        trend_life_eta(r, 6, &e);
        CHECK(e.state == TREND_ETA_ZERO);
        for (int i = 0; i < 6; i++) r[i].life = 97;   /* no drop */
        trend_life_eta(r, 6, &e);
        CHECK(e.state == TREND_ETA_NO_DROP && e.days == 90);
        trend_eta_text(&e, et, sizeof et);
        CHECK(!strcmp(et, "no estimate yet: life left has not dropped in 90 days"));
        r[0].life = 99;                 /* a drop, but 13 days only */
        r[1].year = 2026; r[1].month = 1; r[1].day = 14;
        trend_life_eta(r, 2, &e);
        CHECK(e.state == TREND_ETA_SHORT && e.days == 13);
        trend_eta_text(&e, et, sizeof et);
        CHECK(!strcmp(et, "no estimate yet: 13 days of reads, it needs 14"));
        trend_life_eta(r, 1, &e);
        CHECK(e.state == TREND_ETA_SHORT && e.reads == 1);
        for (int i = 0; i < 6; i++) r[i].life = -1;   /* an HDD: no life value */
        trend_life_eta(r, 6, &e);
        CHECK(e.state == TREND_ETA_NO_LIFE && e.reads == 0);
        r[0].life = 90; r[0].month = 12; r[0].day = 31; r[0].year = 2025; r[0].hour = 12;   /* across a year end */
        r[5].life = 89;
        trend_life_eta(r, 6, &e);
        CHECK(e.days == 90 && e.reads == 2 && e.state == TREND_ETA_OVER);
        /* one point a day: the last read of the day; an earlier read of that day, lower or higher, changes nothing */
        trend_rec q[4];
        trend_eta e2;
        memset(q, 0, sizeof q);
        const int qd[4][4] = {{1, 1, 12, 60}, {2, 1, 12, 52}, {3, 1, 10, 50}, {3, 1, 12, 44}};   /* month, day, hour, life */
        for (int i = 0; i < 4; i++) { q[i].year = 2026; q[i].month = qd[i][0]; q[i].day = qd[i][1]; q[i].hour = qd[i][2]; q[i].life = qd[i][3]; }
        trend_life_eta(q, 4, &e);
        trend_rec q3[3] = {q[0], q[1], q[3]};
        trend_life_eta(q3, 3, &e2);
        CHECK(e.state == TREND_ETA_OK && e2.state == TREND_ETA_OK && e.months == e2.months && e.reads == 4 && e2.reads == 3);
        /* the console clock set back: a read dated before an earlier one is left out; as the last read, no estimate */
        trend_rec c[5];
        memset(c, 0, sizeof c);
        const int cd[5][3] = {{2026, 1, 1}, {2000, 1, 1}, {2026, 2, 1}, {2026, 3, 1}, {2000, 1, 2}};
        const int cl[5] = {60, 59, 52, 44, 43};
        for (int i = 0; i < 5; i++) { c[i].year = cd[i][0]; c[i].month = cd[i][1]; c[i].day = cd[i][2]; c[i].life = cl[i]; }
        trend_life_eta(c, 4, &e);
        CHECK(e.state == TREND_ETA_OK && e.back == 1 && e.days == 59 && e.drop == 16 && e.reads == 3);
        trend_life_eta(c, 5, &e);
        CHECK(e.state == TREND_ETA_CLOCK && e.back == 2 && e.days == 59);
        trend_eta_text(&e, et, sizeof et);
        CHECK(!strcmp(et, "no estimate: the last read is dated before an earlier one (clock set back?)"));
        c[4].life = 0;                          /* life at 0 says more than the clock */
        trend_life_eta(c, 5, &e);
        CHECK(e.state == TREND_ETA_ZERO);
    }

    /* heat.c: the families, the rise, the paste verdict, the halving */
    {
        CHECK(heat_family_parse("slim\n") == FAMILY_SLIM && heat_family_parse("superslim") == FAMILY_SUPER);
        CHECK(heat_family_parse("fat\r\n") == FAMILY_FAT && heat_family_parse("unknown") == FAMILY_UNKNOWN);
        CHECK(heat_family_parse("slimmer") == FAMILY_NONE && heat_family_parse("") == FAMILY_NONE);
        CHECK(heat_family_get(FAMILY_NONE) == NULL && heat_family_get(FAMILY_UNKNOWN) == NULL);
        CHECK(!strcmp(heat_family_get(FAMILY_SUPER)->key, "superslim"));
        for (int f = FAMILY_FAT; f <= FAMILY_SUPER; f++)
            CHECK(heat_family_get(f)->peak_max < 75 && heat_family_parse(heat_family_get(f)->key) == f);
        short c[40];
        for (int i = 0; i < 40; i++) c[i] = (short)(50 + i / 3);   /* every 2 s: +10 C at 60 s */
        CHECK(heat_rise(c, 40, 2) == 10);
        CHECK(heat_rise(c, 30, 2) == -1);       /* 58 s only */
        c[30] = -1;
        CHECK(heat_rise(c, 40, 2) == -1);
        CHECK(heat_rise(c, 40, 0) == -1);
        heat_paste p;
        heat_paste_judge(FAMILY_SUPER, 55, 71, 11, 49, 0, &p);   /* the author's console, about */
        CHECK(p.verdict == PASTE_OK && p.over == 0);
        heat_paste_judge(FAMILY_SUPER, 55, 74, 11, 49, 0, &p);
        CHECK(p.verdict == PASTE_CHECK && p.over == 2);
        heat_paste_judge(FAMILY_SUPER, 61, 71, 6, 25, 0, &p);    /* idle 61 as in 2.2.0-rc1's soak, right after a test: over the 59 C limit */
        CHECK(p.verdict == PASTE_CHECK && p.over == 1);
        heat_paste_judge(FAMILY_SUPER, 61, 71, 6, 25, HEAT_WARM, &p);   /* the same as a warm start: idle and rise left out */
        CHECK(p.verdict == PASTE_OK && p.over == 0 && strstr(heat_paste_text(&p), "Rest about 10 minutes"));
        heat_paste_judge(FAMILY_SUPER, 58, 70, 20, -1, HEAT_WARM, &p);  /* a steep rise after a warm start does not count */
        CHECK(p.verdict == PASTE_OK && p.over == 0);
        heat_paste_judge(FAMILY_SUPER, 64, 74, 15, 65, HEAT_WARM, &p);  /* warm, and the peak and the fan above */
        CHECK(p.verdict == PASTE_LIKELY && p.over == 10 && strstr(heat_paste_text(&p), "Warm start"));
        heat_paste_judge(FAMILY_SUPER, 58, 70, -1, -1, HEAT_WARM, &p);  /* warm still needs a minute of test */
        CHECK(p.verdict == PASTE_NONE);
        heat_paste_judge(FAMILY_SUPER, 64, 74, 15, -1, 0, &p);   /* no fan read: three values still judge */
        CHECK(p.verdict == PASTE_LIKELY && p.over == 7);
        heat_paste_judge(FAMILY_SUPER, 55, 70, -1, -1, 0, &p);   /* under a minute */
        CHECK(p.verdict == PASTE_NONE && strstr(heat_paste_text(&p), "a minute"));
        heat_paste_judge(FAMILY_SUPER, 55, 70, -1, 40, 0, &p);   /* the fan does not stand in for the rise */
        CHECK(p.verdict == PASTE_NONE);
        heat_paste_judge(FAMILY_SUPER, 55, 75, -1, 40, HEAT_HOT_EARLY, &p);   /* the stop at 40 s: likely, with no rise */
        CHECK(p.verdict == PASTE_LIKELY && (p.over & 2) && strstr(heat_paste_text(&p), "first minute"));
        heat_paste_judge(FAMILY_SUPER, 62, 75, -1, 40, HEAT_HOT_EARLY | HEAT_WARM, &p);   /* warm or not */
        CHECK(p.verdict == PASTE_LIKELY && strstr(heat_paste_text(&p), "first minute"));
        heat_paste_judge(FAMILY_UNKNOWN, 55, 75, -1, 40, HEAT_HOT_EARLY, &p);   /* no model: no estimate */
        CHECK(p.verdict == PASTE_NONE && strstr(heat_paste_text(&p), "L1"));
        heat_paste_judge(FAMILY_FAT, 62, 74, 14, 70, 0, &p);     /* at the limits: not above */
        CHECK(p.verdict == PASTE_OK);
        CHECK(!heat_idle_hot(FAMILY_SUPER, 62) && heat_idle_hot(FAMILY_SUPER, 63));   /* the idle warning: 59 C and 3 */
        CHECK(!heat_idle_hot(FAMILY_NONE, 69) && heat_idle_hot(FAMILY_UNKNOWN, 70) && !heat_idle_hot(FAMILY_FAT, -1));
        CHECK(heat_warm_start(599, -1, -1) && !heat_warm_start(600, -1, -1) && !heat_warm_start(-1, -1, 60));   /* 10 min of rest */
        CHECK(heat_warm_start(-1, 56, 60) && !heat_warm_start(-1, 56, 59) && !heat_warm_start(-1, 56, -1));    /* 3 C over the open */
        heat_report hr = {FAMILY_SUPER, 58, 55, 67, 62, 67, 62, 9, 25, 33, 0, 120, 0, 1, 0};
        char hl[160];
        CHECK(heat_compat_line(&hr, 1, hl, sizeof hl) == (int)strlen(hl));
        CHECK(!strcmp(hl, "Super slim, Cell/RSX C, idle 58/55, peak 67/62, rise 9/min, fan 25>33%, load 120 s, 6 SPUs"));
        heat_compat_line(&hr, 0, hl, sizeof hl);
        CHECK(!strcmp(hl, "Super slim, Cell/RSX C, idle 58/55, peak 67/62, rise 9/min"));
        hr.family = FAMILY_UNKNOWN; hr.rsx0 = hr.rise = hr.fan0 = hr.fan_end = -1;   /* not read: left out, no -1 */
        hr.cpu_max = 70; hr.soak = 30; hr.seconds = 1805; hr.stopped = hr.warm = 1;
        heat_compat_line(&hr, 1, hl, sizeof hl);
        CHECK(!strcmp(hl, "model not set, Cell/RSX C, idle 58/-, peak 67/62, max 70/62, soak 1805 s (stopped), 6 SPUs, warm start"));
        CHECK(heat_compat_line(&hr, 1, hl, 12) == 11 && !strcmp(hl, "model not s"));   /* cut at the buffer */
        short h[5] = {50, 52, -1, -1, 49};
        CHECK(heat_halve(h, 5) == 3 && h[0] == 52 && h[1] == -1 && h[2] == 49);
        short g[4] = {-1, 60, 61, -1};
        CHECK(heat_halve(g, 4) == 2 && g[0] == 60 && g[1] == 61);
    }

    /* ping.c: the echo packet, the statistics, the verdict */
    {
        uint8_t pk[64], ip[84];
        int len = ping_request(pk, sizeof pk, 0x1234, 7);
        CHECK(len == 8 + PING_PAYLOAD && pk[0] == 8 && pk[4] == 0x12 && pk[5] == 0x34 && pk[7] == 7);
        CHECK(ping_csum(pk, len) == 0);         /* a packet with its checksum sums to 0 */
        CHECK(ping_request(pk, 10, 1, 1) == -1);
        const uint8_t odd[3] = {0x01, 0x02, 0x03};
        CHECK(ping_csum(odd, 3) == (uint16_t)~(0x0102 + 0x0300));
        pk[0] = 0;                              /* the reply: type 0 */
        CHECK(ping_is_reply(pk, len, 0x1234, 7) && !ping_is_reply(pk, len, 0x1234, 8) && !ping_is_reply(pk, len, 0x1235, 7));
        memset(ip, 0, sizeof ip);
        ip[0] = 0x45; ip[9] = 1;                /* IPv4, 20 bytes, ICMP */
        memcpy(ip + 20, pk, (size_t)len);
        CHECK(ping_is_reply(ip, 20 + len, 0x1234, 7));
        ip[9] = 6;                              /* TCP: not ours */
        CHECK(!ping_is_reply(ip, 20 + len, 0x1234, 7));
        pk[0] = 8;                              /* our own request echoed by a loopback: not a reply */
        CHECK(!ping_is_reply(pk, len, 0x1234, 7) && !ping_is_reply(pk, 4, 0x1234, 7));
        const int t[6] = {1000, 1200, -1, 900, 1100, 800};
        ping_stats st;
        ping_calc(t, 6, &st);
        CHECK(st.sent == 6 && st.got == 5 && st.loss_pct == 16 && st.min_us == 800 && st.max_us == 1200 && st.avg_us == 1000);
        CHECK(st.jitter_us == 233);             /* |200|, then |200| and |300| after the loss: 700 / 3 */
        const int lost[3] = {-1, -1, -1};
        ping_calc(lost, 3, &st);
        CHECK(st.got == 0 && st.loss_pct == 100 && st.avg_us == 0 && st.jitter_us == 0);
        net_facts f;
        char why[128];
        memset(&f, 0, sizeof f);
        f.have_ip = 1; f.wired = 1; f.link_type = 6; f.rssi = -1; f.dns_ms = 20;
        ping_calc(t, 6, &f.ping);
        CHECK(ping_judge(&f, why, sizeof why) == 1 && strstr(why, "16 %"));   /* 16 % lost */
        f.ping.loss_pct = 0;
        CHECK(ping_judge(&f, why, sizeof why) == 0 && !strcmp(why, "the cable link and the router look fine"));
        f.link_type = 4;
        CHECK(ping_judge(&f, why, sizeof why) == 1 && strstr(why, "100 Mbit/s full duplex"));
        f.link_type = 0; f.dns_ms = -2;
        CHECK(ping_judge(&f, why, sizeof why) == 1 && strstr(why, "DNS"));
        f.dns_ms = -1; f.wired = 0; f.rssi = 30;
        CHECK(ping_judge(&f, why, sizeof why) == 1 && strstr(why, "30 %"));
        f.rssi = 80; f.ping.avg_us = 25000;
        CHECK(ping_judge(&f, why, sizeof why) == 1 && strstr(why, "25 ms"));
        ping_calc(lost, 3, &f.ping);
        CHECK(ping_judge(&f, why, sizeof why) == 2);
        f.have_ip = 0;
        CHECK(ping_judge(&f, why, sizeof why) == 2 && !strncmp(why, "no address", 10));
        CHECK(!strcmp(ping_link_text(6), "1000 Mbit/s full duplex") && !strcmp(ping_link_text(9), "unknown"));
    }

    /* tone.c: levels, fades, channels, the sweep, the stop */
    {
        static float buf[TONE_RATE * 8];
        tone tn;
        CHECK(fabs(tone_db(-18) - 0.1259) < 0.001 && fabs(tone_db(0) - 1) < 1e-9);
        tone_beeps(&tn, 8, 1u << 2, 440, -18);
        tone_fill(&tn, buf, TONE_RATE);         /* one second: a beep and its gap */
        float peak = 0, step = 0, others = 0;
        for (int i = 0; i < TONE_RATE; i++) {
            float v = buf[i * 8 + 2];
            if (fabsf(v) > peak) peak = fabsf(v);
            if (i && fabsf(v - buf[(i - 1) * 8 + 2]) > step) step = fabsf(v - buf[(i - 1) * 8 + 2]);
            for (int ch = 0; ch < 8; ch++) if (ch != 2) others += fabsf(buf[i * 8 + ch]);
        }
        CHECK(peak <= tone_db(-18) + 1e-6 && peak > tone_db(-18) * 0.99);
        CHECK(others == 0);                     /* only the masked channel */
        CHECK(step < 0.0075);                    /* no jump bigger than a 440 Hz sine's own step (2 pi f / rate x amp = 0.00724) */
        CHECK(fabsf(buf[2]) < 1e-6 && buf[(TONE_RATE * 6 / 10 + 10) * 8 + 2] == 0);   /* starts at 0, silent in the gap */
        CHECK(!tone_done(&tn));
        tone_stop(&tn);
        tone_fill(&tn, buf, TONE_RATE / 10);
        CHECK(tone_done(&tn));
        tone_fill(&tn, buf, 100);
        CHECK(buf[0] == 0 && buf[799] == 0);
        tone_beeps(&tn, 2, 1, 440, -18);        /* a stop inside a beep fades out, no click */
        tone_fill(&tn, buf, TONE_RATE / 5);
        tone_stop(&tn);
        tone_fill(&tn, buf, TONE_RATE / 20);
        step = 0;
        for (int i = 1; i < TONE_RATE / 20; i++) if (fabsf(buf[i * 2] - buf[(i - 1) * 2]) > step) step = fabsf(buf[i * 2] - buf[(i - 1) * 2]);
        CHECK(step < 0.0075 && buf[(TONE_RATE / 20 - 1) * 2] == 0 && buf[1] == 0);
        tone_sweep(&tn, 2, 3, 20, 20000, 10, -18);
        CHECK(fabs(tone_freq(&tn) - 20) < 1e-9);
        for (int k = 0; k < 5; k++) tone_fill(&tn, buf, TONE_RATE);
        CHECK(fabs(tone_freq(&tn) - 632.5) < 1);   /* half way on a log scale: sqrt(20 x 20000) */
        for (int k = 0; k < 5; k++) tone_fill(&tn, buf, TONE_RATE);
        CHECK(tone_done(&tn) && fabs(tone_freq(&tn) - 20000) < 1e-6);
        float last = buf[(TONE_RATE - 1) * 2];
        CHECK(fabsf(last) < 1e-4);              /* faded out at the end */
        tone_sweep(&tn, 2, 3, 20, 20000, 10, -18);   /* the exit's cut inside the sweep's 0.5 s fade out: silent in 40 ms, no click */
        tone_fill(&tn, buf, TONE_RATE * 2);
        float before = buf[(TONE_RATE * 2 - 1) * 2];
        tone_stop(&tn);
        tone_fill(&tn, buf, TONE_RATE / 10);
        before = buf[(TONE_RATE / 10 - 1) * 2];
        tone_cut(&tn, TONE_RATE / 25);
        tone_cut(&tn, 10);                      /* a second cut changes nothing */
        CHECK(!tone_done(&tn) && tn.cut_len == TONE_RATE / 25);
        tone_fill(&tn, buf, TONE_RATE / 20);
        step = fabsf(buf[0] - before);
        for (int i = 1; i < TONE_RATE / 20; i++) if (fabsf(buf[i * 2] - buf[(i - 1) * 2]) > step) step = fabsf(buf[i * 2] - buf[(i - 1) * 2]);
        CHECK(step < 0.0075 && tone_done(&tn) && buf[(TONE_RATE / 25) * 2] == 0 && buf[(TONE_RATE / 20 - 1) * 2] == 0);
    }

    printf(fails ? "%d check(s) failed\n" : "all checks passed\n", fails);
    return fails != 0;
}
