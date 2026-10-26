/*
 * kfweb.c -- the keyframe engine and its transient detector in a browser (WebAssembly): one ONESHOT+ track
 * (a voice a channel) played live, for explaining and auditioning it. GNU AGPL-3.0-or-later.
 *
 *   sh tools/build_kfweb.sh        (KF=dir: another checkout's kf/ sources, e.g. ../dt2-keyframer-elemod/kf)
 *
 * kf/kf.c and kf/tkeo.c are included unchanged (as kfborder.c does), the splice hook logging each of the left
 * voice's splices for the picture. The glue follows kf/sharc/dt2.c's ONESHOT path: STR's stretch (4 q, linear),
 * TUNE in semitones through kf_exp2, SIZE's K through kf_size_k, PLAY's four modes (reverse turns both rates
 * round), a play-once note silent past its edge (kf_edge), its end fade, the retrigger's declick, and the
 * transients THIN keeps (tk_cache_cuts in the grid's mode, as WERP+'s thin) as the engine's borders. Every
 * number is the C's: no fused multiply-adds (wasm32 has none; the host build -ffp-contract=off).
 *
 * The page runs two instances: its own (kw_analyze, kw_thin: the waveform, the transients, the cuts) and the
 * audio thread's (kw_render), which takes the cuts through kw_cuts / kw_set_cuts. kf/host/kfweb_host.c drives
 * the same calls natively; tools/check_kfweb.sh compares the two, and the host build with kfrender.
 *
 * The detector's constants are variables (tkeo.h's TK_TUNABLE, as kf/host/tkweb.c), the unit's values
 * (kf/host/kfweb_def.c) until the page changes one: its smoothing (the follower's SVF, constants 0..2) and the
 * onset delay that follows it (7, kw_calibrate). -DKW_FIXED builds the constants fixed, for the check that the
 * variables at the unit's values give the same numbers.
 */
#include <stdint.h>

#ifndef KW_KF_C
#define KW_KF_C "../kf.c"
#endif
#ifndef KW_TKEO_C
#define KW_TKEO_C "../tkeo.c"
#endif

static void kw_splice(const void *v);
#define KF_HOOK_SPLICE(v_) kw_splice(v_)
#define TK_TRACE kw_trace
#ifndef KW_FIXED
#define TK_TUNABLE
#endif
#include KW_KF_C
#include KW_TKEO_C

#define KW_MAXN   (48000 * 120)   /* frames a channel: 120 s */
#define KW_RING   4096            /* keyframes a voice (dt2.c's KF_RING) */
#define KW_BLOCK  32              /* the Digitakt's block */
#define KW_DEC    16              /* the envelope trace: one point per 16 frames */
#define KW_LOG    256             /* splices kept for the picture (a ring) */
#define KW_KEYS   1024            /* keyframes copied out for the picture, at most */

/* kf/sharc/dt2.c's constants */
#define KW_END_FADE   64
#define KW_DECLICK    16
#define KW_EDGE_OUT   192
#define KW_EDGE_IN    64

#ifdef __wasm__
/* freestanding: what clang may call for struct copies and clears */
void *memset(void *d, int c, unsigned long n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}
#endif

#ifdef TK_TUNABLE
/* the detector's constants, by index in kf/host/tkweb.c's order: 0 SVF g, 1 SVF num, 2 SVF 1/den, 3 floor,
   4 hysteresis, 5 onset fraction, 6 valley, 7 delay, 8 zero-crossing back, 9 forward, 10 gap, 11 end gap, 12 the
   cut at the peak, 13 rise, 14 the floor relative to the highest peak */
