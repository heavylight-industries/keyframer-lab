/*
 * tkeo.c -- transient detection for slicing (tkeo.h). GNU AGPL-3.0-or-later.
 *
 * No hardware loops (tools/build_dsp.sh: the firmware's loop stack is not ours): every loop is a
 * while with a test of its own, never a constant-count for.
 */
#include "tkeo.h"

#define TK_MASK (TK_ENV_N - 1)

/* sample j of the mono sum, full scale 1 (int16 planar, as kf.c's KX) */
static int32_t tk_x16(uintptr_t pcm, int32_t j)
{
    const uintptr_t a = pcm + 2u * (uint32_t)j;
    const uint32_t w = *(const uint32_t *)(a & ~(uintptr_t)3);
    if ((a & 2u) != 0u) return (int32_t)w >> 16;
    return (int32_t)(w << 16) >> 16;
}

/* the hot loop's (tk_run): tk_x16 and tk_env's expressions without a call. The SHARC's compiler calls every function:
   before 2026-10-24 the analysis took about 460 instructions a frame, 130 of them tk_x16, tk_sum and tk_env's calls */
#define TK_W16(pcm, j)  (*(const uint32_t *)(((pcm) + 2u * (uint32_t)(j)) & ~(uintptr_t)3))
#define TK_X16(pcm, j)  ((((pcm) + 2u * (uint32_t)(j)) & 2u) ? (int32_t)TK_W16(pcm, j) >> 16 \
                                                             : (int32_t)(TK_W16(pcm, j) << 16) >> 16)
#define TK_ENV(s, lag)  ((s)->env[((s)->env_w - (lag)) & TK_MASK])

static int32_t tk_sum(const tk_state *s, int32_t j)   /* L + R, int */
{
    return tk_x16(s->pcm_l, j) + tk_x16(s->pcm_r, j);
}

/* 1/x for x > 0 by multiplications only (as kf.c krecip): the seed 2^-k with x times it in [0.5, 1), so five Newton
   steps reach the float's precision. (Before 2026-10-23 the seed put x r in [1, 2): from near 2 five steps left up to
   21% (1/512,000 came out 0.79 of itself), and a GRID line asked for the cut of a lower index: slices of no length,
   the last ones short, THIN's live slices scaled, on samples whose length's mantissa is near 2: the owner's unit) */
static float tk_recip(float x)
{
    float r = 1.0f;
    float t = x;
    while (t >= 2.0f) { t *= 0.5f; r *= 0.5f; }
    while (t < 1.0f) { t *= 2.0f; r *= 2.0f; }
    r *= 0.5f;   /* (x r = t / 2: [0.5, 1)) */
    r = r * (2.0f - x * r);
    r = r * (2.0f - x * r);
    r = r * (2.0f - x * r);
    r = r * (2.0f - x * r);
    r = r * (2.0f - x * r);
    return r;
}

static float tk_env(const tk_state *s, int32_t lag)   /* the envelope lag steps ago */
{
    return s->env[(s->env_w - lag) & TK_MASK];
}

/* the best cut at or near t: the zero crossing of the mono sum closest before it (within TK_ZC_BACK),
   else after it (within TK_ZC_FWD), else t. Of a crossing's two samples, the one nearer zero */
static int32_t tk_zc(const tk_state *s, int32_t t)
{
    int32_t j = t;
    int32_t lo = t - TK_ZC_BACK;
    if (lo < 1) lo = 1;
    if (j > s->n - 1) j = s->n - 1;
    while (j >= lo) {
        const int32_t a = tk_sum(s, j - 1), b = tk_sum(s, j);
        if (b == 0) return j;
        if ((a < 0) != (b < 0)) {
            const int32_t aa = a < 0 ? -a : a, bb = b < 0 ? -b : b;
            return (aa < bb) ? j - 1 : j;
        }
        j--;
    }
    j = t + 1;
    int32_t hi = t + TK_ZC_FWD;
    if (hi > s->n - 1) hi = s->n - 1;
    if (j < 1) j = 1;
    while (j <= hi) {
        const int32_t a = tk_sum(s, j - 1), b = tk_sum(s, j);
        if (b == 0) return j;
        if ((a < 0) != (b < 0)) {
            const int32_t aa = a < 0 ? -a : a, bb = b < 0 ? -b : b;
            return (aa < bb) ? j - 1 : j;
        }
        j++;
    }
    if (t < 0) return 0;
    if (t > s->n - 1) return s->n - 1;
    return t;
}

