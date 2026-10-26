/*
 * kfrender.c -- render a WAV through the keyframe engine on the host, in the
 * Digitakt's 32-sample blocks, one independent engine per channel.
 * GNU AGPL-3.0-or-later.
 *
 *   kfrender in.wav out.wav [options]
 *
 *   --stretch A   the reference's rate (bipolar; 0 freezes)         default 1
 *   --pitch A     the play head's rate, as a ratio (bipolar)         default 1
 *   --tune ST     semitones, multiplied into the pitch               default 0
 *   --k A         the leash, keyframes                               default 64
 *   --start F     the region's start, a fraction of the sample       default 0
 *   --end F       the region's end                                   default 1
 *   --seconds S   output length       default: the region at the starting stretch, 1..120 s
 *   --eps N       the analyzer's threshold, int16 units              default 33 (0.001)
 *   --max-fade MS cap on a leash crossfade, ms (0: none)             default 0
 *   --trans 1     transient borders: the input's transients (kf/tkeo.c, a VOL grid of up to 64) given to
 *                 the engine: each attack played once whatever STR and the pitch do
 *   --once 1      play once (a play-once trig): the note ends as the reference reaches the region's
 *                 edge, faded to silence over its last 192 samples as kf/sharc/dt2.c does, then silence
 *
 * A (automation): a constant, or breakpoints "t:v,t:v,..." in output seconds,
 * linear between them and held past the ends, e.g. --pitch 0:1,2:-1,4:1
 * scratches through zero and back. The output is 32-bit float.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kf.h"
#include "../tkeo.h"

#define BLOCK 32
#define RING  2048
#define MAXPT 64

typedef struct { int n; double t[MAXPT], v[MAXPT]; } autom;

static void die(const char *m) { fprintf(stderr, "kfrender: %s\n", m); exit(1); }

static void auto_parse(autom *a, const char *s)
{
    a->n = 0;
    if (!strchr(s, ':')) { a->t[0] = 0; a->v[0] = atof(s); a->n = 1; return; }
    const char *p = s;
    while (*p && a->n < MAXPT) {
        char *q;
        a->t[a->n] = strtod(p, &q);
        if (*q != ':') die("automation: want t:v,t:v,...");
        a->v[a->n] = strtod(q + 1, &q);
        a->n++;
        p = (*q == ',') ? q + 1 : q;
    }
}

static double auto_at(const autom *a, double t)
{
    if (t <= a->t[0]) return a->v[0];
    for (int i = 1; i < a->n; i++)
        if (t < a->t[i]) return a->v[i - 1] + (a->v[i] - a->v[i - 1]) * (t - a->t[i - 1]) / (a->t[i] - a->t[i - 1]);
    return a->v[a->n - 1];
}

/* ---- WAV ---------------------------------------------------------------- */

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* the file's first two channels as int16, planar; each buffer padded to a word */
static int wav_read(const char *path, int16_t **ch, int *nch, int32_t *frames, uint32_t *rate)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open input");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)size);
    if (fread(d, 1, (size_t)size, f) != (size_t)size) die("short read");
    fclose(f);
    if (size < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) die("not a WAV file");
    int fmt = 0, chans = 0, bits = 0;
    const unsigned char *data = NULL;
    uint32_t dlen = 0;
    for (long o = 12; o + 8 <= size;) {
        uint32_t cl = le32(d + o + 4);
        if (!memcmp(d + o, "fmt ", 4)) {
            fmt = le16(d + o + 8);
            chans = le16(d + o + 10);
            *rate = le32(d + o + 12);
            bits = le16(d + o + 22);
            if (fmt == 0xfffe) fmt = le16(d + o + 32);   /* WAVE_FORMAT_EXTENSIBLE: the subformat */
        } else if (!memcmp(d + o, "data", 4)) {
            data = d + o + 8;
            dlen = cl;
            if ((long)(o + 8 + dlen) > size) dlen = (uint32_t)(size - o - 8);
        }
        o += 8 + cl + (cl & 1);
    }
    if (!data || !chans) die("no fmt or data chunk");
    int bps = bits / 8;
    if (!((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) die("want 16/24/32-bit PCM or 32-bit float");
    *frames = (int32_t)(dlen / (uint32_t)(bps * chans));
    *nch = chans > 2 ? 2 : chans;
    for (int c = 0; c < *nch; c++) {
        ch[c] = calloc((size_t)*frames + 4, 2);
        for (int32_t i = 0; i < *frames; i++) {
            const unsigned char *s = data + ((size_t)i * chans + c) * bps;
            double x;
            if (fmt == 3) { float fl; memcpy(&fl, s, 4); x = fl; }
            else if (bits == 16) x = (int16_t)le16(s) / 32768.0;
            else if (bits == 24) x = ((int32_t)(s[0] << 8 | s[1] << 16 | (uint32_t)s[2] << 24) >> 8) / 8388608.0;
            else x = (int32_t)le32(s) / 2147483648.0;
            long q = lrint(x * 32768.0);
            ch[c][i] = (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
        }
    }
    free(d);
    return 0;
}

static void put32(FILE *f, uint32_t v) { unsigned char b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }
static void put16(FILE *f, uint16_t v) { unsigned char b[2] = { v, v >> 8 }; fwrite(b, 1, 2, f); }

static void wav_write(const char *path, const float *x, int nch, int32_t frames, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot open output");
    uint32_t dlen = (uint32_t)frames * nch * 4;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + dlen); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 3); put16(f, (uint16_t)nch);
    put32(f, rate); put32(f, rate * nch * 4); put16(f, (uint16_t)(nch * 4)); put16(f, 32);
    fwrite("data", 1, 4, f); put32(f, dlen);
    fwrite(x, 4, (size_t)frames * nch, f);   /* (little-endian host) */
    fclose(f);
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 3) die("usage: kfrender in.wav out.wav [--stretch A] [--pitch A] [--tune ST] [--k A] "
                      "[--start F] [--end F] [--seconds S] [--eps N] [--max-fade MS] [--once 1]");
    autom st, pi, kk;
    auto_parse(&st, "1");
    auto_parse(&pi, "1");
    auto_parse(&kk, "64");
    double tune = 0, start = 0, end = 1, seconds = 0, maxfade_ms = 0;
    int eps = KF_EPS, once = 0, trans = 0;
    for (int i = 3; i < argc; i++) {
        const char *o = argv[i];
        if (i + 1 >= argc) die("option without a value");
        const char *a = argv[++i];
        if (!strcmp(o, "--stretch")) auto_parse(&st, a);
        else if (!strcmp(o, "--pitch")) auto_parse(&pi, a);
        else if (!strcmp(o, "--k")) auto_parse(&kk, a);
        else if (!strcmp(o, "--tune")) tune = atof(a);
        else if (!strcmp(o, "--start")) start = atof(a);
        else if (!strcmp(o, "--end")) end = atof(a);
        else if (!strcmp(o, "--seconds")) seconds = atof(a);
        else if (!strcmp(o, "--eps")) eps = atoi(a);
        else if (!strcmp(o, "--max-fade")) maxfade_ms = atof(a);
        else if (!strcmp(o, "--once")) once = atoi(a);
        else if (!strcmp(o, "--trans")) trans = atoi(a);
        else die("unknown option");
    }
    int16_t *ch[2];
    int nch;
    int32_t frames;
    uint32_t rate = 48000;
    wav_read(argv[1], ch, &nch, &frames, &rate);
    if (frames < 4) die("sample too short");
    int32_t s = (int32_t)(start * frames), e = (int32_t)(end * frames);
    if (seconds <= 0) {
        double r = fabs(st.v[0]);
        seconds = r > 1e-3 ? (e - s) / (r * rate) : 10.0;
        if (seconds < 1) seconds = 1;
        if (seconds > 120) seconds = 120;
        if (once) seconds += 0.25;   /* (the end, then a little silence) */
    }
    int32_t out_frames = (int32_t)(seconds * rate);
    out_frames -= out_frames % BLOCK;
    float *out = calloc((size_t)out_frames * nch, 4);
    static int32_t ring[2][RING];
    kf_voice v[2];
    const double tmul = pow(2.0, tune / 12.0);
    for (int c = 0; c < nch; c++) {
        kf_init(&v[c], ring[c], RING);
        kf_trig(&v[c], (uintptr_t)ch[c], frames, s);
        if (once) v[c].once = 3;
    }
    static int32_t tcuts[TK_SLICES];
    if (trans) {   /* (the transients of both channels' sum, as the unit's detector hears them) */
        static tk_event tev[TK_MAX_EV];
        static tk_state tst;
        static int32_t scratch[TK_MAX_EV];
        tk_init(&tst, (uintptr_t)ch[0], (uintptr_t)ch[nch > 1 ? 1 : 0], frames, tev);
        tk_run(&tst, frames);
        const int32_t nc = tk_grid_mode(&tst, tev, tst.count, frames, TK_SLICES, TK_LOUD, 0, tcuts, scratch);
        for (int32_t i = 1; i < nc; i++)
            for (int32_t j = i; j > 0 && tcuts[j - 1] > tcuts[j]; j--) { int32_t x = tcuts[j]; tcuts[j] = tcuts[j - 1]; tcuts[j - 1] = x; }
        for (int c = 0; c < nch; c++) { v[c].tp = tcuts; v[c].tn = nc; }
        fprintf(stderr, "transients: %d\n", (int)nc);
    }
    int32_t ended[2] = { -1, -1 };
    float blk[BLOCK];
    for (int32_t b = 0; b < out_frames; b += BLOCK) {
        const double t = (double)b / rate;
        kf_params p;
        p.stretch = (float)auto_at(&st, t);
        p.pitch = (float)(auto_at(&pi, t) * tmul);
        p.k = (int32_t)lrint(auto_at(&kk, t));
        p.s = s;
        p.e = e;
        p.eps = eps;
        p.max_fade = (int32_t)(maxfade_ms * rate / 1000.0);
        p.gain = 1.0f / 32768.0f;
        for (int c = 0; c < nch; c++) {
            kf_render(&v[c], &p, blk, BLOCK);
            for (int i = 0; i < BLOCK; i++) out[(size_t)(b + i) * nch + c] = blk[i];
            if (once && ended[c] < 0 && v[c].ended >= 2) ended[c] = b + v[c].ended_at;
        }
    }
    for (int c = 0; c < nch && once; c++) {   /* the glue's end: faded to silence at the edge, then nothing */
        const int32_t E = ended[c] < 0 ? out_frames : ended[c];
        for (int32_t i = 0; i < out_frames; i++) {
            float g = 1.0f;
            if (i >= E) g = 0.0f;
            else if (i >= E - 192) g = (float)(E - i) / 193.0f;
            out[(size_t)i * nch + c] *= g;
        }
    }
    wav_write(argv[2], out, nch, out_frames, rate);
    for (int c = 0; c < nch; c++)
        fprintf(stderr, "channel %d: %u splices (%.1f/s, %u forced at the region edge), %.0f keyframes/s found, mean crossfade %.1f ms\n", c,
                v[c].n_splice, v[c].n_splice / seconds, v[c].n_forced, v[c].n_keys / seconds,
                v[c].n_splice ? 1000.0 * v[c].sum_fade / v[c].n_splice / rate : 0.0);
    fprintf(stderr, "wrote %s: %.2f s, %d channel(s), %u Hz\n", argv[2], (double)out_frames / rate, nch, rate);
    return 0;
}
