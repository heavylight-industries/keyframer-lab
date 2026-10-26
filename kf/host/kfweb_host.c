/*
 * kfweb_host.c -- kf/host/kfweb.c (the browser app's engine) driven natively from a script, for
 * tools/check_kfweb.sh: the same calls the page makes, the output written as raw float32 so the WebAssembly
 * build's (kf/host/kfweb_check.mjs, the same script) can be compared byte for byte. GNU AGPL-3.0-or-later.
 *
 *   kfweb_host in.wav script.txt out.f32
 *
 * The script, a command a line ('#' a comment):
 *   analyze              the detector over the sample (kw_analyze)
 *   thin MODE N          THIN's cuts (kw_thin: 1 VOL, 2 GRID, 3 RAND), then kw_set_cuts with them
 *   set ID VALUE         kw_set (0 STR, 1 TUNE, 2 SIZE, 3 PLAY, 4 start, 5 end, 6 transients, 7 gain)
 *   trig SLICE           kw_trig
 *   stop                 kw_stop
 *   render FRAMES        kw_render in pieces of 128 frames (an AudioWorklet's quantum), both channels
 *                        interleaved into the output
 * Each line's result goes to stdout (the page's check prints the same).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kfweb.c"

static void die(const char *m) { fprintf(stderr, "kfweb_host: %s\n", m); exit(1); }

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* the file's first two channels as int16 into kw_pcm (as kfrender.c reads them): the channels */
static int wav_read(const char *path, int32_t *frames)
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
            bits = le16(d + o + 22);
            if (fmt == 0xfffe) fmt = le16(d + o + 32);
        } else if (!memcmp(d + o, "data", 4)) {
            data = d + o + 8;
            dlen = cl;
            if ((long)(o + 8 + dlen) > size) dlen = (uint32_t)(size - o - 8);
        }
        o += 8 + cl + (cl & 1);
    }
    if (!data || !chans) die("no fmt or data chunk");
    if (fmt != 1 || bits != 16) die("want 16-bit PCM");
    *frames = (int32_t)(dlen / (uint32_t)(2 * chans));
    if (*frames > kw_maxn()) *frames = kw_maxn();
    const int nch = chans > 2 ? 2 : chans;
    for (int c = 0; c < nch; c++) {
        int16_t *p = kw_pcm(c);
        for (int32_t i = 0; i < *frames; i++) p[i] = (int16_t)le16(data + ((size_t)i * chans + c) * 2);
    }
    free(d);
    return nch;
}

int main(int argc, char **argv)
{
    if (argc != 4) die("usage: kfweb_host in.wav script.txt out.f32");
    int32_t frames;
    const int nch = wav_read(argv[1], &frames);
    kw_sample(frames, nch > 1);
    printf("sample %d %d\n", frames, nch);
    FILE *sc = fopen(argv[2], "r"), *of = fopen(argv[3], "wb");
    if (!sc || !of) die("cannot open the script or the output");
    char line[256];
    while (fgets(line, sizeof line, sc)) {
        char cmd[32];
        double a = 0, b = 0;
        const int got = sscanf(line, "%31s %lf %lf", cmd, &a, &b);
        if (got < 1 || cmd[0] == '#') continue;
        if (!strcmp(cmd, "analyze")) printf("analyze %d\n", kw_analyze());
        else if (!strcmp(cmd, "thin")) {
            const int32_t n = kw_thin((int32_t)a, (int32_t)b);
            kw_set_cuts(n);
            kw_set_chan_cuts(0, kw_chan_n(0));   /* (stereo detection: each voice its own) */
            kw_set_chan_cuts(1, kw_chan_n(1));
            printf("thin %d of %d:", n, kw_thin_max((int32_t)a));
            for (int32_t k = 0; k < n; k++) printf(" %d", kw_cuts()[k]);
            for (int32_t c = 0; c < 2; c++) {   /* (each voice's borders) */
                printf(" |");
                for (int32_t k = 0; k < kw_chan_n(c); k++) printf(" %d", kw_chan_cuts(c)[k]);
            }
            printf("\n");
        } else if (!strcmp(cmd, "set")) kw_set((int32_t)a, (float)b);
        else if (!strcmp(cmd, "trig")) kw_trig((int32_t)a);
        else if (!strcmp(cmd, "stop")) kw_stop();
        else if (!strcmp(cmd, "render")) {
            int32_t left = (int32_t)a, pl = 0;
            static float il[2 * 128];
            while (left > 0) {
                const int32_t q = left < 128 ? left : 128;
                pl = kw_render(q);
                for (int32_t i = 0; i < q; i++) { il[2 * i] = kw_out(0)[i]; il[2 * i + 1] = kw_out(1)[i]; }
                fwrite(il, 4, (size_t)(2 * q), of);
                left -= q;
            }
            printf("render %d: playing %d, splices %u, jumps %u, borders %u, forced %u, keys %u; head %d %d\n",
                   (int)a, pl, kw_stat(0), kw_stat(1), kw_stat(2), kw_stat(3), kw_stat(4), kw_head(0), kw_head(1));
        } else die("unknown command");
    }
    fclose(of);
    return 0;
}
