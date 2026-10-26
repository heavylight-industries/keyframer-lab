/*
 * kf.c -- the keyframe engine (see kf.h). GNU AGPL-3.0-or-later.
 *
 * selcc does not inline and a call costs it tens of instructions, so the
 * per-sample work (a head's step, its pointer's seek, the PCM read) is written
 * as macros inside kf_render's loop, with the heads in locals for the block; the
 * rare events (a splice, the reference crossing the region's edge) go through
 * functions on the voice. The analyzer slides its four samples along, one new
 * read a position.
 */
#include "kf.h"

/* host checks only (kf/host/kfborder.c): every sample's reads and every splice. Empty everywhere else */
#ifndef KF_HOOK_READ
#define KF_HOOK_READ(v_, play_, temp_, splicing_, t_)
#endif
#ifndef KF_HOOK_SPLICE
#define KF_HOOK_SPLICE(v_)
#endif

/* ---- the PCM ------------------------------------------------------------ */

/* sample r (wrapped), int16, to x_: the aligned word and a shift, as the SHARC
   build reads it (no 16-bit loads there); the buffer is readable to the next word */
#define KX(pcm_, r_, x_) do { \
        const uintptr_t a_ = (pcm_) + 2u * (uint32_t)(r_); \
        const uint32_t w_ = *(const uint32_t *)(a_ & ~(uintptr_t)3); \
        if ((a_ & 2u) != 0u) x_ = (int32_t)w_ >> 16; \
        else x_ = (int32_t)(w_ << 16) >> 16; \
    } while (0)

static int32_t kx(const kf_voice *v, int32_t r)
{
    int32_t x;
    KX(v->pcm, r, x);
    return x;
}

/* an unwrapped position into [0, n): positions stay within a few samples' or a
   leash's reach of the sample, so this loops at most a couple of times */
static int32_t kwrap(const kf_voice *v, int32_t r)
{
    while (r >= v->n) r -= v->n;
    while (r < 0) r += v->n;
    return r;
}

/* ---- the analyzer ------------------------------------------------------- */

/* The window: keyframe logical indices [base, base + count), each the sample
   index (unwrapped) of a keyframe, ascending; positions lo..hi analysed. Logical
   indices never change under growth or eviction, so the heads' pointers stay
   valid. A pointer below base or above the top reads as the window's edge. */
static int32_t kpos(const kf_voice *v, int32_t m)
{
    if (m < v->base) return v->lo;
    if (m >= v->base + v->count) return v->hi;
    return v->ring[m & v->mask];
}

/* a head's pointer to the last keyframe at or before it. A few steps from where it was; further (a splice's
   or a note's jump: hundreds of keyframes in dense material, each step a slow read on the DSP), a bisection
   of the rest, the ring ascending: the same keyframe either way */
#define KSEEK_WALK 4   /* (a probe this far, not a counted walk: selcc makes a counted loop a hardware loop) */
static void kbisect(const kf_voice *v, kf_head *h, int32_t a, int32_t b)   /* the last at or before ip in [a, b]: a's is */
{
    int32_t c;
    while (a < b) {
        c = a + ((b - a + 1) >> 1);
        if (v->ring[c & v->mask] <= h->ip) a = c;
        else b = c - 1;
    }
    h->m = a;
}
static void kseek(const kf_voice *v, kf_head *h)
{
    const int32_t top = v->base + v->count - 1;
    int32_t m = h->m;
    if (m > top) m = top;
    if (m < v->base - 1) m = v->base - 1;
    if (m + KSEEK_WALK <= top) {
        if (v->ring[(m + KSEEK_WALK) & v->mask] <= h->ip) { kbisect(v, h, m + KSEEK_WALK, top); return; }
    }
    if (m - KSEEK_WALK >= v->base) {   /* (base - 1: none, never read) */
        if (v->ring[(m - KSEEK_WALK) & v->mask] > h->ip) { kbisect(v, h, v->base - 1, m - KSEEK_WALK - 1); return; }
    }
    for (;;) {
        if (m >= top) break;
        if (v->ring[(m + 1) & v->mask] > h->ip) break;
        m++;
    }
    for (;;) {
        if (m < v->base) break;
        if (v->ring[m & v->mask] <= h->ip) break;
        m--;
    }
    h->m = m;
}

/* a fresh window at position u (wrapped r): one keyframe there, the anchor */
static void kreset(kf_voice *v, int32_t u, int32_t r)
{
    v->base = 0;
    v->count = 1;
    v->ring[0] = u;
    v->lo = u;
    v->hi = u;
    v->lo_r = r;
    v->hi_r = r;
    v->v_lo = kx(v, r);
    v->v_hi = v->v_lo;
    v->lo_stale = 0;
    v->hi_stale = 0;
    v->hc_tag = (int32_t)0x80000000u;   /* (no position: run.asm's sample cache empty) */
    v->ref.m = -1;
    v->play.m = -1;
    v->temp.m = -1;
    kseek(v, &v->ref);
    kseek(v, &v->play);
    kseek(v, &v->temp);
}

/* Is position c a keyframe? The derivative is the central difference
   d(u) = x(u+1) - x(u-1); an extremum lies where it changes sign between c-1 and
   c, snapped to c: d0 = x(c) - x(c-2), d1 = x(c+1) - x(c-1). Every input is the
   PCM around c, so the test is the same whichever direction the window grows --
   except the threshold, which compares x(c) with the outermost keyframe on the
   side being extended (vref). */
