/* SPDX-License-Identifier: GPL-3.0-only
 * FM-313 voice: oscillator, diode ladder VCF, MEG/VEG, accent sweep, slide.
 * Runs at 2x (88.2 kHz) and decimates with a 47-tap half-band FIR.
 */
#include "fm313_dsp.h"

#define PI_F 3.14159265f

/* half-band taps at odd offsets from the centre (centre tap = 0.5) */
#if FM313_OS == 2
static const float HB[24] = {
    -1.265510146e-05f, 1.181462895e-04f, -4.450010681e-04f, 1.218258849e-03f,
    -2.776675122e-03f, 5.596694868e-03f, -1.034215109e-02f, 1.801261863e-02f,
    -3.042448020e-02f, 5.197862381e-02f, -9.868616627e-02f, 3.157643884e-01f,
     3.157643884e-01f, -9.868616627e-02f, 5.197862381e-02f, -3.042448020e-02f,
     1.801261863e-02f, -1.034215109e-02f, 5.596694868e-03f, -2.776675122e-03f,
     1.218258849e-03f, -4.450010681e-04f, 1.181462895e-04f, -1.265510146e-05f };
#endif

/* ladder constants */
#define LAD_C4      0.5f     /* top capacitor (bigger cap, slower last pole) */
#define LAD_IN      0.42f    /* signal level into the ladder (diode Vt units) */
#define LAD_FB_HP   95.0f    /* coupling capacitor in the resonance path */
#define LAD_OUT     5.0f

static float k_crit = 26.99f;  /* tests/kcrit: measure_kcrit() with these constants */

static inline float fast_tan(float x)   /* Pade, fine up to ~1.3 rad */
{
    float x2 = x*x;
    return x*(15.f - x2)/(15.f - 6.f*x2);
}
static inline float onepole_coef(float hz, float fs)  /* 1-exp(-2pi f/fs) */
{
    return 1.f - fm313_exp2(-2.f*PI_F*hz/fs*1.44269504f);
}
static inline float tau_coef(float tau_s, float fs)    /* exp(-1/(tau fs)) */
{
    return fm313_exp2(-1.44269504f/(tau_s*fs));
}
static inline float polyblep(float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t*t - 1.f; }
    if (t > 1.f - dt) { t = (t - 1.f)/dt; return t*t + t + t + 1.f; }
    return 0.f;
}
static inline float frand(fm313_voice_t *v)
{
    v->seed = v->seed*1664525u + 1013904223u;
    return (float)(v->seed >> 8) * (1.f/8388608.f) - 1.f;
}

/* one trapezoidal step of the linearised diode ladder.
 * a: prewarped cutoff, k: feedback, x: input. Returns the top output. */
