/* The Drive module: IDENTIFY + SMART of the PS3's internal drive. Three
 * pages: the attribute table, a summary, and the trend of the full reads
 * (trend.txt). Plus the speed test and the short and extended self-tests.
 * The QR code of the report is on the Help screen. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ppu-lv2.h>
#include <lv2/systime.h>
#include <sys/file.h>
#include <sys/thread.h>
#include <ps3gfx/ui.h>
#include "app.h"
#include "report.h"
#include "trend.h"
#include "vendor.h"

#define PAGES 3
#define ROW_H 36
#define CT (TOP + 162)                       /* the pages start under the drive header */

drive_state D;
int mod_drive_probed(void) { return D.probed; }

static void job_probe(void) { drive_probe(&D, progress); }
static void job_console(void) { console_info(&D); }
static void job_smart(void) { drive_read_smart(&D, 1); }
static void job_speed(void) { drive_speed_test(&D); }
static int job_rc, st_type;                  /* the self-test being started: 1 short, 2 extended, 0 found running */
static void job_selftest(void) { job_rc = drive_start_selftest(&D, st_type); }
static void job_errlog(void) { drive_read_error_log(&D); }
static void job_gpl(void) { drive_read_gpl(&D); }

/* A job, then the report and the drive's dump files in the same thread (up to
 * 10 files after a full read), so the counter runs during the writes. The UI
 * thread only draws the job screen meanwhile. */
static void (*report_after)(void);
static void job_and_report(void)
{
    report_after();
    progress("Writing the report");
    write_report();
}
static void run_job_report(const char *msg, void (*fn)(void))
{
    report_after = fn;
    run_job(msg, job_and_report);
}

typedef struct {
    const char *name;       /* the first journal entry the job writes: its state decides the prompt */
    const char *title, *l1, *l2, *l3, *progress;
    void (*job)(void);
    int (*wanted)(void);
} optional_step;

static int want_errlog(void) { return D.ata_ok && !D.demo; }
static int want_gpl(void) { return D.ata_ok && !D.demo && D.id.gpl; }

static const optional_step steps[] = {
    {"error_log", "SMART READ LOG 01h (the error log)",
     "Reads the drive's summary error log: how many command errors it recorded, and the last one.",
     "Same path and same kind of read as the self-test log, which this console already ran.",
     "Read-only. 512 bytes. Shown on the summary page and in the report.",
     "SMART READ LOG 01h (the error log)", job_errlog, want_errlog},
    {"gpl_devstat", "READ LOG EXT 2Fh (general purpose logs)",
     "Reads two logs: device statistics (04h: exact sectors written, endurance used, temperature extremes)",
     "and the SATA Phy counters (11h: CRC errors, link resets).",
     "Read-only. A 48-bit data-in command, the first on this path. Up to 5 reads of 512 bytes.",
     "READ LOG EXT (device statistics, Phy counters)", job_gpl, want_gpl},
};

/* Runs the optional steps: prompt on the first run, skip a declined one, run the rest. -1 on exit. */
static int optional_steps(void)
{
    for (unsigned k = 0; k < sizeof steps / sizeof steps[0]; k++) {
        const optional_step *st = &steps[k];
        if (!st->wanted()) continue;
        int js = journal_state(st->name);
        if (js == J_SKIPPED) continue;
        if (js == J_NONE) {
            int r = first_run_prompt(st->title, st->l1, st->l2, st->l3);
            if (r < 0) return -1;
            if (r == 0) { journal_skip(st->name); continue; }
        }
        run_job(st->progress, st->job);
    }
    return 0;
}

/* ---- screens -------------------------------------------------------------- */

static const char *gen_text(int g) { return g == 3 ? "6" : g == 2 ? "3" : g == 1 ? "1.5" : "?"; }
static double rate(double mb, double sec) { return mb / (sec > 0 ? sec : 1); }

static u32 attr_color(const smart_attr *a)
{
    if (a->thresh && a->thresh < 0xFE && a->value <= a->thresh) return RED;
    if (a->thresh && a->thresh < 0xFE && a->worst <= a->thresh) return YELLOW;
    if (smart_counter(a->id) && (a->raw & 0xFFFFFFFFull)) return YELLOW;
    long long dl;
    if (D.have_last && smart_delta(&D.s, &D.last, a->id, &dl) && dl) return BLUE;   /* changed since the previous read */
    return WHITE;
}

static void draw_probe_screen(void)
{
    float w = SW - 2 * MG, y = TOP + 10;
    card(MG, y, w, 190);
    float x = MG + 32, cx = MG + 300;
    y += 26;
    text(x, y, F_BODY, GREY, "609 device info");
    text(cx, y, F_BODY, D.info_rc ? YELLOW : WHITE, "rc 0x%08x   %llu sectors", (unsigned)D.info_rc, (unsigned long long)D.info_sectors);
    y += 46;
    text(x, y, F_BODY, GREY, "600 open");
    text(cx, y, F_BODY, D.opened ? WHITE : YELLOW, "rc 0x%08x", (unsigned)D.open_rc);
    y += 46;
    text(x, y, F_BODY, GREY, "616 IDENTIFY");
    if (D.opened) text_fit(cx, y, F_BODY, D.identify_frozen ? RED : YELLOW, w - 300, "rc 0x%08x   %s", (unsigned)D.identify_rc, D.identify_note);
    else text(cx, y, F_BODY, DIM, "not sent");
    y = TOP + 240;
    if (D.demo) text_wrap(MG, y, F_MED, YELLOW, w, 2, "No demo sectors: " DEMO_DIR " needs identify.bin and smart.bin.");
    else if (!D.opened) text_wrap(MG, y, F_MED, YELLOW, w, 2, "The system refused to open the drive (syscall 600).");
    else if (D.identify_frozen) text_wrap(MG, y, F_MED, RED, w, 2, "IDENTIFY froze this console before: the app does not retry it.");
    else text_wrap(MG, y, F_MED, YELLOW, w, 2, "This console refuses ATA commands through syscall 616: not supported.");
    text_wrap(MG, y + 100, F_BODY, GREY, w, 2, "The report in " APP_DIR " has the details.");
    text_wrap(MG, y + 150, F_BODY, WHITE, w, 3, "Please report this setup: Help (TRIANGLE on the home screen) has a QR code that "
                                                "opens a prefilled GitHub issue. Nothing is sent until you submit it.");
}

