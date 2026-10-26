/*
 * kf.h -- the keyframe engine: one channel of one Digitakt voice.
 * GNU AGPL-3.0-or-later.
 *
 * The keyframes are never heard. An analyzer marks the derivative zero
 * crossings (extrema, snapped to whole samples) of the channel's PCM in a small
 * window around the heads, storing only their sample indices. Two heads walk
 * the PCM: the reference at the stretch rate, the play head at the pitch rate.
 * Their distance is measured in the sparse domain -- how many keyframes lie
 * between them -- and when it exceeds K, a temp head starts at the reference and
 * the play head crossfades (linearly) into it over the uniform span between the
 * two heads' keyframes. Audio is a linear-interpolated read of the PCM.
 *
 * Positions are fixed point (whole samples and a 32-bit fraction), rates too, so a head's place
 * after j samples is exactly its place plus j times its rate: no float accumulates, the floor in
 * either direction is the carry, and the DSP may take several samples' steps at once.
 *
 * In sparse material (keyframes further apart than KF_DIST_PER_K) the leash holds in
 * time: the heads at most K c^2 / spacing apart, so grains shorten as keyframes thin.
 * A play-once note's edge is a border: the play head never reads past it. When
 * it would reach the edge before the reference (it runs ahead), it splices back
 * to the reference, or, with the reference within two border fades of the edge,
 * to two border fades before it: it loops that much until the reference arrives
 * and ends the note, and never goes further back than the reference.
 *
 * Positions live on an unwrapped timeline (whole samples + a fraction, so no
 * float ever grows large); reads wrap around the whole sample. The reference
 * loops inside the region [s, e): crossing an edge moves it by the region's
 * length and forces a short crossfade there. A region that moves past the
 * reference (STRT, LEN, their LFOs) takes it to its edge with such a crossfade
 * at most once every KF_DRAG_HOLD samples; in between it holds the reference at
 * the nearer edge and only the leash splices.
 *
 * Written for the SHARC's selcc as well as the host: no struct passed or
 * returned by value, no unions, no typedef of a floating type, no integer
 * division or modulo (the device runtime has none).
 */
#ifndef KF_H
#define KF_H

#include <stdint.h>

