/* The Display module: the video output, the visible area of the TV (measured
 * at the first start, then every screen of the app is drawn 1:1 inside it),
 * full-screen test patterns, and a second page with what the TV told the
 * console (videoGetDeviceInfo: the port, the colour space and range, the EDID
 * latency, the primaries and the gamma, the modes it accepts; one journaled
 * read, display_tv). No system call beyond the two video reads. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sysutil/video.h>
#include <lv2/systime.h>
#include "app.h"
#include <ps3gfx/ui.h>
#include "report.h"

static videoState vs;
static videoResolution vr;
static int have_vs;
static int visited;
static videoDeviceInfo tv;                   /* the TV's data, read once per session */
static int tv_rc = -1, tv_frozen, tv_read, tv_devices;   /* tv_rc: the read's rc, -1 not read; tv_devices: on the output, -1 not read */
static int page;                             /* 0 the output, 1 the TV */
static const char *const page_names[] = {"Output", "TV"};

static const char *res_name(int id)
{
    switch (id) {
    case VIDEO_RESOLUTION_1080: return "1080";
    case VIDEO_RESOLUTION_720: return "720";
    case VIDEO_RESOLUTION_480: return "480";
    case VIDEO_RESOLUTION_576: return "576";
    case VIDEO_RESOLUTION_1600x1080: return "1600x1080";
    case VIDEO_RESOLUTION_1440x1080: return "1440x1080";
    case VIDEO_RESOLUTION_1280x1080: return "1280x1080";
    case VIDEO_RESOLUTION_960x1080: return "960x1080";
    default: return "?";
    }
}

static const char *color_space(int c) { return c == 1 ? "RGB" : c == 2 ? "YUV" : c == 4 ? "xvYCC" : "?"; }

/* The TV's colour spaces: a mask of the three (the test TV gave 7). */
static void color_spaces(unsigned bits, char *out, int n)
{
    int j = 0;
    out[0] = 0;
    if (bits & 1) j += snprintf(out + j, n - j, "RGB");
    if (bits & 2) j += snprintf(out + j, n - j, "%sYUV", j ? ", " : "");
    if (bits & 4) j += snprintf(out + j, n - j, "%sxvYCC", j ? ", " : "");
    if (!j) snprintf(out, n, "none listed (%u)", bits);
}

static void refresh_text(unsigned bits, char *out, int n)
{
    int j = 0;
    out[0] = 0;
    if (bits & 1) j += snprintf(out + j, n - j, "%s59.94", j ? "/" : "");
    if (bits & 2) j += snprintf(out + j, n - j, "%s50", j ? "/" : "");
    if (bits & 4) j += snprintf(out + j, n - j, "%s60", j ? "/" : "");
    if (bits & 8) j += snprintf(out + j, n - j, "%s30", j ? "/" : "");
    if (!j) snprintf(out, n, "?");
}

static void read_video(void)
{
    memset(&vs, 0, sizeof vs);
    memset(&vr, 0, sizeof vr);
    have_vs = videoGetState(0, 0, &vs) == 0;
    if (have_vs) videoGetResolution(vs.displayMode.resolution, &vr);
}

static void mode_line(char *out, int n)
{
    char rr[32];
    if (!have_vs) { snprintf(out, n, "video state not readable"); return; }
    refresh_text(vs.displayMode.refreshRates, rr, sizeof rr);
    snprintf(out, n, "%ux%u %s%s, %s, %s, %s Hz", vr.width, vr.height, res_name(vs.displayMode.resolution),
             vs.displayMode.scanMode == VIDEO_SCANMODE_PROGRESSIVE ? "p" : "i",
             vs.displayMode.aspect == VIDEO_ASPECT_16_9 ? "16:9" : vs.displayMode.aspect == VIDEO_ASPECT_4_3 ? "4:3" : "aspect auto",
             color_space(vs.colorSpace), rr);
}

static int at_1080(void) { return have_vs && vr.width == 1920 && vr.height == 1080; }

/* ---- test patterns -------------------------------------------------------- */

static const char *const pattern_names[] = {"white", "black", "red", "green", "blue", "gray ramp", "crosshatch and circle",
                                            "1-pixel stripes", "lag timer"};