static void draw_header(void)
{
    const ata_identity *i = &D.id;
    const smart_data *s = &D.s;
    char type[16], serial[sizeof i->serial];
    serial_mask(i->serial, serial, sizeof serial);   /* as in the report: the screen can end up in a photo */
    if (i->rotation == 1) snprintf(type, sizeof type, "SSD");
    else if (i->rotation > 0x400 && i->rotation < 0xFFFF) snprintf(type, sizeof type, "HDD %d rpm", i->rotation);
    else snprintf(type, sizeof type, "drive");
    float x = text(MG, TOP, F_MED, WHITE, "%s", i->model);
    text_fit(x + 24, TOP + base_dy(F_MED, F_BODY), F_BODY, GREY, SW - MG - x - 24, "firmware %s    serial %s", i->firmware, serial);
    x = text(MG, TOP + 48, F_BODY, GREY, "%s    %.1f GB    SATA %s Gb/s (max %s)    TRIM %s    SMART %s", type,
             (double)i->sectors * i->logical_size / 1e9, gen_text(i->sata_cur_gen), gen_text(i->sata_max_gen),
             i->trim ? "yes" : "no", i->smart_enabled ? "on" : "off");
    if (D.have_smart && s->temperature >= 0)
        text(x + 32, TOP + 48, F_BODY, s->temperature >= D.sum.temp_limit ? YELLOW : GREY, "%d C", s->temperature);
}

static void draw_health(void)
{
    char why[256];
    int h = smart_health(&D.s, D.have_stlog ? &D.log : NULL, D.sum.temp_limit, why, sizeof why);
    u32 hc = h == HEALTH_OK ? GREEN : h == HEALTH_WARN ? YELLOW : RED;
    u32 bg = h == HEALTH_OK ? 0x14402aff : h == HEALTH_WARN ? 0x4a3c0cff : 0x4c1616ff;
    const char *word = h == HEALTH_OK ? "Health OK" : h == HEALTH_WARN ? "Health WARN" : "Health FAIL";
    float y = TOP + 92, w = text_w(F_MED, word) + 40;
    round_rect(MG, y, w, 50, 12, bg);
    text(MG + 20, y + 5, F_MED, hc, "%s", word);
    text_fit(MG + w + 24, y + 5 + base_dy(F_MED, F_BODY), F_BODY, hc, SW - 2 * MG - w - 24, "%s", why);
}

static int table_rows(void)
{
    int r = (BOTTOM - CT - 66) / ROW_H;
    return r < 4 ? 4 : r;
}

static void draw_attributes(int scroll)
{
    const smart_data *s = &D.s;
    int rows = table_rows();
    float x0 = MG, x1 = SW - MG, h = 66 + rows * ROW_H;
    card(x0, CT, x1 - x0, h);
    /* numeric columns from the right, right-aligned; the name takes the rest */
    float since_r = x1 - 44, raw_r = since_r - 210, thresh_r = raw_r - 270, worst_r = thresh_r - 120, value_r = worst_r - 120;
    float name_x = x0 + 104, name_w = value_r - 90 - name_x;
    float y = CT + 18;
    text_r(x0 + 72, y, F_SMALL, GREY, "ID");
    text(name_x, y, F_SMALL, GREY, "Attribute");
    text_r(value_r, y, F_SMALL, GREY, "Value");
    text_r(worst_r, y, F_SMALL, GREY, "Worst");
    text_r(thresh_r, y, F_SMALL, GREY, "Thresh");
    text_r(raw_r, y, F_SMALL, GREY, "Raw");
    if (D.have_prev) text_r(since_r, y, F_SMALL, GREY, "Since last");
    y = CT + 56;
    for (int r = 0; r < rows && scroll + r < s->count; r++, y += ROW_H) {
        const smart_attr *a = &s->a[scroll + r];
        u32 c = attr_color(a);
        long long dl;
        if (r & 1) rect(x0 + 10, y - 3, x1 - x0 - 20, ROW_H, PANEL2);
        text_r(x0 + 72, y, F_BODY | TNUM, c, "%u", a->id);
        text_fit(name_x, y, F_BODY, c, name_w, "%s", vendor_attr_name(D.vendor, a->id));
        text_r(value_r, y, F_BODY | TNUM, c, "%u", a->value);
        text_r(worst_r, y, F_BODY | TNUM, c, "%u", a->worst);
        text_r(thresh_r, y, F_BODY | TNUM, c, "%u", a->thresh);
        if (a->id == 194 || a->id == 190) text_r(raw_r, y, F_BODY | TNUM, c, "%u C", (unsigned)(a->raw & 0xff));
        else text_r(raw_r, y, F_BODY | TNUM, c, "%llu", (unsigned long long)a->raw);
        if (D.have_prev && smart_delta(s, &D.prev, a->id, &dl) && dl) text_r(since_r, y, F_BODY | TNUM, c, "%+lld", dl);
    }
    if (s->count > rows) {                   /* a scroll bar on the right edge of the card */
        float ty = CT + 56, th = rows * ROW_H - 6;
        round_rect(x1 - 22, ty, 6, th, 3, LINE);
        round_rect(x1 - 22, ty + th * scroll / s->count, 6, th * rows / s->count, 3, GREY);
    }
}