#ifndef KF_EPS
#define KF_EPS        33        /* the analyzer's threshold, int16 units (0.001 of full scale: "transparent") */
#endif
#ifndef KF_MIN_FADE
#define KF_MIN_FADE   48        /* the shortest crossfade, samples */
#endif
#ifndef KF_WRAP_FADE
#define KF_WRAP_FADE  1024      /* the forced crossfade at the region's edge, samples */
#endif
#ifndef KF_DRAG_HOLD
#define KF_DRAG_HOLD  2400      /* the least time between two forced crossfades of a moving region, samples */
#endif
#ifndef KF_DIST_PER_K
#define KF_DIST_PER_K 48        /* the leash in sparse material, c (samples: 1 ms). Where the keyframes between the
                                   heads lie further apart than c on average (sp = distance / keyframes), the leash
                                   holds at a distance of K c^2 / sp, not K keyframes: the sparser, the shorter the
                                   grain (a kick's body: two or three cycles), where K keyframes alone would let the
                                   heads drift apart by seconds. Below c the keyframes' leash is untouched. Silence: K c^2
                                   at one keyframe, a distance of c sqrt(K), at least KF_SPARSE_MIN. A play-once note's last grain is at most
                                   K c. 0: none */
#endif
#ifndef KF_BORDER_FADE
#define KF_BORDER_FADE 256      /* a border splice's crossfade, samples of head travel (a play-once note's edge) */
#endif
#ifndef KF_BORDER_FADE_MAX
#define KF_BORDER_FADE_MAX 4096 /* the longest border fade (kbfade; KF_BORDER_FADE the shortest) */
#endif
#ifndef KF_TRANS_FADE
#define KF_TRANS_FADE 64        /* the play head's jump onto a transient the reference crossed: its crossfade, samples
                                   of head travel (the cut sits before the attack: the attack itself is not faded) */
#endif
#ifndef KF_ATTACK_HOLD
#define KF_ATTACK_HOLD 1024     /* after a jump onto a transient, no leash splice for this many output samples: the
                                   attack plays (a short SIZE's leash took the play head off it at once) */
#endif
#ifndef KF_TRANS_TOL
#define KF_TRANS_TOL  128       /* no jump when the play head is already this close to the transient (STR = pitch:
                                   the heads together, playback as before) */
#endif
#define KF_SPARSE_MIN (2 * (KF_BORDER_FADE + 2))   /* the sparse leash never holds the heads closer than this
                                   (silence, a small K: no splice every few ms, unheard and not free) */

typedef struct {
    int32_t  ip;    /* position: whole samples, unwrapped */
    uint32_t fq;    /* ... and the fraction, fq / 2^32: stepping is exact integer arithmetic (KSTEP) */
    int32_t ri;     /* ip wrapped into the sample, [0, n) */
    int32_t m;      /* logical index of the last keyframe at or before ip (base - 1: none) */
} kf_head;

typedef struct {
    float   stretch;    /* the reference's rate, source samples per output sample (bipolar) */
    float   pitch;      /* the play head's rate (bipolar) */
    int32_t k;          /* the leash, keyframes */
    int32_t s, e;       /* the region, sample indices, 0 <= s < e <= n */
    int32_t eps;        /* the analyzer's threshold, int16 units */
    int32_t max_fade;   /* a leash crossfade's longest span, samples (0: no cap) */
    float   gain;       /* the output's scale: 1/32768 for full scale */
} kf_params;

typedef struct {
    /* the run's inputs, set by kf_render each block (kf/sharc/run.asm reads them
       at these word offsets: 0..5) */
    float     r_sig, r_tau, r_gain;
    int32_t   r_k, r_s, r_e;
    /* the sample: int16, one channel */
    uintptr_t pcm;          /* byte address of sample 0 */
    int32_t   n;            /* samples */
    /* the keyframe window: logical indices [base, base + count) in ring slots & mask;
       count < mask (two slots stay free: run.asm puts its sentinels there, +big above the top and
       -big below the base) */
    int32_t  *ring;
    int32_t   mask;
    int32_t   base, count;
    int32_t   lo, hi;       /* the analysed positions, unwrapped, inclusive */
    int32_t   lo_r, hi_r;   /* ... wrapped */
    int32_t   v_lo, v_hi;   /* the PCM values at the outermost keyframes (the threshold's references) */
    /* the heads */
    kf_head   ref, play, temp;
    int32_t   splicing;
    int32_t   force;        /* the next splice is a forced, short one */
    float     t, inc;       /* the crossfade's phase and its step per unit of |pitch| */
    /* counters (host statistics) */
    uint32_t  n_splice, n_forced, n_keys;
    float     sum_fade;
    int32_t   hold;         /* samples until a moving region may force a crossfade again */
    /* an end evicted by growth at the other end: its lo_r and v_lo (hi_r, v_hi)
       are recomputed (kwrap, kx) when that end next grows, not at each eviction */
    int32_t   lo_stale, hi_stale;
    /* kf/sharc/run.asm's: x(hi - 1), x(hi), x(hi + 1) as kf_grow_hi last left them,
       valid while hc_tag == hi (kreset clears it); the C never reads them */
    int32_t   hc_tag, hc_x[3];
    /* once (the glue sets it): bit 0 play once: the reference reaching the region's edge ends
       the note (ended 1, then 2 with ended_at the sample it happened at; the
       engine itself goes on, wrapped, for the glue's fade); bit 1 cut at the edge: the play
       head jumps with the reference, no crossfade (the glue fades the output to silence there) */
    int32_t   once, ended, ended_at;
    /* the block's rates as fixed point, set with r_sig and r_tau (krate): whole samples (a floor)
       and the fraction, so a head's step is fq += f and ip += i + the carry, for kone and run.asm */
    int32_t   r_sig_i;
    uint32_t  r_sig_f;
    int32_t   r_tau_i;
    uint32_t  r_tau_f;
    /* the block's borders (kf_render), after the fields kf/sharc/run.asm reads: a grain's longest in
       time, K c (samples, 0: none; a play-once note's last grain), and the play head's limits in a play-once note, the edge less a border
       fade (eb: running forward; sb: backwards; out of reach when not playing once that way), and the
       end zone: the reference within a grain (the cap, at least two border fades) of the edge the note
       ends at (ze forward, zs backwards), where a play head behind it is left to run (no leash): the
       border decides there */
    int32_t   r_d, r_eb, r_sb, r_ze, r_zs;
    float     r_kc2;        /* K c^2 (KF_DIST_PER_K): the sparse leash, distance^2 > K c^2 keyframes */
    uint32_t  n_border;     /* border splices (host statistics) */
    /* transient borders (2026-10-22): the sample's transients, cut positions in time order (the glue's: the
       detector's cache, a grid's or chops'; none: tn 0), and the one ahead of the reference this block: te
       running forward, ts backwards (none: 0x7fffffff, 0x80000000). It is a border as a play-once note's edge
       is: a play head running ahead waits before it, looping its last grain, until the reference crosses it;
       then the play head jumps onto it (ahead or behind: each attack plays once, none dropped) */
    int32_t   tn;
    int32_t   r_te, r_ts;
    uint32_t  n_tjump;      /* transient jumps (host statistics) */
    int32_t   ahold;        /* output samples left after a jump with no leash (the attack plays: KF_ATTACK_HOLD) */
    const int32_t *tp;
    int32_t   r_bfr;        /* this block's border room: its border fade (samples of head travel) + 2 (kbfade: the fade
                               half SIZE's grain here, from the keyframes; the shortest grain before a border two of it) */
    int32_t   bf_s, bf_e, bf_k, bf_te, bf_ts;   /* what r_bfr was made for (the region, K, the transients either side):
                               made again only when one changes, never block to block, so the SHARC's kf_fast (which
                               cannot make it, and declines a block whose region or K differ: run.asm) and the C agree */
} kf_voice;

/* the rest of a block from sample i, after an event stopped kf_run there: for
   kf/sharc/run.asm's kf_block, which does the start of a block itself */
typedef struct {
    kf_params p;        /* (as kf_render keeps them: inside the sample) */
    int32_t   reach;
    int32_t   i;
    int32_t   count;
} kf_tail_args;

void kf_init(kf_voice *v, int32_t *ring, int32_t ring_size);    /* ring_size: a power of two */
void kf_trig(kf_voice *v, uintptr_t pcm, int32_t n, int32_t pos); /* a note: every head at pos */
void kf_render(kf_voice *v, const kf_params *p, float *out, int32_t count);
void kf_tail(kf_voice *v, const kf_tail_args *a, float *out);

#endif