static const char *const pattern_notes[] = {
    "A dark dot is a dead pixel. Also look for uneven brightness at the edges.",
    "A lit dot is a stuck pixel. Also look for light bleeding in from the edges.",
    "A dot in another colour is a stuck sub-pixel.",
    "A dot in another colour is a stuck sub-pixel.",
    "A dot in another colour is a stuck sub-pixel.",
    "16 steps from black to white: each step should look different from its neighbours.",
    "Straight lines, even squares and a round circle: the TV keeps the geometry and the aspect.",
    "",
    "Film the TV next to this screen's source: the timer difference is the lag. CROSS flashes one frame.",
};
#define NPAT ((int)(sizeof pattern_names / sizeof pattern_names[0]))

static void draw_pattern(int p, s64 t0, int frame, int flash)
{
    switch (p) {
    case 0: rect(0, 0, 1920, 1080, WHITE); break;
    case 1: rect(0, 0, 1920, 1080, BLACK); break;
    case 2: rect(0, 0, 1920, 1080, 0xff0000ff); break;
    case 3: rect(0, 0, 1920, 1080, 0x00ff00ff); break;
    case 4: rect(0, 0, 1920, 1080, 0x0000ffff); break;
    case 5:
        for (int k = 0; k < 16; k++) {
            u32 v = k * 17;
            rect(k * 120, 0, 120, 1080, (v << 24) | (v << 16) | (v << 8) | 0xff);
            text_c(k * 120 + 60, 540, F_SMALL, k < 8 ? WHITE : BLACK, "%u", v);
        }
        break;
    case 6:
        rect(0, 0, 1920, 1080, 0x202020ff);
        for (int x = 0; x <= 1920; x += 120) rect(x - 1, 0, 2, 1080, WHITE);
        for (int y = 0; y <= 1080; y += 108) rect(0, y - 1, 1920, 2, WHITE);
        rect(959, 0, 2, 1080, YELLOW);
        rect(0, 539, 1920, 2, YELLOW);
        ring(960, 540, 432, 3, YELLOW);      /* round on a 16:9 picture shown at the right aspect */
        break;
    case 7:
        /* every other pixel: even stripes mean the TV shows each pixel 1:1;
         * moire or soft bands mean it scales the picture (overscan) */
        rect(0, 0, 1920, 1080, BLACK);
        batch_begin(WHITE);                  /* 1020 stripes: one polygon */
        for (int x = 0; x < 960; x += 2) batch_rect(x, 0, 1, 1080);
        for (int y = 0; y < 1080; y += 2) batch_rect(960, y, 960, 1);
        batch_end();
        break;
    case 8: {
        double ms = (double)(sysGetSystemTime() - t0) / 1e3;
        if (flash) rect(0, 0, 1920, 1080, WHITE);
        float x = (frame * 8) % 1920;
        rect(x, 160, 12, 760, WHITE);
        for (int k = 0; k <= 16; k++) rect(k * 120 - 1, 120, 2, 24, GREY);
        text(safe_l + 60, safe_t + 220, F_LARGE | TNUM, GREEN, "%.0f ms", ms);
        text(safe_l + 60, safe_t + 300, F_MED | TNUM, WHITE, "frame %d", frame);
        break;
    }
    }
}

static void patterns_screen(void)
{
    int p = 0, frame = 0, flash = 0;
    s64 t0 = sysGetSystemTime(), shown = t0;
    safe_override(1);
    while (!quit) {
        read_pad();
        if (pressed & BTN_CIRCLE) break;
        if (pressed & BTN_RIGHT) { p = (p + 1) % NPAT; shown = t0 = sysGetSystemTime(); frame = 0; }
        if (pressed & BTN_LEFT) { p = (p + NPAT - 1) % NPAT; shown = t0 = sysGetSystemTime(); frame = 0; }
        flash = (pressed & BTN_CROSS) != 0;
        begin_frame();
        draw_pattern(p, t0, frame, flash);
        if (sysGetSystemTime() - shown < 3000000 || p >= 7) {     /* the legend, inside the visible area */
            float x = safe_l + 40, y = safe_b - 190, w = safe_r - safe_l - 80;
            round_rect(x, y, w, 150, 16, 0x0b1424e0);
            text(x + 32, y + 22, F_MED, WHITE, "%d/%d  %s", p + 1, NPAT, pattern_names[p]);
            if (p == 7)
                text_fit(x + 32, y + 66, F_BODY, at_1080() ? GREY : YELLOW, w - 64, "%s",
                         at_1080() ? "Even stripes: the TV shows every pixel. Moire bands: it scales the picture (overscan on)."
                                   : "Only meaningful at 1080p output: this output scales the picture.");
            else text_fit(x + 32, y + 66, F_BODY, GREY, w - 64, "%s", pattern_notes[p]);
            footer_at(x + 32, y + 104, "LEFT/RIGHT pattern  CIRCLE back");
        }
        ui_flip();
        frame++;
    }
    safe_override(0);
    pressed = 0;
}

