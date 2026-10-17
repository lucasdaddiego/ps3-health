/* heat.h: the families, the idle warning, the warm start, the paste verdict,
 * the rise, the halving and the compat line. */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "heat.h"

/* The ranges are provisional. The super slim row comes from one console, the
 * author's CECH-4004 (2.0.0 load test, 2 minutes: Cell 51 to 67 C, RSX 55 to
 * 71 C, fan 33 to 49 %, with a margin). Its idle reads 56 to 58 C cold over
 * the 2.0.x to 2.2.0 tests, so the idle limit is 59 C; 61 C when a test
 * starts right after another one, which is a warm start (heat_warm_start):
 * then idle and rise leave the verdict. The fat and slim rows are guesses
 * from the chips' generations: the 90 and 65 nm fat consoles run hotter than
 * the 45 and 40 nm slims. No fat or slim console was measured. The compat
 * report carries the Cooling summary so that real readings can replace
 * them. The peak limit stays under the load test's stop at 75 C. */
static const heat_family families[] = {
    {"fat", "Fat", "CECHA to CECHQ", 62, 74, 14, 70},
    {"slim", "Slim", "CECH-20xx to 30xx", 56, 72, 12, 60},
    {"superslim", "Super slim", "CECH-40xx", 59, 72, 12, 60},
};

const heat_family *heat_family_get(int family)
{
    return family >= FAMILY_FAT && family <= FAMILY_SUPER ? &families[family - FAMILY_FAT] : NULL;
}

int heat_family_parse(const char *s)
{
    size_t n = strcspn(s, "\r\n");
    if (n == 7 && !strncmp(s, "unknown", 7)) return FAMILY_UNKNOWN;
    for (int f = FAMILY_FAT; f <= FAMILY_SUPER; f++)
        if (strlen(families[f - FAMILY_FAT].key) == n && !strncmp(s, families[f - FAMILY_FAT].key, n)) return f;
    return FAMILY_NONE;
}

int heat_idle_hot(int family, int t)
{
    const heat_family *f = heat_family_get(family);
    if (t < 0) return 0;
    return f ? t > f->idle_max + HEAT_IDLE_MARGIN : t >= HEAT_IDLE_WARN;
}

int heat_warm_start(long since_s, int open_c, int start_c)
{
    if (since_s >= 0 && since_s < HEAT_REST_S) return 1;
    return open_c >= 0 && start_c > open_c + HEAT_WARM_C;
}

int heat_rise(const short *v, int n, int step_s)
{
    int k = step_s > 0 ? 60 / step_s : 0;
    if (k <= 0 || k >= n || v[0] < 0 || v[k] < 0) return -1;
    return (v[k] - v[0]) * 60 / (k * step_s);
}

void heat_paste_judge(int family, int idle, int peak, int rise, int fan, int flags, heat_paste *p)
{
    const heat_family *f = heat_family_get(family);
    const int v[4] = {idle, peak, rise, fan};
    p->family = family, p->idle = idle, p->peak = peak, p->rise = rise, p->fan = fan, p->flags = flags;
    p->over = 0;
    p->verdict = PASTE_NONE;
    if (!f) return;
    const int lim[4] = {f->idle_max, f->peak_max, f->rise_max, f->fan_max};
    int n_over = 0;
    for (int i = 0; i < 4; i++) {
        if (v[i] < 0 || ((flags & HEAT_WARM) && (i == 0 || i == 2))) continue;   /* a warm start: idle and rise are the last load's */
        if (v[i] > lim[i]) { p->over |= 1 << i; n_over++; }
    }
    if (flags & HEAT_HOT_EARLY) { p->verdict = PASTE_LIKELY; return; }   /* the stop within a minute: no rise to wait for */
    if (idle < 0 || peak < 0 || rise < 0) return;   /* the three temperatures at least (a minute of test): the fan read may be skipped */
    p->verdict = n_over == 0 ? PASTE_OK : n_over == 1 ? PASTE_CHECK : PASTE_LIKELY;
}

const char *heat_paste_text(const heat_paste *p)
{
    if (p->verdict == PASTE_LIKELY && (p->flags & HEAT_HOT_EARLY))
        return "Estimate: the test reached the app's limit within its first minute. Dry paste or dust is likely: clean it first.";
    if (p->verdict != PASTE_NONE && (p->flags & HEAT_WARM)) switch (p->verdict) {
        case PASTE_OK: return "Warm start: the peak and the fan are in the usual range. Rest about 10 minutes for a full estimate.";
        case PASTE_CHECK: return "Warm start: one value above the usual range. Rest about 10 minutes, then test again for a full estimate.";
        default: return "Warm start, and the peak and the fan are above the usual range: dry paste or dust is likely. Clean it first.";
        }
    switch (p->verdict) {
    case PASTE_OK: return "Estimate: within the usual range for this model, no sign of dry paste.";
    case PASTE_CHECK: return "Estimate: one value above the usual range. Dust or old paste is possible.";
    case PASTE_LIKELY: return "Estimate: several values above the usual range. Dry paste or dust is likely: clean it first.";
    default:
        return heat_family_get(p->family) ? "No paste estimate: the test needs a minute or more of readings."
                                          : "No paste estimate: choose the console model with L1.";
    }
}

int heat_halve(short *v, int n)
{
    int m = 0;
    for (int i = 0; i < n; i += 2) {
        short a = v[i], b = i + 1 < n ? v[i + 1] : -1;
        v[m++] = a > b ? a : b;              /* -1 only when both are -1 */
    }
    return m;
}

/* Appends to out at j, as snprintf; j stays at the end when out is full. */
static int app(char *out, int n, int j, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int app(char *out, int n, int j, const char *fmt, ...)
{
    if (j >= n - 1) return j;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(out + j, (size_t)(n - j), fmt, ap);
    va_end(ap);
    return k < 0 ? j : j + k < n ? j + k : n - 1;
}

/* ", idle 58/55": a Cell and RSX pair, "-" for one not read; nothing when both are not. */
static int pair(char *out, int n, int j, const char *key, int c, int r)
{
    char a[12] = "-", b[12] = "-";           /* any int: gcc's format-truncation check */
    if (c < 0 && r < 0) return j;
    if (c >= 0) snprintf(a, sizeof a, "%d", c);
    if (r >= 0) snprintf(b, sizeof b, "%d", r);
    return app(out, n, j, ", %s %s/%s", key, a, b);
}

int heat_compat_line(const heat_report *r, int detail, char *out, int n)
{
    const heat_family *f = heat_family_get(r->family);
    int j = app(out, n, 0, "%s, Cell/RSX C", f ? f->name : "model not set");
    j = pair(out, n, j, "idle", r->cpu0, r->rsx0);
    j = pair(out, n, j, "peak", r->cpu_peak, r->rsx_peak);
    if (detail && (r->cpu_max != r->cpu_peak || r->rsx_max != r->rsx_peak)) j = pair(out, n, j, "max", r->cpu_max, r->rsx_max);
    if (r->rise >= 0) j = app(out, n, j, ", rise %d/min", r->rise);
    if (!detail) return j;
    if (r->fan0 >= 0 && r->fan_end >= 0) j = app(out, n, j, ", fan %d>%d%%", r->fan0, r->fan_end);
    j = app(out, n, j, ", %s %d s%s, %s", r->soak ? "soak" : "load", r->seconds, r->stopped ? " (stopped)" : "", r->spus ? "6 SPUs" : "no SPUs");
    if (r->warm) j = app(out, n, j, ", warm start");
    return j;
}
