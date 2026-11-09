/* SPDX-License-Identifier: GPL-3.0-only
 * Host renderer: plays FM-313 patterns with knob automation into a WAV file.
 *   render out.wav scene
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../firmware/engine/fm313_dsp.h"
#include "../firmware/engine/fm313_seq.h"
#include "../firmware/engine/fm313_fx.h"

static int parse_pattern(fm313_pattern_t *p, const char *txt)
{
    static const char *names[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    memset(p, 0, sizeof *p);
    int n = 0;
    char buf[1024]; strncpy(buf, txt, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char *tok = strtok(buf, " "); tok && n < FM313_STEPS; tok = strtok(NULL, " ")) {
        fm313_step_t *s = &p->s[n++];
        if (tok[0] == '-') { s->type = FM313_REST; continue; }
        if (tok[0] == '_') { s->type = FM313_TIE;
            if (strchr(tok, 'S')) s->flags |= FM313_F_SLIDE; continue; }
        int best = -1, bl = 0;
        for (int i = 0; i < 12; i++) {
            int l = (int)strlen(names[i]);
            if (!strncmp(tok, names[i], l) && l > bl) { best = i; bl = l; }
        }
        s->note = (uint8_t)(best < 0 ? 0 : best);
        for (const char *c = tok + bl; *c; c++) {
            if (*c == '\'') s->oct++;
            else if (*c == ',') s->oct--;
            else if (*c == 'A') s->flags |= FM313_F_ACC;
            else if (*c == 'S') s->flags |= FM313_F_SLIDE;
            else if (*c == '^') s->note = 12;
        }
    }
    p->len = (uint8_t)n;
    return n;
}

typedef struct { float t0, t1, v0, v1; float *dst; } ramp_t;

static void write_wav(const char *fn, const float *x, int n)
{
    FILE *f = fopen(fn, "wb");
    int sr = 44100, ch = 2, bits = 16;
    int bytes = n*ch*2;
    fwrite("RIFF", 1, 4, f); int v = 36 + bytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    short s = 1; fwrite(&s, 2, 1, f); s = (short)ch; fwrite(&s, 2, 1, f);
    fwrite(&sr, 4, 1, f); v = sr*ch*2; fwrite(&v, 4, 1, f);
    s = (short)(ch*2); fwrite(&s, 2, 1, f); s = (short)bits; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    for (int i = 0; i < n*ch; i++) {
        float y = x[i]; if (y > 1) y = 1; if (y < -1) y = -1;
        short q = (short)lrintf(y*32767.f); fwrite(&q, 2, 1, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "render out.wav scene\n"); return 1; }
    int scene = atoi(argv[2]);
    static fm313_voice_t v; static fm313_seq_t q; static fm313_pattern_t pat; static fm313_fx_t fx;
    fm313_init(&v);
    fm313_fx_init(&fx);
    float bpm = 126.f, secs = 32.f;
    const char *pt = "C, C C'A C, D#S F C,A C' C, A#,S C C'A G, C C,A D#'S D#";
    ramp_t r[8]; int nr = 0;
    v.p.wave = FM313_SAW; v.p.reso = 0.75f; v.p.envmod = 0.55f; v.p.decay = 0.45f; v.p.accent = 0.7f;
    v.p.cutoff = 0.15f;
    float shuffle = 0.f;

    switch (scene) {
    case 0:   /* classic saw: cutoff sweep up and down */
        r[nr++] = (ramp_t){ 0, 16, 0.05f, 0.75f, &v.p.cutoff };
        r[nr++] = (ramp_t){ 16, 32, 0.75f, 0.10f, &v.p.cutoff };
        r[nr++] = (ramp_t){ 8, 24, 0.55f, 0.85f, &v.p.reso };
        break;
    case 1:   /* square, high resonance, envmod sweep */
        v.p.wave = FM313_SQUARE; v.p.reso = 0.88f;
        pt = "C,A C C, C'S C' C C,A D# C,S F#,S G, C C'A C, A#, G,A";
        r[nr++] = (ramp_t){ 0, 32, 0.1f, 0.9f, &v.p.envmod };
        r[nr++] = (ramp_t){ 0, 32, 0.25f, 0.35f, &v.p.cutoff };
        bpm = 132.f;
        break;
    case 2:   /* accent "wow": runs of accents, resonance up */
        pt = "C,A C,A C,A C,A - C,A C,A C,A C'A C'A - C,A C,A D#,A C,A G,AS";
        r[nr++] = (ramp_t){ 0, 32, 0.3f, 0.97f, &v.p.reso };
        r[nr++] = (ramp_t){ 0, 32, 0.25f, 0.95f, &v.p.accent };
        v.p.cutoff = 0.2f; v.p.envmod = 0.3f; v.p.decay = 0.3f;
        break;
    case 3:   /* decay + shuffle, distortion */
        pt = "C, C,S C _ C' - C,A D#S F G,A C - A#,S C' C,A - C";
        shuffle = 0.35f;
        r[nr++] = (ramp_t){ 0, 32, 0.0f, 1.0f, &v.p.decay };
        r[nr++] = (ramp_t){ 0, 32, 0.25f, 0.45f, &v.p.cutoff };
        v.p.reso = 0.8f; v.p.envmod = 0.7f;
        v.p.dist_type = FM313_DIST_DIST; v.p.dist_drive = 0.5f; v.p.dist_tone = 0.5f;
        fx.delay_mix = 0.22f; fx.delay_fb = 0.45f; fx.delay_div = 3;   /* dotted 8th */
        break;
    case 4:   /* 4-oscillator bass with delay + reverb */
        v.p.wave = FM313_SUPER; v.p.reso = 0.55f; v.p.envmod = 0.45f; v.p.decay = 0.6f;
        pt = "C, C C, G, A#, C _ C, C,A G,S A#, C' - C, D#S F";
        r[nr++] = (ramp_t){ 0, 32, 0.15f, 0.6f, &v.p.cutoff };
        fx.delay_mix = 0.25f; fx.delay_fb = 0.5f; fx.delay_div = 2;
        fx.rev_mix = 0.18f;
        bpm = 122.f;
        break;
    case 5:   /* dry reference: single notes at four settings */
        pt = "C,A - - - C - - - C' - - - C,S G, - - -";
        v.p.cutoff = 0.3f; v.p.reso = 0.5f;
        r[nr++] = (ramp_t){ 0, 32, 0.0f, 1.0f, &v.p.reso };
        bpm = 100.f;
        break;
    }
    parse_pattern(&pat, pt);
    fm313_seq_init(&q, &pat, bpm); q.shuffle = shuffle; q.root = 36;
    fm313_seq_start(&q);
    fx.bpm = bpm;

    int N = (int)(secs*FM313_FS);
    float *mono = calloc(N, sizeof(float));
    float *st = calloc(N*2, sizeof(float));
    int pos = 0;
    while (pos < N) {
        float t = pos/FM313_FS;
        for (int i = 0; i < nr; i++) {
            float u = (t - r[i].t0)/(r[i].t1 - r[i].t0);
            if (u < 0) u = 0; if (u > 1) u = 1;
            *r[i].dst = r[i].v0 + (r[i].v1 - r[i].v0)*u;
        }
        int m = fm313_seq_advance(&q, &v, 32);
        if (pos + m > N) m = N - pos;
        fm313_render(&v, mono + pos, m);
        fm313_fx_process(&fx, mono + pos, st + 2*pos, m);
        pos += m;
    }
    float pk = 0; double rms = 0;
    for (int i = 0; i < 2*N; i++) { float a = fabsf(st[i]); if (a > pk) pk = a; rms += st[i]*st[i]; }
    fprintf(stderr, "scene %d: peak %.3f rms %.3f kcrit %.2f\n", scene, pk, sqrt(rms/(2*N)), fm313_kcrit());
    write_wav(argv[1], st, N);
    return 0;
}
