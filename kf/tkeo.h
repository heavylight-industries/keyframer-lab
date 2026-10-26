/*
 * tkeo.h -- transient detection for slicing: Capicola's detector (lib/Detector.h) run offline over
 * a whole sample. GNU AGPL-3.0-or-later.
 *
 * Detection runs once per sample and keeps EVERY peak; the slice grids (ENV, GRID, RAND) are separate,
 * cheap passes over the stored peaks.
 *
 * Detection, per sample of the mono sum: the Teager-Kaiser energy from a cubic read of the signal at
 * lag 2 (Capicola's ReadBSplineFull), smoothed by a 2-pole state-variable lowpass (the follower: its
 * cutoff is the detector's main control), and a peak picker on the envelope's derivative (rising to
 * falling) with the peak refined to a fraction of a sample, and hysteresis on the level: a hump's top
 * counts once the envelope falls TK_HYST below it (a higher top before then replaces it, the hump's onset
 * kept), so ripple on a flat top is not a train of peaks a few samples apart. Each peak above the
 * silence floor is stored with its height (the envelope there).
 *
 * Each stored peak also gets its cut: from the peak, back along the envelope (a rolling buffer) to where
 * it was TK_ON_FRAC of the peak (silence) or to the valley before it (TK_VALLEY), then forward from the
 * lowest point passed to where it had risen TK_ON_FRAC of the way from there to the peak (so a hit over a
 * bed, a pad or a violin, starts where it leaves the bed), less the smoothing's delay (TK_DELAY, measured by kf/host/tkslice.c --calibrate),
 * then to the best zero crossing of the PCM there. (TK_ONSET_PEAK, tried in kf/host/tkweb.c: the cut at
 * the peak itself.)
 *
 * Written for the SHARC's selcc as well as the host, as kf.c: no struct passed or returned by value,
 * no unions, no integer division or modulo, no floating-point division or library calls.
 */
#ifndef TKEO_H
#define TKEO_H

#include <stdint.h>

#ifdef TK_TUNABLE   /* (kf/host/tkweb.c, the browser's tuning page: every constant below a variable) */
extern float   tk_t_svf_g, tk_t_svf_num, tk_t_svf_inv_den, tk_t_floor, tk_t_hyst, tk_t_on_frac, tk_t_valley;
extern int32_t tk_t_delay, tk_t_zc_back, tk_t_zc_fwd, tk_t_gap, tk_t_end_gap, tk_t_onset_peak;
extern float   tk_t_rise, tk_t_rel_floor;
#define TK_SVF_G        tk_t_svf_g
#define TK_SVF_NUM      tk_t_svf_num
#define TK_SVF_INV_DEN  tk_t_svf_inv_den
#define TK_FLOOR        tk_t_floor
#define TK_HYST         tk_t_hyst
#define TK_ON_FRAC      tk_t_on_frac
#define TK_VALLEY       tk_t_valley
#define TK_DELAY        tk_t_delay
#define TK_ZC_BACK      tk_t_zc_back
#define TK_ZC_FWD       tk_t_zc_fwd
#define TK_GAP          tk_t_gap
#define TK_END_GAP      tk_t_end_gap
#define TK_ONSET_PEAK   tk_t_onset_peak
#define TK_RISE         tk_t_rise
#define TK_REL_FLOOR    tk_t_rel_floor
#endif
#define TK_ENV_N    2048        /* the envelope's rolling buffer, samples (a power of two) */
#define TK_MAX_EV   2048        /* stored peaks a sample, at most */
#define TK_SLICES   64          /* the Digitakt's slice table */

/* the follower: SVF lowpass at 25.6 Hz (the module's kTkeoFc is 34 Hz; tuned by ear on a corpus in
   kf/host/tkweb.html, 2026-10-19), Q 0.5; g = tan(pi fc / 48000), num = 2r + g, 1 / (1 + 2rg + g^2), r = 1 / 2Q */
#ifndef TK_SVF_G
#define TK_SVF_G        0.0016748235f
#define TK_SVF_NUM      2.0016747f
#define TK_SVF_INV_DEN  0.99665874f
#endif
#ifndef TK_FLOOR
#define TK_FLOOR        1e-5f       /* silence: a lower peak is no event (Detector kAbsFloor) */
#endif
#ifndef TK_HYST
#define TK_HYST         0.69f       /* a peak counts once the envelope falls this far below it (of it): */
#endif
                                    /* ripple on one hump's top is one peak, two hits with a dip between two */