void tk_init(tk_state *s, uintptr_t pcm_l, uintptr_t pcm_r, int32_t n, tk_event *ev)
{
    int32_t k = 0;
    s->pcm_l = pcm_l;
    s->pcm_r = pcm_r ? pcm_r : pcm_l;
    s->n = n;
    s->i = 0;
    s->delay = TK_DELAY;
    s->h1 = s->h2 = s->h3 = s->h4 = 0.0f;
    s->s1 = s->s2 = 0.0f;
    s->prev_d1 = 0.0f;
    s->last_peak_i = -TK_ENV_N;
    s->pend = 0;
    s->pend_val = 0.0f;
    s->pend_pk = s->pend_on = s->pend_i = 0;
    s->env_w = 0;
    s->count = 0;
    s->ev = ev;
    s->lo = 0.0f;
    while (k < TK_ENV_N && k >= 0) { s->env[k] = 0.0f; k++; }   /* (two tests: no constant-count loop) */
}

/* one input sample, x the mono sum (full scale 1) of frame s->i; 1 if it ended a peak, its envelope
   value, its lag (fraction) and onset lag set */
/* a candidate peak (tk_run: the envelope's derivative at lag 2 turned from < 0 to >= 0) and what tk_run takes from it */
typedef struct {
    float   older, newer;   /* the derivative, the step before and this one */
    float   value, lag;     /* the peak's height and lag */
    int32_t on;             /* its onset's lag */
} tk_hit;

/* the candidate's top, by the cubic through the envelope, and its onset: 1 a peak above the floor (h's value, lag,
   on), 0 none. (Two arguments: selcc passes stack arguments wrongly) */
static int32_t tk_peak(tk_state *s, tk_hit *h)
{
    const float older = h->older, newer = h->newer;
    const float dd = older - newer;
    const float frac = (dd < -1e-20f) ? older * -tk_recip(-dd) : 0.5f;
    const float lg = 3.0f - frac;
    /* the value there: the cubic read at lag lg (its integer part 2, t = 1 - frac; 3 when frac is 0) */
    const int32_t li = (frac > 0.0f) ? 2 : 3;
    const float t = lg - (float)li;
    const float e0 = tk_env(s, li - 1), e1 = tk_env(s, li), e2 = tk_env(s, li + 1), e3 = tk_env(s, li + 2);
    const float cc = (e2 - e0) * 0.5f;
    const float vv = e1 - e2;
    const float ww = cc + vv;
    const float aa = ww + vv + (e3 - e1) * 0.5f;
    const float bb = ww + aa;
    const float val = e1 + t * (cc + t * (-bb + t * aa));
    if (!(val > TK_FLOOR)) return 0;
    /* the onset: back from the peak to where the envelope was TK_ON_FRAC of it, or its valley, no
       further than the last peak or the buffer */
    int32_t k = li + 1;
    int32_t kmax = s->i - s->last_peak_i;
    if (kmax > TK_ENV_N - 8) kmax = TK_ENV_N - 8;
    s->lo = 0.0f;
#ifdef TK_TUNABLE
    if (TK_ONSET_PEAK) k = (int32_t)(lg + 0.5f); else   /* (the bench's A/B: the peak itself) */
#endif
    {
        const float lim = val * TK_ON_FRAC;
        float lo = val;
        int32_t klo = k;
        while (k < kmax) {   /* back to silence (TK_ON_FRAC of the peak) or a valley, its lowest point kept */
            const float ek = tk_env(s, k);
            if (ek < lo) { lo = ek; klo = k; }
            if (ek <= lim) break;
            if (ek > lo * TK_VALLEY) break;   /* well up again going back: the valley */
            k++;
        }
        {   /* then forward from the lowest to where the hump had risen TK_ON_FRAC of the way from it to the
               peak: a hit over a bed (a pad, a violin) starts where it leaves the bed, not where the bed dipped */
            const float rise = lo + (val - lo) * TK_ON_FRAC;
            k = klo;
            while (k > li + 1 && tk_env(s, k - 1) <= rise) k--;
        }
        s->lo = lo;
    }
    h->value = val;
    h->lag = lg;
    h->on = k;
    return 1;
}