static float ladder_step(fm313_voice_t *v, float a, float k, float x)
{
    float *y = v->y;
    float xp = x + k*v->fb_lp;               /* u = x - k*(y4 - lp(y4)) */
    float u  = xp - k*y[3];
    float g0 = fm313_tanh_over_x(u - y[0]);
    float g1 = fm313_tanh_over_x(y[0] - y[1]);
    float g2 = fm313_tanh_over_x(y[1] - y[2]);
    float g3 = fm313_tanh_over_x(y[2] - y[3]);
    float c4 = LAD_C4;

    /* J y + B x' (rows) */
    float J00 = -(g0+g1), J01 = g1,           J03 = -g0*k;
    float J10 = g1,       J11 = -(g1+g2),     J12 = g2;
    float J21 = g2,       J22 = -(g2+g3),     J23 = g3;
    float J32 = c4*g3,    J33 = -c4*g3;

    float r0 = y[0] + a*(J00*y[0] + J01*y[1] + J03*y[3] + g0*(xp + v->u_prev));
    float r1 = y[1] + a*(J10*y[0] + J11*y[1] + J12*y[2]);
    float r2 = y[2] + a*(J21*y[1] + J22*y[2] + J23*y[3]);
    float r3 = y[3] + a*(J32*y[2] + J33*y[3]);
    v->u_prev = xp;

    /* M = I - aJ :  tridiagonal + M03 */
    float m00 = 1.f - a*J00, m01 = -a*J01, m03 = -a*J03;
    float m10 = -a*J10, m11 = 1.f - a*J11, m12 = -a*J12;
    float m21 = -a*J21, m22 = 1.f - a*J22, m23 = -a*J23;
    float m32 = -a*J32, m33 = 1.f - a*J33;

    /* eliminate column 0 from row 1 (pivots inverted once: 4 divisions, not 7) */
    float i00 = 1.f/m00;
    float f = m10*i00;
    m11 -= f*m01; float m13 = -f*m03; r1 -= f*r0;
    float i11 = 1.f/m11;
    f = m21*i11;
    m22 -= f*m12; m23 -= f*m13; r2 -= f*r1;
    float i22 = 1.f/m22;
    f = m32*i22;
    m33 -= f*m23; r3 -= f*r2;
    float y3 = r3/m33;
    float y2 = (r2 - m23*y3)*i22;
    float y1 = (r1 - m12*y2 - m13*y3)*i11;
    float y0 = (r0 - m01*y1 - m03*y3)*i00;
    y[0] = y0; y[1] = y1; y[2] = y2; y[3] = y3;
    return y3;
}

#ifdef FM313_MEASURE
/* find the feedback at which the small-signal ladder starts to oscillate */
static void measure_kcrit(void)
{
    fm313_voice_t t;
    float lo = 2.f, hi = 40.f;
    float a = fast_tan(PI_F*1000.f/FM313_FS_INT);
    for (int it = 0; it < 22; it++) {
        float k = 0.5f*(lo + hi);
        for (int i = 0; i < 4; i++) t.y[i] = 0.f;
        t.fb_lp = 0.f; t.u_prev = 0.f;
        float peak_early = 0.f, peak_late = 0.f;
        float fbc = onepole_coef(LAD_FB_HP, FM313_FS_INT);
        for (int n = 0; n < 12000; n++) {
            float x = (n == 0) ? 1e-3f : 0.f;
            float o = ladder_step(&t, a, k, x);
            t.fb_lp += fbc*(o - t.fb_lp);
            float ao = o < 0 ? -o : o;
            if (n > 1000 && n < 2000 && ao > peak_early) peak_early = ao;
            if (n > 11000 && ao > peak_late) peak_late = ao;
        }
        if (peak_late > peak_early) hi = k; else lo = k;
    }
    k_crit = 0.5f*(lo + hi);
}
#endif

float fm313_kcrit(void) { return k_crit; }

void fm313_init(fm313_voice_t *v)
{
    char *c = (char *)v;
    for (unsigned i = 0; i < sizeof *v; i++) c[i] = 0;
    v->seed = 0x2badf00du;
    v->pitch = v->pitch_target = 45.f;
    v->meg_tau = 0.1f;
    v->phase2[0] = 0.13f; v->phase2[1] = 0.57f; v->phase2[2] = 0.31f; v->phase2[3] = 0.83f;
    v->p.cutoff = 0.35f; v->p.reso = 0.6f; v->p.envmod = 0.5f;
    v->p.decay = 0.4f; v->p.accent = 0.6f; v->p.wave = FM313_SAW;
    v->p.level = 0.8f;
    v->sm_level = 0.8f; v->dist_fade = 1.f; v->sm_tone = 0.5f;
#ifdef FM313_MEASURE
    static int measured;
    if (!measured) { measure_kcrit(); measured = 1; }
#endif
}

void fm313_note_on(fm313_voice_t *v, int note, int accent, int slide_in)
{
    v->pitch_target = (float)note;
    v->accented = accent;
    if (slide_in && v->gate) {
        v->sliding = 1;            /* legato: no envelope retrigger */
    } else {
        v->sliding = 0;
        v->pitch = (float)note;
        v->meg = 1.f;
        v->rnd = 1.f;
        /* accent forces the shortest decay */
        float d = accent ? 0.f : v->p.decay;
        float t = 0.2f*fm313_exp2(d*3.3219281f);       /* 0.2 .. 2 s */
        v->meg_tau = t*0.25f;
    }
    v->gate = 1;
}