#define KTURN(xm2_, xm1_, x0_, xp1_, vref_, eps_, key_) do { \
        const int32_t d0_ = (x0_) - (xm2_); \
        const int32_t d1_ = (xp1_) - (xm1_); \
        int32_t t_ = 0; \
        if (d0_ > 0) { if (d1_ <= 0) t_ = 1; } \
        else if (d0_ < 0) { if (d1_ >= 0) t_ = 1; } \
        key_ = 0; \
        if (t_) { \
            int32_t df_ = (x0_) - (vref_); \
            if (df_ < 0) df_ = -df_; \
            if (df_ > (eps_)) key_ = 1; \
        } \
    } while (0)

/* Analyse up to hi_need on the right (kf_grow_hi), then down to lo_need on the
   left (kf_grow_lo). Full, a keyframe added on one side drops the outermost on
   the other. On the DSP these are kf/sharc/run.asm, which must match bit for bit. */
#ifdef KF_ASM
void kf_grow_hi(kf_voice *v, int32_t hi_need, int32_t eps);
void kf_grow_lo(kf_voice *v, int32_t lo_need, int32_t eps);
#else
static void kf_grow_hi(kf_voice *v, int32_t hi_need, int32_t eps)
{
    const uintptr_t pcm = v->pcm;
    const int32_t n = v->n;
    int32_t key;
    if (v->hi_stale) {   /* evicted at this end: its outermost keyframe now */
        v->hi_r = kwrap(v, v->hi);
        v->v_hi = kx(v, v->hi_r);
        v->hi_stale = 0;
    }
    int32_t c = v->hi + 1;
    int32_t rc = v->hi_r + 1;
    if (rc >= n) rc -= n;
    int32_t r, xm2, xm1, x0, xp1;
    r = rc - 2; if (r < 0) r += n; KX(pcm, r, xm2);
    r = rc - 1; if (r < 0) r += n; KX(pcm, r, xm1);
    KX(pcm, rc, x0);
    int32_t rp = rc + 1;
    if (rp >= n) rp -= n;
    KX(pcm, rp, xp1);
    for (;;) {
        KTURN(xm2, xm1, x0, xp1, v->v_hi, eps, key);
        if (key) {
            if (v->count >= v->mask - 1) {   /* full: two slots stay free (the DSP's sentinels, above the top and below the base) */
                v->base++;
                v->count--;
                v->lo = v->ring[v->base & v->mask];
                v->lo_stale = 1;
            }
            v->ring[(v->base + v->count) & v->mask] = c;
            v->count++;
            v->n_keys++;
            v->v_hi = x0;
        }
        if (c >= hi_need) break;
        c++;
        xm2 = xm1;
        xm1 = x0;
        x0 = xp1;
        rc = rp;
        rp = rc + 1;
        if (rp >= n) rp -= n;
        KX(pcm, rp, xp1);
    }
    v->hi = c;
    v->hi_r = rc;
}

static void kf_grow_lo(kf_voice *v, int32_t lo_need, int32_t eps)
{
    const uintptr_t pcm = v->pcm;
    const int32_t n = v->n;
    int32_t key;
    if (v->lo_stale) {   /* evicted at this end: its outermost keyframe now */
        v->lo_r = kwrap(v, v->lo);
        v->v_lo = kx(v, v->lo_r);
        v->lo_stale = 0;
    }
    int32_t c = v->lo - 1;
    int32_t rc = v->lo_r - 1;
    if (rc < 0) rc += n;
    int32_t r, xm2, xm1, x0, xp1;
    r = rc + 1; if (r >= n) r -= n; KX(pcm, r, xp1);
    KX(pcm, rc, x0);
    r = rc - 1; if (r < 0) r += n; KX(pcm, r, xm1);
    int32_t rm = rc - 2;
    if (rm < 0) rm += n;
    KX(pcm, rm, xm2);
    for (;;) {
        KTURN(xm2, xm1, x0, xp1, v->v_lo, eps, key);
        if (key) {
            if (v->count >= v->mask - 1) {   /* full: two slots stay free (the DSP's sentinels, above the top and below the base) */
                v->count--;
                v->hi = v->ring[(v->base + v->count - 1) & v->mask];
                v->hi_stale = 1;
            }
            v->base--;
            v->ring[v->base & v->mask] = c;
            v->count++;
            v->n_keys++;
            v->v_lo = x0;
        }
        if (c <= lo_need) break;
        c--;
        xp1 = x0;
        x0 = xm1;
        xm1 = xm2;
        rc = rc - 1;
        if (rc < 0) rc += n;
        rm = rc - 2;
        if (rm < 0) rm += n;
        KX(pcm, rm, xm2);
    }
    v->lo = c;
    v->lo_r = rc;
}
#endif

static void kcover(kf_voice *v, int32_t lo_need, int32_t hi_need, int32_t eps)
{
    if (v->hi < hi_need) kf_grow_hi(v, hi_need, eps);
    if (v->lo > lo_need) kf_grow_lo(v, lo_need, eps);
}

#ifdef KF_TEST_HOOKS   /* (kf/host/kftest.c) one position at a time */
static void kgrow_hi(kf_voice *v, int32_t eps) { kcover(v, v->lo, v->hi + 1, eps); }
static void kgrow_lo(kf_voice *v, int32_t eps) { kcover(v, v->lo - 1, v->hi, eps); }
#endif

/* ---- the heads ---------------------------------------------------------- */

/* a rate as fixed point: its floor, and the rest as a 32-bit fraction (taken at 31 bits through an
   int32: (rest * 2^31) is below 2^31, so exact on the SHARC as on the host) */
static void krate(float r, int32_t *wi, uint32_t *wf)
{
    int32_t k = (int32_t)r;
    if ((float)k > r) k--;
    *wi = k;
    *wf = (uint32_t)(int32_t)((r - (float)k) * 2147483648.0f) << 1;
}

/* a head on by rate (ri_: its whole part, rf_: its fraction): the fraction's carry and the whole
   part to ip (and ri, wrapped) */