/* the pending hump's peak stored: its cut at the best zero crossing near its onset */
static void tk_commit(tk_state *s)
{
    s->pend = 0;
    s->last_peak_i = s->pend_i;
    if (s->count >= TK_MAX_EV) return;
    {
        const int32_t cut = tk_zc(s, s->pend_on);
        tk_event *p = s->count > 0 ? s->ev + s->count - 1 : 0;
        if (p && cut <= p->pos) {   /* back onto the last cut: the same hit, the higher peak's */
            if (s->pend_val > p->value) { p->peak = s->pend_pk; p->value = s->pend_val; }
        } else if (cut < s->n) {
            tk_event *e = s->ev + s->count;
            e->pos = cut;
            e->peak = s->pend_pk;
            e->value = s->pend_val;
            s->count++;
        }
    }
}

/* a frame's tail, after its common part (tk_run's loop, or tk_fast's on an event): a peak (got: h's top) to the
   pending hump, or the hump, fallen TK_HYST below its top, committed. s->i and s->env_w this frame's */
static void tk_tail(tk_state *s, const tk_hit *h, int32_t got)
{
    if (got) {
        const float val = h->value, lg = h->lag;
        const int32_t on = h->on;
        /* the envelope at lag k is the TKEO of frame i - k - 2, delayed by the smoothing */
        int32_t take = 1;
        if (!s->pend) {   /* a new hump: only one that rose from a valley TK_RISE of its top or lower (a
                             wobble on a decay is the hit before's tail, not a hit) */
            if (s->lo > val * TK_RISE) take = 0;
            else {
                s->pend = 1;
                s->pend_val = 0.0f;
            }
        }
        if (take && val > s->pend_val) {   /* its highest top so far */
            /* the onset, from the envelope behind this top: the hump's first, or one the tops so far are
               below by more than TK_HYST (they were ripple before this hit, not its start: its onset
               kept, a cut fell in the noise ahead of the hit); a top among others of its height keeps
               the hump's onset (ripple on one hump's top) */
            if (val * (1.0f - TK_HYST) >= s->pend_val) s->pend_on = s->i - 2 - on - s->delay;
            s->pend_val = val;
            s->pend_pk = s->i - 2 - (int32_t)(lg + 0.5f) - s->delay;
            s->pend_i = s->i;
        }
    } else if (s->pend && TK_ENV(s, 0) < s->pend_val * (1.0f - TK_HYST)) {
        tk_commit(s);   /* fallen TK_HYST below the top: a peak */
    }
}

#ifdef KF_ASM
/* kf/sharc/tk.asm's tk_fast: the frames' common part in assembly (the C's operations in its order: bit for bit),
   back here for a frame's tail (io.ev 1 a candidate peak, 2 the pending hump fallen: tk_tail) */
typedef struct {
    float   k[8];           /* 2^-16, 0.5, 2, TK_SVF_NUM, TK_SVF_INV_DEN, TK_SVF_G, 1 - TK_HYST, 0 */
    int32_t mask;           /* TK_MASK */
    int32_t ev;             /* 0 none (frames done, or the sample's end), 1 a candidate peak, 2 the pend fallen */
    float   older, newer;   /* the candidate's derivative, the step before and its own */
} tk_fastio;
int32_t tk_fast(tk_state *s, int32_t frames, tk_fastio *io);
#ifdef KF_LAYOUT_CHECK   /* (build_dsp.sh's clang pass) tk.asm reads tk_state and tk_fastio at fixed words */
typedef char tk_fast_layout[(__builtin_offsetof(tk_state, n) == 8 && __builtin_offsetof(tk_state, i) == 12 &&
                             __builtin_offsetof(tk_state, h1) == 20 && __builtin_offsetof(tk_state, s1) == 36 &&
                             __builtin_offsetof(tk_state, prev_d1) == 44 && __builtin_offsetof(tk_state, pend) == 56 &&
                             __builtin_offsetof(tk_state, pend_val) == 60 && __builtin_offsetof(tk_state, env_w) == 76 &&
                             __builtin_offsetof(tk_state, env) == 88 && __builtin_offsetof(tk_fastio, mask) == 32 &&
                             __builtin_offsetof(tk_fastio, ev) == 36 && __builtin_offsetof(tk_fastio, newer) == 44)
                            ? 1 : -1];