void fm313_gate_off(fm313_voice_t *v) { v->gate = 0; }

static float shape(int type, float x, float d)
{
    float y;
    switch (type) {
    case FM313_DIST_OD: {           /* asymmetric soft clip, like a driven op-amp stage */
        float g = 1.f + d*9.f;
        y = fm313_tanh(g*x + 0.25f) - 0.2449f;
        y *= 0.9f/(0.3f + 0.7f*fm313_tanh(g*0.5f));
        break; }
    case FM313_DIST_DIST: {         /* two hard-ish stages */
        float g = 2.f + d*40.f;
        y = fm313_tanh(g*x);
        y = fm313_tanh(2.2f*y)*0.75f;
        break; }
    case FM313_DIST_FUZZ: {         /* rectifying fuzz: adds the octave */
        float g = 2.f + d*30.f;
        float a = fm313_tanh(g*x);
        y = 0.6f*a + 0.5f*(a < 0 ? -a : a) - 0.25f;
        y = fm313_tanh(1.8f*y)*0.8f;
        break; }
    default: {                    /* CRUNCH: folding */
        float g = 1.f + d*6.f;
        float a = g*x;
        while (a > 1.f || a < -1.f) a = a > 1.f ? 2.f - a : -2.f - a;
        y = 0.8f*a;
        break; }
    }
    return y;
}

/* one type with its own tone and DC state */
static float dist_path(int type, float x, float d, float lp_c, float *lp, float *dc)
{
    if (!type) return x;
    float y = shape(type, x, d);
    *lp += lp_c*(y - *lp);                        /* tone */
    y = *lp;
    *dc += 0.0004f*(y - *dc);
    if (*lp < 1e-15f && *lp > -1e-15f) *lp = 0.f;
    if (*dc < 1e-15f && *dc > -1e-15f) *dc = 0.f;
    return y - *dc;
}

