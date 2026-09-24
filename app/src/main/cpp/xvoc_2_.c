/*
 * xvoc - a brutally customizable channel vocoder for the Linux command line.
 *
 *   build:  cc -O3 -march=native -o xvoc xvoc.c -lm
 *   try:    espeak --stdout "hello world" | ./xvoc -i - -o - --preset alien | aplay
 *           ./xvoc -i voice.wav -o out.wav --preset choir
 *           ./xvoc --carrier 8 --randomize -o pad.wav      (no input: just the carrier)
 *           ./xvoc -i voice.wav --randomize 12345          (repeatable random patch)
 *           ./xvoc --help
 *
 * Pure C99 + libm. Reads/writes WAV (PCM 8/16/24/32, float 32/64).
 *
 * How it works: the modulator (your voice) goes through a bank of band-pass
 * filters. An envelope follower on each band measures how loud that band is.
 * A carrier (built-in synth, or another WAV) goes through a *second* bank and
 * each carrier band is scaled by an envelope. Almost every step in between is
 * tweakable: band mapping, formant shifting, spectral warping, envelope
 * shaping, pitch tracking, post effects, and so on.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <getopt.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TWO_PI (2.0 * M_PI)
#define MAX_STAGES 4
#define MAX_NOTES 32

enum { W_SAW, W_SQUARE, W_PULSE, W_TRI, W_SINE, W_NOISE, W_IMPULSE, W_FM, W_ORGAN, W_SUPER, W_FILE };
static const char *wave_names[] = { "saw", "square", "pulse", "tri", "sine", "noise",
                                    "impulse", "fm", "organ", "super", NULL };
enum { SP_LOG, SP_LIN, SP_MEL, SP_BARK, SP_RAND };
static const char *spacing_names[] = { "log", "lin", "mel", "bark", "rand", NULL };

typedef struct {
    const char *in, *out, *carrier_file;
    /* filter bank */
    int bands, order, spacing;
    double fmin, fmax, q, qmul, qsmul;
    /* envelope */
    double attack, release, expo, gate_db, tilt, boost_db;
    /* band mapping / spectral weirdness */
    double formant, lfo_rate, lfo_depth, stretch;
    int reverse, rotate, scramble, skip, smear_w;
    double smear, quant;
    /* carrier */
    int wave, nnotes, unison;
    double notes[MAX_NOTES];
    double pitch, detune, pw, fm_ratio, fm_index, vib_rate, vib_depth;
    double glide, noise_mix, speed;
    int track, snap;
    double track_shift;
    /* sibilance */
    double sib, sib_freq;
    /* mix + post */
    double dry, wet, crush, srr, drive, ring, ring_mix;
    double delay_ms, delay_fb, delay_mix, stereo, tail;
    int bits, verbose, sr, randomized, seed_set, mutate;
    unsigned seed, rand_seed;
    double carrier_secs, mutate_intensity;
} Cfg;

static void defaults(Cfg *c) {
    memset(c, 0, sizeof *c);
    c->out = "out.wav";
    c->bands = 24; c->order = 2; c->spacing = SP_LOG;
    c->fmin = 80; c->fmax = 8000; c->qmul = 1.0;
    c->attack = 5; c->release = 30; c->expo = 1.0; c->gate_db = -100;
    c->stretch = 1.0; c->skip = 1; c->smear_w = 1;
    c->wave = W_SAW; c->nnotes = 1; c->notes[0] = 0; c->unison = 1;
    c->pitch = 110; c->detune = 10; c->pw = 0.3;
    c->fm_ratio = 2.0; c->fm_index = 3.0; c->speed = 1.0;
    c->sib_freq = 4500;
    c->dry = 0; c->wet = 1;
    c->bits = 16; c->seed = 1; c->sr = 44100; c->mutate_intensity = 1.0;
}

/* ------------------------------------------------------------------ */
/* random                                                              */
/* ------------------------------------------------------------------ */
static uint64_t rng_s = 88172645463325252ULL;
static inline double rnd(void) { /* [-1,1) */
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return (double)(rng_s >> 11) * (2.0 / 9007199254740992.0) - 1.0;
}
static inline double rnd01(void) { return (rnd() + 1.0) * 0.5; }

/* ------------------------------------------------------------------ */
/* WAV I/O                                                             */
/* ------------------------------------------------------------------ */
typedef struct { double *d; long n; int sr; } Audio;

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 20, n = 0;
    unsigned char *b = malloc(cap);
    for (;;) {
        if (n == cap) { cap *= 2; b = realloc(b, cap); }
        size_t r = fread(b + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    if (f != stdin) fclose(f);
    *len = n;
    return b;
}
static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const unsigned char *p) { return p[0] | p[1] << 8; }

static int read_wav(const char *path, Audio *a) {
    size_t len;
    unsigned char *b = slurp(path, &len);
    if (!b) { perror(path); return -1; }
    if (len < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a RIFF/WAVE file\n", path); free(b); return -1;
    }
    size_t p = 12, dlen = 0;
    int fmt = 0, ch = 0, bits = 0;
    uint32_t sr = 0;
    const unsigned char *data = NULL;
    while (p + 8 <= len) {
        uint32_t sz = rd32(b + p + 4);
        const unsigned char *c = b + p + 8;
        size_t rem = len - (p + 8);
        if (!memcmp(b + p, "fmt ", 4) && rem >= 16) {
            fmt = rd16(c); ch = rd16(c + 2); sr = rd32(c + 4); bits = rd16(c + 14);
            if (fmt == 0xFFFE && sz >= 26 && rem >= 26) fmt = rd16(c + 24);
        } else if (!memcmp(b + p, "data", 4)) {
            data = c; dlen = (sz == 0 || sz > rem) ? rem : sz; break;
        }
        p += 8 + (size_t)sz + (sz & 1);
    }
    if (!data || ch < 1 || bits < 8 || (fmt != 1 && fmt != 3)) {
        fprintf(stderr, "%s: unsupported WAV (fmt=%d bits=%d ch=%d)\n", path, fmt, bits, ch);
        free(b); return -1;
    }
    int bps = bits / 8;
    long frames = (long)(dlen / ((size_t)bps * ch));
    a->d = malloc(sizeof(double) * (frames + 1));
    a->n = frames; a->sr = (int)sr;
    for (long f = 0; f < frames; f++) {
        double sum = 0;
        for (int c = 0; c < ch; c++) {
            const unsigned char *s = data + ((size_t)f * ch + c) * bps;
            double v = 0;
            if (fmt == 1) {
                if (bits == 8) v = (s[0] - 128) / 128.0;
                else if (bits == 16) v = (int16_t)rd16(s) / 32768.0;
                else if (bits == 24) v = (int32_t)((s[0] << 8 | s[1] << 16 | (uint32_t)s[2] << 24)) / 2147483648.0;
                else if (bits == 32) v = (int32_t)rd32(s) / 2147483648.0;
            } else {
                if (bits == 32) { float x; memcpy(&x, s, 4); v = x; }
                else if (bits == 64) { double x; memcpy(&x, s, 8); v = x; }
            }
            sum += v;
        }
        a->d[f] = sum / ch;
    }
    free(b);
    return 0;
}

static void put32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }

static int write_wav(const char *path, double **ch, int nch, long n, int sr, int bits) {
    int isf = (bits == 32);
    int bps = bits / 8;
    size_t dbytes = (size_t)n * nch * bps;
    size_t hdr = isf ? 46 : 44;
    unsigned char *b = malloc(hdr + dbytes);
    memcpy(b, "RIFF", 4); put32(b + 4, (uint32_t)(hdr - 8 + dbytes)); memcpy(b + 8, "WAVE", 4);
    memcpy(b + 12, "fmt ", 4); put32(b + 16, isf ? 18 : 16);
    put16(b + 20, isf ? 3 : 1); put16(b + 22, nch); put32(b + 24, sr);
    put32(b + 28, sr * nch * bps); put16(b + 32, nch * bps); put16(b + 34, bits);
    size_t o = 36;
    if (isf) { put16(b + 36, 0); o = 38; }
    memcpy(b + o, "data", 4); put32(b + o + 4, (uint32_t)dbytes);
    unsigned char *w = b + o + 8;
    for (long i = 0; i < n; i++)
        for (int c = 0; c < nch; c++) {
            double v = ch[c][i];
            if (isf) { float f = (float)v; memcpy(w, &f, 4); w += 4; continue; }
            if (v > 1) v = 1;
            if (v < -1) v = -1;
            if (bits == 16) { int32_t s = (int32_t)lrint(v * 32767.0); put16(w, (uint32_t)s); w += 2; }
            else { int32_t s = (int32_t)lrint(v * 8388607.0); w[0] = s; w[1] = s >> 8; w[2] = s >> 16; w += 3; }
        }
    FILE *f = strcmp(path, "-") == 0 ? stdout : fopen(path, "wb");
    if (!f) { perror(path); free(b); return -1; }
    fwrite(b, 1, hdr + dbytes, f);
    if (f != stdout) fclose(f); else fflush(f);
    free(b);
    return 0;
}

/* ------------------------------------------------------------------ */
/* filters                                                             */
/* ------------------------------------------------------------------ */
typedef struct { double b0, a1, a2; } BP;             /* RBJ band-pass, 0 dB peak */
typedef struct { double b0, b1, b2, a1, a2, z1, z2; } Bq;