#define KSTEP(h_, ri_, rf_, n_) do { \
        const uint32_t f_ = (h_).fq + (rf_); \
        int32_t k_ = (ri_); \
        if (f_ < (h_).fq) k_++; \
        (h_).fq = f_; \
        if (k_ != 0) { \
            (h_).ip += k_; \
            int32_t r_ = (h_).ri + k_; \
            while (r_ >= (n_)) r_ -= (n_); \
            while (r_ < 0) r_ += (n_); \
            (h_).ri = r_; \
        } \
    } while (0)

/* its pointer, the window unchanged since it was last right (kseek without the clamp) */
#define KSEEK(h_, ring_, mask_, base_, top_) do { \
        int32_t m_ = (h_).m; \
        for (;;) { \
            if (m_ >= (top_)) break; \
            if ((ring_)[(m_ + 1) & (mask_)] > (h_).ip) break; \
            m_++; \
        } \
        for (;;) { \
            if (m_ < (base_)) break; \
            if ((ring_)[m_ & (mask_)] <= (h_).ip) break; \
            m_--; \
        } \
        (h_).m = m_; \
    } while (0)

/* a head's fraction as a float: its top 24 bits, exactly */
#define KFP(h_) ((float)((h_).fq >> 8) * 5.9604644775390625e-08f)

/* linear interpolation at a head, times gain, to y_ */
#define KREAD(h_, pcm_, n_, gain_, y_) do { \
        int32_t r1_ = (h_).ri + 1; \
        if (r1_ >= (n_)) r1_ -= (n_); \
        int32_t xa_, xb_; \
        KX(pcm_, (h_).ri, xa_); \
        KX(pcm_, r1_, xb_); \
        const float a_ = (float)xa_; \
        const float b_ = (float)xb_; \
        y_ = (a_ + KFP(h_) * (b_ - a_)) * (gain_); \
    } while (0)

/* 1/d for a whole d >= 1, by multiplications only: the SHARC's division is a
   reciprocal seed and Newton steps that do not always round as the host's
   does, so the engine never divides. The seed is 2^-(bits of d), within a
   factor of two; five Newton steps reach full precision. The same operations
   in the same order on both, so the same result. */
static float krecip(int32_t d)
{
    float r = 1.0f;
    int32_t b = d;
    while (b > 0) { r *= 0.5f; b >>= 1; }
    const float x = (float)d;
    /* five Newton steps, written out: a constant-count loop becomes a SHARC hardware
       loop under selcc, and the engine runs none (tools/build_dsp.sh checks; the loop
       stack is the firmware's, six deep) */
    r = r * (2.0f - x * r);
    r = r * (2.0f - x * r);
    r = r * (2.0f - x * r);
    r = r * (2.0f - x * r);
    r = r * (2.0f - x * r);
    return r;
}

static int32_t kcross(const kf_voice *v, const kf_params *p);
static void kborder(kf_voice *v, const kf_params *p, int32_t eps);

/* the transient ahead of the reference in its direction (kf.h: r_te forward, r_ts backwards; none: out of
   reach): a binary search of tp (shifts, no division) */
static void ktfind(kf_voice *v)
{
    int32_t lo = 0, hi = v->tn;
    const int32_t x = v->ref.ip;
    v->r_te = 0x7fffffff;
    v->r_ts = (int32_t)0x80000000;
    if (hi <= 0) return;
    while (lo < hi) {   /* lo: the first transient after x */
        const int32_t mid = (lo + hi) >> 1;
        if (v->tp[mid] <= x) lo = mid + 1;
        else hi = mid;
    }
    /* (inside the region only: past its edge the reference wraps or the note ends. And none closer to the one
       before it, or to the region's edge, than a border needs (two border fades): a flam, or the note's own
       first hit, is run through, not looped over) */
    if (v->r_tau > 0.0f) {
        int32_t k = lo;
        while (k < v->tn) {
            const int32_t before = k > 0 ? v->tp[k - 1] : v->r_s;
            if (v->tp[k] - before >= KF_SPARSE_MIN) break;
            k++;
        }
        if (k < v->tn) { if (v->tp[k] < v->r_e) v->r_te = v->tp[k]; }
    }
    if (v->r_tau < 0.0f) {
        int32_t k = lo - 1;
        while (k >= 0) {
            const int32_t after = k + 1 < v->tn ? v->tp[k + 1] : v->r_e;
            if (after - v->tp[k] >= KF_SPARSE_MIN) break;
            k--;
        }
        if (k >= 0) { if (v->tp[k] > v->r_s) v->r_ts = v->tp[k]; }
    }
    /* the attack behind the reference, the other field (the last at or before it, the reference forwards; the first
       after it, backwards), inside the region: a play head running against the reference borders there (kedge_e,
       kedge_s; run.asm's block reads the same fields). Never a crossing: the reference runs away from it */
    if (v->r_tau > 0.0f) { if (lo > 0) { if (v->tp[lo - 1] > v->r_s) v->r_ts = v->tp[lo - 1]; } }
    if (v->r_tau < 0.0f) { if (lo < v->tn) { if (v->tp[lo] < v->r_e) v->r_te = v->tp[lo]; } }
}

/* the last transient at or before x (forward), or the first at or after it (backwards: back < 0), inside the
   region; none: the region's edge it runs from. The border's grain never reaches back past it: it would play
   that attack again (two hits closer than the grain: a flam) */
static int32_t kprev(const kf_voice *v, int32_t x, int32_t back)
{
    int32_t lo = 0, hi = v->tn;
    while (lo < hi) {   /* lo: the first transient after x */
        const int32_t mid = (lo + hi) >> 1;
        if (v->tp[mid] <= x) lo = mid + 1;
        else hi = mid;
    }
    if (back < 0) {
        if (lo > 0) { if (v->tp[lo - 1] == x) lo--; }
        if (lo < v->tn) { if (v->tp[lo] < v->r_e) return v->tp[lo]; }
        return v->r_e - 1;
    }
    if (lo > 0) { if (v->tp[lo - 1] > v->r_s) return v->tp[lo - 1]; }
    return v->r_s;
}