/* ---- the TV page ---------------------------------------------------------- */

static void read_tv(void)
{
    if (tv_read) return;
    tv_read = 1;
    memset(&tv, 0, sizeof tv);
    tv_devices = -1;
    if (fs_ok && !journal_start("display_tv")) { tv_frozen = 1; return; }   /* 0 for a frozen or skipped entry */
    tv_devices = videoGetNumberOfDevice(0);
    tv_rc = videoGetDeviceInfo(0, 0, &tv);
    if (fs_ok) journal_done("display_tv", tv_rc, tv_rc == 0);
}

static const char *port_name(int p)
{
    switch (p) {
    case VIDEO_PORT_NONE: return "none";
    case VIDEO_PORT_HDMI: return "HDMI";
    case VIDEO_PORT_NETWORK: return "network";
    case VIDEO_PORT_COMPOSITE: return "composite (AV multi)";
    case VIDEO_PORT_D: return "D-terminal";
    case VIDEO_PORT_COMPONENT: return "component (AV multi)";
    case VIDEO_PORT_RGB: return "RGB";
    case VIDEO_PORT_SCART: return "SCART";
    case VIDEO_PORT_DSUB: return "D-sub";
    default: return "?";
    }
}

/* "1080p 16:9 59.94/60 Hz" for one of the TV's modes. */
static void mode_text(const videoDisplayMode *m, char *out, int n)
{
    char rr[32];
    refresh_text(m->refreshRates, rr, sizeof rr);
    snprintf(out, n, "%s%s %s %s Hz", res_name(m->resolution), m->scanMode == VIDEO_SCANMODE_PROGRESSIVE ? "p" : "i",
             m->aspect == VIDEO_ASPECT_16_9 ? "16:9" : m->aspect == VIDEO_ASPECT_4_3 ? "4:3" : "auto", rr);
}

/* The modes as one text, the duplicates (the same mode with another
 * conversion flag) once. */
static void modes_text(char *out, int n)
{
    int j = 0, count = tv.availableModeCount > 32 ? 32 : tv.availableModeCount;
    out[0] = 0;
    for (int k = 0; k < count; k++) {
        char m[48];
        mode_text(&tv.availableModes[k], m, sizeof m);
        int dup = 0;
        for (int i = 0; i < k && !dup; i++) {
            char o[48];
            mode_text(&tv.availableModes[i], o, sizeof o);
            dup = !strcmp(m, o);
        }
        if (dup) continue;
        int w = snprintf(out + j, n - j, "%s%s", j ? ", " : "", m);
        if (w < 0 || j + w >= n) break;
        j += w;
    }
    if (!j) snprintf(out, n, "none listed");
}

static const char *tv_not_read(void)
{
    return tv_frozen ? "Reading the TV data froze this console before: not read again. SQUARE twice on the home screen clears the journal."
         : tv_rc     ? "The system did not give the TV data."
                     : "";
}

static float row(float x, float y, float kw, float w, const char *key, u32 c, const char *fmt, ...) __attribute__((format(printf, 7, 8)));
static float row(float x, float y, float kw, float w, const char *key, u32 c, const char *fmt, ...)
{
    va_list ap;
    text(x, y, F_BODY, GREY, "%s", key);
    va_start(ap, fmt);
    vtext_fit(x + kw, y, F_BODY, c, w - kw, fmt, ap);
    va_end(ap);
    return y + 44;
}

/* Every primary is a fraction of 1024 in the EDID (10 bits); the values are
 * shown as the system gives them and, when all are under 1024, as x and y. */
static int primaries_plain(void)
{
    const videoColorInfo *c = &tv.colorInfo;
    return c->redX < 1024 && c->redY < 1024 && c->greenX < 1024 && c->greenY < 1024 && c->blueX < 1024 && c->blueY < 1024 && c->whiteX < 1024 &&
           c->whiteY < 1024;
}

