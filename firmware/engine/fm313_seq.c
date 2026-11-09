/* SPDX-License-Identifier: GPL-3.0-only */
#include "fm313_seq.h"

static uint32_t xr(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

int fm313_seq_len(const fm313_pattern_t *p) { return p->len ? (p->len > FM313_STEPS ? FM313_STEPS : p->len) : 16; }

/* a step's length: a 16th (shuffled: even steps long, odd ones short), or an 8th triplet; SCATTER HALF stretches */
static float step_len(const fm313_seq_t *q, int pos)
{
    float L;
    if (q->pat->pflags & FM313_P_TRIP) L = FM313_FS*60.f/(q->bpm*3.f);
    else {
        float base = FM313_FS*60.f/(q->bpm*4.f);
        float sh = q->shuffle*(1.f/3.f);           /* max: 2:1 swing */
        L = (pos & 1) ? base*(1.f - sh) : base*(1.f + sh);
    }
    if (q->sc_type == SC_HALF) L *= q->sc_depth >= 9 ? 4.f : 2.f;
    return L;
}

void fm313_seq_init(fm313_seq_t *q, fm313_pattern_t *p, float bpm)
{
    char *c = (char *)q;
    for (unsigned i = 0; i < sizeof *q; i++) c[i] = 0;
    q->pat = p; q->bpm = bpm; q->root = 36;
    q->to_gate_off = -1.f;
    q->ext_tick_len = FM313_FS*60.f/(bpm*24.f);
    q->chain_n = 1;
    q->sc_depth = 5;
    q->rng = 0x2545F491u;
}

void fm313_seq_start(fm313_seq_t *q)
{
    q->playing = 1; q->pos = -1; q->step = 0; q->to_step = 0.f; q->sub_left = 0;
    q->prev_slide = 0; q->to_gate_off = -1.f;
    q->ext_ticks = 0;
    q->chain_i = 0;
    if (q->chain_n > 1 && q->chain_base) q->pat = q->chain_base;
}

static void gate_off(fm313_seq_t *q, fm313_voice_t *v)
{
    if (v->gate) q->n_off++;
    fm313_gate_off(v);
}

void fm313_seq_stop(fm313_seq_t *q, fm313_voice_t *v)
{
    q->playing = 0;
    q->to_gate_off = -1.f;
    q->sub_left = 0;
    gate_off(q, v);
}

void fm313_seq_scatter(fm313_seq_t *q, int type, int depth)
{
    if (type < 0 || type >= SC_NUM) type = SC_OFF;
    if (depth < 1) depth = 1;
    if (depth > 16) depth = 16;
    if (type != SC_OFF && q->sc_type == SC_OFF) {
        int len = fm313_seq_len(q->pat);
        q->sc_anchor = q->pos < 0 ? 0 : (q->pos + 1) % len;   /* REPEAT loops from the next step */
    }
    q->sc_type = type; q->sc_depth = depth;
}

float fm313_seq_phase(const fm313_seq_t *q)
{
    if (q->cur_len <= 0.f) return 0.f;
    float f = 1.f - q->to_step/q->cur_len;
    return f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
}

/* the pattern after this one: a queued one first, else the next of a chain */
static fm313_pattern_t *following(const fm313_seq_t *q, int *ci)
{
    *ci = q->chain_i;
    if (q->next_pat) return q->next_pat;
    if (q->chain_n > 1 && q->chain_base) { *ci = (q->chain_i + 1) % q->chain_n; return q->chain_base + *ci; }
    return q->pat;
}

static int next_is_tie(const fm313_seq_t *q)
{
    const fm313_pattern_t *p = q->pat;
    int n = q->pos + 1, ci;
    if (n >= fm313_seq_len(p)) { p = following(q, &ci); n = 0; }
    return p->s[n].type == FM313_TIE;
}

/* one hit of the step idx (a ratchet's later hits: again, never sliding into it) */
static void play_hit(fm313_seq_t *q, fm313_voice_t *v, int idx, int rest, float gate, int nohold)
{
    const fm313_step_t *s = &q->pat->s[idx];
    float sl = q->sub_len;
    if (rest || s->type == FM313_REST) {
        if (!q->prev_slide) gate_off(q, v);
        else q->to_gate_off = sl*0.5f;      /* a slide into a rest still holds half a step */
        q->prev_slide = 0;
        return;
    }
    int slide = !nohold && (s->flags & FM313_F_SLIDE) != 0;
    int hold  = !nohold && (slide || next_is_tie(q));
    if (s->type == FM313_TIE) {
        if (v->gate) q->to_gate_off = hold ? -1.f : sl*gate;
        q->prev_slide = slide;
        return;
    }
    int note = q->root + q->transpose + s->oct*12 + s->note;
    int acc = (s->flags & FM313_F_ACC) != 0;
    fm313_note_on(v, note, acc, q->prev_slide);
    q->on_note = note; q->on_acc = acc; q->on_slide = q->prev_slide; q->n_on++;
    q->prev_slide = slide;
    q->to_gate_off = hold ? -1.f : sl*gate;
}

static float hit_gate(const fm313_seq_t *q)
{
    if (q->sc_type == SC_GATE) return 0.5f - 0.028f*(float)q->sc_depth;   /* depth 16: a 5 % blip */
    if (q->sc_type == SC_RATCHET || q->sc_type == SC_STUTTER) return 0.45f;
    return 0.5f;
}

/* the next logical step, read through SCATTER: which step sounds, how many hits, or none */
static void begin_step(fm313_seq_t *q, fm313_voice_t *v, float L)
{
    q->pos++;
    if (q->pos >= fm313_seq_len(q->pat)) {
        int ci;
        fm313_pattern_t *n = following(q, &ci);
        q->pos = 0;
        if (q->next_pat) q->next_pat = 0;
        q->chain_i = ci;
        q->pat = n;
    }
    int len = fm313_seq_len(q->pat), pos = q->pos, idx = pos, nsub = 1, rest = 0, nohold = 0, d = q->sc_depth;
    uint32_t r = xr(&q->rng) % 100u;
    switch (q->sc_type) {
    case SC_REVERSE: {                                /* backwards in groups of 2 .. 32 steps */
        int g = d <= 3 ? 2 : d <= 6 ? 4 : d <= 9 ? 8 : d <= 12 ? 16 : 32;
        if (g > len) g = len;
        int gs = pos/g*g, ge = gs + g > len ? len : gs + g;
        idx = gs + (ge - 1 - pos);
        break; }
    case SC_REPEAT: {                                 /* a loop of 16 .. 1 steps from where it began */
        int k = 17 - d;
        if (k > len) k = len;
        idx = (q->sc_anchor + ((pos - q->sc_anchor + len) % len) % k) % len;
        break; }
    case SC_RATCHET: nsub = d <= 5 ? 2 : (d <= 11 ? 3 : 4); nohold = 1; break;
    case SC_GATE: nohold = 1; break;
    case SC_SKIP: if (pos && r < (uint32_t)(d*6)) rest = 1; break;
    case SC_RANDOM: if (r < (uint32_t)(d*6)) idx = (int)(xr(&q->rng) % (uint32_t)len); break;
    case SC_STUTTER: if (r < (uint32_t)(d*6)) { nsub = d > 10 ? 3 : 2; nohold = 1; } break;
    default: break;
    }
    if (idx < 0 || idx >= len) idx = pos;
    q->step = idx;
    q->sub_len = L/(float)nsub;
    q->sub_left = nsub - 1;
    q->cur_len = q->sub_len;
    play_hit(q, v, idx, rest, hit_gate(q), nohold || nsub > 1);
}

static void sub_hit(fm313_seq_t *q, fm313_voice_t *v)
{
    q->sub_left--;
    q->prev_slide = 0;
    q->cur_len = q->sub_len;
    play_hit(q, v, q->step, 0, hit_gate(q), 1);
}

int fm313_seq_advance(fm313_seq_t *q, fm313_voice_t *v, int n)
{
    if (!q->playing) return n;
    if (q->to_gate_off >= 0.f && q->to_gate_off < 0.5f) {
        gate_off(q, v); q->to_gate_off = -1.f;
    }
    if (!q->ext_clock) {
        while (q->to_step < 0.5f) {
            if (q->sub_left > 0) sub_hit(q, v);
            else begin_step(q, v, step_len(q, q->pos + 1));
            q->to_step += q->sub_len;
        }
    } else {
        while (q->sub_left > 0 && q->to_step < 0.5f) {   /* MIDI clock: a ratchet's later hits by our own time */
            sub_hit(q, v);
            q->to_step += q->sub_len;
        }
    }
    float next = (!q->ext_clock || q->sub_left > 0) ? q->to_step : (float)n;
    if (q->to_gate_off >= 0.f && q->to_gate_off < next) next = q->to_gate_off;
    int m = (int)(next + 0.5f);
    if (m < 1) m = 1;
    if (m > n) m = n;
    q->to_step -= (float)m;
    if (q->to_gate_off >= 0.f) q->to_gate_off -= (float)m;
    q->since_tick += (float)m;
    return m;
}

void fm313_seq_midi_clock(fm313_seq_t *q, fm313_voice_t *v)
{
    /* measure the clock for gate lengths and the tempo display */
    if (q->since_tick > 10.f && q->since_tick < FM313_FS)
        q->ext_tick_len += 0.2f*(q->since_tick - q->ext_tick_len);
    q->since_tick = 0.f;
    if (!q->playing || !q->ext_clock) return;
    q->bpm = FM313_FS*60.f/(q->ext_tick_len*24.f);
    int fire = 0, ticks = 6;
    if (q->pat->pflags & FM313_P_TRIP) { ticks = 8; fire = q->ext_ticks % 8 == 0; }   /* an 8th triplet: 8 clocks */
    else {
        int sh_ticks = (int)(q->shuffle*2.f + 0.5f);              /* shuffle: odd steps start late (a 16th = 6) */
        int t = q->ext_ticks % 12;
        fire = t == 0 || t == 6 + sh_ticks;
    }
    if (q->sc_type == SC_HALF) {                                  /* HALF: every 2nd (4th) step time */
        int every = q->sc_depth >= 9 ? 4 : 2;
        if (fire && (q->ext_ticks/ticks) % every) fire = 0;
        ticks *= every;
    }
    if (fire) {
        begin_step(q, v, q->ext_tick_len*(float)ticks);
        q->to_step = q->sub_len;
    }
    q->ext_ticks++;
}

void fm313_pattern_clear(fm313_pattern_t *p)
{
    for (int i = 0; i < FM313_STEPS; i++) {
        p->s[i].note = 0; p->s[i].oct = 0; p->s[i].type = FM313_REST; p->s[i].flags = 0;
    }
    p->len = 16;
    p->pflags &= FM313_P_LOCK;                 /* (the app does not clear a locked one) */
    p->tune10 = 0; p->tone = 0;
}

static const uint8_t SCALE[8] = { 0, 0, 0, 3, 5, 7, 10, 12 };   /* root-heavy minor pentatonic */

void fm313_pattern_random(fm313_pattern_t *p, uint32_t *seed, int len)
{
    p->len = (uint8_t)len;
    for (int i = 0; i < FM313_STEPS; i++) {
        fm313_step_t *s = &p->s[i];
        uint32_t r = xr(seed);
        s->note = SCALE[r & 7]; r >>= 3;
        uint32_t o = r % 10; r /= 10;
        s->oct = (int8_t)(o == 0 ? 1 : (o == 1 ? -1 : 0));
        uint32_t t = r % 12; r /= 12;
        s->type = (uint8_t)(t < 2 ? FM313_REST : (t == 2 && i ? FM313_TIE : FM313_NOTE));
        s->flags = 0;
        if (r % 4 == 0) s->flags |= FM313_F_ACC;
        r /= 4;
        if (r % 5 == 0) s->flags |= FM313_F_SLIDE;
    }
}

void fm313_pattern_mutate(fm313_pattern_t *p, uint32_t *seed)
{
    int len = fm313_seq_len(p), n = len/4 < 2 ? 2 : len/4;
    for (int k = 0; k < n; k++) {
        fm313_step_t *s = &p->s[xr(seed) % (uint32_t)len];
        switch (xr(seed) % 6u) {
        case 0: case 1:                               /* another note of the scale */
            if (s->type == FM313_NOTE) s->note = SCALE[xr(seed) & 7]; else { s->type = FM313_NOTE; s->note = SCALE[xr(seed) & 7]; }
            break;
        case 2: s->flags ^= FM313_F_ACC; break;
        case 3: s->flags ^= FM313_F_SLIDE; break;
        case 4: if (s->type == FM313_NOTE) s->oct = (int8_t)(s->oct == 0 ? (xr(seed) & 1 ? 1 : -1) : 0); break;
        default: s->type = (uint8_t)(s->type == FM313_REST ? FM313_NOTE : FM313_REST); break;
        }
        if (s->note > 11 && s->oct > 0) s->note = 11;
    }
}

int fm313_pattern_shift(fm313_pattern_t *p, int semis)
{
    int folded = 0;
    for (int i = 0; i < FM313_STEPS; i++) {
        fm313_step_t *s = &p->s[i];
        if (s->type != FM313_NOTE) continue;
        int a = s->oct*12 + s->note + semis;      /* -12 .. 23 can be written */
        while (a > 23) { a -= 12; folded++; }
        while (a < -12) { a += 12; folded++; }
        int o = a < 0 ? -1 : (a < 12 ? 0 : 1);
        s->oct = (int8_t)o; s->note = (uint8_t)(a - 12*o);
    }
    return folded;
}

uint16_t fm313_step_pack(const fm313_step_t *s)
{
    return (uint16_t)((s->note & 15) | (((s->oct + 1) & 3) << 4) | ((s->type & 3) << 6) | ((s->flags & 3) << 8));
}
void fm313_step_unpack(fm313_step_t *s, uint16_t v)
{
    s->note = (uint8_t)(v & 15); if (s->note > 12) s->note = 12;
    s->oct = (int8_t)(((v >> 4) & 3) - 1); if (s->oct > 1) s->oct = 1;
    s->type = (uint8_t)((v >> 6) & 3); if (s->type > 2) s->type = FM313_REST;
    s->flags = (uint8_t)((v >> 8) & 3);
}
