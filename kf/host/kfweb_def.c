/*
 * kfweb_def.c -- tkeo.h's constants as they are built for the unit, the starting values of kf/host/kfweb.c's
 * variables (TK_TUNABLE there; this file without it, from the same tkeo.h). GNU AGPL-3.0-or-later.
 */
#ifndef KW_TKEO_H
#define KW_TKEO_H "../tkeo.h"
#endif
#include KW_TKEO_H

/* in kfweb.c's kw_tk_set order */
const float kw_tk_def[15] = {
    TK_SVF_G, TK_SVF_NUM, TK_SVF_INV_DEN, TK_FLOOR, TK_HYST, TK_ON_FRAC, TK_VALLEY, (float)TK_DELAY,
    (float)TK_ZC_BACK, (float)TK_ZC_FWD, (float)TK_GAP, (float)TK_END_GAP, (float)TK_ONSET_PEAK, TK_RISE, TK_REL_FLOOR,
};
