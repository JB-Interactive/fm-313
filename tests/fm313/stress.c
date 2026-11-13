/* SPDX-License-Identifier: GPL-3.0-only
 * The engine under random use: every knob anywhere in its range (and jumping), random notes with accents and
 * slides, every wave and distortion, delay and reverb. Checked on every sample: finite, within full scale; over
 * each run: no DC, and silence after the last note (nothing rings on forever).
 *   stress [runs] */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../../firmware/engine/fm313_dsp.h"
#include "../../firmware/engine/fm313_fx.h"

static uint32_t seed = 12345u;
static float rnd(void) { seed = seed*1664525u + 1013904223u; return (float)(seed >> 8)*(1.f/16777216.f); }
static fm313_voice_t v;
static fm313_fx_t fx;

int main(int argc, char **argv)
{
    int runs = argc > 1 ? atoi(argv[1]) : 300, bad = 0;
    float mono[64], out[128], worst = 0, worst_dc = 0, worst_tail = 0;
    for (int r = 0; r < runs && !bad; r++) {
        fm313_init(&v); fm313_fx_init(&fx);
        int extreme = r % 3 == 0;                       /* a third of the runs: the knobs at their ends only */
        double dc = 0; long nsm = 0;
        int blocks = 44100*12/64, note_left = 0, held = 0;
        for (int b = 0; b < blocks; b++) {
            if (b % 40 == 0 && b < blocks/3) {          /* the knobs move every 58 ms, while notes play */
                #define K() (extreme ? (rnd() < 0.5f ? 0.f : 1.f) : rnd())
                v.p.cutoff = K(); v.p.reso = K(); v.p.envmod = K(); v.p.decay = K(); v.p.accent = K();
                v.p.tune = extreme ? (rnd() < 0.5f ? -12.f : 12.f) : rnd()*24.f - 12.f;
                v.p.wave = (int)(rnd()*3.f) % 3; v.p.level = 0.8f;
                v.p.dist_type = (int)(rnd()*(float)FM313_DIST_NUM) % FM313_DIST_NUM;
                v.p.dist_drive = K(); v.p.dist_tone = K();
                v.p.touch_oct = rnd() < 0.2f ? 3.f : 0.f; v.p.vib = rnd() < 0.2f ? 1.f : 0.f; v.p.glide = 0.f;
                fx.bpm = 40.f + rnd()*260.f; fx.delay_mix = K(); fx.delay_fb = K(); fx.delay_div = (int)(rnd()*5.f) % 5;
                fx.rev_mix = K(); fx.rev_time = K(); fx.out_gain = 0.5f + rnd()*1.7f;
            }
            int playing = b < blocks/3;                 /* the last 8 s: no new notes, the tail */
            if (playing && --note_left <= 0) {
                note_left = 20 + (int)(rnd()*120.f);
                if (rnd() < 0.2f) { fm313_gate_off(&v); held = 0; }
                else { fm313_note_on(&v, 12 + (int)(rnd()*84.f), rnd() < 0.4f, held && rnd() < 0.4f); held = 1; }
            }
            if (b == blocks/3) {
                fm313_gate_off(&v); held = 0;
                fx.delay_fb = 0.3f; fx.rev_time = 0.3f;  /* (the knobs at rest: full feedback is meant to ring) */
            }
            fm313_render(&v, mono, 64);
            fm313_fx_process(&fx, mono, out, 64);
            for (int i = 0; i < 128; i++) {
                float x = out[i];
                if (!(x == x) || x > 1.f || x < -1.f) { printf("run %d block %d: sample %g\n", r, b, (double)x); bad = 1; break; }
                if (fabsf(x) > worst) worst = fabsf(x);
                dc += (double)x; nsm++;
            }
            if (b >= blocks - 200) for (int i = 0; i < 128; i++) if (fabsf(out[i]) > worst_tail) worst_tail = fabsf(out[i]);
        }
        float d = (float)fabs(dc/(double)nsm);
        if (d > worst_dc) worst_dc = d;
    }
    printf("stress: %d runs, peak %.3f, DC %.5f, tail %.5f\n", runs, (double)worst, (double)worst_dc, (double)worst_tail);
    if (bad || worst_dc > 0.01f || worst_tail > 0.02f) { printf("stress: FAILED\n"); return 1; }
    return 0;
}