#endif
#endif

int32_t tk_run(tk_state *s, int32_t frames)
{
#ifdef KF_ASM
    {
        tk_fastio io;
        int32_t left = frames;
        io.k[0] = 1.52587890625e-05f;
        io.k[1] = 0.5f;
        io.k[2] = 2.0f;
        io.k[3] = TK_SVF_NUM;
        io.k[4] = TK_SVF_INV_DEN;
        io.k[5] = TK_SVF_G;
        io.k[6] = 1.0f - TK_HYST;
        io.k[7] = 0.0f;
        io.mask = TK_MASK;
        while (left > 0 && s->i < s->n) {
            left -= tk_fast(s, left, &io);
            if (io.ev != 0) {   /* an event's frame: its tail, then on */
                tk_hit hit;
                int32_t got = 0;
                if (io.ev == 1) {
                    hit.older = io.older;
                    hit.newer = io.newer;
                    got = tk_peak(s, &hit);
                }
                tk_tail(s, &hit, got);
                s->i++;
                left--;
            }
        }
        if (s->i >= s->n && s->pend) tk_commit(s);
        return s->i >= s->n;
    }
#else
    /* the per-frame state in locals (the SHARC's compiler loads and stores a struct's field at each use: 357
       instructions a frame for some 40 operations, 2026-10-24); i and env_w written back each frame for what reads
       them (tk_peak, the hump below, tk_commit), the rest at the end. The same operations on the same values */
    int32_t left = frames;
    float h1 = s->h1, h2 = s->h2, h3 = s->h3, h4 = s->h4, s1 = s->s1, s2 = s->s2, d1 = s->prev_d1;
    int32_t i = s->i, ew = s->env_w;
    const int32_t n = s->n;
    const uintptr_t pl = s->pcm_l, pr = s->pcm_r;
    float *const env = s->env;
    while (left > 0 && i < n) {
        tk_hit hit;
        int32_t got = 0;
        const float x = (float)(TK_X16(pl, i) + TK_X16(pr, i)) * 1.52587890625e-05f;   /* / 65536 */
        {   /* the TKEO at lag 2, from the cubic read through lags 1..4 (DeluxeLine::ReadBSplineFull, t = 0) */
            const float x0 = h1, x1 = h2, x2 = h3, x3 = h4;
            const float c = (x2 - x0) * 0.5f;
            const float v = x1 - x2;
            const float w = c + v;
            const float a = w + v + (x3 - x1) * 0.5f;
            const float bneg = w + a;
            float tk = c * c + 2.0f * x1 * bneg;
            if (tk < 0.0f) tk = -tk;
            h4 = h3; h3 = h2; h2 = h1; h1 = x;
            /* the SVF lowpass (filter.h StateVariable, TPT) */
            const float hp = (tk - TK_SVF_NUM * s1 - s2) * TK_SVF_INV_DEN;
            const float bp = TK_SVF_G * hp + s1;
            const float lp = TK_SVF_G * bp + s2;
            s1 = TK_SVF_G * hp + bp;
            s2 = TK_SVF_G * bp + lp;
            ew = (ew + 1) & TK_MASK;
            env[ew] = lp;
            /* the envelope's derivative (along the delay axis) at lag 2; a peak where it turns from < 0 to >= 0 */
            const float newer = (env[(ew - 3) & TK_MASK] - env[(ew - 1) & TK_MASK]) * 0.5f;
            const float older = d1;
            d1 = newer;
            s->i = i;
            s->env_w = ew;
            if (older < 0.0f && newer >= 0.0f) {
                hit.older = older;
                hit.newer = newer;
                got = tk_peak(s, &hit);
            }
        }
#ifdef TK_TRACE   /* (kf/host/tkweb.c: the envelope and its average, for a picture; the other builds as they were) */
        s->h1 = h1; s->h2 = h2; s->h3 = h3; s->h4 = h4; s->s1 = s1; s->s2 = s2; s->prev_d1 = d1;
        TK_TRACE(s);
#endif
        tk_tail(s, &hit, got);
        i++;
        left--;
    }
    s->h1 = h1; s->h2 = h2; s->h3 = h3; s->h4 = h4; s->s1 = s1; s->s2 = s2; s->prev_d1 = d1;
    s->i = i;
    s->env_w = ew;
    if (s->i >= s->n && s->pend) tk_commit(s);
    return s->i >= s->n;
#endif
}

