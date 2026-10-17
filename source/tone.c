/* tone.h: the sine, its envelope and the sweep. */
#include <math.h>
#include <string.h>
#include "tone.h"

#define TWO_PI 6.283185307179586

double tone_db(double dbfs) { return pow(10.0, dbfs / 20.0); }

static void start(tone *t, int channels, unsigned mask, double f0, double f1, double dbfs)
{
    memset(t, 0, sizeof *t);
    t->channels = channels;
    t->mask = mask;
    t->f0 = f0;
    t->f1 = f1;
    t->amp = tone_db(dbfs);
    t->stop = -1;
    t->cut = -1;
}

void tone_beeps(tone *t, int channels, unsigned mask, double hz, double dbfs)
{
    start(t, channels, mask, hz, hz, dbfs);
    t->len = -1;
    t->beep = TONE_RATE * 6 / 10;
    t->period = TONE_RATE;
    t->fade = TONE_RATE / 50;                /* 20 ms */
}

void tone_sweep(tone *t, int channels, unsigned mask, double f0, double f1, double seconds, double dbfs)
{
    start(t, channels, mask, f0, f1, dbfs);
    t->len = (long)(seconds * TONE_RATE);
    t->fade = TONE_RATE / 2;                 /* 0.5 s */
}

void tone_stop(tone *t)
{
    if (t->stop < 0) t->stop = t->pos + t->fade;
}

/* The gain of a running fade out is kept: the cut's own ramp multiplies it,
 * so the sound only gets quieter. A second cut changes nothing (a new ramp
 * would jump back up). */
void tone_cut(tone *t, long samples)
{
    if (samples < 1) samples = 1;
    if (t->cut >= 0) return;
    t->cut = t->pos + samples;
    t->cut_len = samples;
}

int tone_done(const tone *t)
{
    return (t->len >= 0 && t->pos >= t->len) || (t->stop >= 0 && t->pos >= t->stop) || (t->cut >= 0 && t->pos >= t->cut);
}

double tone_freq(const tone *t)
{
    if (t->f1 <= t->f0 || t->len <= 0) return t->f0;
    double x = (double)(t->pos < t->len ? t->pos : t->len) / t->len;
    return t->f0 * pow(t->f1 / t->f0, x);
}

/* 0 to 1 over a fade: half a cosine, so the slope is 0 at both ends. */
static double ramp(long k, long fade) { return k >= fade ? 1 : 0.5 - 0.5 * cos(3.141592653589793 * k / fade); }

/* The envelope at sample pos: the sound's own fades, the beep's, and the
 * fade outs of tone_stop and tone_cut. */
static double envelope(const tone *t, long pos)
{
    double e = 1;
    if (t->len >= 0) {
        if (pos >= t->len) return 0;
        e = ramp(pos, t->fade) * ramp(t->len - pos, t->fade);
    } else {
        long p = pos % t->period;
        if (p >= t->beep) return 0;
        e = ramp(p, t->fade) * ramp(t->beep - p, t->fade);
    }
    if (t->stop >= 0) e *= pos >= t->stop ? 0 : ramp(t->stop - pos, t->fade);
    if (t->cut >= 0) e *= pos >= t->cut ? 0 : ramp(t->cut - pos, t->cut_len);
    return e;
}

void tone_fill(tone *t, float *out, int frames)
{
    memset(out, 0, sizeof(float) * (unsigned)frames * (unsigned)t->channels);
    for (int i = 0; i < frames; i++) {
        if (tone_done(t)) return;
        double e = envelope(t, t->pos);
        float v = (float)(t->amp * e * sin(t->phase));
        for (int c = 0; c < t->channels; c++)
            if (t->mask >> c & 1) out[i * t->channels + c] = v;
        t->phase += TWO_PI * tone_freq(t) / TONE_RATE;
        if (t->phase >= TWO_PI) t->phase -= TWO_PI;
        t->pos++;
    }
}
