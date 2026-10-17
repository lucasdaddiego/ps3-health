/* The sounds of the Audio module: a sine on some channels of an interleaved
 * float frame, as beeps or as one sound, at one frequency or as a sweep, with
 * a raised-cosine fade at every start and end so that no edge clicks. Pure C,
 * no PS3 headers: test/test_smart.c builds this file on the Mac. */
#ifndef TONE_H
#define TONE_H

#define TONE_RATE 48000                      /* the PS3 audio port's rate */

typedef struct {
    int channels;                            /* in the frame: 2 or 8 */
    unsigned mask;                           /* bit c: channel c plays */
    double f0, f1;                           /* Hz; f1 > f0: a sweep from f0 to f1 over len, even on a log scale */
    double amp;                              /* the peak, 0 to 1 */
    long len;                                /* samples of the sound, -1: beeps until tone_stop */
    long beep, period;                       /* beeps: beep samples on in every period */
    long fade;                               /* samples of each fade in and fade out */
    long pos;                                /* samples played */
    long stop;                               /* tone_stop: the sample where the fade out ends, -1 none */
    long cut, cut_len;                       /* tone_cut: the sample where its short fade ends (-1 none), and its length */
    double phase;
} tone;

double tone_db(double dbfs);                 /* -18 -> 0.126: the peak of a sine at that level */
void tone_beeps(tone *t, int channels, unsigned mask, double hz, double dbfs);   /* 0.6 s on, 0.4 s off, until tone_stop */
void tone_sweep(tone *t, int channels, unsigned mask, double f0, double f1, double seconds, double dbfs);
void tone_stop(tone *t);                     /* a fade out from now, then silence */
void tone_cut(tone *t, long samples);        /* a short fade out from now (the app's exit), on top of any fade already running */
int tone_done(const tone *t);                /* the sound ended: silence from here */
double tone_freq(const tone *t);             /* the frequency now */
void tone_fill(tone *t, float *out, int frames);   /* frames x channels samples; silence once done */

#endif