/* the grids' candidates: every peak above the floor, at least TK_GAP apart (of two closer, the higher),
   none in the sample's last TK_END_GAP (no slice of almost nothing at its end); with first (GRID and RAND,
   whose slice 1 is the start) none within TK_GAP of the start either. Their event indices, in time order,
   in scratch; their count */
static int32_t tk_cands(const tk_event *ev, int32_t count, int32_t n, int32_t first, int32_t *scratch)
{
    int32_t m = 0, k = 0, j = 0;
    float fl = 0.0f;
    while (k < count) {   /* (TK_REL_FLOOR: the floor a share of the highest peak) */
        if (ev[k].value > fl) fl = ev[k].value;
        k++;
    }
    fl = fl * TK_REL_FLOOR;
    if (fl < TK_FLOOR) fl = TK_FLOOR;
    k = 0;
    while (k < count) {
        if (ev[k].value > fl) {
            if (m > 0 && ev[k].pos - ev[scratch[m - 1]].pos < TK_GAP) {
                if (ev[k].value > ev[scratch[m - 1]].value) scratch[m - 1] = k;
            } else {
                scratch[m] = k;
                m++;
            }
        }
        k++;
    }
    k = 0;
    while (k < m) {
        const int32_t q = ev[scratch[k]].pos;
        if (q < n - TK_END_GAP && (!first || q >= TK_GAP)) {
            scratch[j] = scratch[k];
            j++;
        }
        k++;
    }
    return j;
}

/* out[0..got) into time order (insertion sort: few) */
static void tk_sort(int32_t *out, int32_t got)
{
    int32_t k = 1;
    while (k < got) {
        int32_t x = out[k];
        int32_t j = k - 1;
        while (j >= 0 && out[j] > x) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = x;
        k++;
    }
}

int32_t tk_grid_max(const tk_event *ev, int32_t count, int32_t n, int32_t mode, int32_t *scratch)
{
    const int32_t first = 1;   /* (slice 1 at the start in every mode: VOL's too, the owner 2026-10-22 (the tester's
                                  "the loop's beginning is missing"); THIN's VOL order counts the start as slice 1) */
    const int32_t m = tk_cands(ev, count, n, first, scratch);
    (void)mode;
    if (first) return m + 1;
    return m > 0 ? m : 1;
}