/* the border fade for this block (the owner, 2026-10-25: before a transient the grains shrank towards a fixed 5 ms
   fade whatever SIZE, a squeak under every large grain): half SIZE's grain where the reference is, from the
   keyframes: K of them at their mean spacing sp over the window, or in sparse material (sp past c) the sparse
   leash's K c^2 / sp. At least KF_BORDER_FADE (a small SIZE as before), at most KF_BORDER_FADE_MAX, and at most a
   quarter of the way from the last attack to the border: its grain (two fades) never reaches back over that
   attack (it would play it again) */
static int32_t kbfade(const kf_voice *v, const kf_params *p, int32_t be, int32_t bs)
{
    float g = (float)KF_BORDER_FADE * 2.0f;
    int32_t f;
    int32_t span = 0x7fffffff;
    if (v->count >= 2) {
        const float sp = (float)(kpos(v, v->base + v->count - 1) - kpos(v, v->base)) * krecip((float)(v->count - 1));
        const float c = (float)KF_DIST_PER_K;
        if (sp > 0.0f) g = (float)p->k * (sp < c ? sp : c * c * krecip(sp));
    }
    f = g > (float)(2 * KF_BORDER_FADE_MAX) ? KF_BORDER_FADE_MAX : (int32_t)(g * 0.5f);
    if (p->pitch > 0.0f && be != 0x7fffffff) span = be - kprev(v, v->ref.ip, 1);
    if (p->pitch < 0.0f && bs != (int32_t)0x80000000) span = kprev(v, v->ref.ip, -1) - bs;
    if (f > (span >> 2)) f = span >> 2;
    if (f < KF_BORDER_FADE) f = KF_BORDER_FADE;
    return f;
}

/* the reference has crossed the transient ahead of it (last block: kf_render checks at a block's start, so up
   to a block late): the play head onto it, a short crossfade, unless it is there already. Ahead (it waited at
   the border) or behind (it would skip the attack), the attack plays once, from its start */
static void ktjump(kf_voice *v, int32_t t_at, int32_t eps)
{
    /* (one crossfade before the cut, in the play head's direction: the fade is over in the quiet before the
       attack, which plays whole, the incoming head running into it) */
    const int32_t at = v->r_sig < 0.0f ? t_at + KF_TRANS_FADE : t_at - KF_TRANS_FADE;
    int32_t d = v->play.ip - at;
    if (d < 0) d = -d;
    if (d < KF_TRANS_TOL) return;
    if (v->splicing) v->play = v->temp;   /* (a crossfade under way: its incoming head is the one to leave) */
    if (at < v->lo) kcover(v, at, v->hi, eps);
    if (at > v->hi) kcover(v, v->lo, at, eps);
    v->temp.ip = at;
    v->temp.fq = 0u;
    v->temp.ri = kwrap(v, at);
    v->temp.m = v->play.m;
    kseek(v, &v->temp);
    kseek(v, &v->play);
    kseek(v, &v->ref);
    v->t = 0.0f;
    v->inc = krecip(KF_TRANS_FADE);
    v->splicing = 1;
    v->force = 0;
    v->n_splice++;
    v->n_tjump++;
    v->ahold = KF_ATTACK_HOLD;
    v->sum_fade += (float)KF_TRANS_FADE;
    KF_HOOK_SPLICE(v);
}

/* a splice: the temp head at the reference, the crossfade over the uniform span
   between the play head's keyframe and the reference's (forced: a short one). In a
   play-once note, one whose fade would carry the play head past the edge it reaches
   first, or past the border's start, is a border splice instead */
static void ksplice(kf_voice *v, const kf_params *p, int32_t eps)
{
    int32_t len;
    if (v->force) {
        len = KF_WRAP_FADE;
        v->n_forced++;
    } else {
        int32_t d = kpos(v, v->play.m) - kpos(v, v->ref.m);
        if (d < 0) d = -d;
        if (p->max_fade > 0) {
            if (d > p->max_fade) d = p->max_fade;
        }
        {   /* the incoming head starts at the reference and moves at the pitch rate, the reference at the
               stretch rate: over the fade (len samples of head travel, len / |pitch| out) they part by
               |pitch - stretch| / |pitch| x len. More than d (the play head running back as the reference runs on:
               reverse loop) and each splice ended further apart than it began, doubling to the window's edge
               (Keyframer Lab's trace, 2026-10-25). Capped there (d |pitch| / |pitch - stretch|) the gap only held,
               and still ran away at other speeds (TUNE -12, +7): the fade at most half that, so a splice ends with
               the heads half as far apart as it began, as a forward leash splice's do (the owner, 2026-10-26) */
            const float sa = p->pitch < 0.0f ? -p->pitch : p->pitch;
            float rel = p->pitch - p->stretch;
            if (rel < 0.0f) rel = -rel;
            if (rel > sa) {
                const float cap = (float)d * sa * krecip(rel) * 0.5f;
                if ((float)d > cap) d = (int32_t)cap;
            }
        }
        if (d < KF_MIN_FADE) d = KF_MIN_FADE;
        len = d;
    }
    {
        int32_t over = 0;
        if (v->r_eb < 0x7fffffff) {   /* (either head past the border's start by the fade's end: the border
                                         would then be noticed too late to stop short of the edge) */
            if (v->play.ip + len > v->r_eb) over = 1;
            if (v->ref.ip + len > v->r_eb) over = 1;
        }
        if (v->r_sb > (int32_t)0x80000000) {
            if (v->play.ip - len < v->r_sb) over = 1;
            if (v->ref.ip - len < v->r_sb) over = 1;
        }
        if (over) {
            int32_t b = kcross(v, p);
            if (!v->force) b = 1;   /* (not crossing: the reference ends the note before the play head
                                       gets there, so the outgoing head never reads past it either) */
            if (b) {
                if (v->force) v->n_forced--;   /* (counted above: it is a border splice now) */
                v->force = 0;
                kborder(v, p, eps);
                return;
            }
        }
    }
    {   /* (nor a leash or forced splice's: the wrap's forced fade carried a play head waiting at the edge past it) */
        int32_t room = len;
        if (p->pitch > 0.0f) room = p->e - 1 - v->play.ip;
        if (p->pitch < 0.0f) room = v->play.ip - p->s;
        if (room < KF_MIN_FADE) room = KF_MIN_FADE;   /* (at least the shortest crossfade; plain statements: selcc
                                                         mis-compiles some compound conditions) */
        if (len > room) len = room;
    }
    v->temp = v->ref;
    v->t = 0.0f;
    v->inc = krecip(len);
    v->splicing = 1;
    v->force = 0;
    v->n_splice++;
    v->sum_fade += (float)len;
    KF_HOOK_SPLICE(v);
}