static void draw_tv(void)
{
    float w = SW - 2 * MG, cw = (w - 32) / 2, h1 = 330, y2 = TOP + h1 + 24;
    char s[640], cs[32];
    card(MG, TOP, cw, h1);
    float cx = MG + 32, kw = 190, y = heading(cx, TOP + 22, "TV");
    if (tv_rc) {
        text_wrap(cx, y + 8, F_BODY, YELLOW, cw - 64, 4, tv_not_read());
        card(MG + cw + 32, TOP, cw, h1);
        heading(MG + cw + 64, TOP + 22, "COLOUR");
        return;
    }
    y += 6;
    y = row(cx, y, kw, cw - 64, "Port", WHITE, "%s%s", port_name(tv.portType), tv_devices > 1 ? " (more devices on the output)" : "");
    y = row(cx, y, kw, cw - 64, "State", tv.state == VIDEO_STATE_ENABLED ? WHITE : YELLOW, "%s", tv.state == VIDEO_STATE_ENABLED ? "enabled" : tv.state == VIDEO_STATE_DISABLED ? "disabled" : tv.state == VIDEO_STATE_BUSY ? "busy" : "?");
    color_spaces(tv.colorSpace, cs, sizeof cs);
    y = row(cx, y, kw, cw - 64, "Colour spaces", WHITE, "%s", cs);
    y = row(cx, y, kw, cw - 64, "RGB range", WHITE, "%s", tv.rgbOutputRange ? "full, 0 to 255" : "limited, 16 to 235");
    if (tv.latency) y = row(cx, y, kw, cw - 64, "Latency", WHITE, "%u ms, from the EDID", tv.latency);
    else y = row(cx, y, kw, cw - 64, "Latency", GREY, "none reported in the EDID");
    row(cx, y, kw, cw - 64, "HDCP", WHITE, "%u receiver key%s", (unsigned)tv.ksvList.count, tv.ksvList.count == 1 ? "" : "s");
    card(MG + cw + 32, TOP, cw, h1);
    cx = MG + cw + 64;
    y = heading(cx, TOP + 22, "COLOUR") + 6;
    const videoColorInfo *c = &tv.colorInfo;
    if (primaries_plain()) {
        y = row(cx, y, kw, cw - 64, "Red", WHITE, "x %.3f  y %.3f", c->redX / 1024.0, c->redY / 1024.0);
        y = row(cx, y, kw, cw - 64, "Green", WHITE, "x %.3f  y %.3f", c->greenX / 1024.0, c->greenY / 1024.0);
        y = row(cx, y, kw, cw - 64, "Blue", WHITE, "x %.3f  y %.3f", c->blueX / 1024.0, c->blueY / 1024.0);
        y = row(cx, y, kw, cw - 64, "White", WHITE, "x %.3f  y %.3f", c->whiteX / 1024.0, c->whiteY / 1024.0);
    } else {
        y = row(cx, y, kw, cw - 64, "Red", WHITE, "%u %u", c->redX, c->redY);
        y = row(cx, y, kw, cw - 64, "Green", WHITE, "%u %u", c->greenX, c->greenY);
        y = row(cx, y, kw, cw - 64, "Blue", WHITE, "%u %u", c->blueX, c->blueY);
        y = row(cx, y, kw, cw - 64, "White", WHITE, "%u %u", c->whiteX, c->whiteY);
    }
    if (c->gamma >= 100 && c->gamma < 400) y = row(cx, y, kw, cw - 64, "Gamma", WHITE, "%u.%02u", c->gamma / 100, c->gamma % 100);
    else y = row(cx, y, kw, cw - 64, "Gamma", GREY, "%u (as given)", c->gamma);
    text_fit(cx, y + 4, F_SMALL, DIM, cw - 64, "CIE 1931 x y from the TV's EDID. Rec. 709: red 0.640 0.330, white 0.313 0.329.");
    if (y2 + 100 > BOTTOM) return;
    card(MG, y2, w, BOTTOM - 4 - y2);
    y = heading(MG + 32, y2 + 22, "MODES THE TV ACCEPTS");
    modes_text(s, sizeof s);
    int lines = (int)((BOTTOM - 4 - y2 - 16 - (y - y2)) / wrap_step(F_BODY));
    text_wrap(MG + 32, y + 8, F_BODY, WHITE, w - 64, lines < 1 ? 1 : lines, s);
}

/* ---- module --------------------------------------------------------------- */

static int measured(void) { return safe_l != SAFE_DEF_L || safe_t != SAFE_DEF_T || safe_r != SAFE_DEF_R || safe_b != SAFE_DEF_B; }

static void display_tile(void)
{
    char l1[48], l2[48];
    if (have_vs)
        snprintf(l1, sizeof l1, "%ux%u%s %s", vr.width, vr.height, vs.displayMode.scanMode == VIDEO_SCANMODE_PROGRESSIVE ? "p" : "i",
                 vs.displayMode.aspect == VIDEO_ASPECT_16_9 ? "16:9" : vs.displayMode.aspect == VIDEO_ASPECT_4_3 ? "4:3" : "");
    else snprintf(l1, sizeof l1, "Video state not readable");
    snprintf(l2, sizeof l2, "Visible area %d x %d", safe_r - safe_l, safe_b - safe_t);
    state_set("display", DOT_OK, l1, l2);
}