int32_t tk_grid_mode(const tk_state *s, const tk_event *ev, int32_t count, int32_t n, int32_t slices,
                     int32_t mode, uint32_t seed, int32_t *out, int32_t *scratch)
{
    const int32_t first = 1;   /* (slice 1 at the start in every mode: VOL's too, the owner 2026-10-22 (the tester's
                                  "the loop's beginning is missing"); THIN's VOL order counts the start as slice 1) */
    const int32_t m = tk_cands(ev, count, n, first, scratch);
    int32_t got = 0, k, j;
    (void)s;
    if (first || m == 0) {   /* slice 1 at the start */
        out[0] = 0;
        got = 1;
    }
    if (slices > got + m) slices = got + m;   /* no more slices than transients */
    if (mode == TK_EVEN) {
        /* GRID: targets at k n / slices, k = 1 .. slices - 1, each to the nearest peak not taken */
        const float step = (float)n * tk_recip((float)slices);
        k = 1;
        while (k < slices) {
            const int32_t t = (int32_t)(step * (float)k);
            int32_t best = -1, bd = 0x7fffffff;
            j = 0;
            while (j < m) {
                if (scratch[j] >= 0) {
                    int32_t d = ev[scratch[j]].pos - t;
                    if (d < 0) d = -d;
                    if (d < bd) {
                        bd = d;
                        best = j;
                    }
                }
                j++;
            }
            out[got] = ev[scratch[best]].pos;   /* (there is one: slices - 1 <= m) */
            scratch[best] = -1;
            got++;
            k++;
        }
    } else if (mode == TK_RAND) {
        /* RAND: slices - 1 of the peaks at random (Fisher-Yates; the candidates are TK_GAP apart already).
           A reroll for each seed */
        k = m - 1;
        while (k > 0) {
            int32_t r, x;
            seed = seed * 1664525u + 1013904223u;
            r = (int32_t)((float)(int32_t)(seed >> 8) * 5.9604645e-08f * (float)(k + 1));   /* [0, k] */
            if (r > k) r = k;
            x = scratch[k];
            scratch[k] = scratch[r];
            scratch[r] = x;
            k--;
        }
        j = 0;
        while (got < slices) {
            out[got] = ev[scratch[j]].pos;
            got++;
            j++;
        }
    } else {
        /* ENV: the slices highest (TKEO height), one by one: slices + 1 is these and the next highest */
        while (got < slices) {
            int32_t best = -1;
            j = 0;
            while (j < m) {
                if (scratch[j] >= 0 && (best < 0 || ev[scratch[j]].value > ev[scratch[best]].value)) best = j;
                j++;
            }
            out[got] = ev[scratch[best]].pos;
            scratch[best] = -1;
            got++;
        }
    }
    tk_sort(out, got);
    return got;
}

void tk_cache_fill(tk_cache *c, const tk_state *s, const tk_event *ev, int32_t count, int32_t n, uint32_t seed,
                   int32_t *scratch)
{
    int32_t m, k, j, keep;
    float floor_v = 0.0f;
    (void)s;
    m = tk_cands(ev, count, n, 1, scratch);
    /* more than TK_CP: the TK_CP highest (the TK_CP-th height found by repeated maxima would cost
       TK_CP passes: a threshold halved from the highest until at most TK_CP stand above it instead) */
    if (m > TK_CP) {
        float hi = 0.0f;
        k = 0;
        while (k < m) {
            if (ev[scratch[k]].value > hi) hi = ev[scratch[k]].value;
            k++;
        }
        floor_v = hi;
        keep = 0;
        while (keep < TK_CP && floor_v > TK_FLOOR) {
            floor_v = floor_v * 0.5f;
            keep = 0;
            k = 0;
            while (k < m) {
                if (ev[scratch[k]].value > floor_v) keep++;
                k++;
            }
        }
        if (keep > TK_CP) floor_v = floor_v * 2.0f;
    }
    c->m = 0;
    k = 0;
    while (k < m && c->m < TK_CP) {
        int32_t take = 0;
        if (ev[scratch[k]].pos >= TK_GAP && ev[scratch[k]].value > floor_v) take = 1;   /* (slice 1 is the start's) */
        if (ev[scratch[k]].pos >= n - TK_END_GAP) take = 0;                            /* (none at the very end) */
        if (take) {
            c->pos[c->m] = ev[scratch[k]].pos;
            c->val[c->m] = ev[scratch[k]].value;
            c->m++;
        }
        k++;
    }
    /* ENV's rank: by height, the higher first (ties: the earlier) */
    k = 0;
    while (k < c->m) {
        int32_t r = 0;
        j = 0;
        while (j < c->m) {
            if (c->val[j] > c->val[k] || (c->val[j] == c->val[k] && j < k)) r++;
            j++;
        }
        c->rank[k] = r;
        k++;
    }
    /* RAND's: a random order (Fisher-Yates over scratch), each taken unless within TK_GAP of one
       taken or of the start */
    k = 0;
    while (k < c->m) {
        scratch[k] = k;
        c->rrank[k] = TK_NONE;
        k++;
    }
    k = c->m - 1;
    while (k > 0) {
        int32_t r, x;
        seed = seed * 1664525u + 1013904223u;
        r = (int32_t)((float)(int32_t)(seed >> 8) * 5.9604645e-08f * (float)(k + 1));
        if (r > k) r = k;
        x = scratch[k];
        scratch[k] = scratch[r];
        scratch[r] = x;
        k--;
    }
    {
        int32_t taken = 0;
        k = 0;
        while (k < c->m) {
            const int32_t q = c->pos[scratch[k]];
            int32_t ok = q >= TK_GAP ? 1 : 0;
            j = 0;
            while (j < c->m && ok) {
                if (c->rrank[j] != TK_NONE) {
                    int32_t d = c->pos[j] - q;
                    if (d < 0) d = -d;
                    if (d < TK_GAP) ok = 0;
                }
                j++;
            }
            if (ok) {
                c->rrank[scratch[k]] = taken;
                taken++;
            }
            k++;
        }
        c->rtaken = taken;
    }
    c->n = n;
    c->gen++;
}