/* a play-once note: does the play head reach the edge it runs to (e forward, s
   backwards) before the reference would end the note there? (The heads' distances
   to it against their rates, multiplied out: no division.) */
/* the border the play head runs to: a play-once note's edge, or the transient ahead of the reference when
   that is nearer (kf.h: r_te, r_ts) */
/* A play head running against the reference (PLAY's reversed modes with the song forwards, or the other way round:
   kf_dt2 flips one head; the owner, 2026-10-26: in reverse loop the play head strayed far from the reference and ran
   through the transients). It runs back from the reference, so the transient border it reaches is the attack the
   reference last crossed (behind it: ktfind keeps it in the field the reference's own direction leaves unused), not
   the one ahead. And in a loop the region's edge is none: the reference wraps at the far end, so a play head held at
   this one waited a whole lap while the gap grew (border splices come before the leash); past it the reads wrap
   (kwrap) and the leash brings the head back. Played once, the edge stays (the play head never reads past it).
   run.asm's block start makes the same borders */
static int32_t kedge_e(const kf_voice *v, const kf_params *p)
{
    int32_t e = p->e;   /* (the region's edge, played once or looped: the play head never reads past it) */
    int32_t against = 0;
    if (p->pitch > 0.0f) { if (p->stretch < 0.0f) against = 1; }
    if (v->once & 1) against = 0;
    if (against) e = 0x7fffffff;
    if (v->r_te < e) e = v->r_te;
    return e;
}
static int32_t kedge_s(const kf_voice *v, const kf_params *p)
{
    int32_t s = p->s;
    int32_t against = 0;
    if (p->pitch < 0.0f) { if (p->stretch > 0.0f) against = 1; }
    if (v->once & 1) against = 0;
    if (against) s = (int32_t)0x80000000;
    if (v->r_ts > s) s = v->r_ts;
    return s;
}

static int32_t kcross(const kf_voice *v, const kf_params *p)
{
    const float sig = p->pitch, tau = p->stretch;
    if (sig > 0.0f) {
        if (tau <= 0.0f) return 1;   /* (the reference runs the other way: it never ends the note here) */
        const int32_t e = kedge_e(v, p);
        const float dp = (float)(e - 1 - v->play.ip), dr = (float)(e - 1 - v->ref.ip);
        return dp * tau < dr * sig;
    }
    if (sig < 0.0f) {
        if (tau >= 0.0f) return 1;
        const int32_t s = kedge_s(v, p);
        const float dp = (float)(v->play.ip - s), dr = (float)(v->ref.ip - s);
        return dp * -tau < dr * -sig;
    }
    return 0;
}

/* a border splice: the play head at its edge less a border fade, ahead of the
   reference. The temp head starts at the reference, as a leash splice's does, or,
   when the reference is already within two border fades of the edge, two border
   fades before it: never further behind the reference than that, so nothing the
   note has left behind plays again (a longer grain here, up to K keyframes, sent
   the play head back by as much as the cap and replayed a fading tail's louder
   part: the owner's clap, KF17), and the play head never comes back to the edge
   in less than a fade (no grains shrinking towards it). The fade is
   r_bfr - 2 samples of travel (kbfade), so the outgoing head stops short of the edge */
static void kborder(kf_voice *v, const kf_params *p, int32_t eps)
{
    const int32_t g = 2 * v->r_bfr;
    int32_t u = v->ref.ip;
    if (p->pitch > 0.0f) {
        const int32_t e = kedge_e(v, p);
        if (u > e - g) u = e - g;
        if (u < p->s) u = p->s;
        if (v->tn > 0) {   /* (not back past the attack the reference last crossed) */
            const int32_t a = kprev(v, v->ref.ip, 1);
            if (u < a) u = a;
        }
        if (u < v->lo) kcover(v, u, v->hi, eps);
    } else {
        const int32_t s = kedge_s(v, p);
        if (u < s + g) u = s + g;
        if (u > p->e - 1) u = p->e - 1;
        if (v->tn > 0) {
            const int32_t a = kprev(v, v->ref.ip, -1);
            if (u > a) u = a;
        }
        if (u > v->hi) kcover(v, v->lo, u, eps);
    }
    if (u == v->ref.ip) {
        v->temp = v->ref;
    } else {
        v->temp.ip = u;
        v->temp.fq = 0u;
        v->temp.ri = kwrap(v, u);
        v->temp.m = v->play.m;
    }
    kseek(v, &v->temp);
    kseek(v, &v->play);
    kseek(v, &v->ref);
    /* the fade's travel capped at the outgoing head's room before the edge: found up to a block late, or with this
       block's longer fade, it would carry the play head past it; at least the shortest crossfade (48 samples). One
       declarator a line, no block: selcc stored only the last of "int32_t f = .., room = f;" (f left unset: a fade
       of 1 / 0, a splice every sample) and the statistic below as an integer (the emulator, 2026-10-25) */
    int32_t f = v->r_bfr - 2;
    int32_t room = f;
    if (p->pitch > 0.0f) room = kedge_e(v, p) - 1 - v->play.ip;
    if (p->pitch < 0.0f) room = v->play.ip - kedge_s(v, p);
    if (room < KF_MIN_FADE) room = KF_MIN_FADE;
    if (f > room) f = room;
    v->t = 0.0f;
    v->inc = krecip(f);
    v->sum_fade += (float)f;
    v->splicing = 1;
    v->force = 0;
    v->n_splice++;
    v->n_border++;
    KF_HOOK_SPLICE(v);
}

