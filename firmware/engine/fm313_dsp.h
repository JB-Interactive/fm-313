/* SPDX-License-Identifier: GPL-3.0-only
 * FM-313: a 303-style bass line engine for the M-VAVE FM-1.
 * Clean-room model built only from public descriptions of the TB-303 circuit
 * (diode ladder VCF, MEG/VEG envelopes, accent "sweep" capacitor, slide RC).
 * No code, tables or data from any Roland firmware.
 *
 * Portable C99, single precision, no libm in the audio path.
 */
#ifndef FM313_DSP_H
#define FM313_DSP_H
#include <stdint.h>

#define FM313_FS      44100.0f          /* output rate */
#ifndef FM313_OS
#define FM313_OS      2                 /* internal oversampling: 2 (quality) or 1 (half the CPU) */
#endif
#define FM313_FS_INT  (FM313_FS * FM313_OS)
#define FM313_CR      4                 /* control-rate divider (internal samples) */

enum { FM313_SAW = 0, FM313_SQUARE = 1, FM313_SUPER = 2 };
enum { FM313_DIST_OFF = 0, FM313_DIST_OD, FM313_DIST_DIST, FM313_DIST_FUZZ, FM313_DIST_CRUNCH, FM313_DIST_NUM };

/* knobs, all 0..1 unless noted */
typedef struct {
    float cutoff, reso, envmod, decay, accent;
    float tune;          /* semitones, -12..12 */
    int   wave;          /* FM313_SAW / FM313_SQUARE / FM313_SUPER */
    float level;
    int   dist_type;     /* runs inside the 2x loop: no aliasing from drive */
    float dist_drive;    /* 0..1 */
    float dist_tone;     /* 0..1 */
    /* 0.5, the keys' touch (pressure) and the XY play: the filter opened by touch_oct octaves, a vibrato of vib
     * semitones, slides glide stretched to `glide` seconds (0: the 303's own) */
    float touch_oct, vib, glide;
} fm313_params_t;

typedef struct {
    /* oscillator */
    float phase, phase2[4];
    float pitch, pitch_target;   /* semitones (MIDI note) */
    float dt, a, a_step;         /* control rate: phase step, ladder coefficient */
    float oct_now;               /* the cutoff in octaves, for the screen */
    int   cr_count;
    float sq_hp;                 /* droop on the square */
    float duty;                  /* the square's duty at this pitch (control rate) */
    int   sliding;
    /* envelopes */
    float meg, veg, acc_cv;
    int   gate, accented;
    float meg_tau;
    /* ladder */
    float y[4];
    float fb_lp;
    float u_prev;
    /* output */
    float hp1, hp1x, hp2, hp2x;
    float dec[48]; int dpos;
    float rnd; uint32_t seed;
    float drift;
    float dist_lp, dist_dc;
    float sm_drive, sm_tone, sm_level, dist_fade;   /* knobs smoothed; dist_fade: the crossfade to a new type */
    int   dist_cur, dist_old;
    float sm_k, sm_base, sm_env, comp_now, dist_lpc;
    float vib_ph, sm_touch;                         /* the vibrato's phase (0..1), the touch's glide */                            /* resonance and the cutoff knob, glided per block */
    float dist_lp2, dist_dc2;                       /* the outgoing type's tone and DC state */
    /* the screen's oscilloscope: the last 2048 output samples and where the oscillator last began a cycle */
    int16_t scope[2048];
    uint32_t out_count, wrap_pos;
    int wrap_pending;
    fm313_params_t p;
} fm313_voice_t;

void fm313_init(fm313_voice_t *v);
/* sequencer / MIDI side */
void fm313_note_on(fm313_voice_t *v, int note, int accent, int slide_in);
void fm313_gate_off(fm313_voice_t *v);
/* render n mono samples at 44.1 kHz (additive = 0 overwrites) */
void fm313_render(fm313_voice_t *v, float *out, int n);
float fm313_kcrit(void);

/* fast math shared with the effects */
static inline float fm313_exp2(float x)
{
    if (x < -30.f) return 0.f;
    if (x > 30.f) x = 30.f;
    int   i = (int)(x + 1024.f) - 1024;      /* floor */
    float f = x - (float)i;
    /* 5th order minimax for 2^f on [0,1) */
    float p = 1.0f + f*(0.693147182f + f*(0.240226507f + f*(0.0555041087f + f*(0.00961812911f + f*0.00133335581f))));
    union { float f; int32_t i; } u;
    u.i = (i + 127) << 23;
    return p * u.f;
}
static inline float fm313_tanh(float x)
{
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    float x2 = x*x;
    return x*(27.f + x2)/(27.f + 9.f*x2);
}
/* tanh(x)/x, the large-signal conductance of a diode pair: a polynomial in
 * x^2 up to |x| = 3 (exact 1 at 0, < 0.1 % off for small signals, no
 * division), then 1/|x| */
static inline float fm313_tanh_over_x(float x)
{
    float u = x*x;
    if (u > 9.f) return 1.f/(x < 0 ? -x : x);
    return 1.f + u*(-3.300393069e-01f + u*(1.171527906e-01f + u*(-3.069411883e-02f +
           u*(4.914241776e-03f + u*(-4.148397360e-04f + u*1.400382459e-05f)))));
}
#endif