#ifndef TK_ON_FRAC
#define TK_ON_FRAC      0.05f       /* the onset: where the envelope rose from this share of the peak, */
#endif
#ifndef TK_VALLEY
#define TK_VALLEY       1.05f       /* or from a valley (back from the peak, 5% up again from its lowest) */
#endif
#ifndef TK_DELAY
#define TK_DELAY        115         /* the smoothing's lag at the onset, samples (tkslice --calibrate at 25.6 Hz, 4 to spare; */
                                    /* kf/host/tkweb.html follows the follower with it) */
#endif
#ifndef TK_ZC_BACK
#define TK_ZC_BACK      96          /* the zero-crossing search: this far before the onset ... (2 ms; 480 until
                                       2026-10-25 found a hat over a low tail's crossing up to 10 ms before the hit:
                                       the wasm session's handover, every hit of its demo within 27 samples at 96) */
#endif
#ifndef TK_ZC_FWD
#define TK_ZC_FWD       96          /* ... then after it (240 until 2026-10-25) */
#endif
#ifndef TK_GAP
#define TK_GAP          1104        /* the least distance between two cuts, every grid's (23 ms) */
#endif
#ifndef TK_ONSET_PEAK
#define TK_ONSET_PEAK   0           /* 1: the cut at the peak itself, no onset search (see above) */
#endif
#ifndef TK_RISE
#define TK_RISE         0.4f        /* a new hump counts only when it rose from a valley at most this share of
                                       its top (the onset search's lowest point): a wobble on a hit's decay, a
                                       few percent up, is no hit (the owner, 2026-10-22: the TKEO's downslope
                                       read as hits; 35-40 % by eye in kf/host/tkweb.html); 1: off */
#endif
#ifndef TK_REL_FLOOR
#define TK_REL_FLOOR    0.001f      /* a peak lower than this share of the sample's highest is no event (the
                                       grids' and the live count's candidates): the noise between hits (the
                                       owner, 2026-10-22: the first 16 or 32 perfect, then 20 of noise); 0: off.
                                       -30 dB of the TKEO's energy: a starting value, for kf/host/tkweb.html */
#endif

typedef struct {
    int32_t pos;        /* the cut: a sample index at a zero crossing */
    int32_t peak;       /* the envelope's peak, its sample index (delay removed) */
    float   value;      /* the envelope there: the peak's height */
} tk_event;

typedef struct {
    uintptr_t pcm_l, pcm_r;     /* byte addresses of the channels' sample 0 (int16; r = l for mono) */
    int32_t   n;                /* frames */
    int32_t   i;                /* the next input frame */
    int32_t   delay;            /* TK_DELAY, or the calibration's */
    float     h1, h2, h3, h4;   /* the input at lags 1..4 */
    float     s1, s2;           /* the SVF's state */
    float     prev_d1;          /* the envelope's derivative at lag 2, last step */
    int32_t   last_peak_i;      /* the step of the last committed hump's top (the onset search stops there) */
    float     lo;               /* the last top's valley (the onset search's lowest point; 0 none searched) */
    int32_t   pend;             /* a hump's top waiting for the envelope to fall TK_HYST below it */
    float     pend_val;
    int32_t   pend_pk, pend_on, pend_i;   /* its top and onset (frames), the step of its top */
    int32_t   env_w;            /* the newest envelope sample's index in env */
    int32_t   count;            /* peaks stored */
    tk_event *ev;
    float     env[TK_ENV_N];    /* the envelope, a rolling buffer */
} tk_state;

#ifdef TK_TRACE
void    TK_TRACE(const tk_state *s);   /* (kf/host/tkweb.c: -DTK_TRACE=tw_trace, called each frame) */
#endif
void    tk_init(tk_state *s, uintptr_t pcm_l, uintptr_t pcm_r, int32_t n, tk_event *ev);
int32_t tk_run(tk_state *s, int32_t frames);   /* analyse up to frames more; 1 when the sample is done */
/* CREATE SLICE GRID's TR.DET modes (its setting: OFF, then these) */
#define TK_LOUD     1           /* ENV: the slices highest peaks (TKEO height), so a grid of slices + 1 is
                                   this one and the next highest */