float   tk_t_svf_g, tk_t_svf_num, tk_t_svf_inv_den, tk_t_floor, tk_t_hyst, tk_t_on_frac, tk_t_valley;
int32_t tk_t_delay, tk_t_zc_back, tk_t_zc_fwd, tk_t_gap, tk_t_end_gap, tk_t_onset_peak;
float   tk_t_rise, tk_t_rel_floor;
extern const float kw_tk_def[15];
static int32_t tk_ready;
void kw_tk_set(int32_t k, float v)
{
    switch (k) {
    case 0: tk_t_svf_g = v; break;
    case 1: tk_t_svf_num = v; break;
    case 2: tk_t_svf_inv_den = v; break;
    case 3: tk_t_floor = v; break;
    case 4: tk_t_hyst = v; break;
    case 5: tk_t_on_frac = v; break;
    case 6: tk_t_valley = v; break;
    case 7: tk_t_delay = (int32_t)v; break;
    case 8: tk_t_zc_back = (int32_t)v; break;
    case 9: tk_t_zc_fwd = (int32_t)v; break;
    case 10: tk_t_gap = (int32_t)v; break;
    case 11: tk_t_end_gap = (int32_t)v; break;
    case 12: tk_t_onset_peak = (int32_t)v; break;
    case 13: tk_t_rise = v; break;
    case 14: tk_t_rel_floor = v; break;
    }
}
float kw_tk_default(int32_t k) { return (k >= 0 && k < 15) ? kw_tk_def[k] : 0.0f; }
void  kw_tk_reset(void) { int32_t k; for (k = 0; k < 15; k++) kw_tk_set(k, kw_tk_def[k]); tk_ready = 1; }
#define TK_READY() do { if (!tk_ready) kw_tk_reset(); } while (0)
#else
void  kw_tk_set(int32_t k, float v) { (void)k; (void)v; }
float kw_tk_default(int32_t k) { (void)k; return 0.0f; }
void  kw_tk_reset(void) {}
#define TK_READY() do {} while (0)
#endif

static int16_t  pcm[2][KW_MAXN + 4] __attribute__((aligned(4)));
static int32_t  n_frames, n_ch = 1;

/* the analysis (the page's instance) */
static tk_state st;
static tk_event ev[TK_MAX_EV];
static int32_t  scratch[TK_MAX_EV + 4];
static int32_t  gridout[TK_MAX_EV + 4];
static int32_t  reg[TK_REGION_SCRATCH + 4];
static tk_cache cache;
static int32_t  cache_mode = -1;
static float    env[KW_MAXN / KW_DEC + 2];

/* the cuts: written by kw_thin (the page) or by the page into the audio thread's (kw_set_cuts) */
static int32_t  cuts[TK_SLICES + 4];
static int32_t  tpl[TK_SLICES + 4];   /* the voices' list (kf_voice tp) */
static int32_t  tn_live, trans_on = 1;

/* the controls, and what they make */
static float    c_str = 1.0f, c_tune = 0.0f, c_size = 60.0f, c_gain = 1.0f / 32768.0f;
static int32_t  c_play = 3, c_s, c_e;
static float    stretch = 1.0f, pitch = 1.0f;
static int32_t  leash = 64, once = 1;

/* the voices and dt2.c's per-voice glue */
static int32_t  ring[2][KW_RING];
static kf_voice kv[2];
static int32_t  playing, slice_note;
static int32_t  done[2], fade[2], since_wrap[2], retrig[2];
static float    last_out[2];
static int32_t  edge_ip[2];
static uint32_t edge_fq[2];
static float    out[2][128 * 8];
static uint32_t kw_frames;   /* output frames since the page began */

/* the splice log: from, to (the play head's and the incoming head's places, wrapped), kind (0 the leash,
   1 forced at the region's edge, 2 a transient jump, 3 a border), the output frame (the block's) */
static int32_t  slog[KW_LOG * 4];
static uint32_t slog_n, seen_tj, seen_b, seen_f;
static int32_t  keys[KW_KEYS];

/* 2^x for |x| <= 8 or so (kf/sharc/dt2.c kf_exp2, dt2-capicola's gen_exp2) */
static float kw_exp2(float x)
{
    int32_t k = (int32_t)x;
    if ((float)k > x) k--;
    const float f = x - (float)k;
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013333f))));
    for (; k > 0; k--) p *= 2.0f;
    for (; k < 0; k++) p *= 0.5f;
    return p;
}

