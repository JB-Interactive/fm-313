/* SPDX-License-Identifier: GPL-3.0-only
 * FM-313 effects after the voice: tempo delay (ping-pong), small FDN reverb,
 * output limiter. Mono in, stereo out, 44.1 kHz.
 * Buffers are int16 to fit the FM-1's RAM pool (~170 KB total).
 */
#ifndef FM313_FX_H
#define FM313_FX_H
#include <stdint.h>

#define FM313_DLY_MAX 33075        /* 0.75 s per side */
#define FM313_REV_LEN 7200

typedef struct {
    float bpm;
    float delay_mix, delay_fb;   /* 0..1 */
    int   delay_div;             /* 0:1/16 1:1/8T 2:1/8 3:1/8. 4:1/4 */
    float rev_mix;               /* 0..1 */
    float rev_time;              /* 0..1 */
    float out_gain;
    /* state */
    int16_t dl[FM313_DLY_MAX], dr[FM313_DLY_MAX];
    int   dpos;
    float dly_lp_l, dly_lp_r, dly_time;   /* the tap in use (samples) */
    float dly_next, dly_xf;               /* a new time: a second tap, crossfaded in over 50 ms (no pitch smear) */
    float rev_buf[FM313_REV_LEN];
    int   rev_len[4], rev_off[4], rev_pos[4];
    float rev_lp[4];
    float ap_buf[2][512]; int ap_pos[2];
    float lim_env;
    float sm_dmix, sm_fb, sm_rmix;   /* the knobs, smoothed per sample: no clicks when ALGORITHM turns */
} fm313_fx_t;

void fm313_fx_init(fm313_fx_t *f);
void fm313_fx_process(fm313_fx_t *f, const float *in, float *out_lr, int n);
#endif
