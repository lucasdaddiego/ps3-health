/* heat.h: the families, the paste verdict, the rise and the halving. */
#include <stddef.h>
#include <string.h>
#include "heat.h"

/* The ranges are provisional. The super slim row comes from one console, the
 * author's CECH-4004 (2.0.0 load test, 2 minutes: Cell 51 to 67 C, RSX 55 to
 * 71 C, fan 33 to 49 %, with a margin; idle 56 to 61 C over the 2.0.x to
 * 2.2.0 tests, 61 C when a test starts right after another one). The fat and slim rows are guesses
 * from the chips' generations: the 90 and 65 nm fat consoles run hotter than
 * the 45 and 40 nm slims. No fat or slim console was measured. The compat
 * report carries the Cooling summary so that real readings can replace
 * them. The peak limit stays under the load test's stop at 75 C. */
static const heat_family families[] = {
    {"fat", "Fat", "CECHA to CECHQ", 62, 74, 14, 70},
    {"slim", "Slim", "CECH-20xx to 30xx", 56, 72, 12, 60},
    {"superslim", "Super slim", "CECH-40xx", 62, 72, 12, 60},
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

int heat_rise(const short *v, int n, int step_s)
{
    int k = step_s > 0 ? 60 / step_s : 0;
    if (k <= 0 || k >= n || v[0] < 0 || v[k] < 0) return -1;
    return (v[k] - v[0]) * 60 / (k * step_s);
}

void heat_paste_judge(int family, int idle, int peak, int rise, int fan, heat_paste *p)
{
    const heat_family *f = heat_family_get(family);
    const int v[4] = {idle, peak, rise, fan};
    p->family = family, p->idle = idle, p->peak = peak, p->rise = rise, p->fan = fan;
    p->over = p->measured = 0;
    p->verdict = PASTE_NONE;
    if (!f) return;
    const int lim[4] = {f->idle_max, f->peak_max, f->rise_max, f->fan_max};
    int n_over = 0;
    for (int i = 0; i < 4; i++) {
        if (v[i] < 0) continue;
        p->measured++;
        if (v[i] > lim[i]) { p->over |= 1 << i; n_over++; }
    }
    if (idle < 0 || peak < 0 || rise < 0) return;   /* the three temperatures at least (a minute of test): the fan read may be skipped */
    p->verdict = n_over == 0 ? PASTE_OK : n_over == 1 ? PASTE_CHECK : PASTE_LIKELY;
}

const char *heat_paste_text(const heat_paste *p)
{
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