static void make_bp(BP *c, double f, double Q, double sr) {
    if (f < 10) f = 10;
    if (f > 0.49 * sr) f = 0.49 * sr;
    if (Q < 0.3) Q = 0.3;
    double w = TWO_PI * f / sr, al = sin(w) / (2 * Q), a0 = 1 + al;
    c->b0 = al / a0; c->a1 = -2 * cos(w) / a0; c->a2 = (1 - al) / a0;
}
static inline double bp_run(const BP *c, double *z1, double *z2, double x) {
    double y = c->b0 * x + *z1;
    *z1 = *z2 - c->a1 * y;
    *z2 = -c->b0 * x - c->a2 * y;
    return y;
}
static void make_hp(Bq *q, double f, double sr) {
    double w = TWO_PI * f / sr, cs = cos(w), al = sin(w) / (2 * 0.7071), a0 = 1 + al;
    q->b0 = (1 + cs) / 2 / a0; q->b1 = -(1 + cs) / a0; q->b2 = q->b0;
    q->a1 = -2 * cs / a0; q->a2 = (1 - al) / a0; q->z1 = q->z2 = 0;
}
static inline double bq_run(Bq *q, double x) {
    double y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

/* ------------------------------------------------------------------ */
/* band layout                                                         */
/* ------------------------------------------------------------------ */
static double warp(int sp, double f) {
    switch (sp) {
    case SP_LIN: return f;
    case SP_MEL: return 2595.0 * log10(1 + f / 700.0);
    case SP_BARK: return 6.0 * asinh(f / 600.0);
    default: return log(f);
    }
}
static double unwarp(int sp, double u) {
    switch (sp) {
    case SP_LIN: return u;
    case SP_MEL: return 700.0 * (pow(10, u / 2595.0) - 1);
    case SP_BARK: return 600.0 * sinh(u / 6.0);
    default: return exp(u);
    }
}
static int cmpd(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static void band_freqs(const Cfg *c, double *f) {
    int n = c->bands;
    double lo = warp(c->spacing, c->fmin), hi = warp(c->spacing, c->fmax);
    for (int b = 0; b < n; b++) {
        double u;
        if (c->spacing == SP_RAND) u = lo + (hi - lo) * rnd01();
        else u = n == 1 ? (lo + hi) / 2 : lo + (hi - lo) * b / (n - 1);
        f[b] = unwarp(c->spacing == SP_RAND ? SP_LOG : c->spacing, u);
    }
    if (c->spacing == SP_RAND) qsort(f, n, sizeof(double), cmpd);
}

/* ------------------------------------------------------------------ */
/* pitch tracker (normalized autocorrelation, offline)                 */
/* ------------------------------------------------------------------ */
static double *track_pitch(const double *x, long n, int sr, long hop, long *nh_out) {
    int D = sr / 8000; if (D < 1) D = 1;
    double srd = (double)sr / D;
    long nd = n / D;
    double *xd = calloc(nd + 1, sizeof(double));
    double peak = 1e-12;
    for (long i = 0; i < nd; i++) {
        double s = 0;
        for (int k = 0; k < D; k++) s += x[i * D + k];
        xd[i] = s / D;
        if (fabs(xd[i]) > peak) peak = fabs(xd[i]);
    }
    int lmin = (int)(srd / 600); if (lmin < 2) lmin = 2;
    int lmax = (int)(srd / 50);
    int W = lmax * 2, F = W + lmax + 3;
    long nh = n / hop + 1;
    double *pf = calloc(nh, sizeof(double));
    double *fr = malloc(F * sizeof(double)), *ns = calloc(lmax + 3, sizeof(double));
    for (long h = 0; h < nh; h++) {
        long s0 = (h * hop) / D - W / 2;
        for (int j = 0; j < F; j++) { long k = s0 + j; fr[j] = (k >= 0 && k < nd) ? xd[k] : 0; }
        double e0 = 0;
        for (int i = 0; i < W; i++) e0 += fr[i] * fr[i];
        if (sqrt(e0 / W) < 0.02 * peak) continue;
        double best = 0;
        for (int l = lmin - 1; l <= lmax + 1; l++) {
            double r = 0, m = 0;
            for (int i = 0; i < W; i++) { r += fr[i] * fr[i + l]; m += fr[i] * fr[i] + fr[i + l] * fr[i + l]; }
            ns[l] = m > 0 ? 2 * r / m : 0;
            if (l >= lmin && l <= lmax && ns[l] > best) best = ns[l];
        }
        if (best < 0.55) continue;
        for (int l = lmin; l <= lmax; l++) {
            if (ns[l] > ns[l - 1] && ns[l] >= ns[l + 1] && ns[l] >= 0.9 * best) {
                double a = ns[l - 1], bb = ns[l], cc = ns[l + 1], d = a - 2 * bb + cc;
                double lag = l + (d != 0 ? 0.5 * (a - cc) / d : 0);
                pf[h] = srd / lag;
                break;
            }
        }
    }
    /* fill unvoiced gaps by holding, back-fill the start */
    long first = -1;
    for (long h = 0; h < nh; h++) if (pf[h] > 0) { first = h; break; }
    if (first >= 0) {
        for (long h = 0; h < first; h++) pf[h] = pf[first];
        for (long h = first + 1; h < nh; h++) if (pf[h] <= 0) pf[h] = pf[h - 1];
        /* 5-point median to kill octave blips */
        double *t = malloc(nh * sizeof(double));
        for (long h = 0; h < nh; h++) {
            double w5[5]; int k = 0;
            for (long j = h - 2; j <= h + 2; j++) w5[k++] = pf[j < 0 ? 0 : (j >= nh ? nh - 1 : j)];
            qsort(w5, 5, sizeof(double), cmpd);
            t[h] = w5[2];
        }
        memcpy(pf, t, nh * sizeof(double)); free(t);
    }
    free(xd); free(fr); free(ns);
    *nh_out = nh;
    return first >= 0 ? pf : (free(pf), NULL);
}

/* ------------------------------------------------------------------ */
/* oscillators                                                         */
/* ------------------------------------------------------------------ */
static inline double polyblep(double t, double dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}
static inline double pulse_blep(double t, double dt, double pw) {
    double v = t < pw ? 1.0 : -1.0;
    double t2 = t + 1.0 - pw; t2 -= floor(t2);
    return v + polyblep(t, dt) - polyblep(t2, dt);
}
static inline double voice_sample(const Cfg *c, int wave, double t, double t2, double dt) {
    switch (wave) {
    case W_SAW: return 2 * t - 1 - polyblep(t, dt);
    case W_SQUARE: return pulse_blep(t, dt, 0.5);
    case W_PULSE: return pulse_blep(t, dt, c->pw);
    case W_TRI: return 4 * fabs(t - 0.5) - 1;
    case W_SINE: return sin(TWO_PI * t);
    case W_NOISE: return rnd();
    case W_IMPULSE: { double d = dt < 1e-5 ? 1e-5 : dt; return t < d ? (1 - t / d) / sqrt(d) * 0.577 : 0; }
    case W_FM: return sin(TWO_PI * t + c->fm_index * sin(TWO_PI * t2));
    case W_ORGAN: {
        double p = TWO_PI * t;
        return 0.6 * (sin(p) + 0.6 * sin(2 * p) + 0.4 * sin(3 * p) + 0.3 * sin(4 * p) + 0.2 * sin(6 * p) + 0.15 * sin(8 * p));
    }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* presets                                                             */
/* ------------------------------------------------------------------ */
static void set_notes(Cfg *c, int n, const double *v) {
    c->nnotes = n;
    for (int i = 0; i < n; i++) c->notes[i] = v[i];
}
static int apply_preset(Cfg *c, const char *n) {
    if (!strcmp(n, "robot")) { c->wave = W_SAW; c->pitch = 110; c->bands = 20; c->attack = 3; c->release = 25; }
    else if (!strcmp(n, "whisper")) { c->wave = W_NOISE; c->bands = 32; c->qmul = 1.5; c->attack = 4; c->release = 20; }
    else if (!strcmp(n, "dalek")) { c->wave = W_SAW; c->pitch = 80; c->bands = 14; c->fmin = 200; c->fmax = 5000;
        c->ring = 30; c->ring_mix = 1; c->drive = 6; c->attack = 2; c->release = 15; }
    else if (!strcmp(n, "choir")) { double nt[] = { 0, 4, 7, 12 }; set_notes(c, 4, nt);
        c->wave = W_SUPER; c->pitch = 130; c->unison = 5; c->detune = 14; c->vib_rate = 5.5; c->vib_depth = 12;
        c->attack = 15; c->release = 90; c->bands = 28; c->delay_ms = 260; c->delay_fb = 0.35; c->delay_mix = 0.25; c->stereo = 0.6; }
    else if (!strcmp(n, "alien")) { c->wave = W_FM; c->pitch = 140; c->fm_ratio = 1.414; c->fm_index = 5;
        c->formant = 7; c->stretch = 1.25; c->bands = 32; c->vib_rate = 7; c->vib_depth = 35;
        c->lfo_rate = 0.4; c->lfo_depth = 3; c->stereo = 0.5; }
    else if (!strcmp(n, "underwater")) { c->wave = W_SUPER; c->pitch = 100; c->bands = 16; c->qmul = 0.5; c->formant = -4;
        c->release = 120; c->smear = 0.6; c->smear_w = 2; c->lfo_rate = 0.35; c->lfo_depth = 2.5;
        c->delay_ms = 90; c->delay_fb = 0.4; c->delay_mix = 0.3; }
    else if (!strcmp(n, "chipmunk")) { c->wave = W_SAW; c->pitch = 300; c->formant = 12; c->bands = 24; c->fmin = 200; c->fmax = 12000; }
    else if (!strcmp(n, "monster")) { c->wave = W_SQUARE; c->pitch = 45; c->formant = -8; c->drive = 14; c->bands = 24; c->release = 60; }
    else if (!strcmp(n, "glitch")) { c->wave = W_PULSE; c->pitch = 200; c->pw = 0.2; c->quant = 5; c->srr = 5; c->crush = 7;
        c->bands = 40; c->scramble = 1; c->attack = 1; c->release = 8; }
    else if (!strcmp(n, "ghost")) { double nt[] = { 0, 7, 12, 19 }; set_notes(c, 4, nt);
        c->wave = W_SINE; c->unison = 2; c->noise_mix = 0.25; c->smear = 0.5; c->release = 220;
        c->delay_ms = 380; c->delay_fb = 0.6; c->delay_mix = 0.45; c->stereo = 1; c->bands = 32; }
    else if (!strcmp(n, "insect")) { c->wave = W_IMPULSE; c->pitch = 220; c->bands = 48; c->fmin = 800; c->fmax = 14000;
        c->formant = 12; c->attack = 1; c->release = 6; c->vib_rate = 30; c->vib_depth = 60; }
    else if (!strcmp(n, "autotune")) { c->wave = W_SAW; c->track = 1; c->snap = 1; c->bands = 28; c->attack = 4; c->release = 30;
        c->vib_rate = 5; c->vib_depth = 10; }
    else if (!strcmp(n, "inverted")) { c->wave = W_SAW; c->reverse = 1; c->pitch = 120; c->bands = 24; }
    else return 0;
    return 1;
}
static void list_things(void) {
    puts("presets:   robot whisper dalek choir alien underwater chipmunk monster glitch ghost insect autotune inverted");
    fputs("waveforms:", stdout); for (int i = 0; wave_names[i]; i++) printf(" %s", wave_names[i]);
    fputs("\nspacings: ", stdout); for (int i = 0; spacing_names[i]; i++) printf(" %s", spacing_names[i]);
    puts("");
}

/* ------------------------------------------------------------------ */
/* randomizer                                                          */
/* ------------------------------------------------------------------ */
static uint64_t rz_s;
static double rz(void) { /* splitmix64 -> [0,1) */
    rz_s += 0x9E3779B97F4A7C15ULL;
    uint64_t z = rz_s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}
static double ur(double a, double b) { return a + (b - a) * rz(); }
static double lr(double a, double b) { return exp(ur(log(a), log(b))); }
static int ir(int a, int b) { int v = a + (int)(rz() * (b - a + 1)); return v > b ? b : v; }
static int chance(double p) { return rz() < p; }

static unsigned fresh_seed(void) {
    unsigned s = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(&s, sizeof s, 1, f) != 1) s = 0; fclose(f); }
    if (!s) s = (unsigned)time(NULL) ^ ((unsigned)getpid() << 16);
    s %= 99999999u;
    return s + 1;
}

static void randomize_cfg(Cfg *c, unsigned seed) {
    static const struct { int n; double v[4]; } chords[] = {
        {3, {0, 4, 7}}, {3, {0, 3, 7}}, {3, {0, 7, 12}}, {3, {0, 5, 7}}, {4, {0, 4, 7, 11}},
        {4, {0, 3, 7, 10}}, {2, {0, 7}}, {2, {0, 12}}, {3, {-12, 0, 7}}, {3, {0, 2, 7}},
        {3, {0, 1, 6}}, {3, {0, 4, 8}}, {4, {0, 5, 10, 15}}
    };
    static const double fmr[] = { 0.5, 1, 1.414, 1.5, 2, 2.5, 3, 3.5, 4, 5.19 };
    static const double tsh[] = { -12, -7, -5, 0, 0, 5, 7, 12 };
    rz_s = (uint64_t)seed * 0x9E3779B97F4A7C15ULL + 0xD1B54A32D192ED03ULL;
    for (int i = 0; i < 4; i++) rz();
    c->randomized = 1; c->rand_seed = seed; c->seed = seed;

    /* carrier */
    c->wave = ir(W_SAW, W_SUPER);
    c->pitch = lr(40, 400);
    c->pw = ur(0.08, 0.5);
    c->fm_ratio = fmr[ir(0, 9)]; c->fm_index = lr(0.5, 9);
    if (chance(0.45)) {
        int k = ir(0, (int)(sizeof chords / sizeof chords[0]) - 1);
        c->nnotes = chords[k].n;
        for (int i = 0; i < c->nnotes; i++) c->notes[i] = chords[k].v[i];
    } else { c->nnotes = 1; c->notes[0] = 0; }
    c->unison = chance(0.3) ? ir(3, 7) : 1; c->detune = ur(4, 30);
    if (chance(0.4)) { c->vib_rate = ur(3, 9); c->vib_depth = ur(5, 50); } else c->vib_rate = c->vib_depth = 0;
    c->glide = chance(0.2) ? ur(20, 150) : 0;
    c->noise_mix = chance(0.2) ? ur(0.1, 0.6) : 0;
    c->track = chance(0.2);
    c->track_shift = c->track ? tsh[ir(0, 7)] : 0;
    c->snap = c->track ? chance(0.5) : 0;

    /* filter bank */
    c->bands = ir(8, 64);
    c->spacing = ir(SP_LOG, SP_RAND);
    c->fmin = lr(60, 300); c->fmax = lr(3000, 12000);
    c->q = 0; c->qmul = lr(0.4, 2.5); c->qsmul = 0;
    c->order = ir(1, 3);

    /* envelopes */
    c->attack = lr(1, 20); c->release = lr(5, 150);
    c->expo = chance(0.5) ? ur(0.6, 1.8) : 1.0;
    c->tilt = chance(0.3) ? ur(-3, 4) : 0;
    if (chance(0.3)) { c->smear = ur(0.2, 0.9); c->smear_w = ir(1, 3); } else { c->smear = 0; c->smear_w = 1; }
    c->quant = chance(0.2) ? ur(2, 8) : 0;
    c->sib = chance(0.4) ? ur(0.5, 2.0) : 0;

    /* band mapping */
    c->formant = chance(0.7) ? ur(-12, 12) : 0;
    if (chance(0.3)) { c->lfo_rate = lr(0.1, 6); c->lfo_depth = ur(1, 8); } else c->lfo_rate = c->lfo_depth = 0;
    c->stretch = chance(0.4) ? ur(0.6, 1.6) : 1.0;
    c->reverse = chance(0.15);
    c->rotate = chance(0.2) ? ir(1, c->bands / 2 > 0 ? c->bands / 2 : 1) : 0;
    c->scramble = chance(0.2);
    c->skip = chance(0.1) ? ir(2, 3) : 1;

    /* post */
    c->dry = chance(0.15) ? ur(0.1, 0.4) : 0; c->wet = 1;
    if (chance(0.15)) { c->ring = lr(20, 400); c->ring_mix = ur(0.3, 1); } else c->ring = c->ring_mix = 0;
    c->srr = chance(0.15) ? ur(2, 8) : 0;
    c->crush = chance(0.15) ? ur(4, 10) : 0;
    c->drive = chance(0.3) ? ur(3, 15) : 0;
    if (chance(0.35)) { c->delay_ms = lr(60, 500); c->delay_fb = ur(0.2, 0.6); c->delay_mix = ur(0.15, 0.45); c->tail = 1.5; }
    else { c->delay_ms = c->delay_fb = c->delay_mix = 0; c->tail = 0; }
    c->stereo = chance(0.4) ? ur(0.3, 1.0) : 0;
}

static void print_summary(const Cfg *c) {
    FILE *e = stderr;
    fprintf(e, "xvoc: wave=%s pitch=%.1f bands=%d spacing=%s order=%d qmul=%.2f att/rel=%.0f/%.0fms",
            c->wave == W_FILE ? "file" : wave_names[c->wave], c->pitch, c->bands,
            spacing_names[c->spacing], c->order, c->qmul, c->attack, c->release);
    if (c->wave == W_FM) fprintf(e, " fm=%.2f/%.1f", c->fm_ratio, c->fm_index);
    if (c->wave == W_PULSE) fprintf(e, " pw=%.2f", c->pw);
    if (c->nnotes > 1) { fputs(" notes=", e); for (int k = 0; k < c->nnotes; k++) fprintf(e, "%s%g", k ? "," : "", c->notes[k]); }
    if (c->unison > 1) fprintf(e, " unison=%dx%.0fc", c->unison, c->detune);
    if (c->formant != 0) fprintf(e, " formant=%+.1f", c->formant);
    if (c->lfo_depth != 0) fprintf(e, " formant-lfo=%.2fHz/%.1fst", c->lfo_rate, c->lfo_depth);
    if (c->stretch != 1) fprintf(e, " stretch=%.2f", c->stretch);
    if (c->reverse) fputs(" reverse", e);
    if (c->rotate) fprintf(e, " rotate=%d", c->rotate);
    if (c->scramble) fputs(" scramble", e);
    if (c->skip > 1) fprintf(e, " skip=%d", c->skip);
    if (c->smear > 0) fprintf(e, " smear=%.2f,%d", c->smear, c->smear_w);
    if (c->quant > 0) fprintf(e, " quant=%.1fdB", c->quant);
    if (c->expo != 1) fprintf(e, " exp=%.2f", c->expo);
    if (c->tilt != 0) fprintf(e, " tilt=%+.1f", c->tilt);
    if (c->vib_depth != 0) fprintf(e, " vib=%.1fHz/%.0fc", c->vib_rate, c->vib_depth);
    if (c->glide > 0) fprintf(e, " glide=%.0fms", c->glide);
    if (c->noise_mix > 0) fprintf(e, " noise=%.2f", c->noise_mix);
    if (c->sib > 0) fprintf(e, " sib=%.2f", c->sib);
    if (c->track) fprintf(e, " track%+.0fst%s", c->track_shift, c->snap ? "+snap" : "");
    if (c->ring > 0) fprintf(e, " ring=%.0fHz/%.2f", c->ring, c->ring_mix);
    if (c->srr > 1) fprintf(e, " srr=%.1f", c->srr);
    if (c->crush > 0) fprintf(e, " crush=%.1fbit", c->crush);
    if (c->drive > 0) fprintf(e, " drive=%.0fdB", c->drive);
    if (c->delay_ms > 0) fprintf(e, " delay=%.0f,%.2f,%.2f", c->delay_ms, c->delay_fb, c->delay_mix);
    if (c->stereo > 0) fprintf(e, " stereo=%.2f", c->stereo);
    if (c->dry > 0) fprintf(e, " dry=%.2f", c->dry);
    fputc('\n', e);
}

/* ------------------------------------------------------------------ */
/* mutate                                                              */
/* ------------------------------------------------------------------ */
static double clampd(double v, double a, double b) { return v < a ? a : (v > b ? b : v); }
static int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }
static double jmul(double v, double amt) { return v * exp(ur(-amt, amt)); }  /* multiplicative jitter */
static double jadd(double v, double amt) { return v + ur(-amt, amt); }        /* additive jitter       */

static void mutate_cfg(Cfg *c, double inten, unsigned seed) {
    rz_s = (uint64_t)seed * 0x9E3779B97F4A7C15ULL + 0xA24BAED4963EE407ULL;
    for (int i = 0; i < 4; i++) rz();
    double in = inten < 0 ? 0 : inten;
#define PR(x) clampd((x) * in, 0.0, 0.9)

    if (c->wave != W_FILE && chance(PR(0.22))) c->wave = ir(W_SAW, W_SUPER);
    c->pitch = clampd(jmul(c->pitch, 0.35 * in), 30, 2000);

    c->bands = clampi((int)lrint(c->bands * exp(ur(-0.25 * in, 0.25 * in))), 4, 200);
    if (chance(PR(0.2))) c->spacing = ir(SP_LOG, SP_RAND);
    c->fmin = clampd(jmul(c->fmin, 0.25 * in), 20, 4000);
    c->fmax = clampd(jmul(c->fmax, 0.25 * in), c->fmin * 1.4, 20000);
    if (c->q > 0) { c->q = chance(PR(0.12)) ? 0 : clampd(jmul(c->q, 0.3 * in), 0.3, 30); }
    else if (chance(PR(0.14))) c->q = lr(0.5, 15);
    c->qmul = clampd(jmul(c->qmul, 0.3 * in), 0.2, 4);
    if (c->qsmul > 0) c->qsmul = chance(PR(0.1)) ? 0 : clampd(jmul(c->qsmul, 0.3 * in), 0.2, 4);
    else if (chance(PR(0.1))) c->qsmul = lr(0.2, 4);
    if (chance(PR(0.22))) c->order = ir(1, MAX_STAGES);

    c->attack = clampd(jmul(c->attack, 0.3 * in), 0.3, 60);
    c->release = clampd(jmul(c->release, 0.3 * in), 2, 400);
    c->expo = chance(PR(0.08)) ? 1.0 : clampd(jadd(c->expo, 0.25 * in), 0.3, 2.5);
    if (c->gate_db > -100) c->gate_db = clampd(jadd(c->gate_db, 8 * in), -120, -20);
    else if (chance(PR(0.1))) c->gate_db = ur(-70, -30);
    c->tilt = clampd(jadd(c->tilt, 2 * in), -8, 8);
    c->boost_db = clampd(jadd(c->boost_db, 1.5 * in), -12, 12);

    if (c->smear > 0) { c->smear = chance(PR(0.1)) ? 0 : clampd(jadd(c->smear, 0.2 * in), 0.05, 0.95); c->smear_w = clampi(c->smear_w + (chance(PR(0.15)) ? (chance(0.5) ? 1 : -1) : 0), 1, 4); }
    else if (chance(PR(0.2))) { c->smear = ur(0.2, 0.7); c->smear_w = ir(1, 3); }
    if (c->quant > 0) c->quant = chance(PR(0.1)) ? 0 : clampd(jadd(c->quant, 1.5 * in), 1, 10);
    else if (chance(PR(0.15))) c->quant = ur(1.5, 8);
    if (c->sib > 0) c->sib = chance(PR(0.1)) ? 0 : clampd(jadd(c->sib, 0.4 * in), 0.1, 3);
    else if (chance(PR(0.18))) c->sib = ur(0.4, 2.2);

    c->formant = clampd(jadd(c->formant, 3 * in), -24, 24);
    if (c->lfo_depth != 0) { if (chance(PR(0.1))) c->lfo_rate = c->lfo_depth = 0; else { c->lfo_rate = clampd(jmul(c->lfo_rate, 0.3 * in), 0.05, 8); c->lfo_depth = clampd(jadd(c->lfo_depth, 1.5 * in), 0.5, 12); } }
    else if (chance(PR(0.18))) { c->lfo_rate = lr(0.1, 6); c->lfo_depth = ur(1, 8); }
    c->stretch = clampd(jmul(c->stretch, 0.15 * in), 0.4, 2.2);
    if (chance(PR(0.14))) c->reverse = !c->reverse;
    if (chance(PR(0.14))) c->rotate = ir(0, c->bands / 2 > 0 ? c->bands / 2 : 1);
    if (chance(PR(0.14))) c->scramble = !c->scramble;
    if (chance(PR(0.1))) c->skip = ir(1, 3);

    if (c->wave != W_FILE) {
        if (chance(PR(0.18))) {
            static const struct { int n; double v[4]; } chords[] = {
                {3, {0, 4, 7}}, {3, {0, 3, 7}}, {3, {0, 7, 12}}, {3, {0, 5, 7}}, {4, {0, 4, 7, 11}},
                {4, {0, 3, 7, 10}}, {2, {0, 7}}, {2, {0, 12}}, {3, {-12, 0, 7}}, {3, {0, 2, 7}},
                {3, {0, 1, 6}}, {3, {0, 4, 8}}, {4, {0, 5, 10, 15}}, {1, {0}}
            };
            int k = ir(0, (int)(sizeof chords / sizeof chords[0]) - 1);
            c->nnotes = chords[k].n;
            for (int i = 0; i < c->nnotes; i++) c->notes[i] = chords[k].v[i];
        } else for (int i = 0; i < c->nnotes; i++) c->notes[i] = jadd(c->notes[i], 0.6 * in);
        if (chance(PR(0.18))) c->unison = ir(1, 7);
        c->detune = clampd(jadd(c->detune, 8 * in), 0, 60);
        c->pw = clampd(jadd(c->pw, 0.18 * in), 0.02, 0.9);
        c->fm_ratio = clampd(jmul(c->fm_ratio, 0.3 * in), 0.1, 8);
        c->fm_index = clampd(jmul(c->fm_index, 0.35 * in), 0.1, 15);
        if (c->vib_depth != 0) { if (chance(PR(0.12))) c->vib_rate = c->vib_depth = 0; else { c->vib_rate = clampd(jmul(c->vib_rate, 0.3 * in), 0.5, 15); c->vib_depth = clampd(jadd(c->vib_depth, 8 * in), 1, 80); } }
        else if (chance(PR(0.18))) { c->vib_rate = lr(2, 10); c->vib_depth = ur(5, 40); }
        if (c->glide > 0) c->glide = chance(PR(0.12)) ? 0 : clampd(jmul(c->glide, 0.3 * in), 5, 300);
        else if (chance(PR(0.1))) c->glide = ur(15, 150);
        if (c->noise_mix > 0) c->noise_mix = chance(PR(0.12)) ? 0 : clampd(jadd(c->noise_mix, 0.15 * in), 0.05, 0.8);
        else if (chance(PR(0.12))) c->noise_mix = ur(0.1, 0.5);
        if (chance(PR(0.1))) c->track = !c->track;
        c->track_shift = clampd(jadd(c->track_shift, 2 * in), -24, 24);
        if (chance(PR(0.1))) c->snap = !c->snap;
    }

    c->dry = clampd(jadd(c->dry, 0.12 * in), 0, 0.7);
    c->wet = clampd(jadd(c->wet, 0.05 * in), 0.3, 1.2);
    if (c->ring > 0) { if (chance(PR(0.12))) c->ring = 0; else { c->ring = clampd(jmul(c->ring, 0.3 * in), 10, 800); c->ring_mix = clampd(jadd(c->ring_mix, 0.2 * in), 0.1, 1); } }
    else if (chance(PR(0.15))) { c->ring = lr(20, 400); c->ring_mix = ur(0.3, 1); }
    if (c->srr > 1) c->srr = chance(PR(0.12)) ? 0 : clampd(jmul(c->srr, 0.3 * in), 1.5, 12);
    else if (chance(PR(0.13))) c->srr = ur(2, 8);
    if (c->crush > 0) c->crush = chance(PR(0.12)) ? 0 : clampd(jadd(c->crush, 1.5 * in), 2, 12);
    else if (chance(PR(0.13))) c->crush = ur(4, 10);
    if (c->drive > 0) c->drive = chance(PR(0.12)) ? 0 : clampd(jadd(c->drive, 3 * in), 1, 22);
    else if (chance(PR(0.2))) c->drive = ur(3, 16);
    if (c->delay_ms > 0) {
        if (chance(PR(0.12))) c->delay_ms = c->delay_fb = c->delay_mix = 0;
        else {
            c->delay_ms = clampd(jmul(c->delay_ms, 0.3 * in), 20, 900);
            c->delay_fb = clampd(jadd(c->delay_fb, 0.15 * in), 0, 0.85);
            c->delay_mix = clampd(jadd(c->delay_mix, 0.12 * in), 0.05, 0.6);
        }
    } else if (chance(PR(0.22))) { c->delay_ms = lr(60, 500); c->delay_fb = ur(0.2, 0.6); c->delay_mix = ur(0.15, 0.45); }
    if (c->delay_ms > 0 && c->tail < 1.0) c->tail = 1.5;
    c->stereo = clampd(jadd(c->stereo, 0.25 * in), 0, 1);
#undef PR
}

static void variant_name(const char *out, int idx, char *buf, size_t n) {
    const char *dot = strrchr(out, '.'), *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash)) {
        int base = (int)(dot - out);
        if (base > (int)n - 16) base = (int)n - 16;
        snprintf(buf, n, "%.*s_%d%s", base, out, idx, dot);
    } else snprintf(buf, n, "%s_%d", out, idx);
}