/* the reference has crossed the region's edge: on by the region's length, a
   short crossfade forced, and a fresh window if it landed outside this one */
static void kwrapref(kf_voice *v, const kf_params *p, int32_t reach, int32_t eps)
{
    const int32_t len = p->e - p->s;
    while (v->ref.ip >= p->e) v->ref.ip -= len;
    while (v->ref.ip < p->s) v->ref.ip += len;
    v->ref.ri = v->ref.ip;   /* (the region lies inside the sample) */
    v->force = 1;
    int32_t outside = 0;
    if (v->ref.ip < v->lo) outside = 1;
    if (v->ref.ip > v->hi) outside = 1;
    if (outside) {
        kreset(v, v->ref.ip, v->ref.ri);
        kcover(v, v->ref.ip - reach, v->ref.ip + reach, eps);
    }
    kseek(v, &v->ref);
    ktfind(v);   /* (the transients ahead of it, from its new place) */
    if (v->once & 2) {   /* a cut, not a crossfade (the glue has faded the output to silence here: SLICE,
                            whose next slice starts on a transient a crossfade would play) */
        v->play = v->ref;
        v->splicing = 0;
        v->force = 0;
    }
}

/* One sample with whatever event it brings (a splice's start, the play head
   outside the window, the reference crossing the region's edge): the reference
   semantics, which kf_run follows between events. */
static void kone(kf_voice *v, const kf_params *p, int32_t reach, int32_t eps, float *y_out)
{
    const float sig = p->pitch;   /* (the steps: v's fixed-point rates, krate of this) */
    const float sig_abs = sig < 0.0f ? -sig : sig;
    const float gain = p->gain;
    const uintptr_t pcm = v->pcm;
    const int32_t n = v->n;
    if (!v->splicing) {
        int32_t go = v->force;
        if (v->play.ip < v->lo) { go = 1; v->force = 1; }   /* the play head outside the window: no distance */
        if (v->play.ip > v->hi) { go = 1; v->force = 1; }
        int32_t hold = 0;   /* the end zone: a play head behind the reference runs on */
        if (v->ref.ip >= v->r_ze) { if (v->play.ip < v->ref.ip) hold = 1; }
        if (v->ref.ip <= v->r_zs) { if (v->play.ip > v->ref.ip) hold = 1; }
        if (!hold) {
            int32_t d = v->play.m - v->ref.m;
            if (d < 0) d = -d;
            if (d > p->k) go = 1;
            int32_t ds = v->play.ip - v->ref.ip;   /* the sparse leash: distance^2 > K c^2 keyframes */
            if (ds < 0) ds = -ds;
            if (v->r_d > 0) {
                if (ds > KF_SPARSE_MIN) {
                    const float f = (float)ds;
                    const float kd = (float)(d < 1 ? 1 : d);
                    if (f * f > v->r_kc2 * kd) go = 1;
                }
            }
        }
        int32_t border = 0;
        if (v->play.ip >= v->r_eb) border = 1;
        if (v->play.ip <= v->r_sb) border = 1;
        if (border) border = kcross(v, p);
        if (border) kborder(v, p, eps);
        else if (go) ksplice(v, p, eps);
    }
    float y;
    if (v->splicing) {
        float a, b;
        KREAD(v->temp, pcm, n, gain, a);
        KREAD(v->play, pcm, n, gain, b);
        y = b + v->t * (a - b);
        KF_HOOK_READ(v, v->play, v->temp, 1, v->t);
        KSTEP(v->play, v->r_sig_i, v->r_sig_f, n);   /* (the outgoing head needs no pointer) */
        KSTEP(v->temp, v->r_sig_i, v->r_sig_f, n);
        kseek(v, &v->temp);
        v->t += v->inc * sig_abs;
        if (v->t >= 1.0f) {
            v->play = v->temp;
            v->splicing = 0;
        }
    } else {
        KREAD(v->play, pcm, n, gain, y);
        KF_HOOK_READ(v, v->play, v->temp, 0, 0.0f);
        KSTEP(v->play, v->r_sig_i, v->r_sig_f, n);
        kseek(v, &v->play);
    }
    KSTEP(v->ref, v->r_tau_i, v->r_tau_f, n);
    {
        int32_t edge = 0;
        if (v->ref.ip >= p->e) edge = 1;
        if (v->ref.ip < p->s) edge = 1;
        if (edge) {
            if (v->once & 1) {
                if (!v->ended) v->ended = 1;   /* play once: the note's end (kf_render notes where) */
            }
            kwrapref(v, p, reach, eps);
        }
        else kseek(v, &v->ref);
    }
    *y_out = y;
}