/* K from SIZE h (0..0x7800; x = h / 256): 2^(x / 10), at most KW_RING - 512, as the unit plays it (kf/sharc/dt2.c kf_size_k) */
static int32_t kw_size_k(uint32_t h)
{
    if (h > 0x7800u) h = 0x7800u;
    const uint32_t a = (h * 26215u) >> 26;
    const uint32_t u = ((h - a * 2560u) * 1677722u) >> 16;
    uint32_t r = 5186u;
    r = ((r * u) >> 16) + 14742u;
    r = ((r * u) >> 16) + 45608u;
    r = ((r * u) >> 16) + 65536u;
    {   /* at most the ring less 512 (dt2.c KF_K_MAX, KF60: past it every splice is a forced one) */
        const uint32_t k = ((r << a) + 32768u) >> 16;
        return (int32_t)(k > (uint32_t)(KW_RING - 512) ? (uint32_t)(KW_RING - 512) : k);
    }
}

static void kw_splice(const void *vp)
{
    const kf_voice *v = vp;
    int32_t kind = 0, *e;
    if (v != &kv[0]) return;
    if (v->n_tjump != seen_tj) kind = 2;
    else if (v->n_border != seen_b) kind = 3;
    else if (v->n_forced != seen_f) kind = 1;
    seen_tj = v->n_tjump;
    seen_b = v->n_border;
    seen_f = v->n_forced;
    e = slog + 4 * (slog_n & (KW_LOG - 1));
    e[0] = v->play.ri;
    e[1] = v->temp.ri;
    e[2] = kind;
    e[3] = (int32_t)kw_frames;
    slog_n++;
}

void kw_trace(const tk_state *s)
{
    const int32_t b = s->i / KW_DEC;
    const float e = s->env[s->env_w];
    if (s->i % KW_DEC == 0 || e > env[b]) env[b] = e;
}

/* the rates and the leash from the controls (dt2.c kf_controls) */
static void derive(void)
{
    const float dir = (c_play <= 1) ? -1.0f : 1.0f;
    float q = c_str * 0.25f;
    float h = c_size * 256.0f;
    if (q > 1.0f) q = 1.0f;
    if (q < -1.0f) q = -1.0f;
    stretch = 4.0f * q * dir;
    pitch = kw_exp2(c_tune * (1.0f / 12.0f)) * dir;
    if (h < 0.0f) h = 0.0f;
    leash = kw_size_k((uint32_t)h);
    once = (c_play == 0 || c_play == 3) ? 1 : 0;
}

/* ---- the sample and its analysis ---------------------------------------- */

int16_t *kw_pcm(int32_t ch) { return pcm[ch ? 1 : 0]; }
int32_t  kw_maxn(void) { return KW_MAXN; }
int32_t  kw_dec(void) { return KW_DEC; }
float   *kw_env(void) { return env; }
int32_t *kw_cuts(void) { return cuts; }
float   *kw_out(int32_t ch) { return out[ch ? 1 : 0]; }
int32_t *kw_log(void) { return slog; }
uint32_t kw_log_n(void) { return slog_n; }
int32_t  kw_log_size(void) { return KW_LOG; }
int32_t *kw_keybuf(void) { return keys; }

/* a new sample, already in kw_pcm (stereo 0: the left channel only): the note stopped, the region the whole */
void kw_sample(int32_t n, int32_t stereo)
{
    int32_t c;
    if (n > KW_MAXN) n = KW_MAXN;
    if (n < 0) n = 0;
    n_frames = n;
    n_ch = stereo ? 2 : 1;
    c_s = 0;
    c_e = n;
    playing = 0;
    tn_live = 0;
    cache_mode = -1;
    for (c = 0; c < 2; c++) {
        kf_init(&kv[c], ring[c], KW_RING);
        kv[c].tp = tpl;
        kv[c].tn = 0;
        done[c] = 1;
        last_out[c] = 0.0f;
    }
    derive();
}

/* the detector over the sample (both channels' sum, as the unit hears it; a mono sample's left twice): the
   peaks stored */
int32_t kw_analyze(void)
{
    TK_READY();
    tk_init(&st, (uintptr_t)pcm[0], n_ch > 1 ? (uintptr_t)pcm[1] : 0, n_frames, ev);
    while (!tk_run(&st, 1 << 16)) {}
    cache_mode = -1;
    return st.count;
}
int32_t kw_ev_count(void) { return st.count; }