#define TK_EVEN     2           /* GRID: slice 1 at the start, then targets at k n / slices, k = 1 .. slices
                                   - 1, each to the nearest peak not taken */
#define TK_RAND     3           /* RAND: slice 1 at the start, then slices - 1 of the peaks at random
                                   (uniform). A reroll for each seed */
#ifndef TK_END_GAP
#define TK_END_GAP  2400        /* no cut in a sample's last 50 ms (ENV, GRID, RAND, the live count): no
                                   slice of almost nothing at its end */
#endif
/* the grid's cuts, in time order: slices of them, but never more than there are transients (the peaks
   above the floor, TK_GAP apart, none in the last TK_END_GAP; GRID and RAND add the start, and leave out
   a peak within TK_GAP of it): tk_grid_max. Their count. scratch: count int32s */
int32_t tk_grid_mode(const tk_state *s, const tk_event *ev, int32_t count, int32_t n, int32_t slices,
                     int32_t mode, uint32_t seed, int32_t *out, int32_t *scratch);
int32_t tk_grid_max(const tk_event *ev, int32_t count, int32_t n, int32_t mode, int32_t *scratch);

/* A sample's transients kept for the engine (LEN as the live slice count, SLICE's i-th of them): up to
   TK_CP peaks in time order, their heights, ENV's rank (0 the highest) and RAND's (TK_NONE: dropped by
   the least distance). tk_cache_fill runs where there is time (the RPC task); tk_cache_region where
   there is none (a block): only a scan of the peaks. */
#define TK_CP       256
#define TK_NONE     0x7fffffff
typedef struct {
    uintptr_t key_l, key_r;     /* the sample's channels (the voice records' w00) */
    int32_t   n;                /* its frames */
    int32_t   mode;             /* the last grid's TR.DET mode (TK_LOUD ..); 0: the slice table rules */
    int32_t   gen;              /* bumped by each fill (a voice's region is recomputed) */
    int32_t   m;                /* the peaks kept */
    int32_t   slices;           /* the last grid's N: LEN's live count goes no higher (its slice table's) */
    int32_t   pos[TK_CP];
    float     val[TK_CP];
    int32_t   rank[TK_CP];
    int32_t   rrank[TK_CP];
    int32_t   rtaken;           /* RAND's ranks taken (rrank 0 .. rtaken - 1) */
} tk_cache;
void    tk_cache_fill(tk_cache *c, const tk_state *s, const tk_event *ev, int32_t count, int32_t n, uint32_t seed,
                      int32_t *scratch);
/* slice i (0-based, wrapped round the slices there are) of n_slices in c's mode: [*start, *end). n_slices
   negative: VOL's and RAND's order the other way round (THIN's left half, the user 2026-10-19: the quietest
   first, RAND's last taken first), among the grid's transients (c->slices); GRID as positive.
   scratch: TK_REGION_SCRATCH int32s (not the stack: the engine's own stack is small) */
#define TK_REGION_SCRATCH (2 * TK_SLICES + 2)
void    tk_cache_region(const tk_cache *c, int32_t n_slices, int32_t i, int32_t *start, int32_t *end,
                        int32_t *scratch);
/* ... all of them: the cuts of n_slices in c's mode, in time order (the first 0), into scratch's first entries;
   their count (WERP+'s THIN: its reset points, the owner 2026-10-24) */
int32_t tk_cache_cuts(const tk_cache *c, int32_t n_slices, int32_t *scratch);

/* TRN, a modulation source from the transients at a play head p (frames): CONTOUR a smoothstep between
   the strengths of the transients either side of p, PULSE each transient's strength decaying over
   TK_PULSE frames after it. Strength by ENV's rank: 1 the strongest, towards 0 the weakest. *cursor: the
   caller's (any start), walked to p's neighbour each call: cheap while p moves little */
#define TK_CONTOUR  1
#define TK_PULSE_SHAPE 2
#define TK_PULSE    4800        /* PULSE's decay, frames (100 ms) */
float   tk_trn(const tk_cache *c, int32_t p, int32_t shape, int32_t *cursor);

#endif