/* The samples before the next event, at most count (kone's semantics, the event
   checks hoisted): stops before a sample that would start a splice (forced, the
   play head outside the window, or the leash) or move the reference across the
   region's edge. Within a run the window does not change and the heads stay in
   it, except a play head that a splice's end lands outside it (checked then).
   On the DSP this is kf/sharc/run.asm, which must match it bit for bit. */
#ifdef KF_ASM
int32_t kf_run(kf_voice *v, float *out, int32_t count);
#else
static int32_t kf_run(kf_voice *v, float *out, int32_t count)
{
    const float sig = v->r_sig, gain = v->r_gain;
    const float sig_abs = sig < 0.0f ? -sig : sig;
    const int32_t si = v->r_sig_i, ti = v->r_tau_i;
    const uint32_t sf = v->r_sig_f, tf = v->r_tau_f;
    const int32_t k = v->r_k, s = v->r_s, e = v->r_e;
    const uintptr_t pcm = v->pcm;
    const int32_t n = v->n;
    int32_t *ring = v->ring;
    const int32_t mask = v->mask, base = v->base, top = v->base + v->count - 1;
    const int32_t lo = v->lo, hi = v->hi;
    kf_head ref = v->ref, play = v->play, temp = v->temp;
    int32_t splicing = v->splicing;
    float t = v->t;
    const float dt = v->inc * sig_abs;
    const int32_t dd = v->r_d, eb = v->r_eb, sb = v->r_sb, ze = v->r_ze, zs = v->r_zs;
    const float kc2 = v->r_kc2;
    int32_t check = 1;   /* the play head's place in the window, to check before its next sample */
    int32_t i;
    for (i = 0; i < count; i++) {
        if (!splicing) {
            if (v->force) break;
            if (check) {
                if (play.ip < lo) break;
                if (play.ip > hi) break;
                check = 0;
            }
            int32_t hold = 0;
            if (ref.ip >= ze) { if (play.ip < ref.ip) hold = 1; }
            if (ref.ip <= zs) { if (play.ip > ref.ip) hold = 1; }
            if (!hold) {
                int32_t d = play.m - ref.m;
                if (d < 0) d = -d;
                if (d > k) break;
                int32_t ds = play.ip - ref.ip;
                if (ds < 0) ds = -ds;
                if (dd > 0) {
                    if (ds > KF_SPARSE_MIN) {
                        const float f = (float)ds;
                        const float kd = (float)(d < 1 ? 1 : d);
                        if (f * f > kc2 * kd) break;
                    }
                }
            }
            if (play.ip >= eb) break;
            if (play.ip <= sb) break;
        }
        kf_head nref = ref;
        KSTEP(nref, ti, tf, n);
        if (nref.ip >= e) break;
        if (nref.ip < s) break;
        float y;
        if (splicing) {
            float a, b;
            KREAD(temp, pcm, n, gain, a);
            KREAD(play, pcm, n, gain, b);
            y = b + t * (a - b);
            KF_HOOK_READ(v, play, temp, 1, t);
            KSTEP(play, si, sf, n);
            KSTEP(temp, si, sf, n);
            KSEEK(temp, ring, mask, base, top);
            t += dt;
            if (t >= 1.0f) {
                play = temp;
                splicing = 0;
                check = 1;
            }
        } else {
            KREAD(play, pcm, n, gain, y);
            KF_HOOK_READ(v, play, temp, 0, 0.0f);
            KSTEP(play, si, sf, n);
            KSEEK(play, ring, mask, base, top);
        }
        ref = nref;
        KSEEK(ref, ring, mask, base, top);
        out[i] = y;
    }
    v->ref = ref;
    v->play = play;
    v->temp = temp;
    v->splicing = splicing;
    v->t = t;
    return i;
}
#endif

/* ---- the interface ------------------------------------------------------ */

void kf_init(kf_voice *v, int32_t *ring, int32_t ring_size)
{
    uint8_t *b = (uint8_t *)v;
    uint32_t i;
    volatile uint32_t size = (uint32_t)sizeof *v;   /* (not a constant: no hardware loop, as krecip) */
    const uint32_t nb = size;
    for (i = 0; i < nb; i++) b[i] = 0;
    v->ring = ring;
    v->mask = ring_size - 1;
    v->hc_tag = (int32_t)0x80000000u;
}

void kf_trig(kf_voice *v, uintptr_t pcm, int32_t n, int32_t pos)
{
    v->pcm = pcm;
    v->n = n;
    if (n < 4) return;
    const int32_t r = kwrap(v, pos);
    v->ref.ip = r;
    v->ref.fq = 0u;
    v->ref.ri = r;
    v->play = v->ref;
    v->temp = v->ref;
    v->splicing = 0;
    v->force = 0;
    v->t = 0.0f;
    v->hold = 0;
    v->ended = 0;
    v->ended_at = 0;
    v->ahold = 0;
    v->r_bfr = KF_BORDER_FADE + 2;
    v->bf_k = 0;   /* (a note: its border fade made at its first block) */
    v->r_te = 0x7fffffff;   /* (kf_render finds them: no crossing at a note's start) */
    v->r_ts = (int32_t)0x80000000;
    kreset(v, r, r);
}