/* the peak nearest t not in used[0..nu) (pos in time order: a binary search, then outwards) */
static int32_t tk_nearest(const tk_cache *c, int32_t t, const int32_t *used, int32_t nu)
{
    int32_t lo = 0, hi = c->m, a, b, u;
    while (lo < hi) {
        const int32_t mid = (lo + hi) >> 1;
        if (c->pos[mid] < t) lo = mid + 1;
        else hi = mid;
    }
    a = lo - 1;
    b = lo;
    while (a >= 0 || b < c->m) {
        int32_t pick = -1;
        if (a < 0) pick = b;
        else if (b >= c->m) pick = a;
        else pick = (t - c->pos[a] <= c->pos[b] - t) ? a : b;
        int32_t hit = 0;
        u = 0;
        while (u < nu) {   /* (the exit by the bound, set here: selcc makes a hardware loop of a simpler form) */
            if (used[u] == pick) {
                hit = 1;
                u = nu;
            } else {
                u++;
            }
        }
        if (!hit) return pick;
        if (pick == a) a--;
        else b++;
    }
    return -1;
}

int32_t tk_cache_cuts(const tk_cache *c, int32_t n_slices, int32_t *scratch)
{
    int32_t *cuts = scratch;                     /* TK_SLICES + 1 */
    int32_t *used = scratch + TK_SLICES + 1;     /* TK_SLICES */
    int32_t got = 1, k, j, rev = 0;
    if (n_slices < 0) {
        rev = 1;
        n_slices = -n_slices;
    }
    if (n_slices < 1) n_slices = 1;
    if (n_slices > TK_SLICES) n_slices = TK_SLICES;
    cuts[0] = 0;
    if (c->mode == TK_EVEN) {   /* GRID: targets k n / N, each to the nearest peak not taken */
        const float step = (float)c->n * tk_recip((float)n_slices);
        k = 1;
        while (k < n_slices) {
            const int32_t t = (int32_t)(step * (float)k);
            const int32_t p = tk_nearest(c, t, used, got - 1);
            if (p < 0) break;
            used[got - 1] = p;
            cuts[got] = c->pos[p];
            got++;
            k++;
        }
        /* in time order (insertion sort: few) */
        k = 2;
        while (k < got) {
            const int32_t x = cuts[k];
            j = k - 1;
            while (j >= 1 && cuts[j] > x) {
                cuts[j + 1] = cuts[j];
                j--;
            }
            cuts[j + 1] = x;
            k++;
        }
    } else {   /* VOL, RAND: the peaks ranked below N - 1, in time order; reversed, the order's other end among
                  the grid's (s_: its transients ranked) */
        const int32_t *rk = (c->mode == TK_RAND) ? c->rrank : c->rank;
        int32_t s_ = (c->mode == TK_RAND) ? c->rtaken : c->m;
        if (s_ > c->slices) s_ = c->slices;
        k = 0;
        while (k < c->m && got < n_slices) {
            int32_t r = rk[k];
            if (rev) r = (r < s_) ? s_ - 1 - r : TK_NONE;
            if (r < n_slices - 1) {
                cuts[got] = c->pos[k];
                got++;
            }
            k++;
        }
    }
    return got;
}