static void print_flags(FILE *f, const Cfg *c) {
    if (c->carrier_secs > 0) fprintf(f, "--carrier %.17g --sr %d", c->carrier_secs, c->sr);
    else fprintf(f, "-i %s", c->in ? c->in : "-");
    if (c->wave == W_FILE) fprintf(f, " -f %s --speed %.17g", c->carrier_file, c->speed);
    else fprintf(f, " --wave %s --pitch %.17g", wave_names[c->wave], c->pitch);
    fprintf(f, " -n %d --spacing %s --fmin %.17g --fmax %.17g --order %d --qmul %.17g",
            c->bands, spacing_names[c->spacing], c->fmin, c->fmax, c->order, c->qmul);
    if (c->q > 0) fprintf(f, " -q %.17g", c->q);
    if (c->qsmul > 0) fprintf(f, " --qsmul %.17g", c->qsmul);
    fprintf(f, " --attack %.17g --release %.17g", c->attack, c->release);
    if (c->expo != 1.0) fprintf(f, " --exp %.17g", c->expo);
    if (c->gate_db > -100) fprintf(f, " --gate %.17g", c->gate_db);
    if (c->tilt != 0) fprintf(f, " --tilt %.17g", c->tilt);
    if (c->boost_db != 0) fprintf(f, " --boost %.17g", c->boost_db);
    if (c->smear > 0) fprintf(f, " --smear %.17g,%d", c->smear, c->smear_w);
    if (c->quant > 0) fprintf(f, " --quant %.17g", c->quant);
    if (c->formant != 0) fprintf(f, " --formant %.17g", c->formant);
    if (c->lfo_rate != 0 && c->lfo_depth != 0) fprintf(f, " --formant-lfo %.17g,%.17g", c->lfo_rate, c->lfo_depth);
    if (c->stretch != 1.0) fprintf(f, " --stretch %.17g", c->stretch);
    if (c->reverse) fputs(" --reverse", f);
    if (c->rotate) fprintf(f, " --rotate %d", c->rotate);
    if (c->scramble) fputs(" --scramble", f);
    if (c->skip > 1) fprintf(f, " --skip %d", c->skip);
    if (c->wave != W_FILE) {
        fputs(" --notes ", f);
        for (int i = 0; i < c->nnotes; i++) fprintf(f, "%s%.17g", i ? "," : "", c->notes[i]);
        fprintf(f, " --unison %d --detune %.17g --pw %.17g --fm-ratio %.17g --fm-index %.17g",
                c->unison, c->detune, c->pw, c->fm_ratio, c->fm_index);
        if (c->vib_depth != 0) fprintf(f, " --vib %.17g,%.17g", c->vib_rate, c->vib_depth);
        if (c->glide > 0) fprintf(f, " --glide %.17g", c->glide);
        if (c->noise_mix > 0) fprintf(f, " --noise %.17g", c->noise_mix);
        if (c->track) fprintf(f, " --track --track-shift %.17g", c->track_shift);
        if (c->snap) fputs(" --snap", f);
    }
    if (c->sib > 0) fprintf(f, " --sib %.17g --sib-freq %.17g", c->sib, c->sib_freq);
    if (c->dry > 0) fprintf(f, " --dry %.17g", c->dry);
    if (c->wet != 1.0) fprintf(f, " --wet %.17g", c->wet);
    if (c->ring > 0) fprintf(f, " --ring %.17g,%.17g", c->ring, c->ring_mix);
    if (c->srr > 1) fprintf(f, " --srr %.17g", c->srr);
    if (c->crush > 0) fprintf(f, " --crush %.17g", c->crush);
    if (c->drive > 0) fprintf(f, " --drive %.17g", c->drive);
    if (c->delay_ms > 0) fprintf(f, " --delay %.17g,%.17g,%.17g", c->delay_ms, c->delay_fb, c->delay_mix);
    if (c->stereo > 0) fprintf(f, " --stereo %.17g", c->stereo);
    if (c->tail > 0) fprintf(f, " --tail %.17g", c->tail);
    fprintf(f, " --bits %d --seed %u -o %s\n", c->bits, c->seed, c->out);
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */
static const char *HELP =
"xvoc - customizable channel vocoder\n"
"usage: xvoc -i voice.wav [-o out.wav] [options]     ('-' = stdin/stdout)\n"
"       xvoc --carrier SECONDS [-o out.wav] [options]  (carrier only, no input)\n\n"
"I/O\n"
"  -i, --in FILE         modulator (the voice) WAV\n"
"  -o, --out FILE        output WAV (default out.wav)\n"
"      --carrier SEC     NO INPUT: just generate SEC seconds of the carrier synth\n"
"                        (pitch/wave/notes/vib/stereo/post effects all apply)\n"
"      --sr HZ           sample rate for --carrier mode (default 44100)\n"
"  -f, --carrier-file FILE  use this WAV as the carrier instead of the synth (loops)\n"
"      --speed X         carrier file playback speed (default 1)\n"
"      --randomize [SEED]  randomize all settings; explicit flags still win.\n"
"                        no SEED = pick one; the seed is printed so you can repeat it\n"
"      --mutate          also render 5 variants of your settings, nudged at random:\n"
"                        OUT.wav plus OUT_1.wav .. OUT_5.wav; flags for each are printed\n"
"      --mutate-intensity X  how far each variant strays from your settings (default 1;\n"
"                        try 0.3 for close cousins, 2+ for wild) (needs -o FILE, not stdout)\n"
"      --bits N          output 16, 24 or 32 (=float)  (default 16)\n"
"      --tail SEC        extra seconds after input ends (for delay tails)\n"
"      --preset NAME     start from a preset (see --list); flags override it\n"
"      --list            list presets, waveforms, spacings\n"
"      --seed N          random seed (scramble, rand spacing, phases)\n"
"  -v, --verbose\n\n"
"FILTER BANK\n"
"  -n, --bands N         number of bands (1..256, default 24)\n"
"      --fmin HZ / --fmax HZ   frequency range (80 / 8000)\n"
"      --spacing S       log|lin|mel|bark|rand (default log)\n"
"  -q, --q Q             fixed Q for all bands (default: auto from spacing)\n"
"      --qmul X          scale analysis Q (0.3 = smeary, 3 = ringy) (1)\n"
"      --qsmul X         scale synthesis Q separately (default = qmul)\n"
"      --order N         filter stages per band 1..4 (steepness) (2)\n\n"
"ENVELOPES\n"
"      --attack MS       (5)        --release MS   (30)\n"
"      --exp X           envelope power: >1 expands, <1 squashes (1)\n"
"      --gate DB         bands below this level are muted\n"
"      --tilt DB         synth brightness tilt, dB per octave (0)\n"
"      --boost DB        wet trim before mixing/drive\n"
"      --smear AMT[,W]   blur envelopes over W neighbour bands, AMT 0..1\n"
"      --quant DB        quantize band levels to steps of DB (robotic stair-steps)\n\n"
"BAND MAPPING (the weird stuff)\n"
"      --formant ST      shift synth bands vs analysis bands, semitones\n"
"      --formant-lfo RATE,DEPTH   wobble the formant shift (Hz, semitones)\n"
"      --stretch X       warp synth band spacing around the centre (1 = off)\n"
"      --reverse         low bands of voice drive high bands of carrier\n"
"      --rotate N        rotate mapping by N bands\n"
"      --scramble        random permutation of the band mapping (see --seed)\n"
"      --skip K          use only every Kth band (comb-like holes)\n\n"
"CARRIER SYNTH\n"
"  -w, --wave W          saw|square|pulse|tri|sine|noise|impulse|fm|organ|super\n"
"  -p, --pitch HZ        base pitch (110)\n"
"      --notes LIST      chord in semitones, e.g. 0,4,7,12\n"
"      --unison N        detuned copies per note (super defaults to 7)\n"
"      --detune CENTS    unison spread (10)\n"
"      --pw X            pulse width (0.3)\n"
"      --fm-ratio X --fm-index X   for the fm wave (2, 3)\n"
"      --vib RATE,CENTS  vibrato\n"
"      --glide MS        pitch portamento\n"
"      --noise X         mix noise into carrier 0..1\n"
"      --track           follow the pitch of the voice\n"
"      --track-shift ST  offset tracked pitch (semitones)\n"
"      --snap            quantize carrier pitch to semitones (robot autotune)\n\n"
"CONSONANTS\n"
"      --sib X           inject noise driven by the voice's hiss (s, t, f...)\n"
"      --sib-freq HZ     hiss crossover (4500)\n\n"
"MIX + POST\n"
"      --dry X --wet X   dry/wet levels (0 / 1)\n"
"      --ring HZ[,MIX]   ring modulator on output\n"
"      --srr X           sample-rate reduction factor\n"
"      --crush BITS      bit crusher (fractional ok)\n"
"      --drive DB        tanh saturation\n"
"      --delay MS,FB,MIX echo\n"
"      --stereo W        0..1 spread bands across the stereo field\n\n"
"EXAMPLES\n"
"  espeak --stdout \"exterminate\" | xvoc -i - -o - --preset dalek | aplay\n"
"  xvoc -i v.wav --wave super --notes 0,3,7,10 --formant -3 --smear 0.5 --delay 300,0.5,0.4\n"
"  xvoc -i v.wav --wave fm --scramble --spacing rand --bands 60 --quant 6 --crush 6\n"
"  xvoc -i v.wav -f drums.wav --bands 32 --release 60   (talking drums)\n"
"  xvoc --carrier 10 --wave super --notes 0,3,7,10 --stereo 1 -o pad.wav\n"
"  xvoc -i v.wav --randomize            (prints the seed; rerun with --randomize SEED)\n"
"  xvoc -i v.wav --randomize 4242 --wave saw --stereo 0   (random, but pinned)\n"
"  xvoc -i v.wav --preset dalek --mutate --mutate-intensity 1.5 -o d.wav  (d.wav + d_1..d_5.wav)\n";

enum { O_PRESET = 256, O_LIST, O_FMIN, O_FMAX, O_SPACING, O_QMUL, O_QSMUL, O_ORDER, O_ATTACK, O_RELEASE, O_EXP,
       O_GATE, O_TILT, O_BOOST, O_SMEAR, O_QUANT, O_FORMANT, O_FLFO, O_STRETCH, O_REVERSE, O_ROTATE, O_SCRAMBLE,
       O_SKIP, O_NOTES, O_UNISON, O_DETUNE, O_PW, O_FMR, O_FMI, O_VIB, O_GLIDE, O_NOISE, O_SPEED, O_TRACK,
       O_TSHIFT, O_SNAP, O_SIB, O_SIBF, O_DRY, O_WET, O_RING, O_SRR, O_CRUSH, O_DRIVE, O_DELAY, O_STEREO,
       O_TAIL, O_BITS, O_SEED, O_CARRIER, O_SR, O_RANDOMIZE, O_MUTATE, O_MUTATE_INT };

static const struct option longopts[] = {
    {"in", 1, 0, 'i'}, {"out", 1, 0, 'o'}, {"carrier-file", 1, 0, 'f'},
    {"carrier", 1, 0, O_CARRIER}, {"sr", 1, 0, O_SR}, {"randomize", 2, 0, O_RANDOMIZE},
    {"mutate", 0, 0, O_MUTATE}, {"mutate-intensity", 1, 0, O_MUTATE_INT}, {"bands", 1, 0, 'n'}, {"wave", 1, 0, 'w'},
    {"pitch", 1, 0, 'p'}, {"q", 1, 0, 'q'}, {"verbose", 0, 0, 'v'}, {"help", 0, 0, 'h'},
    {"preset", 1, 0, O_PRESET}, {"list", 0, 0, O_LIST}, {"fmin", 1, 0, O_FMIN}, {"fmax", 1, 0, O_FMAX},
    {"spacing", 1, 0, O_SPACING}, {"qmul", 1, 0, O_QMUL}, {"qsmul", 1, 0, O_QSMUL}, {"order", 1, 0, O_ORDER},
    {"attack", 1, 0, O_ATTACK}, {"release", 1, 0, O_RELEASE}, {"exp", 1, 0, O_EXP}, {"gate", 1, 0, O_GATE},
    {"tilt", 1, 0, O_TILT}, {"boost", 1, 0, O_BOOST}, {"smear", 1, 0, O_SMEAR}, {"quant", 1, 0, O_QUANT},
    {"formant", 1, 0, O_FORMANT}, {"formant-lfo", 1, 0, O_FLFO}, {"stretch", 1, 0, O_STRETCH},
    {"reverse", 0, 0, O_REVERSE}, {"rotate", 1, 0, O_ROTATE}, {"scramble", 0, 0, O_SCRAMBLE},
    {"skip", 1, 0, O_SKIP}, {"notes", 1, 0, O_NOTES}, {"unison", 1, 0, O_UNISON}, {"detune", 1, 0, O_DETUNE},
    {"pw", 1, 0, O_PW}, {"fm-ratio", 1, 0, O_FMR}, {"fm-index", 1, 0, O_FMI}, {"vib", 1, 0, O_VIB},
    {"glide", 1, 0, O_GLIDE}, {"noise", 1, 0, O_NOISE}, {"speed", 1, 0, O_SPEED}, {"track", 0, 0, O_TRACK},
    {"track-shift", 1, 0, O_TSHIFT}, {"snap", 0, 0, O_SNAP}, {"sib", 1, 0, O_SIB}, {"sib-freq", 1, 0, O_SIBF},
    {"dry", 1, 0, O_DRY}, {"wet", 1, 0, O_WET}, {"ring", 1, 0, O_RING}, {"srr", 1, 0, O_SRR},
    {"crush", 1, 0, O_CRUSH}, {"drive", 1, 0, O_DRIVE}, {"delay", 1, 0, O_DELAY}, {"stereo", 1, 0, O_STEREO},
    {"tail", 1, 0, O_TAIL}, {"bits", 1, 0, O_BITS}, {"seed", 1, 0, O_SEED}, {0, 0, 0, 0}
};

static int lookup(const char **names, const char *s) {
    for (int i = 0; names[i]; i++) if (!strcmp(names[i], s)) return i;
    return -1;
}
static void die(const char *fmt, const char *arg) { fprintf(stderr, fmt, arg); fputc('\n', stderr); exit(1); }

static int is_uint(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

static void parse_args(Cfg *c, int argc, char **argv) {
    /* presets first so that explicit flags win */
    for (int i = 1; i < argc; i++) {
        const char *nm = NULL;
        if (!strcmp(argv[i], "--preset") && i + 1 < argc) nm = argv[i + 1];
        else if (!strncmp(argv[i], "--preset=", 9)) nm = argv[i] + 9;
        if (nm && !apply_preset(c, nm)) die("unknown preset '%s' (try --list)", nm);
    }
    /* --randomize [SEED] (before real parsing so explicit flags override it) */
    for (int i = 1; i < argc; i++) {
        const char *val = NULL; int hit = 0;
        if (!strcmp(argv[i], "--randomize")) {
            hit = 1;
            if (i + 1 < argc && is_uint(argv[i + 1])) val = argv[i + 1];
        } else if (!strncmp(argv[i], "--randomize=", 12)) { hit = 1; if (is_uint(argv[i] + 12)) val = argv[i] + 12; }
        if (hit) { randomize_cfg(c, val ? (unsigned)strtoul(val, NULL, 10) : fresh_seed()); break; }
    }
    int o;
    while ((o = getopt_long(argc, argv, "i:o:f:n:w:p:q:vh", longopts, NULL)) != -1) {
        switch (o) {
        case 'i': c->in = optarg; break;
        case 'o': c->out = optarg; break;
        case 'f': c->carrier_file = optarg; c->wave = W_FILE; break;
        case O_CARRIER: c->carrier_secs = atof(optarg); break;
        case O_SR: c->sr = atoi(optarg); break;
        case O_RANDOMIZE: break;
        case 'n': c->bands = atoi(optarg); break;
        case 'w': c->wave = lookup(wave_names, optarg); if (c->wave < 0) die("unknown waveform '%s'", optarg); break;
        case 'p': c->pitch = atof(optarg); break;
        case 'q': c->q = atof(optarg); break;
        case 'v': c->verbose = 1; break;
        case 'h': fputs(HELP, stdout); exit(0);
        case O_PRESET: break;
        case O_LIST: list_things(); exit(0);
        case O_FMIN: c->fmin = atof(optarg); break;
        case O_FMAX: c->fmax = atof(optarg); break;
        case O_SPACING: c->spacing = lookup(spacing_names, optarg); if (c->spacing < 0) die("unknown spacing '%s'", optarg); break;
        case O_QMUL: c->qmul = atof(optarg); break;
        case O_QSMUL: c->qsmul = atof(optarg); break;
        case O_ORDER: c->order = atoi(optarg); break;
        case O_ATTACK: c->attack = atof(optarg); break;
        case O_RELEASE: c->release = atof(optarg); break;
        case O_EXP: c->expo = atof(optarg); break;
        case O_GATE: c->gate_db = atof(optarg); break;
        case O_TILT: c->tilt = atof(optarg); break;
        case O_BOOST: c->boost_db = atof(optarg); break;
        case O_SMEAR: sscanf(optarg, "%lf,%d", &c->smear, &c->smear_w); break;
        case O_QUANT: c->quant = atof(optarg); break;
        case O_FORMANT: c->formant = atof(optarg); break;
        case O_FLFO: sscanf(optarg, "%lf,%lf", &c->lfo_rate, &c->lfo_depth); break;
        case O_STRETCH: c->stretch = atof(optarg); break;
        case O_REVERSE: c->reverse = 1; break;
        case O_ROTATE: c->rotate = atoi(optarg); break;
        case O_SCRAMBLE: c->scramble = 1; break;
        case O_SKIP: c->skip = atoi(optarg); break;
        case O_NOTES: {
            char *s = strdup(optarg), *tok = strtok(s, ", ");
            c->nnotes = 0;
            while (tok && c->nnotes < MAX_NOTES) { c->notes[c->nnotes++] = atof(tok); tok = strtok(NULL, ", "); }
            if (!c->nnotes) { c->nnotes = 1; c->notes[0] = 0; }
            free(s); break;
        }
        case O_UNISON: c->unison = atoi(optarg); break;
        case O_DETUNE: c->detune = atof(optarg); break;
        case O_PW: c->pw = atof(optarg); break;
        case O_FMR: c->fm_ratio = atof(optarg); break;
        case O_FMI: c->fm_index = atof(optarg); break;
        case O_VIB: sscanf(optarg, "%lf,%lf", &c->vib_rate, &c->vib_depth); break;
        case O_GLIDE: c->glide = atof(optarg); break;
        case O_NOISE: c->noise_mix = atof(optarg); break;
        case O_SPEED: c->speed = atof(optarg); break;
        case O_TRACK: c->track = 1; break;
        case O_TSHIFT: c->track_shift = atof(optarg); c->track = 1; break;
        case O_SNAP: c->snap = 1; break;
        case O_SIB: c->sib = atof(optarg); break;
        case O_SIBF: c->sib_freq = atof(optarg); break;
        case O_DRY: c->dry = atof(optarg); break;
        case O_WET: c->wet = atof(optarg); break;
        case O_RING: c->ring_mix = 1; sscanf(optarg, "%lf,%lf", &c->ring, &c->ring_mix); break;
        case O_SRR: c->srr = atof(optarg); break;
        case O_CRUSH: c->crush = atof(optarg); break;
        case O_DRIVE: c->drive = atof(optarg); break;
        case O_DELAY: c->delay_fb = 0.4; c->delay_mix = 0.3;
            sscanf(optarg, "%lf,%lf,%lf", &c->delay_ms, &c->delay_fb, &c->delay_mix); break;
        case O_STEREO: c->stereo = atof(optarg); break;
        case O_TAIL: c->tail = atof(optarg); break;
        case O_BITS: c->bits = atoi(optarg); break;
        case O_SEED: c->seed = (unsigned)atoi(optarg); c->seed_set = 1; break;
        case O_MUTATE: c->mutate = 1; break;
        case O_MUTATE_INT: c->mutate_intensity = atof(optarg); break;
        default: fputs("try --help\n", stderr); exit(1);
        }
    }
    if (c->bands < 1) c->bands = 1;
    if (c->bands > 256) c->bands = 256;
    if (c->order < 1) c->order = 1;
    if (c->order > MAX_STAGES) c->order = MAX_STAGES;
    if (c->skip < 1) c->skip = 1;
    if (c->unison < 1) c->unison = 1;
    if (c->stretch <= 0.05) c->stretch = 0.05;
    if (c->bits != 16 && c->bits != 24 && c->bits != 32) c->bits = 16;
    if (c->sr < 8000) c->sr = 8000;
    if (c->sr > 384000) c->sr = 384000;
    if (c->carrier_secs > 3600) c->carrier_secs = 3600;
    if (c->mutate_intensity < 0) c->mutate_intensity = 0;
    if (c->mutate_intensity > 5) c->mutate_intensity = 5;
    if (c->fmin < 20) c->fmin = 20;
    if (c->fmax <= c->fmin) c->fmax = c->fmin * 2;
}

static int run_vocoder(Cfg c, Audio *mod, Audio *car) {
    int carrier_only = c.carrier_secs > 0;

    rng_s = 88172645463325252ULL;
    rng_s ^= (uint64_t)c.seed * 0x9E3779B97F4A7C15ULL; if (!rng_s) rng_s = 1;
    for (int i = 0; i < 8; i++) rnd();
    double sr = mod->sr;
    if (c.fmax > 0.45 * sr) c.fmax = 0.45 * sr;
    if (c.fmin >= c.fmax) c.fmin = c.fmax / 4;

    long N = mod->n, Nout = (carrier_only ? (long)(c.carrier_secs * sr) : N) + (long)(c.tail * sr);
    if (Nout < 1) Nout = 1;
    int n = c.bands, nch = c.stereo > 0 ? 2 : 1;
    if (c.verbose) fprintf(stderr, "xvoc: %ld samples @ %d Hz, %d bands, wave=%s\n", Nout, mod->sr,
                           n, c.wave == W_FILE ? "file" : wave_names[c.wave]);

    /* --- band layout, Q, mapping ------------------------------------ */
    double *fr = malloc(n * sizeof(double)), *autoq = malloc(n * sizeof(double));
    band_freqs(&c, fr);
    for (int b = 0; b < n; b++) {
        double sp;
        if (n == 1) sp = fr[0] * 0.5;
        else if (b == 0) sp = fr[1] - fr[0];
        else if (b == n - 1) sp = fr[n - 1] - fr[n - 2];
        else sp = 0.5 * (fr[b + 1] - fr[b - 1]);
        double q = sp > 1e-6 ? fr[b] / sp : 200;
        autoq[b] = q < 0.7 ? 0.7 : (q > 200 ? 200 : q);
    }
    double comp = sqrt(pow(2.0, 1.0 / c.order) - 1.0);   /* keep BW constant across stages */
    double qsm = c.qsmul > 0 ? c.qsmul : c.qmul;
    int *map = malloc(n * sizeof(int)), *active = malloc(n * sizeof(int));
    for (int s = 0; s < n; s++) map[s] = ((s + c.rotate) % n + n) % n;
    if (c.reverse) for (int s = 0; s < n; s++) map[s] = n - 1 - map[s];
    if (c.scramble) {
        int *perm = malloc(n * sizeof(int));
        for (int i = 0; i < n; i++) perm[i] = i;
        for (int i = n - 1; i > 0; i--) { int j = (int)(rnd01() * (i + 1)); if (j > i) j = i; int t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
        for (int s = 0; s < n; s++) map[s] = perm[map[s]];
        free(perm);
    }
    for (int b = 0; b < n; b++) active[b] = (b % c.skip) == 0;

    /* --- filter state ------------------------------------------------ */
    BP *ac = malloc(n * sizeof(BP)), *sc = malloc(n * sizeof(BP));
    double *az1 = calloc(n * MAX_STAGES, sizeof(double)), *az2 = calloc(n * MAX_STAGES, sizeof(double));
    double *sz1 = calloc(n * MAX_STAGES, sizeof(double)), *sz2 = calloc(n * MAX_STAGES, sizeof(double));
    double *sgain = malloc(n * sizeof(double)), *panL = malloc(n * sizeof(double)), *panR = malloc(n * sizeof(double));
    for (int b = 0; b < n; b++) {
        double qa = (c.q > 0 ? c.q : autoq[b]) * c.qmul;
        make_bp(&ac[b], fr[b], qa * comp, sr);
        double p = 0.5 + (nch == 2 ? c.stereo * ((b & 1) ? 0.5 : -0.5) : 0.0);
        panL[b] = nch == 2 ? cos(p * M_PI / 2) : 1; panR[b] = sin(p * M_PI / 2);
    }
    double pivot = sqrt(c.fmin * c.fmax);
    int dyn = c.lfo_rate != 0 && c.lfo_depth != 0;
#define UPDATE_SYNTH(TT) do { \
        double sh = c.formant + (dyn ? c.lfo_depth * sin(TWO_PI * c.lfo_rate * (TT)) : 0); \
        double mult = pow(2.0, sh / 12.0); \
        for (int s = 0; s < n; s++) { \
            double fs = pivot * pow(fr[s] / pivot, c.stretch) * mult; \
            double qs = (c.q > 0 ? c.q : autoq[s]) * qsm / c.stretch; \
            make_bp(&sc[s], fs, qs * comp, sr); \
            sgain[s] = c.tilt != 0 ? pow(10.0, c.tilt * log2(fs / 1000.0) / 20.0) : 1.0; \
        } } while (0)
    UPDATE_SYNTH(0.0);

    /* --- envelope constants ------------------------------------------ */
    double ea = c.attack > 0 ? exp(-1.0 / (c.attack * 1e-3 * sr)) : 0;
    double er = c.release > 0 ? exp(-1.0 / (c.release * 1e-3 * sr)) : 0;
    double gate = pow(10.0, c.gate_db / 20.0);
    double *env = calloc(n, sizeof(double)), *tmp = calloc(n, sizeof(double));

    /* --- carrier voices ---------------------------------------------- */
    int base_wave = c.wave;
    if (c.wave == W_SUPER) { base_wave = W_SAW; if (c.unison < 2) c.unison = 7; }
    int nv = c.nnotes * c.unison;
    double *ratio = malloc(nv * sizeof(double)), *ph = malloc(nv * sizeof(double)), *ph2 = malloc(nv * sizeof(double));
    for (int k = 0, v = 0; k < c.nnotes; k++)
        for (int u = 0; u < c.unison; u++, v++) {
            double cents = c.notes[k] * 100.0 + (c.unison > 1 ? ((double)u / (c.unison - 1) - 0.5) * 2 * c.detune : 0);
            ratio[v] = pow(2.0, cents / 1200.0);
            ph[v] = rnd01(); ph2[v] = rnd01();
        }
    double vnorm = 1.0 / sqrt((double)nv);
    int cstereo = carrier_only && nch == 2 && c.wave != W_FILE;
    double *vpL = malloc(nv * sizeof(double)), *vpR = malloc(nv * sizeof(double));
    for (int k = 0, v = 0; k < c.nnotes; k++)
        for (int u = 0; u < c.unison; u++, v++) {
            double pos = c.unison > 1 ? (double)u / (c.unison - 1) : (c.nnotes > 1 ? (double)k / (c.nnotes - 1) : 0.5);
            double p = 0.5 + c.stereo * (pos - 0.5);
            vpL[v] = cos(p * M_PI / 2); vpR[v] = sin(p * M_PI / 2);
        }

    /* --- pitch tracking ---------------------------------------------- */
    double *pf = NULL; long nh = 0, H = (long)(sr * 0.004); if (H < 1) H = 1;
    if (c.track && c.wave != W_FILE && !carrier_only) {
        pf = track_pitch(mod->d, N, mod->sr, H, &nh);
        if (!pf) fputs("xvoc: no pitch found in input, using --pitch\n", stderr);
        else if (c.verbose) fprintf(stderr, "xvoc: pitch tracked (%ld frames)\n", nh);
    }
    double glide = c.glide;
    if (pf && glide < 1) glide = 15;
    double gk = glide > 0 ? 1.0 - exp(-1.0 / (glide * 1e-3 * sr)) : 1.0;
    double lf = log2(c.pitch);
    double tsh = pow(2.0, c.track_shift / 12.0);

    /* --- sibilance ---------------------------------------------------- */
    Bq hpm[2], hpn[2];
    double sib_env = 0;
    if (c.sib > 0) for (int k = 0; k < 2; k++) { make_hp(&hpm[k], c.sib_freq, sr); make_hp(&hpn[k], c.sib_freq, sr); }

    /* --- output buffers ---------------------------------------------- */
    double *wet[2] = { calloc(Nout, sizeof(double)), nch == 2 ? calloc(Nout, sizeof(double)) : NULL };
    double cpos = 0, cstep = c.speed * (c.wave == W_FILE ? (double)car->sr / sr : 1.0);
    double dn = 1e-15, boost = pow(10.0, c.boost_db / 20.0);

    /* ================= main sample loop ================================ */
    for (long i = 0; i < Nout; i++) {
        double t = i / sr;
        if (dyn && !carrier_only && (i & 31) == 0) UPDATE_SYNTH(t);
        double m = i < N ? mod->d[i] : 0.0;

        /* carrier */
        double cs, csL = 0, csR = 0;
        if (c.wave == W_FILE) {
            long i0 = (long)cpos; double fr_ = cpos - i0;
            long i1 = i0 + 1; if (i1 >= car->n) i1 = 0;
            cs = car->d[i0] * (1 - fr_) + car->d[i1] * fr_;
            cpos += cstep; if (cpos >= car->n) cpos -= car->n;
        } else {
            double ft = c.pitch;
            if (pf) {
                long h = i / H; if (h >= nh) h = nh - 1;
                if (pf[h] > 0) ft = pf[h] * tsh;
                if (c.snap) ft = 440.0 * pow(2.0, floor(12.0 * log2(ft / 440.0) + 0.5) / 12.0);
            } else if (c.snap) ft = 440.0 * pow(2.0, floor(12.0 * log2(ft / 440.0) + 0.5) / 12.0);
            lf += (log2(ft) - lf) * gk;
            double f = exp2(lf);
            if (c.vib_depth != 0) f *= pow(2.0, c.vib_depth / 1200.0 * sin(TWO_PI * c.vib_rate * t));
            double acc = 0, accL = 0, accR = 0;
            for (int v = 0; v < nv; v++) {
                double dt = f * ratio[v] / sr; if (dt > 0.45) dt = 0.45;
                double vs = voice_sample(&c, base_wave, ph[v], ph2[v], dt);
                acc += vs; accL += vs * vpL[v]; accR += vs * vpR[v];
                ph[v] += dt; if (ph[v] >= 1) ph[v] -= 1;
                ph2[v] += dt * c.fm_ratio; ph2[v] -= floor(ph2[v]);
            }
            cs = acc * vnorm; csL = accL * vnorm; csR = accR * vnorm;
        }
        if (c.noise_mix > 0) {
            double nz = 0.7 * rnd();
            cs = (1 - c.noise_mix) * cs + c.noise_mix * nz;
            csL = (1 - c.noise_mix) * csL + c.noise_mix * nz; csR = (1 - c.noise_mix) * csR + c.noise_mix * nz;
        }
        if (carrier_only) {   /* skip the vocoder: the carrier IS the output */
            wet[0][i] = cstereo ? csL : cs;
            if (nch == 2) wet[1][i] = cstereo ? csR : cs;
            continue;
        }

        /* analysis */
        dn = -dn;
        double x0 = m + dn;
        for (int b = 0; b < n; b++) {
            if (!active[b]) { env[b] = 0; continue; }
            double a = x0;
            for (int st = 0; st < c.order; st++) a = bp_run(&ac[b], &az1[b * MAX_STAGES + st], &az2[b * MAX_STAGES + st], a);
            double e = fabs(a);
            env[b] = e > env[b] ? ea * env[b] + (1 - ea) * e : er * env[b] + (1 - er) * e;
        }
        /* envelope shaping */
        if (c.smear > 0) {
            for (int b = 0; b < n; b++) {
                double sum = 0; int cnt = 0;
                for (int k = -c.smear_w; k <= c.smear_w; k++) {
                    int j = b + k; if (k == 0 || j < 0 || j >= n) continue;
                    sum += env[j]; cnt++;
                }
                tmp[b] = (1 - c.smear) * env[b] + c.smear * (cnt ? sum / cnt : env[b]);
            }
        } else memcpy(tmp, env, n * sizeof(double));
        for (int b = 0; b < n; b++) {
            double v = tmp[b];
            if (v < gate) v = 0;
            else if (v > 0) {
                if (c.expo != 1.0) v = 0.05 * pow(v / 0.05, c.expo);
                if (c.quant > 0) v = pow(10.0, floor(20.0 * log10(v + 1e-12) / c.quant + 0.5) * c.quant / 20.0);
            }
            tmp[b] = v;
        }
        /* synthesis */
        double yL = 0, yR = 0, cin = cs + dn;
        for (int s = 0; s < n; s++) {
            double ev = tmp[map[s]] * sgain[s];
            if (ev < 1e-9) continue;
            double y = cin;
            for (int st = 0; st < c.order; st++) y = bp_run(&sc[s], &sz1[s * MAX_STAGES + st], &sz2[s * MAX_STAGES + st], y);
            y *= ev;
            yL += y * panL[s]; yR += y * panR[s];
        }
        /* consonant hiss */
        if (c.sib > 0) {
            double hm = bq_run(&hpm[1], bq_run(&hpm[0], m));
            double he = fabs(hm);
            sib_env = he > sib_env ? ea * sib_env + (1 - ea) * he : er * sib_env + (1 - er) * he;
            double hn = bq_run(&hpn[1], bq_run(&hpn[0], rnd()));
            double h = c.sib * sib_env * hn * 2.0;
            yL += h * (nch == 2 ? 0.707 : 1); yR += h * 0.707;
        }
        wet[0][i] = yL;
        if (nch == 2) wet[1][i] = yR;
    }

    /* --- level-match wet to modulator, mix with dry ------------------- */
    double rm = 0, rw = 0;
    for (long i = 0; i < N; i++) rm += mod->d[i] * mod->d[i];
    for (long i = 0; i < Nout; i++) rw += wet[0][i] * wet[0][i];
    rm = sqrt(rm / (N ? N : 1)); rw = sqrt(rw / (Nout ? Nout : 1));
    double wg = rw > 1e-12 ? rm / rw : 0;
    if (rm < 1e-9) wg = 0;
    if (carrier_only) wg = 1;
    for (int ch = 0; ch < nch; ch++)
        for (long i = 0; i < Nout; i++) {
            double d = i < N ? mod->d[i] : 0;
            wet[ch][i] = wet[ch][i] * wg * boost * c.wet + d * c.dry;
        }
    double pk = 0;
    for (int ch = 0; ch < nch; ch++) for (long i = 0; i < Nout; i++) if (fabs(wet[ch][i]) > pk) pk = fabs(wet[ch][i]);
    if (pk > 0) for (int ch = 0; ch < nch; ch++) for (long i = 0; i < Nout; i++) wet[ch][i] *= 0.9 / pk;

    /* --- post effects -------------------------------------------------- */
    long dlen = c.delay_ms > 0 ? (long)(c.delay_ms * 1e-3 * sr) + 1 : 0;
    double g_drive = pow(10.0, c.drive / 20.0);
    for (int ch = 0; ch < nch; ch++) {
        double *y = wet[ch];
        double *dbuf = dlen ? calloc(dlen, sizeof(double)) : NULL;
        long dpos = 0; double hold = 0, cnt = 0;
        double qlev = c.crush > 0 && c.crush < 24 ? pow(2.0, c.crush - 1) : 0;
        for (long i = 0; i < Nout; i++) {
            double v = y[i];
            if (c.ring > 0) v *= (1 - c.ring_mix) + c.ring_mix * sin(TWO_PI * c.ring * i / sr);
            if (c.srr > 1) { if (cnt <= 0) { hold = v; cnt += c.srr; } cnt -= 1; v = hold; }
            if (qlev > 0) v = floor(v * qlev + 0.5) / qlev;
            if (c.drive > 0) v = tanh(g_drive * v);
            if (dlen) {
                double d = dbuf[dpos];
                dbuf[dpos] = v + c.delay_fb * d;
                v += c.delay_mix * d;
                if (++dpos >= dlen) dpos = 0;
            }
            y[i] = v;
        }
        free(dbuf);
    }
    pk = 0;
    for (int ch = 0; ch < nch; ch++) for (long i = 0; i < Nout; i++) {
        if (!isfinite(wet[ch][i])) wet[ch][i] = 0;
        if (fabs(wet[ch][i]) > pk) pk = fabs(wet[ch][i]);
    }
    if (pk > 0) for (int ch = 0; ch < nch; ch++) for (long i = 0; i < Nout; i++) wet[ch][i] *= 0.95 / pk;

    if (write_wav(c.out, wet, nch, Nout, mod->sr, c.bits)) return 1;
    if (c.verbose) fprintf(stderr, "xvoc: wrote %s (%d ch, %.2f s)\n", c.out, nch, Nout / sr);
    return 0;

    return 0;
}

int main(int argc, char **argv) {
    Cfg c;
    defaults(&c);
    parse_args(&c, argc, argv);
    int carrier_only = c.carrier_secs > 0;
    if (!c.in && !carrier_only) { fputs("need -i INPUT.wav or --carrier SECONDS  (try --help)\n", stderr); return 1; }
    if (c.mutate && !strcmp(c.out, "-")) { fputs("xvoc: --mutate needs -o FILE (can't write multiple variants to stdout)\n", stderr); return 1; }
    if (c.randomized) {
        fprintf(stderr, "xvoc: randomize seed = %u   (repeat with: --randomize %u)\n", c.rand_seed, c.rand_seed);
        print_summary(&c);
    }

    Audio mod = {0}, car = {0};
    if (carrier_only) {
        if (c.in) fputs("xvoc: --carrier given, ignoring -i (no vocoding)\n", stderr);
        mod.d = calloc(1, sizeof(double)); mod.n = 0; mod.sr = c.sr;
    } else if (read_wav(c.in, &mod)) return 1;
    if (c.wave == W_FILE && read_wav(c.carrier_file, &car)) return 1;
    if (c.wave == W_FILE && car.n < 2) die("carrier file is empty%s", "");

    if (!c.mutate) return run_vocoder(c, &mod, &car);

    /* --mutate: one normal run, then N randomized variants of the same settings */
    int rc = run_vocoder(c, &mod, &car);
    if (rc) return rc;
    unsigned base = c.seed_set ? c.seed : fresh_seed();
    if (!c.seed_set)
        fprintf(stderr, "xvoc: mutate seed = %u   (repeat this whole batch with: --seed %u --mutate ...)\n", base, base);
    fprintf(stderr, "xvoc: base   -> %s\n", c.out);
    for (int k = 1; k <= 5; k++) {
        Cfg m = c;
        unsigned vseed = base * 2654435761u + (unsigned)k * 40503u + 7;
        mutate_cfg(&m, c.mutate_intensity, vseed);
        m.seed = vseed; m.seed_set = 1; m.mutate = 0;
        char vname[4160];
        variant_name(c.out, k, vname, sizeof vname);
        m.out = vname;
        rc = run_vocoder(m, &mod, &car);
        if (rc) return rc;
        fprintf(stderr, "xvoc: mutate %d/5 -> %s\n    ", k, vname);
        print_flags(stderr, &m);
    }
    return 0;
}