static void draw_output(void)
{
    char mode[96];
    mode_line(mode, sizeof mode);
    float y = TOP, w = SW - 2 * MG;
    card(MG, y, w, 168);
    heading(MG + 32, y + 22, "OUTPUT");
    text(MG + 32, y + 62, F_MED, WHITE, "%s", mode);
    text_fit(MG + 32, y + 112, F_BODY, GREY, w - 64, "%s",
             at_1080() ? "The app draws 1:1 at this output: every pixel of its 1920 x 1080 space is a pixel on the TV."
                       : "The app draws in a 1920 x 1080 space; the console scales it to this output.");
    y += 196;
    card(MG, y, w, 168);
    heading(MG + 32, y + 22, "VISIBLE AREA");
    text(MG + 32, y + 62, F_MED, measured() ? GREEN : WHITE, "%d x %d  %s", safe_r - safe_l, safe_b - safe_t,
         measured() ? "measured on this TV" : "the default, 93 % of the screen");
    text_fit(MG + 32, y + 112, F_BODY, GREY, w - 64, "left %d, top %d, right %d, bottom %d.  CROSS adjusts it.", safe_l, safe_t,
             safe_r, safe_b);
    y += 196;
    if (y + 168 <= BOTTOM) {
        card(MG, y, w, 168);
        heading(MG + 32, y + 22, "TEST PATTERNS");
        text_wrap(MG + 32, y + 62, F_BODY, WHITE, w - 64, 3,
                  "TRIANGLE: plain colours for dead pixels, a gray ramp, a crosshatch with a circle for geometry and "
                  "aspect, 1-pixel stripes that show whether the TV scales the picture, and a lag timer.");
    }
}

int mod_display_open(void)
{
    read_video();
    read_tv();
    visited = 1;
    page = 0;
    while (1) {
        read_pad();
        int back = ui_leave();
        if (back <= 0) { display_tile(); return back; }
        if (pressed & (BTN_LEFT | BTN_RIGHT)) page ^= 1;
        if (page == 0 && (pressed & BTN_CROSS)) safe_calibrate(0);
        if (page == 0 && (pressed & BTN_TRIANGLE)) patterns_screen();
        begin_frame();
        title();
        tabs(page_names, 2, page);
        if (page == 0) draw_output();
        else draw_tv();
        if (page == 0) footer("CROSS adjust the visible area  TRIANGLE test patterns  RIGHT the TV  CIRCLE home  START exit");
        else footer("LEFT the output  CIRCLE home  START exit");
        ui_flip();
    }
}

void mod_display_report(void)
{
    if (!visited) return;
    char mode[96], s[640], cs[32];
    mode_line(mode, sizeof mode);
    rep_out("--- display ---\noutput %s (resolution id %u, color space %u, refresh bits 0x%x)\n", mode,
            vs.displayMode.resolution, vs.colorSpace, vs.displayMode.refreshRates);
    rep_out("visible area %s: left %d top %d right %d bottom %d of 1920 x 1080\n", measured() ? "measured" : "default",
            safe_l, safe_t, safe_r, safe_b);
    if (!tv_read) return;
    if (tv_rc) { rep_out("tv: not read (%s)\n", tv_frozen ? "froze before" : "refused"); if (tv_rc != -1 && !tv_frozen) rep_out("tv rc 0x%08x\n", (unsigned)tv_rc); return; }
    const videoColorInfo *c = &tv.colorInfo;
    color_spaces(tv.colorSpace, cs, sizeof cs);
    rep_out("tv: port %s (%u), state %u, colour spaces %s (%u), RGB range %s, latency %u ms, HDCP keys %u, devices %d\n", port_name(tv.portType), tv.portType,
            tv.state, cs, tv.colorSpace, tv.rgbOutputRange ? "full" : "limited", tv.latency, (unsigned)tv.ksvList.count, tv_devices);
    rep_out("tv colour: red %u %u, green %u %u, blue %u %u, white %u %u (EDID, of 1024), gamma %u\n", c->redX, c->redY, c->greenX, c->greenY, c->blueX,
            c->blueY, c->whiteX, c->whiteY, c->gamma);
    modes_text(s, sizeof s);
    rep_out("tv modes (%u listed): %s\n", tv.availableModeCount, s);
}