void tk_cache_region(const tk_cache *c, int32_t n_slices, int32_t i, int32_t *start, int32_t *end,
                     int32_t *scratch)
{
    const int32_t *cuts = scratch;
    const int32_t got = tk_cache_cuts(c, n_slices, scratch);
    while (i >= got) i -= got;   /* (no % on the SHARC: no ___mod32 is linked) */
    while (i < 0) i += got;
    *start = cuts[i];
    *end = (i + 1 < got) ? cuts[i + 1] : c->n;
}

float tk_trn(const tk_cache *c, int32_t p, int32_t shape, int32_t *cursor)
{
    const int32_t m = c->m;
    int32_t k = *cursor, moving;
    float inv, sk, sn, t;
    if (m <= 0) return 0.0f;
    if (k < 0) k = 0;
    if (k > m - 1) k = m - 1;
    moving = 1;   /* (a flag, not a compound condition: selcc makes a hardware loop of the simpler form) */
    while (moving) {
        if (k + 1 < m && c->pos[k + 1] <= p) k++;
        else moving = 0;
    }
    moving = 1;
    while (moving) {
        if (k > 0 && c->pos[k] > p) k--;
        else moving = 0;
    }
    *cursor = k;
    inv = tk_recip((float)m);
    if (p < c->pos[0]) {   /* before the first transient: rising to it from nothing */
        if (shape != TK_CONTOUR) return 0.0f;
        sk = 0.0f;
        sn = 1.0f - (float)c->rank[0] * inv;
        t = (float)p * tk_recip((float)(c->pos[0] > 0 ? c->pos[0] : 1));
    } else {
        sk = 1.0f - (float)c->rank[k] * inv;
        if (shape != TK_CONTOUR) {   /* PULSE: the last transient's strength, decaying (squared) */
            float x = 1.0f - (float)(p - c->pos[k]) * 2.0833333e-04f;   /* (1 / TK_PULSE: no float division on the SHARC) */
            if (x < 0.0f) x = 0.0f;
            return sk * x * x;
        }
        if (k + 1 < m) {
            sn = 1.0f - (float)c->rank[k + 1] * inv;
            t = (float)(p - c->pos[k]) * tk_recip((float)(c->pos[k + 1] - c->pos[k]));
        } else {   /* after the last: falling to nothing at the sample's end */
            sn = 0.0f;
            t = (float)(p - c->pos[k]) * tk_recip((float)(c->n - c->pos[k] > 0 ? c->n - c->pos[k] : 1));
        }
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return sk + (sn - sk) * (t * t * (3.0f - 2.0f * t));
}

void tk_trn_parts(const tk_cache *c, int32_t p, tk_trnp *o)
{
    const int32_t m = c->m;
    int32_t k = o->cursor, moving;
    float inv, t;
    o->k = 0;
    o->sk = 0.0f;
    o->sn = 0.0f;
    o->t = 0.0f;
    if (m <= 0) return;
    if (k < 0) k = 0;
    if (k > m - 1) k = m - 1;
    moving = 1;   /* (tk_trn's walk) */
    while (moving) {
        if (k + 1 < m && c->pos[k + 1] <= p) k++;
        else moving = 0;
    }
    moving = 1;
    while (moving) {
        if (k > 0 && c->pos[k] > p) k--;
        else moving = 0;
    }
    o->cursor = k;
    inv = tk_recip((float)m);
    if (p < c->pos[0]) {   /* before the first transient: rising to it from nothing */
        o->sn = 1.0f - (float)c->rank[0] * inv;
        t = (float)p * tk_recip((float)(c->pos[0] > 0 ? c->pos[0] : 1));
    } else {
        o->k = k + 1;
        o->sk = 1.0f - (float)c->rank[k] * inv;
        if (k + 1 < m) {
            o->sn = 1.0f - (float)c->rank[k + 1] * inv;
            t = (float)(p - c->pos[k]) * tk_recip((float)(c->pos[k + 1] - c->pos[k]));
        } else {   /* after the last: falling to nothing at the sample's end */
            t = (float)(p - c->pos[k]) * tk_recip((float)(c->n - c->pos[k] > 0 ? c->n - c->pos[k] : 1));
        }
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    o->t = t;
    if (o->sk < 0.0f) o->sk = 0.0f;   /* (a transient without a rank: TK_NONE past m) */
    if (o->sn < 0.0f) o->sn = 0.0f;
}