/* the follower's lag for the constants now, as kf/host/tkslice.c --calibrate finds it (kf/host/tkweb.c
   tw_calibrate): a synthetic hit (the page writes it into kw_cal_buf: silence, then at KW_CAL_T0 a 2 kHz burst,
   instant attack, 60 ms decay) analysed with no delay and no zero-crossing search; the onset's distance from the
   hit. -100000: no peak, or the constants fixed. (It writes the envelope trace's start: analyse after it) */
#define KW_CAL_N  16384
#define KW_CAL_T0 4096
static int16_t  cal[KW_CAL_N + 4] __attribute__((aligned(4)));
int16_t *kw_cal_buf(void) { return cal; }
int32_t  kw_calibrate(void)
{
#ifdef TK_TUNABLE
    static tk_state cst;
    static tk_event cev[64];
    const int32_t zb = tk_t_zc_back, zf = tk_t_zc_fwd;
    TK_READY();
    tk_t_zc_back = tk_t_zc_fwd = 0;
    tk_init(&cst, (uintptr_t)cal, 0, KW_CAL_N, cev);
    cst.delay = 0;
    while (!tk_run(&cst, 4096)) {}
    tk_t_zc_back = zb;
    tk_t_zc_fwd = zf;
    return cst.count > 0 ? cev[0].pos - KW_CAL_T0 : -100000;
#else
    return -100000;
#endif
}
int32_t kw_ev_pos(int32_t k) { return (k >= 0 && k < st.count) ? ev[k].pos : -1; }

/* the cache as tk_rpc9 fills it at a grid of mode (1 VOL, 2 GRID, 3 RAND) holding every transient (up to 64):
   the analysis's seed, RAND's refilled at the grid's (kf/host/tkslice.c --region) */
static void cache_for(int32_t mode)
{
    const uint32_t gseed = 1u * 747796405u + 2891336453u;
    if (mode == cache_mode) return;
    tk_cache_fill(&cache, &st, ev, st.count, n_frames, 1u, scratch);
    if (mode == TK_RAND) tk_cache_fill(&cache, &st, ev, st.count, n_frames, gseed, scratch);
    cache.mode = mode;
    cache.slices = tk_grid_mode(&st, ev, st.count, n_frames, TK_SLICES, mode, gseed, gridout, scratch);
    cache_mode = mode;
}

/* the most THIN can keep in mode */
int32_t kw_thin_max(int32_t mode)
{
    if (n_frames < 8) return 0;
    cache_for(mode);
    return cache.slices;
}

/* THIN: the n strongest cuts of mode's grid, in time order (the first 0), into kw_cuts; their count */
int32_t kw_thin(int32_t mode, int32_t n)
{
    int32_t k = 0, got;
    if (n_frames < 8) return 0;
    cache_for(mode);
    if (n > cache.slices) n = cache.slices;
    if (n < 1) n = 1;
    got = tk_cache_cuts(&cache, n, reg);
    while (k < got) {
        cuts[k] = reg[k];
        k++;
    }
    return got;
}

/* the engine's transients: the first n of kw_cuts (0: none), now (a note playing finds the next one) */
void kw_set_cuts(int32_t n)
{
    int32_t k = 0, c;
    if (n > TK_SLICES) n = TK_SLICES;
    if (n < 0) n = 0;
    while (k < n) {
        tpl[k] = cuts[k];
        k++;
    }
    tn_live = n;
    for (c = 0; c < 2; c++) {
        kv[c].tn = trans_on ? tn_live : 0;
        if (playing) ktfind(&kv[c]);
    }
}

/* ---- the controls and the note ------------------------------------------ */

/* 0 STR (-4 .. 4, the stretch), 1 TUNE (semitones), 2 SIZE (0 .. 120), 3 PLAY (0 rev once, 1 rev loop, 2 fwd
   loop, 3 fwd once), 4 the region's start, 5 its end (frames), 6 transients on (1) / off (0), 7 the gain */