void kf_render(kf_voice *v, const kf_params *p_in, float *out, int32_t count)
{
    int32_t i;
    if (v->n < 4) {
        for (i = 0; i < count; i++) out[i] = 0.0f;
        return;
    }
    /* the parameters, kept inside the sample */
    kf_params pp = *p_in;
    kf_params *p = &pp;
    if (p->s < 0) p->s = 0;
    if (p->s > v->n - 2) p->s = v->n - 2;
    if (p->e > v->n) p->e = v->n;
    if (p->e < p->s + 2) p->e = p->s + 2;
    if (p->k < 1) p->k = 1;
    const int32_t eps = p->eps;
    const float sig = p->pitch;
    const float tau = p->stretch;
    const float gain = p->gain;
    const float sig_abs = sig < 0.0f ? -sig : sig;
    float rate = tau < 0.0f ? -tau : tau;
    if (sig_abs > rate) rate = sig_abs;
    const int32_t reach = (int32_t)(rate * (float)count) + 3;   /* how far a head can move this block */

    /* the reference into the region (the region may have moved): to the edge it
       runs from, by a forced crossfade, at most once every KF_DRAG_HOLD samples;
       in between, held at the nearer edge (only the leash splices) */
    int32_t moved = 0;   /* (the reference put somewhere: no transient crossed) */
    {
        int32_t out_of = 0;
        if (v->ref.ip < p->s) out_of = 1;
        if (v->ref.ip >= p->e) out_of = 1;
        if (out_of) {
            moved = 1;
            if (v->hold <= 0) {
                v->ref.ip = (tau < 0.0f) ? p->e - 1 : p->s;
                v->force = 1;
                v->hold = KF_DRAG_HOLD;
            } else {
                v->ref.ip = (v->ref.ip < p->s) ? p->s : p->e - 1;
            }
            v->ref.ri = v->ref.ip;
        }
        if (v->hold > 0) v->hold -= count;
    }
    /* the window: a reference far outside it (a jump) starts a fresh one */
    {
        int32_t far = 0;
        if (v->ref.ip < v->lo - 2 * reach) far = 1;
        if (v->ref.ip > v->hi + 2 * reach) far = 1;
        if (far) { kreset(v, v->ref.ip, v->ref.ri); moved = 1; }
    }
    /* covered: the reference, and the head it is measured against unless that
       one has jumped away (it then splices, below) */
    {
        int32_t lo_need = v->ref.ip - reach;
        int32_t hi_need = v->ref.ip + reach;
        kf_head *o = v->splicing ? &v->temp : &v->play;
        int32_t near = 1;
        if (o->ip < v->lo - 2 * reach) near = 0;
        if (o->ip > v->hi + 2 * reach) near = 0;
        if (near) {
            if (o->ip - reach < lo_need) lo_need = o->ip - reach;
            if (o->ip + reach > hi_need) hi_need = o->ip + reach;
        }
        kcover(v, lo_need, hi_need, eps);
        kseek(v, &v->ref);
        kseek(v, o);
    }

    /* the block: runs between events by kf_run (kf/sharc/run.asm on the DSP), each
       event's sample by kone */
    v->r_sig = sig;
    v->r_tau = tau;
    krate(sig, &v->r_sig_i, &v->r_sig_f);
    krate(tau, &v->r_tau_i, &v->r_tau_f);
    v->r_gain = gain;
    v->r_k = p->k;
    v->r_s = p->s;
    v->r_e = p->e;
    v->r_d = p->k * KF_DIST_PER_K;
    v->r_kc2 = (float)p->k * (float)(KF_DIST_PER_K * KF_DIST_PER_K);
    v->r_eb = 0x7fffffff;
    v->r_sb = (int32_t)0x80000000;
    v->r_ze = 0x7fffffff;
    v->r_zs = (int32_t)0x80000000;
    /* the transient the reference crossed last block: the play head onto it (not after a jump of the
       reference's own); then the one ahead now */
    if (!moved) {
        int32_t crossed = 0;
        if (v->ref.ip >= v->r_te) { ktjump(v, v->r_te, eps); crossed = 1; }
        else if (v->ref.ip < v->r_ts) { ktjump(v, v->r_ts, eps); crossed = 1; }
        if (crossed) v->ahold = KF_ATTACK_HOLD;   /* (jumped or already there: the attack plays, no leash) */
    }
    ktfind(v);
    {   /* the borders: a play-once note's edge, or the transient ahead when nearer (kedge_e, kedge_s) */
        const int32_t be = kedge_e(v, p), bs = kedge_s(v, p);
        int32_t z = v->r_d;   /* the end zone: a grain at most (kborder: the cap, at least two fades) */
        if (p->s != v->bf_s || p->e != v->bf_e || p->k != v->bf_k || v->r_te != v->bf_te || v->r_ts != v->bf_ts) {
            v->bf_s = p->s;
            v->bf_e = p->e;
            v->bf_k = p->k;
            v->bf_te = v->r_te;
            v->bf_ts = v->r_ts;
            v->r_bfr = kbfade(v, p, be, bs) + 2;
        }
        if (z < 2 * v->r_bfr) z = 2 * v->r_bfr;
        if (be != 0x7fffffff) {
            if (sig > 0.0f) v->r_eb = be - 1 - v->r_bfr;
            if (sig > 0.0f) { if (tau > 0.0f) v->r_ze = be - z; }
        }
        if (bs != (int32_t)0x80000000) {
            if (sig < 0.0f) v->r_sb = bs + v->r_bfr;
            if (sig < 0.0f) { if (tau < 0.0f) v->r_zs = bs + z; }
        }
    }
    if (v->ahold > 0) {   /* an attack just jumped onto: no leash this block (the borders stay) */
        p->k = 0x3fffffff;
        v->r_k = p->k;
        v->r_d = 0;
        v->ahold -= count;
    }
    i = 0;
    for (;;) {
        i += kf_run(v, out + i, count - i);
        if (i >= count) break;
        kone(v, p, reach, eps, out + i);
        if (v->ended == 1) { v->ended = 2; v->ended_at = i; }
        i++;
    }
}

void kf_tail(kf_voice *v, const kf_tail_args *a, float *out)
{
    int32_t i = a->i;
    for (;;) {
        kone(v, &a->p, a->reach, a->p.eps, out + i);
        if (v->ended == 1) { v->ended = 2; v->ended_at = i; }
        i++;
        if (i >= a->count) break;
        i += kf_run(v, out + i, a->count - i);
        if (i >= a->count) break;
    }
}
