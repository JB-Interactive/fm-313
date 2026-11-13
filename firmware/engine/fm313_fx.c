/* SPDX-License-Identifier: GPL-3.0-only */
#include "fm313_fx.h"
#include "fm313_dsp.h"

static const int REV_L[4] = { 1427, 1663, 1907, 2203 };
static const int AP_L[2]  = { 347, 113 };

void fm313_fx_init(fm313_fx_t *f)
{
    char *c = (char *)f;
    for (unsigned i = 0; i < sizeof *f; i++) c[i] = 0;
    f->bpm = 126.f; f->delay_div = 2; f->delay_fb = 0.4f;
    f->rev_time = 0.5f; f->out_gain = 1.f;
    int off = 0;
    for (int i = 0; i < 4; i++) { f->rev_len[i] = REV_L[i]; f->rev_off[i] = off; off += REV_L[i]; }
}

static inline float rd16(const int16_t *b, float pos, int len)
{
    int i = (int)pos; float fr = pos - (float)i;
    int j = i + 1; if (j >= len) j -= len;
    return ((float)b[i] + ((float)b[j] - (float)b[i])*fr)*(1.f/16384.f);
}
static inline int16_t wr16(float x)
{
    x *= 16384.f;
    if (x > 32767.f) x = 32767.f; if (x < -32768.f) x = -32768.f;
    return (int16_t)x;
}

/* the output's ceiling (0.7.5): clean up to 0.6 of full scale, then a soft knee to 0.95. Before, a tanh over the
 * whole range bent every signal: a clean sine at half scale came out with its third harmonic at -34 dB (2 %), and
 * delay and reverb tails intermodulated with the dry sound. Now nothing is added below the knee */
static inline float soft_knee(float x)
{
    float a = x < 0.f ? -x : x;
    if (a <= 0.6f) return x;
    a = 0.6f + 0.35f*fm313_tanh((a - 0.6f)*(1.f/0.35f));
    return x < 0.f ? -a : a;
}

void fm313_fx_process(fm313_fx_t *f, const float *in, float *out, int n)
{
    static const float divs[5] = { 0.25f, 1.f/3.f, 0.5f, 0.75f, 1.0f };
    float target = FM313_FS*60.f/f->bpm*divs[f->delay_div < 0 ? 0 : (f->delay_div > 4 ? 4 : f->delay_div)];
    if (target > FM313_DLY_MAX - 4) target = FM313_DLY_MAX - 4;
    if (f->dly_time <= 0.f) f->dly_time = target;
    const float sk = 0.0015f;                     /* ~15 ms */
    float g_rev = 0.70f + 0.27f*f->rev_time;
    float damp = 0.35f;

    for (int i = 0; i < n; i++) {
        float x = in[i];
        float l = x, r = x;

        f->sm_dmix += sk*(f->delay_mix - f->sm_dmix);
        f->sm_fb += sk*(f->delay_fb*0.92f - f->sm_fb);
        f->sm_rmix += sk*(f->rev_mix - f->sm_rmix);
        float fb = f->sm_fb;
        /* ping-pong delay */
        if (f->delay_mix > 0.f || f->sm_dmix > 1e-4f) {
            if (f->dly_xf <= 0.f && (target - f->dly_time > 1.f || f->dly_time - target > 1.f)) {
                f->dly_next = target; f->dly_xf = 1e-6f;            /* start the crossfade to the new tap */
            }
            float rp = (float)f->dpos - f->dly_time; if (rp < 0) rp += FM313_DLY_MAX;
            float yl = rd16(f->dl, rp, FM313_DLY_MAX);
            float yr = rd16(f->dr, rp, FM313_DLY_MAX);
            if (f->dly_xf > 0.f) {
                float rq = (float)f->dpos - f->dly_next; if (rq < 0) rq += FM313_DLY_MAX;
                float x2 = f->dly_xf;
                yl += (rd16(f->dl, rq, FM313_DLY_MAX) - yl)*x2;
                yr += (rd16(f->dr, rq, FM313_DLY_MAX) - yr)*x2;
                f->dly_xf += 1.f/2205.f;
                if (f->dly_xf >= 1.f) { f->dly_time = f->dly_next; f->dly_xf = 0.f; }
            }
            f->dly_lp_l += 0.35f*(yl - f->dly_lp_l);
            f->dly_lp_r += 0.35f*(yr - f->dly_lp_r);
            f->dl[f->dpos] = wr16(x + fb*f->dly_lp_r);
            f->dr[f->dpos] = wr16(fb*f->dly_lp_l);
            l += f->sm_dmix*f->dly_lp_l;
            r += f->sm_dmix*f->dly_lp_r;
        } else {
            f->dl[f->dpos] = 0; f->dr[f->dpos] = 0;
        }
        if (++f->dpos >= FM313_DLY_MAX) f->dpos = 0;

        /* FDN reverb, 4 lines, Householder mix */
        if (f->rev_mix > 0.f || f->sm_rmix > 1e-4f) {
            float s = 0.5f*(l + r);
            for (int a = 0; a < 2; a++) {           /* input diffusion */
                float *b = f->ap_buf[a]; int p = f->ap_pos[a];
                float d = b[p];
                float w = s + 0.6f*d;
                if (w < 1e-15f && w > -1e-15f) w = 0.f;
                b[p] = w; s = d - 0.6f*w;
                if (++p >= AP_L[a]) p = 0; f->ap_pos[a] = p;
            }
            float o[4], sum = 0.f;
            for (int k = 0; k < 4; k++) {
                float *b = f->rev_buf + f->rev_off[k];
                o[k] = b[f->rev_pos[k]];
                f->rev_lp[k] += (1.f - damp)*(o[k] - f->rev_lp[k]);
                if (f->rev_lp[k] < 1e-15f && f->rev_lp[k] > -1e-15f) f->rev_lp[k] = 0.f;
                o[k] = f->rev_lp[k];
                sum += o[k];
            }
            sum *= 0.5f;
            for (int k = 0; k < 4; k++) {
                float *b = f->rev_buf + f->rev_off[k];
                float w = g_rev*(o[k] - sum) + s*0.35f;
                if (w < 1e-15f && w > -1e-15f) w = 0.f;        /* no denormals in the tail */
                b[f->rev_pos[k]] = w;
                if (++f->rev_pos[k] >= f->rev_len[k]) f->rev_pos[k] = 0;
            }
            l += f->sm_rmix*(o[0] + o[2]);
            r += f->sm_rmix*(o[1] + o[3]);
        }

        /* soft limiter */
        float a = l < 0 ? -l : l, b = r < 0 ? -r : r;
        float pk = a > b ? a : b;
        if (pk > f->lim_env) f->lim_env += 0.3f*(pk - f->lim_env);
        else f->lim_env += 0.0002f*(pk - f->lim_env);
        if (f->lim_env < 1e-9f) f->lim_env = 0.f;
        float g = f->out_gain;
        if (f->lim_env > 0.9f) g *= 0.9f/f->lim_env;
        out[2*i]   = soft_knee(l*g);
        out[2*i+1] = soft_knee(r*g);
    }
}