void kw_set(int32_t id, float x)
{
    int32_t c;
    switch (id) {
    case 0: c_str = x; break;
    case 1: c_tune = x; break;
    case 2: c_size = x; break;
    case 3: c_play = (int32_t)x & 3; break;
    case 4: c_s = (int32_t)x; break;
    case 5: c_e = (int32_t)x; break;
    case 6:
        trans_on = x != 0.0f;
        for (c = 0; c < 2; c++) {
            kv[c].tn = trans_on ? tn_live : 0;
            if (playing) ktfind(&kv[c]);
        }
        break;
    case 7: c_gain = x; break;
    }
    derive();
}

static void region(int32_t *s, int32_t *e)
{
    int32_t a = c_s, b = c_e;
    if (b > n_frames) b = n_frames;
    if (a < 0) a = 0;
    if (b - a < 64) {   /* (the shortest region: a block and some) */
        b = a + 64;
        if (b > n_frames) { b = n_frames; a = b - 64; }
        if (a < 0) a = 0;
    }
    *s = a;
    *e = b;
}

/* a note on the region (slice 1: a slice's note, its edges cut and faded as dt2.c's ov_on). A reference running
   backwards (STR below 0, or PLAY reversed) starts at the region's end */
void kw_trig(int32_t slice)
{
    int32_t s, e, at, c;
    if (n_frames < 64) return;
    region(&s, &e);
    at = s;
    if (stretch < 0.0f) at = e - 1;
    if (stretch == 0.0f && c_play <= 1) at = e - 1;
    for (c = 0; c < n_ch; c++) {
        retrig[c] = playing && !done[c];
        kf_trig(&kv[c], (uintptr_t)pcm[c], n_frames, at);
        kv[c].tp = tpl;
        kv[c].tn = trans_on ? tn_live : 0;
        done[c] = 0;
        fade[c] = 0;
        since_wrap[c] = slice ? 0 : 1 << 20;
    }
    seen_tj = kv[0].n_tjump;
    seen_b = kv[0].n_border;
    seen_f = kv[0].n_forced;
    slice_note = slice;
    playing = 1;
}

void kw_stop(void)
{
    int32_t c;
    playing = 0;
    for (c = 0; c < 2; c++) {
        done[c] = 1;
        last_out[c] = 0.0f;
    }
}

/* a play-once note's or a slice's edges (dt2.c kf_edge): faded to silence as the reference meets its edge,
   silent after it when the note plays once, else faded in from the wrap */
static void kw_edge(int32_t c, float *o, const kf_params *p, int32_t n)
{
    int32_t i, wrapped = 0;
    const float ref0 = (float)edge_ip[c] + (float)(int32_t)(edge_fq[c] >> 8) * (1.0f / 16777216.0f);
    float te = -1.0f;
    if (p->stretch > 0.0f) te = ((float)p->e - ref0) * tk_recip(p->stretch);
    if (p->stretch < 0.0f) te = (ref0 - (float)p->s) * tk_recip(-p->stretch);
    for (i = 0; i < n; i++) {
        float gi = 1.0f;
        if (since_wrap[c] < KW_EDGE_IN) {
            const float a = (float)(since_wrap[c] + i) * (1.0f / (float)KW_EDGE_IN);
            if (a < gi) gi = a;
        }
        if (te >= 0.0f) {
            const float d = te - (float)i;
            if (d > 0.0f) {
                const float a = d * (1.0f / (float)KW_EDGE_OUT);
                if (a < gi) gi = a;
            } else {
                wrapped = 1;
                if (kv[c].once & 1) gi = 0.0f;
                else {
                    const float a = -d * (1.0f / (float)KW_EDGE_IN);
                    if (a < gi) gi = a;
                }
            }
        }
        o[i] *= gi;
    }
    if (wrapped) since_wrap[c] = n - (int32_t)te;
    else if (since_wrap[c] < (1 << 20)) since_wrap[c] += n;
}