void fm313_render(fm313_voice_t *v, float *out, int n)
{
    const float fs = FM313_FS_INT;
    const fm313_params_t *p = &v->p;
    float meg_c   = tau_coef(v->meg_tau, fs);
    float slide_c = tau_coef(v->sliding ? 0.024f + p->glide : 0.0008f + 0.5f*p->glide, fs);
    float att_c   = tau_coef(0.0009f, fs);
    float rel_c   = tau_coef(0.0035f, fs);
    float vdec_c  = tau_coef(1.6f, fs);
    float acc_c   = tau_coef(0.004f + 0.075f*p->reso, fs);
    float fb_c    = onepole_coef(LAD_FB_HP, fs);
    float hp1_c   = onepole_coef(18.f, fs), hp2_c = onepole_coef(45.f, fs);
    float sqhp_c  = onepole_coef(30.f, fs);
    float drift_c = onepole_coef(0.3f, fs/FM313_CR);
    /* knobs move in detents: everything a knob sets glides at the control rate (~6 ms), not once per audio
     * block (0.2.6 stepped once per 5.8 ms block: still a click at high resonance) */
    const float kc = onepole_coef(26.f, fs/FM313_CR);
    float lvl0 = v->sm_level, lvl1 = v->sm_level + 0.25f*(p->level - v->sm_level);
    v->sm_level = lvl1;
    float rr = p->reso*(2.f - p->reso);          /* pot taper */
    /* a knob moves in detents: glide resonance and the cutoff's base over a few blocks (no clicks at high Q) */
    float k_t = rr*0.96f*k_crit;
    float base_t = 7.75f + p->cutoff*4.6f - p->envmod*0.9f;   /* 2^7.75 = 215 Hz */
    float env_t = p->envmod*4.2f;
    if (v->sm_base == 0.f) {
        v->sm_base = base_t; v->sm_k = k_t; v->sm_env = env_t;
        v->sm_drive = p->dist_drive; v->sm_tone = p->dist_tone;
        v->dist_lpc = onepole_coef(1500.f*fm313_exp2(v->sm_tone*3.5f), fs);
        v->comp_now = 1.f + 0.11f*k_t;
    }
    float acc_amt  = p->accent;

    for (int i = 0; i < n; i++) {
        float acc_vca = 0.f;
        for (int o = 0; o < FM313_OS; o++) {
            /* control rate: pitch, cutoff and envelopes' targets every FM313_CR internal samples */
            if (v->cr_count == 0) {
                v->cr_count = FM313_CR;
                v->sm_k += kc*(k_t - v->sm_k);
                v->sm_base += kc*(base_t - v->sm_base);
                v->sm_env += kc*(env_t - v->sm_env);
                v->comp_now = 1.f + 0.11f*v->sm_k;      /* partial make-up for the passband loss */
                v->sm_drive += kc*(p->dist_drive - v->sm_drive);
                float tn = v->sm_tone + kc*(p->dist_tone - v->sm_tone);
                if (tn != v->sm_tone) { v->sm_tone = tn; v->dist_lpc = onepole_coef(1500.f*fm313_exp2(tn*3.5f), fs); }
                v->drift += drift_c*(frand(v)*0.06f - v->drift);
                v->sm_touch += kc*(p->touch_oct - v->sm_touch);
                v->vib_ph += 5.6f*FM313_CR/fs; if (v->vib_ph >= 1.f) v->vib_ph -= 1.f;
                float tri = v->vib_ph < 0.5f ? 4.f*v->vib_ph - 1.f : 3.f - 4.f*v->vib_ph;   /* -1..1 */
                float sine = tri*(1.5f - 0.5f*tri*tri);                                     /* rounded */
                float semis = v->pitch + p->tune + v->drift + p->vib*sine - 69.f;
                v->dt = 440.f*fm313_exp2(semis*(1.f/12.f))/fs;
                float oct = v->sm_base + v->sm_env*v->meg + 3.4f*v->acc_cv + v->sm_touch;
                v->oct_now = oct;
                float fc = fm313_exp2(oct);
                if (fc > 0.40f*fs) fc = 0.40f*fs;
                float a1 = fast_tan(PI_F*fc/fs);
                v->a_step = (a1 - v->a)*(1.f/FM313_CR);
            }
            v->cr_count--;
            v->a += v->a_step;
            v->pitch = v->pitch_target + (v->pitch - v->pitch_target)*slide_c;
            float dt = v->dt;

            /* oscillator */
            float s;
            if (p->wave == FM313_SUPER) {
                static const float det[4] = { -0.0118f, -0.0041f, 0.0047f, 0.0121f };
                s = 0.f;
                for (int j = 0; j < 4; j++) {
                    float dj = dt*(1.f + det[j]);
                    float ph = v->phase2[j] + dj; if (ph >= 1.f) { ph -= 1.f; if (!j) v->wrap_pending = 1; }
                    v->phase2[j] = ph;
                    s += 2.f*ph - 1.f - polyblep(ph, dj);
                }
                s *= 0.36f;
            } else {
                float ph = v->phase + dt; if (ph >= 1.f) { ph -= 1.f; v->wrap_pending = 1; }
                v->phase = ph;
                /* capacitor ramp: slightly bowed saw */
                float saw = 2.f*(ph + 0.17f*(ph - ph*ph)) - 1.f - polyblep(ph, dt);
                if (p->wave == FM313_SQUARE) {
                    const float duty = 0.47f;
                    float pt = ph + (1.f - duty); if (pt >= 1.f) pt -= 1.f;
                    float pulse = (ph < duty ? 1.f : -1.f) + polyblep(ph, dt) - polyblep(pt, dt);
                    float sq = 0.88f*pulse + 0.22f*saw;   /* shaped from the ramp: tilted tops */
                    v->sq_hp += sqhp_c*(sq - v->sq_hp);
                    s = sq - v->sq_hp;
                } else s = saw;
            }

            /* envelopes */
            v->meg *= meg_c;
            float acc_t = v->accented ? v->meg*acc_amt : 0.f;
            v->acc_cv = acc_t + (v->acc_cv - acc_t)*acc_c;   /* C13 "wow" capacitor */
            if (v->gate) {
                v->rnd *= vdec_c;                            /* rnd: slow VEG peak */
                v->veg = v->rnd + (v->veg - v->rnd)*att_c;
            } else {
                v->veg *= rel_c;
            }
            if (v->meg < 1e-6f) v->meg = 0.f;            /* no denormals (slow on any FPU) */
            if (v->veg < 1e-6f && !v->gate) v->veg = 0.f;
            if (v->acc_cv < 1e-6f) v->acc_cv = 0.f;
            acc_vca = v->accented ? acc_amt*(0.35f + 0.65f*v->meg) : 0.f;

            float a = v->a;
            float y = ladder_step(v, a, v->sm_k, s*LAD_IN + 1e-9f);   /* + a floor: the ladder never decays into denormals */
            v->fb_lp += fb_c*(y - v->fb_lp);

            float sig = y*LAD_OUT*v->comp_now*v->veg*(1.f + 0.9f*acc_vca);
            /* a new distortion type: both run and cross over ~23 ms (0.2.4's dip still clicked) */
            if (p->dist_type != v->dist_cur && v->dist_fade >= 1.f) {
                v->dist_old = v->dist_cur; v->dist_lp2 = v->dist_lp; v->dist_dc2 = v->dist_dc;
                v->dist_cur = p->dist_type; v->dist_lp = v->dist_dc = 0.f;
                v->dist_fade = 0.f;
            }
            {
                float wet = dist_path(v->dist_cur, sig, v->sm_drive, v->dist_lpc, &v->dist_lp, &v->dist_dc);
                if (v->dist_fade < 1.f) {
                    float was = dist_path(v->dist_old, sig, v->sm_drive, v->dist_lpc, &v->dist_lp2, &v->dist_dc2);
                    wet = was + (wet - was)*v->dist_fade;
                    v->dist_fade += 1.f/2048.f;
                }
                sig = wet;
            }
            /* output coupling */
            v->hp1 += hp1_c*(sig - v->hp1); sig -= v->hp1;
            v->hp2 += hp2_c*(sig - v->hp2); sig -= v->hp2;
            sig += 1e-12f; sig -= 1e-12f;            /* flushes |sig| < ~1e-19 to 0 */
            if (v->hp1 < 1e-15f && v->hp1 > -1e-15f) v->hp1 = 0.f;
            if (v->hp2 < 1e-15f && v->hp2 > -1e-15f) v->hp2 = 0.f;

            v->dec[v->dpos] = sig;
            v->dpos = (v->dpos + 1) % 48;
        }
#if FM313_OS == 2
        /* decimate: newest sample at dpos-1 */
        int c = v->dpos - 24; if (c < 0) c += 48;
        float acc = 0.5f*v->dec[c];
        int idx = v->dpos + 1; if (idx >= 48) idx -= 48;   /* age 46 */
        for (int j = 0; j < 24; j++) {
            acc += HB[j]*v->dec[idx];
            idx += 2; if (idx >= 48) idx -= 48;
        }
#else
        float acc = v->dec[(v->dpos + 47) % 48];
#endif
        out[i] = acc*(lvl0 + (lvl1 - lvl0)*(float)i/(float)n);
        {   /* the oscilloscope's memory (the trigger: the cycle's start) */
            float q = acc*16384.f;
            if (q > 32767.f) q = 32767.f; if (q < -32768.f) q = -32768.f;
            v->scope[v->out_count & 2047u] = (int16_t)q;
            if (v->wrap_pending) { v->wrap_pos = v->out_count; v->wrap_pending = 0; }
            v->out_count++;
        }
    }
}