/* One summary row: a grey label and its value. */
static float kv_x, kv_w;
static void kv(float y, const char *key, u32 color, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void kv(float y, const char *key, u32 color, const char *fmt, ...)
{
    char v[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    text(MG + 32, y, F_BODY, GREY, "%s", key);
    text_fit(kv_x, y, F_BODY, color, kv_w, "%s", v);
}

static void draw_summary(void)
{
    const smart_summary *m = &D.sum;
    const ata_identity *i = &D.id;
    const dev_stats *ds = &D.ds;
    char h[48], dtext[256];
    char eta[96];
    int has_eta = m->life_left >= 0 && drive_life_eta(eta, sizeof eta);
    int nrows = 9 + (i->rotation == 1) + D.have_phy + has_eta;
    float avail = BOTTOM - CT - 36 - 3 * 34 - 2 * 12;
    int eta_row = has_eta && avail / nrows >= 32;   /* its own row where it fits, else short on the life row */
    if (has_eta && !eta_row) {
        nrows--;
        eta[strcspn(eta, "(")] = 0;          /* "about 30 months to 0 % " */
    }
    float step = avail / nrows;
    step = step > 42 ? 42 : step < 31 ? 31 : step;   /* 31: ten rows end inside the card at the smallest visible area */
    kv_x = MG + 300;
    kv_w = SW - MG - 32 - kv_x;
    card(MG, CT, SW - 2 * MG, BOTTOM - CT);
    float y = CT + 18;

    heading(MG + 32, y, "DRIVE");
    text_r(SW - MG - 32, y, F_SMALL, DIM, "attribute names: %s table", vendor_get(D.vendor)->name);
    y += 34;
    hours_text(m->hours, h, sizeof h);
    if (m->cycles >= 0) kv(y, "Power-on", WHITE, "%s, %d power cycles", h, m->cycles);
    else kv(y, "Power-on", WHITE, "%s", h);
    y += step;
    /* The endurance indicator (device statistics) is "percent used" by the
     * standard, but the Dahua reports 100 on a drive at 100% life: some
     * firmware stores the remaining percent. So it is shown as a number, never
     * as a verdict, and never in red. */
    int eu = ds->have_ssd ? ds->endurance_used : -1;
    if (has_eta && !eta_row)
        kv(y, "Life", m->life_left <= 10 ? RED : WHITE, "%d%% left (attribute %d), wear trend: %s", m->life_left, m->life_id, eta);
    else if (m->life_left >= 0 && eu >= 0)
        kv(y, "Life", m->life_left <= 10 ? RED : WHITE, "%d%% left (attribute %d)    endurance indicator %d (device statistics)",
           m->life_left, m->life_id, eu);
    else if (m->life_left >= 0) kv(y, "Life", m->life_left <= 10 ? RED : WHITE, "%d%% left (attribute %d)", m->life_left, m->life_id);
    else if (eu >= 0) kv(y, "Life", GREY, "endurance indicator %d (device statistics; 0 = new, 100 = used up, some firmware inverts it)", eu);
    else kv(y, "Life", GREY, "no known attribute for this vendor");
    y += step;
    if (eta_row) {                           /* trend.h: a straight line through the reads of trend.txt */
        kv(y, "Wear trend", eta[0] == 'n' ? GREY : WHITE, "%s", eta);
        y += step;
    }
    if (ds->have_general && ds->sectors_written >= 0)
        kv(y, "Host writes", WHITE, "%.2f TB (device statistics, %lld sectors)%s", (double)ds->sectors_written * i->logical_size / 1e12,
           ds->sectors_written, m->writes_id && m->tb_written >= 0 ? "" : "    no vendor attribute");
    else if (m->writes_id && m->tb_written >= 0)
        kv(y, "Host writes", WHITE, "%.2f TB (attribute %d, %s)", m->tb_written, m->writes_id, writes_unit_text(m->writes_unit));
    else if (m->writes_id)
        kv(y, "Host writes", GREY, "attribute %d raw %llu (%s)", m->writes_id, (unsigned long long)m->writes_raw, writes_unit_text(m->writes_unit));
    else kv(y, "Host writes", GREY, "not reported");
    y += step;
    if (i->rotation == 1) {
        /* the PS3 firmware predates TRIM and never sends it: the SSD's own
         * garbage collection does all the work, so spare area (over-provisioning) matters */
        kv(y, "TRIM", GREY, "never sent by the PS3. Over-provisioning: set it on a PC before the install (README).");
        y += step;
    }
    y += 12;

    heading(MG + 32, y, "HEALTH");
    y += 34;
    {
        char mx[32] = "";
        if (ds->have_temp && ds->temp_max != -999) snprintf(mx, sizeof mx, ", max ever %d", ds->temp_max);
        if (D.cpu_temp >= 0)
            kv(y, "Temperature", WHITE, "drive %d C (warning at %d%s)    Cell %d C    RSX %d C", D.s.temperature, m->temp_limit, mx,
               D.cpu_temp, D.rsx_temp);
        else
            kv(y, "Temperature", WHITE, "drive %d C (warning at %d%s)    console: not read (383 rc 0x%x)", D.s.temperature,
               m->temp_limit, mx, (unsigned)D.temps_rc);
        y += step;
    }
    {
        char el[48] = "";
        if (D.have_errlog) snprintf(el, sizeof el, "    ATA error log: %d", D.elog.error_count);
        if (D.have_stlog && D.log.count)
            kv(y, "Self-test", D.have_errlog && D.elog.error_count ? YELLOW : WHITE, "last %s, %s, at %u h%s",
               selftest_type_text(D.log.e[0].type), selftest_status_text(D.log.e[0].status), D.log.e[0].hours, el);
        else kv(y, "Self-test", GREY, "%s%s", D.have_stlog ? "the log is empty" : "the log is not readable", el);
        y += step;
    }
    if (D.have_prev) {
        int j = 0, changed = 0;
        dtext[0] = 0;
        for (int k = 0; k < D.s.count; k++) {
            long long dl;
            if (!smart_delta(&D.s, &D.prev, D.s.a[k].id, &dl) || !dl) continue;
            changed++;
            if (j < (int)sizeof dtext) j += snprintf(dtext + j, sizeof dtext - j, "%s%u %+lld", j ? ", " : "", D.s.a[k].id, dl);
        }
        kv(y, "Since last read", changed ? BLUE : GREY, "%s: %s", D.prev_when, changed ? dtext : "no change");
    } else kv(y, "Since last read", GREY, "no previous read on this console");
    y += step + 12;

    heading(MG + 32, y, "SPEED");
    y += 34;
    kv(y, "SATA link", WHITE, "%s Gb/s now, drive max %s Gb/s.%s", gen_text(i->sata_cur_gen), gen_text(i->sata_max_gen),
       i->sata_cur_gen == 1 ? " The PS3 port is 1.5 Gb/s: about 150 MB/s at most." : "");
    y += step;
    if (D.have_phy) {
        const phy_counters *p = &D.phy;
        long long crc = (p->icrc > 0 ? p->icrc : 0) + (p->crc_h2d > 0 ? p->crc_h2d : 0);
        kv(y, "Link counters", crc ? YELLOW : WHITE, "%lld CRC errors, %lld resets, %lld PhyRdy drops (SATA Phy log)", crc,
           p->comreset < 0 ? 0 : p->comreset, p->phy_nrdy < 0 ? 0 : p->phy_nrdy);
        y += step;
    }
    if (D.speed_done)
        kv(y, "Speed test", GREEN, "write %.0f MB/s, read %.0f MB/s (%.0f MB file through the file system)",
           rate(D.speed_mb, D.speed_wsec), rate(D.speed_mb, D.speed_rsec), D.speed_mb);
    else if (D.speed_frozen) kv(y, "Speed test", RED, "it froze this console before, skipped");
    else if (D.speed_rc) kv(y, "Speed test", YELLOW, "failed, rc 0x%08x", (unsigned)D.speed_rc);
    else if (D.demo) kv(y, "Speed test", GREY, "not available in demo mode");
    else kv(y, "Speed test", GREY, "not run. R2 twice writes and reads a 64 MB file in the app folder.");
}

/* ---- the trend page: trend.txt -------------------------------------------- */

#define TREND_TAIL 8191                      /* the end of the file that the page reads */
#define TREND_MAX 304                        /* the tail holds at most 303 of the app's lines (27 bytes and more), and the first read */

static trend_rec tr[TREND_MAX];              /* oldest first: tr[0] is the first read */
static int tr_n, tr_bad, tr_cut;             /* records, unreadable lines, 1 when lines after the first read were not read */
static int tr_seq = -1;                      /* the trend_seq of the last load */
static float tc_hours, tc_temp, tc_life, tc_wr, tc_err;   /* the right edges of the columns */

static void tr_add(const char *line)
{
    trend_rec r;
    if (!strncmp(line, "drive ", 6)) return;         /* the header, not an unreadable line */
    if (trend_parse(line, &r)) { tr_bad++; return; }
    if (tr_n == TREND_MAX) {                 /* lines shorter than the app's: the first read and the newest stay */
        memmove(tr + 1, tr + 2, (TREND_MAX - 2) * sizeof tr[0]);
        tr_n--;
        tr_cut = 1;
    }
    tr[tr_n++] = r;
}

/* trend.txt of this drive, read in the UI thread when the page draws after an
 * append: the head (the first line must name this drive, the second is the
 * first read), then at most the last 8 KB from the second line on. A longer
 * file loses the cut first line of its tail, as journal_load does. */
static void trend_load(void)
{
    static char buf[TREND_TAIL + 1];
    char head[192];
    s32 fd;
    u64 size = 0, pos = 0, got = 0;
    tr_n = tr_bad = tr_cut = 0;
    tr_seq = trend_seq;
    int n = fs_read_file(TREND_FILE, head, sizeof head - 1);
    if (n <= 0) return;
    head[n] = 0;
    char *nl = strchr(head, '\n');
    if (!nl || strncmp(head, "drive ", 6)) return;
    *nl = 0;
    if (!model_match(head + 6, D.id.model)) return;  /* another drive's file */
    u64 second = (u64)(nl + 1 - head);
    if (sysLv2FsOpen(TREND_FILE, SYS_O_RDONLY, &fd, 0, NULL, 0)) return;
    sysLv2FsLSeek64(fd, 0, SEEK_END, &size);
    tr_cut = size > second + TREND_TAIL;
    sysLv2FsLSeek64(fd, tr_cut ? size - TREND_TAIL : second, SEEK_SET, &pos);
    sysLv2FsRead(fd, buf, TREND_TAIL, &got);
    sysLv2FsClose(fd);
    buf[got] = 0;
    char *p = buf;
    if (tr_cut) {
        char *e = strchr(nl + 1, '\n');      /* the first read, from the head */
        if (e) { *e = 0; tr_add(nl + 1); }
        p = strchr(buf, '\n');               /* the tail from its first whole line */
        p = p ? p + 1 : buf + got;
    }
    while (*p) {                             /* empty lines are skipped */
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        if (*p) tr_add(p);
        if (!e) break;
        p = e + 1;
    }
}

/* The estimate over trend.txt's reads and this session's read when the
 * file does not have it yet (the report adds its line after this text). */
int drive_life_eta(char *out, int n)
{
    static trend_rec all[TREND_MAX + 1];
    trend_eta e;
    if (!D.probed || D.demo || !D.ata_ok || !D.have_smart) return 0;
    if (tr_seq != trend_seq) trend_load();
    int k = tr_n;
    memcpy(all, tr, sizeof tr[0] * (size_t)k);
    if (D.trend_due) {
        struct tm tm;
        local_tm(&tm);
        memset(&all[k], 0, sizeof all[k]);
        all[k].year = tm.tm_year + 1900, all[k].month = tm.tm_mon + 1, all[k].day = tm.tm_mday;
        all[k].hour = tm.tm_hour, all[k].minute = tm.tm_min;
        all[k++].life = D.sum.life_left;
    }
    trend_life_eta(all, k, &e);
    trend_eta_text(&e, out, n);
    return 1;
}

/* Host writes in TB with the vendor's unit, else the bare raw value (the
 * header says raw: 12 digits fit the 210 px column); "-" when not reported. */
static void tr_writes(float y, long long w, u32 c)
{
    double b = w < 0 ? -1 : writes_bytes((uint64_t)w, vendor_get(D.vendor)->writes_unit);
    if (w < 0) text_r(tc_wr, y, F_BODY, DIM, "-");
    else if (b < 0) text_r(tc_wr, y, F_BODY | TNUM, c, "%lld", w);
    else text_r(tc_wr, y, F_BODY | TNUM, c, "%.3f TB", b / 1e12);
}

/* One read in the columns, the date from x: the colours of the attribute
 * page, or all grey for the pinned first read. */
static void tr_row(const trend_rec *r, float x, float y, int grey)
{
    u32 c = grey ? GREY : WHITE;
    text(x, y, F_BODY | TNUM, c, "%04d-%02d-%02d %02d:%02d", r->year, r->month, r->day, r->hour, r->minute);
    if (r->hours < 0) text_r(tc_hours, y, F_BODY, DIM, "-");
    else text_r(tc_hours, y, F_BODY | TNUM, c, "%d h", r->hours);
    if (r->temp < 0) text_r(tc_temp, y, F_BODY, DIM, "-");
    else text_r(tc_temp, y, F_BODY | TNUM, grey ? GREY : r->temp >= D.sum.temp_limit ? YELLOW : WHITE, "%d C", r->temp);
    if (r->life < 0) text_r(tc_life, y, F_BODY, DIM, "-");
    else text_r(tc_life, y, F_BODY | TNUM, grey ? GREY : r->life <= 10 ? RED : WHITE, "%d%%", r->life);
    tr_writes(y, r->writes, c);
    text_r(tc_err, y, F_BODY | TNUM, grey ? GREY : r->errors > 0 ? YELLOW : WHITE, "%lld", r->errors);
}

/* The change from the first read to the newest: hours, the temperature range
 * of the reads, life, host writes, error counts. */
static void tr_change(float y)
{
    const trend_rec *a = &tr[0], *b = &tr[tr_n - 1];
    int lo = 256, hi = -1;
    for (int i = 0; i < tr_n; i++)
        if (tr[i].temp >= 0) {
            if (tr[i].temp < lo) lo = tr[i].temp;
            if (tr[i].temp > hi) hi = tr[i].temp;
        }
    if (a->hours >= 0 && b->hours >= 0) text_r(tc_hours, y, F_BODY | TNUM, WHITE, "%+d h", b->hours - a->hours);
    else text_r(tc_hours, y, F_BODY, DIM, "-");
    if (hi < 0) text_r(tc_temp, y, F_BODY, DIM, "-");
    else if (lo == hi) text_r(tc_temp, y, F_BODY | TNUM, WHITE, "%d C", hi);
    else text_r(tc_temp, y, F_BODY | TNUM, WHITE, "%d to %d C", lo, hi);
    if (a->life >= 0 && b->life >= 0) text_r(tc_life, y, F_BODY | TNUM, WHITE, "%+d%%", b->life - a->life);
    else text_r(tc_life, y, F_BODY, DIM, "-");
    int unit = vendor_get(D.vendor)->writes_unit;
    if (a->writes < 0 || b->writes < 0) text_r(tc_wr, y, F_BODY, DIM, "-");
    else if (writes_bytes(0, unit) < 0) text_r(tc_wr, y, F_BODY | TNUM, WHITE, "%+lld", b->writes - a->writes);
    else text_r(tc_wr, y, F_BODY | TNUM, WHITE, "%+.1f GB",
                (writes_bytes((uint64_t)b->writes, unit) - writes_bytes((uint64_t)a->writes, unit)) / 1e9);
    long long de = b->errors - a->errors;
    text_r(tc_err, y, F_BODY | TNUM, de > 0 ? YELLOW : WHITE, "%+lld", de);
}

/* The reads newest first in the frame of the attribute table, and two rows
 * pinned at its foot: the first read and the change since. */
static void draw_trend(int scroll)
{
    int rows = table_rows(), list = rows - 2;
    float x0 = MG, x1 = SW - MG, h = 66 + rows * ROW_H;
    card(x0, CT, x1 - x0, h);
    float y = CT + 18;
    if (D.demo) {
        text(x0 + 32, y, F_BODY, GREY, "Not available in demo mode: demo reads add no line.");
        return;
    }
    tc_err = x1 - 44;
    tc_wr = tc_err - 180;
    tc_life = tc_wr - 210;
    tc_temp = tc_life - 130;
    tc_hours = tc_temp - 190;
    const vendor_info *v = vendor_get(D.vendor);   /* a host-writes attribute in no known unit: the cells hold its raw value */
    text(x0 + 32, y, F_SMALL, GREY, "Read (local time)");
    text_r(tc_hours, y, F_SMALL, GREY, "Power-on");
    text_r(tc_temp, y, F_SMALL, GREY, "Drive temp");
    text_r(tc_life, y, F_SMALL, GREY, "Life left");
    text_r(tc_wr, y, F_SMALL, GREY, "Host writes%s", v->writes_id && writes_bytes(0, v->writes_unit) < 0 ? " (raw)" : "");
    text_r(tc_err, y, F_SMALL, GREY, "Error counts");
    y = CT + 56;
    if (!tr_n) {
        text(x0 + 32, y, F_BODY, GREY, "No read of this drive in trend.txt yet.");
        return;
    }
    for (int r = 0; r < list && scroll + r < tr_n; r++, y += ROW_H) {
        if (r & 1) rect(x0 + 10, y - 3, x1 - x0 - 20, ROW_H, PANEL2);
        tr_row(&tr[tr_n - 1 - scroll - r], x0 + 32, y, 0);
    }
    if (tr_n > list) {                       /* a scroll bar on the right edge of the list */
        float ty = CT + 56, th = list * ROW_H - 6;
        round_rect(x1 - 22, ty, 6, th, 3, LINE);
        round_rect(x1 - 22, ty + th * scroll / tr_n, 6, th * list / tr_n, 3, GREY);
    }
    y = CT + 56 + list * ROW_H;
    rect(x0 + 20, y - 3, x1 - x0 - 40, 2, LINE);
    float lx = text(x0 + 32, y, F_BODY, GREY, "First read");
    tr_row(&tr[0], lx + 24, y, 1);
    y += ROW_H;
    if (tr_n == 1) text(x0 + 32, y, F_BODY, GREY, "One read so far: the next read adds a line.");
    else {
        text(x0 + 32, y, F_BODY, GREY, "Change since first read");
        tr_change(y);
    }
}

/* The status line of the page: the file, or a failed append. */
static void trend_status(void)
{
    char first[16] = "";
    float x, w = SW - 2 * MG;
    if (trend_rc) {
        status_linef(YELLOW, "Could not add the line to trend.txt: rc 0x%08x", (unsigned)trend_rc);
        return;
    }
    if (tr_n) snprintf(first, sizeof first, "%04d-%02d-%02d", tr[0].year, tr[0].month, tr[0].day);
    if (!tr_n) x = text_fit(MG, Y_STATUS, F_BODY, GREY, w, "trend.txt: no read of this drive");
    else if (tr_cut) x = text_fit(MG, Y_STATUS, F_BODY, GREY, w, "trend.txt: the first read and the last %d, since %s", tr_n - 1, first);
    else x = text_fit(MG, Y_STATUS, F_BODY, GREY, w, "trend.txt: %d %s since %s", tr_n, tr_n == 1 ? "read" : "reads", first);
    if (tr_bad) text(x, Y_STATUS, F_BODY, DIM, ", %d unreadable", tr_bad);
}

/* After the speed test: the two rates, until any button. */
static void speed_screen(void)
{
    while (1) {
        read_pad();
        if (quit || pressed) { pressed = 0; return; }   /* the press that leaves acts nowhere else */
        begin_frame();
        title();
        text(MG, TOP, F_MED, WHITE, "Speed test");
        float y = TOP + 70, w = (SW - 2 * MG - 32) / 2;
        if (D.speed_done) {
            card(MG, y, w, 190);
            card(MG + w + 32, y, w, 190);
            text(MG + 36, y + 30, F_BODY, GREY, "Write");
            text(MG + 36, y + 80, F_LARGE, GREEN, "%.0f MB/s", rate(D.speed_mb, D.speed_wsec));
            text(MG + w + 68, y + 30, F_BODY, GREY, "Read");
            text(MG + w + 68, y + 80, F_LARGE, GREEN, "%.0f MB/s", rate(D.speed_mb, D.speed_rsec));
            y += 230;
            text(MG, y, F_BODY, GREY, "%.0f MB file in " APP_DIR ": written in %.2f s, read in %.2f s, deleted.", D.speed_mb,
                 D.speed_wsec, D.speed_rsec);
            text_wrap(MG, y + 44, F_BODY, GREY, SW - 2 * MG, 2,
                      "Through the file system, the path games use. The PS3 port is 1.5 Gb/s: about 150 MB/s at most.");
        } else if (D.speed_frozen) {
            text_wrap(MG, y, F_MED, RED, SW - 2 * MG, 2, "The speed test froze this console before: skipped.");
        } else {
            text(MG, y, F_MED, YELLOW, "The speed test failed: rc 0x%08x", (unsigned)D.speed_rc);
            text_wrap(MG, y + 60, F_BODY, GREY, SW - 2 * MG, 2, "The file goes to " APP_DIR ". A full drive gives ENOSPC (0x80010023).");
        }
        footer("Any button: back to the summary page");
        ui_flip();
    }
}

static int selftest_percent(void);
static int running, seen_running;        /* the self-test state, with its functions below */

static void draw_main(int page, int scroll, const char *st_msg, u32 st_color)
{
    static const char *const names[PAGES] = {"Attributes", "Summary", "Trend"};
    tabs(names, PAGES, page);
    draw_header();
    if (!D.have_smart) {
        text_wrap(MG, TOP + 100, F_MED, YELLOW, SW - 2 * MG, 2,
                  D.smart_frozen ? "SMART READ DATA froze the console before: skipped." : "SMART READ DATA was refused.");
        if (!D.smart_frozen) text(MG, TOP + 160, F_BODY, GREY, "rc 0x%08x", (unsigned)D.smart_rc);
    } else {
        draw_health();
        if (page == 0) draw_attributes(scroll);
        else if (page == 1) draw_summary();
        else draw_trend(scroll);
    }

    char line[200];
    if (armed == BTN_SQUARE) status_line("Press SQUARE again to clear the freeze journal. Any other button cancels.", YELLOW);
    else if (armed == BTN_R2) status_line("Press R2 again: the app writes a 64 MB file in its folder, reads it back, deletes it.", YELLOW);
    else if (running && selftest_percent() >= 0) {
        snprintf(line, sizeof line, "Self-test running: %d%% done. TRIANGLE shows the test screen.", selftest_percent());
        status_line(line, BLUE);
    } else if (running) status_line("Self-test started, waiting for the first progress report. TRIANGLE shows the test screen.", BLUE);
    else if (st_msg) status_line(st_msg, st_color);
    else if (page == 2) { if (D.have_smart && !D.demo) trend_status(); }
    else if (page == 0 && D.have_stlog && D.log.count) {
        snprintf(line, sizeof line, "Last self-test: %s, %s, at %u h", selftest_type_text(D.log.e[0].type),
                 selftest_status_text(D.log.e[0].status), D.log.e[0].hours);
        status_line(line, WHITE);
    } else if (page == 0) {
        if (D.have_stlog) snprintf(line, sizeof line, "Self-test log: empty");
        else snprintf(line, sizeof line, "Self-test log: not readable (rc 0x%08x)", (unsigned)D.stlog_rc);
        status_line(line, GREY);
    }
    /* only what works now: no re-read or speed test during a self-test or in demo mode */
    int idle = !running && !D.demo;
    const char *end = running ? "" : "  CIRCLE home  START exit";
    if (page != 1)                           /* the attribute table and the trend scroll */
        snprintf(line, sizeof line, "LEFT/RIGHT page  UP/DOWN scroll  %s%s%sSELECT USB%s", idle ? "CROSS re-read  " : "",
                 running ? "TRIANGLE test screen  " : drive_can_selftest(&D) ? "TRIANGLE self-test  " : "",
                 idle ? "R2 twice speed test  " : "", end);
    else
        snprintf(line, sizeof line, "LEFT/RIGHT page  %sSQUARE twice clear journal  %sSELECT USB%s", idle ? "CROSS re-read  " : "",
                 idle ? "R2 twice speed test  " : "", end);
    footer(line);
}

/* Before the drive is touched: CROSS reads it, TRIANGLE loads the demo
 * sectors, CIRCLE goes home. Returns 1 read, 2 demo, 0 home, -1 exit. */
static int start_screen(void)
{
    while (1) {
        read_pad();
        if (quit || (pressed & BTN_START)) return -1;
        if (pressed & BTN_CIRCLE) return 0;
        if (pressed & BTN_CROSS) return 1;
        if (pressed & BTN_TRIANGLE) return 2;
        begin_frame();
        title();
        float w = (SW - 2 * MG - 32) / 2, h = 300;
        option_card(MG, TOP + 10, w, h, BTN_CROSS, 1, "Read the drive",
                    "IDENTIFY, then SMART: read-only. Each step is written to the journal in " APP_DIR " before it runs. "
                    "If a step freezes the console, the next start skips it.");
        option_card(MG + w + 32, TOP + 10, w, h, BTN_TRIANGLE, 1, "Demo mode",
                    "Shows the sectors saved in " DEMO_DIR " and never touches the drive. This is how a dump from an "
                    "issue is reproduced.");
        footer("CROSS read the drive  TRIANGLE demo mode  CIRCLE home  START exit");
        ui_flip();
    }
}

/* ---- self-test ------------------------------------------------------------ */

static s64 next_poll, started, deadline;
static selftest_entry log_before;
static int log_before_count;
static char st_msg[160];
static u32 st_color = WHITE;

/* The poll's SMART reads run in a thread on a copy of D (P), so the screen
 * and its counter keep moving while the drive is slow with the test; the copy
 * goes back into D in the UI thread. The copy is safe because, while a test
 * runs, the UI thread writes D only through SELECT (write_report clears
 * dumps_dirty) and touches the journal only through SQUARE and SELECT, and
 * both wait for the poll first (poll_settle); CROSS, R2 and the test start
 * are blocked by running. A new write to D during a test must wait for the
 * poll too, or the copy loses it. The thread owns the journal while it runs. */
static drive_state P;
static sys_ppu_thread_t poll_th;
static volatile int poll_done;
static int poll_busy, poll_rc;
static u8 poll_st;                           /* byte 363 of the first read: the decision's branch, as before */
static s64 poll_at;                          /* the poll's start: its decision uses this time, as before */

static void poll_reads(void)
{
    poll_rc = drive_read_smart(&P, 0);
    poll_st = P.s.selftest;
    if (poll_rc == 0 && (poll_st >> 4) != 0xF) drive_read_smart(&P, 1);   /* no longer running: the log too */
}

static void poll_thread(void *arg)
{
    (void)arg;
    poll_reads();
    __sync_synchronize();
    poll_done = 1;
    sysThreadExit(0);
}

/* Byte 363 high nibble 0xF = still running. The result comes only from a
 * new entry in the self-test log: a drive that accepts the command and
 * never runs the test shows no green pass. */
static void poll_decide(void)
{
    s64 now = poll_at;
    D = P;
    if (poll_rc != 0) {
        running = 0;
        snprintf(st_msg, sizeof st_msg, "SMART read failed during the self-test: rc 0x%08x", (unsigned)D.smart_rc);
        st_color = RED;
    } else if ((poll_st >> 4) == 0xF) {
        seen_running = 1;
        deadline = now + 60000000;   /* the log entry may come a little after the status byte */
    } else {
        int logged = D.have_stlog && D.log.count &&
                     (D.log.count != log_before_count || memcmp(&D.log.e[0], &log_before, sizeof log_before));
        if (logged) {
            running = 0;
            write_report();
            snprintf(st_msg, sizeof st_msg, "Self-test finished: %s (log entry at %u h)",
                     selftest_status_text(D.log.e[0].status), D.log.e[0].hours);
            st_color = (D.log.e[0].status >> 4) == 0 ? GREEN : RED;
        } else if (now > deadline) {
            running = 0;
            write_report();
            snprintf(st_msg, sizeof st_msg, "Self-test: %s, and no new log entry (byte 363 = 0x%02x)",
                     seen_running ? "the drive stopped reporting progress" : "the drive reported no progress",
                     D.s.selftest);
            st_color = YELLOW;
        }
    }
}

/* The thread's end: the join orders its writes to P before the copy. */
static void poll_finish(void)
{
    u64 ret;
    sysThreadJoin(poll_th, &ret);
    poll_busy = 0;
    poll_decide();
}

/* Waits for a running poll: before the journal is cleared or copied, and before a quit. */
static void poll_settle(void)
{
    if (poll_busy) poll_finish();
}

/* Polls SMART data every 5 s while a test runs, one poll at a time. */
static void selftest_poll(void)
{
    if (poll_busy) {
        if (poll_done) poll_finish();
        return;
    }
    if (!running || sysGetSystemTime() < next_poll) return;
    poll_at = sysGetSystemTime();
    next_poll = poll_at + 5000000;
    P = D;
    poll_done = 0;
    if (sysThreadCreate(&poll_th, poll_thread, NULL, 1500, 0x20000, THREAD_JOINABLE, "poll") == 0) {
        poll_busy = 1;
        return;
    }
    poll_reads();                            /* no thread: the reads in the UI thread, as before */
    poll_decide();
}

/* Percent done from the status byte, -1 before the first progress report. */
static int selftest_percent(void)
{
    return seen_running ? 100 - (D.s.selftest & 0xF) * 10 : -1;
}

static int short_min(void) { return D.s.short_minutes ? D.s.short_minutes : 2; }
static int ext_min(void) { return D.s.ext_minutes ? D.s.ext_minutes : 60; }

/* Short or extended: CROSS short, TRIANGLE extended, CIRCLE cancel. Returns 1, 2 or 0. */
static int selftest_choice(void)
{
    char t1[64], t2[96];
    while (1) {
        read_pad();
        if (quit || (pressed & (BTN_CIRCLE | BTN_START))) return 0;
        if (pressed & BTN_CROSS) return 1;
        if ((pressed & BTN_TRIANGLE) && !D.selftest_long_frozen) return 2;
        begin_frame();
        title();
        text(MG, TOP, F_MED, WHITE, "Which self-test?");
        float w = (SW - 2 * MG - 32) / 2, h = 260, y = TOP + 64;
        snprintf(t1, sizeof t1, "Short, about %d min", short_min());
        option_card(MG, y, w, h, BTN_CROSS, 1, t1, "The drive checks its electronics and a small part of the surface. Nothing is written.");
        if (D.selftest_long_frozen) snprintf(t2, sizeof t2, "Extended: froze this console before");
        else snprintf(t2, sizeof t2, "Extended, about %d min", ext_min());
        option_card(MG + w + 32, y, w, h, BTN_TRIANGLE, !D.selftest_long_frozen, t2,
                    D.selftest_long_frozen ? "The journal holds a freeze inside this command, so the app does not offer it."
                                           : "The drive reads its whole surface. Nothing is written.");
        text_wrap(MG, y + h + 36, F_BODY, YELLOW, SW - 2 * MG, 3,
                  "The console is very slow while a test runs. Stay in the app until the test ends: START is blocked during "
                  "a test. Neither test can be aborted from this app.");
        footer(D.selftest_long_frozen ? "CROSS short  CIRCLE cancel" : "CROSS short  TRIANGLE extended  CIRCLE cancel");
        ui_flip();
    }
}

static const char *selftest_title(void)
{
    return st_type == 2 ? "Extended self-test" : st_type == 1 ? "Short self-test" : "Self-test in progress (started earlier)";
}

/* The self-test as its own screen: progress bar, counter, then the result.
 * Any button goes back to the table; the drive continues the test. */
static void selftest_screen(void)
{
    while (1) {
        read_pad();
        if (quit) return;
        if (pressed & ~(running ? BTN_START : 0)) { pressed = 0; return; }   /* START stays blocked during a test */
        selftest_poll();
        begin_frame();
        title();
        double sec = (double)(sysGetSystemTime() - started) / 1e6;
        text(MG, TOP, F_MED, WHITE, "%s", selftest_title());
        float y = TOP + 70;
        if (running) {
            int pct = selftest_percent();
            if (pct >= 0) text(MG, y, F_LARGE | TNUM, GREEN, "%d%% done", pct);
            else text(MG, y, F_LARGE, GREEN, "Starting");
            spinner(SW - MG - 150, y + 34, 26, sec);
            text_r(SW - MG, y + base_dy(F_LARGE, F_MED), F_MED | TNUM, WHITE, "%.0f s", sec);
            bar(MG, y + 96, SW - 2 * MG, 18, pct > 0 ? pct / 100.0f : 0, GREEN);
            y += 160;
            text(MG, y, F_BODY, GREY, "The drive runs the test by itself and reports tenths. About %d min.",
                 st_type == 2 ? ext_min() : short_min());
            text_wrap(MG, y + 44, F_BODY, GREY, SW - 2 * MG, 2, "The counter runs while the console works. If it stops, the console froze.");
            text_wrap(MG, y + 88, F_BODY, YELLOW, SW - 2 * MG, 2,
                      "START is blocked: the console is very slow while a test runs. Wait for the result.");
            footer("Any other button: back to the table, the drive continues the test");
        } else {
            text(MG, y, F_LARGE, st_color, "%s", st_color == GREEN ? "Passed" : st_color == RED ? "Failed" : "No result");
            text_wrap(MG, y + 100, F_BODY, st_color, SW - 2 * MG, 3, st_msg);
            footer("Any button: back to the table");
        }
        report_footnote();
        ui_flip();
    }
}

static int run(void)
{
    int scroll = 0, page = 0, tscroll = 0;   /* the attribute table's scroll, the trend list's */
    running = seen_running = 0;
    log_before_count = 0;
    st_msg[0] = 0;
    st_color = WHITE;
    memset(&log_before, 0, sizeof log_before);
    if (D.have_smart && !D.demo && (D.s.selftest >> 4) == 0xF) {    /* a test from an earlier session still runs */
        running = seen_running = 1;
        st_type = 0;
        log_before_count = D.have_stlog ? D.log.count : 0;
        if (log_before_count) log_before = D.log.e[0];
        started = sysGetSystemTime();
        next_poll = started + 3000000;
        deadline = started + 60000000;
    }
    while (1) {
        read_pad();
        if (quit) {
            poll_settle();
            return -1;
        }
        if ((pressed & BTN_START) && !running) return -1;   /* not running: no poll runs */
        if ((pressed & BTN_CIRCLE) && !running) return 0;
        unsigned gb = BTN_SQUARE;              /* only gestures that can fire now arm */
        if (!running && !D.demo) gb |= BTN_R2;
        unsigned fired = gesture(gb);
        if (pressed) st_msg[0] = 0;
        if ((pressed & (BTN_START | BTN_CIRCLE)) && running) {   /* a test in the background makes the console very slow */
            snprintf(st_msg, sizeof st_msg, "START and CIRCLE are blocked while the self-test runs: the console is very slow until it ends.");
            st_color = YELLOW;
        }
        if (running && (pressed & BTN_TRIANGLE)) selftest_screen();   /* back to the running test */
        if (fired == BTN_SQUARE) {
            poll_settle();
            int rc = journal_clear();
            if (rc == 0) snprintf(st_msg, sizeof st_msg, "Freeze journal cleared. Start the app again to run the skipped steps.");
            else snprintf(st_msg, sizeof st_msg, "Could not clear the freeze journal: rc 0x%08x", (unsigned)rc);
            st_color = rc == 0 ? GREEN : RED;
        }
        int rows = table_rows();
        int max_scroll = D.s.count > rows ? D.s.count - rows : 0;
        if (pressed & BTN_RIGHT) page = (page + 1) % PAGES;
        if (pressed & BTN_LEFT) page = (page + PAGES - 1) % PAGES;
        if (page == 0) {
            if (pressed & BTN_DOWN) scroll++;
            if (pressed & BTN_UP) scroll--;
            if (pressed & BTN_R1) scroll += rows;
            if (pressed & BTN_L1) scroll -= rows;
            scroll = scroll < 0 ? 0 : scroll > max_scroll ? max_scroll : scroll;
        } else if (page == 2) {              /* the rows above the two pinned ones */
            int list = rows - 2, max_t = tr_n > list ? tr_n - list : 0;
            if (pressed & BTN_DOWN) tscroll++;
            if (pressed & BTN_UP) tscroll--;
            if (pressed & BTN_R1) tscroll += list;
            if (pressed & BTN_L1) tscroll -= list;
            tscroll = tscroll < 0 ? 0 : tscroll > max_t ? max_t : tscroll;
        }

        if ((pressed & BTN_CROSS) && !running && D.ata_ok && !D.demo) {
            run_job_report("Reading SMART data", job_smart);
        }
        if (pressed & BTN_SELECT) {
            poll_settle();                   /* the copy reads the journal */
            st_color = report_to_usb(st_msg, sizeof st_msg);
        }
        if (fired == BTN_R2 && !running && !D.demo) {
            run_job_report("Speed test: writing and reading a 64 MB file", job_speed);
            speed_screen();             /* the result; the summary page keeps its own line */
            page = 1;
        }
        if ((pressed & BTN_TRIANGLE) && !running && drive_can_selftest(&D) && (st_type = selftest_choice()) != 0) {
            log_before_count = D.have_stlog ? D.log.count : 0;
            if (log_before_count) log_before = D.log.e[0];
            run_job(st_type == 2 ? "Starting the extended self-test" : "Starting the short self-test", job_selftest);
            if (job_rc == 0) {
                running = 1;
                seen_running = 0;
                started = sysGetSystemTime();
                next_poll = started + 3000000;
                deadline = started + 60000000;
                selftest_screen();
            } else {
                snprintf(st_msg, sizeof st_msg, "Self-test start refused: rc 0x%08x", (unsigned)D.selftest_rc);
                st_color = RED;
            }
        }
        selftest_poll();
        if (page == 2 && tr_seq != trend_seq && D.ata_ok && D.have_smart && !D.demo) {   /* a line was added since the last load */
            trend_load();
            tscroll = 0;
        }

        begin_frame();
        title();
        if (D.ata_ok && D.have_identify) draw_main(page, page == 2 ? tscroll : scroll, st_msg[0] ? st_msg : NULL, st_color);
        else {
            draw_probe_screen();
            if (st_msg[0]) status_line(st_msg, st_color);
            footer(D.demo ? "SQUARE twice clear journal  SELECT report to USB  CIRCLE home  START exit"
                          : "SQUARE twice clear journal  R2 twice speed test  SELECT report to USB  CIRCLE home  START exit");
        }
        report_footnote();
        ui_flip();
    }
}

/* The tile on the home screen: the health line, then the model (small, on
 * up to two lines). Demo data is not kept. */
static void drive_tile(void)
{
    char l1[48], l2[48], why[256];
    if (D.demo) return;
    if (!D.ata_ok || !D.have_smart) {
        snprintf(l1, sizeof l1, "%s", !D.opened ? "Open refused" : D.identify_frozen ? "IDENTIFY froze before"
                                       : !D.ata_ok ? "ATA commands refused" : "SMART not readable");
        snprintf(l2, sizeof l2, "%.44s", D.have_identify ? D.id.model : "Drive not read");
        state_set("drive", DOT_WARN, l1, l2);
        return;
    }
    int hl = smart_health(&D.s, D.have_stlog ? &D.log : NULL, D.sum.temp_limit, why, sizeof why);
    if (D.sum.hours >= 0) snprintf(l1, sizeof l1, "Health %s, %d h", hl == HEALTH_OK ? "OK" : hl == HEALTH_WARN ? "WARN" : "FAIL", D.sum.hours);
    else snprintf(l1, sizeof l1, "Health %s", hl == HEALTH_OK ? "OK" : hl == HEALTH_WARN ? "WARN" : "FAIL");
    snprintf(l2, sizeof l2, "%.44s", D.id.model);
    state_set("drive", hl == HEALTH_OK ? DOT_OK : hl == HEALTH_WARN ? DOT_WARN : DOT_BAD, l1, l2);
}

int mod_drive_open(void)
{
    if (!fs_ok) return 0;                    /* the home screen says why */
    if (!D.ata_ok) {                         /* the first open, after demo mode, or after a refused open or IDENTIFY */
        int r = start_screen();
        if (r <= 0) return r;
        drive_close(&D);                     /* a refused IDENTIFY leaves the drive open; probe and demo start fresh */
        armed = 0;
        if (r == 2) {
            if (drive_demo_load(&D)) {
                memset(&D, 0, sizeof D);
                D.demo = 1;
                snprintf(D.identify_note, sizeof D.identify_note, "no demo sectors in " DEMO_DIR);
            }
            ui_demo = 1;
            D.probed = 1;
        } else {
            drive_load_prev(&D);
            run_job("Reading the drive", job_probe);   /* drive_probe clears D, so the flag comes after it */
            D.probed = 1;
            if (optional_steps() < 0) return -1;
        }
        run_job_report("Reading the console temperatures", job_console);
    }
    int back = run();
    drive_tile();
    if (D.demo) {                            /* demo data is not the drive: the next open asks again */
        memset(&D, 0, sizeof D);
        D.cpu_temp = D.rsx_temp = -1;
        ui_demo = 0;
    } else help_keep();                      /* Help in a later session shows this result */
    return back;
}

/* ---- the quick check ----------------------------------------------------- */

/* The drive's first read of the session, as CROSS on the start screen (no
 * optional step: their prompts come in the module), else a new SMART read;
 * then the console temperatures, as the module's open. No self-test. */
void drive_quick(void)
{
    if (!D.ata_ok) {
        drive_close(&D);
        drive_load_prev(&D);
        drive_probe(&D, progress);           /* it clears D, so the flag comes after it */
        D.probed = 1;
    } else {
        progress("Reading SMART data");
        drive_read_smart(&D, 1);
    }
    console_info(&D);
}

int drive_quick_end(char *line, int n)
{
    char why[256];
    drive_tile();
    help_keep();
    if (!D.ata_ok || !D.have_smart) {
        snprintf(line, n, "%s", !D.opened ? "the system refused to open the drive" : D.identify_frozen ? "IDENTIFY froze this console before"
                                  : !D.ata_ok ? "ATA commands refused" : "SMART not readable");
        return DOT_WARN;
    }
    int hl = smart_health(&D.s, D.have_stlog ? &D.log : NULL, D.sum.temp_limit, why, sizeof why);
    snprintf(line, n, "Health %s: %s", hl == HEALTH_OK ? "OK" : hl == HEALTH_WARN ? "WARN" : "FAIL", why);
    return hl == HEALTH_OK ? DOT_OK : hl == HEALTH_WARN ? DOT_WARN : DOT_BAD;
}

void mod_drive_report(void)
{
    if (!D.probed) return;
    drive_report(&D, rep_when());
}