/* frames (a multiple of 32, at most 1024) into kw_out(0), kw_out(1): 1 while a note plays */
int32_t kw_render(int32_t frames)
{
    int32_t b, c, i;
    if (frames > 1024) frames = 1024;
    for (b = 0; b + KW_BLOCK <= frames; b += KW_BLOCK) {
        kf_params p;
        int32_t s, e, edge;
        if (!playing) {
            for (c = 0; c < 2; c++)
                for (i = 0; i < KW_BLOCK; i++) out[c][b + i] = 0.0f;
            kw_frames += KW_BLOCK;
            continue;
        }
        region(&s, &e);
        p.stretch = stretch;
        p.pitch = pitch;
        p.k = leash;
        p.s = s;
        p.e = e;
        p.eps = KF_EPS;
        p.max_fade = 0;
        p.gain = c_gain;
        edge = (slice_note || once) ? 1 : 0;
        for (c = 0; c < n_ch; c++) {
            float *o = out[c] + b;
            if (done[c]) {
                for (i = 0; i < KW_BLOCK; i++) o[i] = 0.0f;
                continue;
            }
            if (kv[c].ended == 2) {   /* (play once: the note ended last block; its fade from this one's start) */
                kv[c].ended = 3;
                fade[c] = KW_END_FADE;
            }
            kv[c].once = once | (edge ? 2 : 0);
            edge_ip[c] = kv[c].ref.ip;
            edge_fq[c] = kv[c].ref.fq;
            kf_render(&kv[c], &p, o, KW_BLOCK);
            if (edge) kw_edge(c, o, &p, KW_BLOCK);
            if (fade[c] > 0) {
                for (i = 0; i < KW_BLOCK; i++) {
                    if (fade[c] > 0) {
                        o[i] *= (float)fade[c] * (1.0f / (float)(KW_END_FADE + 1));
                        if (edge) o[i] = 0.0f;
                        fade[c]--;
                    } else {
                        o[i] = 0.0f;
                    }
                }
                if (fade[c] == 0) done[c] = 1;
            }
            if (retrig[c]) {   /* (dt2.c kf_retrig_declick) */
                const float dw = (last_out[c] - o[0]) * (1.0f / (float)(KW_DECLICK + 1));
                float w = dw * (float)KW_DECLICK;
                for (i = 0; i < KW_DECLICK; i++) { o[i] += w; w -= dw; }
                retrig[c] = 0;
            }
            last_out[c] = o[KW_BLOCK - 1];
        }
        if (n_ch == 1)
            for (i = 0; i < KW_BLOCK; i++) out[1][b + i] = out[0][b + i];
        if (done[0] && (n_ch == 1 || done[1])) {
            playing = 0;
            last_out[0] = last_out[1] = 0.0f;
        }
        kw_frames += KW_BLOCK;
    }
    return playing;
}

/* ---- for the picture: the left voice ------------------------------------ */

/* 0 the reference, 1 the play head, 2 the incoming head (wrapped frames) */
int32_t kw_head(int32_t which)
{
    if (which == 0) return kv[0].ref.ri;
    if (which == 1) return kv[0].play.ri;
    return kv[0].temp.ri;
}
int32_t  kw_splicing(void) { return kv[0].splicing; }
float    kw_fade_t(void) { return kv[0].t; }
int32_t  kw_playing(void) { return playing; }
uint32_t kw_clock(void) { return kw_frames; }
int32_t  kw_leash(void) { return leash; }
float    kw_stretch(void) { return stretch; }
float    kw_pitch(void) { return pitch; }

/* 0 splices, 1 transient jumps, 2 border splices, 3 forced, 4 keyframes found, 5 transients live */
uint32_t kw_stat(int32_t i)
{
    switch (i) {
    case 0: return kv[0].n_splice;
    case 1: return kv[0].n_tjump;
    case 2: return kv[0].n_border;
    case 3: return kv[0].n_forced;
    case 4: return kv[0].n_keys;
    case 5: return (uint32_t)kv[0].tn;
    }
    return 0;
}

/* the keyframes the left voice holds now (its window around the heads; at most KW_KEYS of them, centred on the
   reference's), wrapped frames, in time order, into kw_keybuf */
int32_t kw_keys(void)
{
    const kf_voice *v = &kv[0];
    int32_t k = 0, m = v->ref.m - KW_KEYS / 2;
    if (m > v->base + v->count - KW_KEYS) m = v->base + v->count - KW_KEYS;
    if (m < v->base) m = v->base;
    while (m + k < v->base + v->count && k < KW_KEYS) {
        keys[k] = kwrap(v, v->ring[(m + k) & v->mask]);
        k++;
    }
    return k;
}
