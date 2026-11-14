/* SPDX-License-Identifier: GPL-3.0-only
 * FM-313 app: controls, screen, patterns, storage. See docs/MANUAL.md.
 */
#include "fm313_app.h"
#include "fm313_gfx.h"
#ifndef FM313_VERSION
#define FM313_VERSION "0.7.7"
#endif

/* ---------------------------------------------------------------- state */
enum { M_PLAY = 0, M_WRITE, M_GLOBAL };
enum { EV_NOTE_ON = 1, EV_NOTE_OFF, EV_START, EV_STOP, EV_PATTERN, EV_TRANSPOSE, EV_CHAIN, EV_SCATTER, EV_MIDI };

typedef struct { uint8_t type, a, b, c; } fm313_ev_t;

/* what is saved */
typedef struct {
    float cutoff, reso, envmod, decay, accent, tune, level;
    float drive, tone, dmix, dfb, rmix, rtime;
    float bpm, shuffle;
    uint8_t wave, dtype, ddiv, pattern;
    uint8_t midi_ch, ext_clock, kb_oct, pad;
    uint8_t preset, midi_out, sc_type, sc_depth;   /* 0.4: the sound's preset (0 none), MIDI out, SCATTER (depth 1-16: the white key of that number) */
} fm313_sound_t;
typedef struct {
    uint8_t touch;      /* 0.5-0.6.2: the keys' velocity and pressure (no longer used) */
    uint8_t vel_thr;    /* a key struck at this velocity or harder plays (writes) an accent */
    uint8_t local_off;  /* the keys only send MIDI */
    uint8_t scope_mode; /* 0 WAVE, 1 LEVEL, 2 OFF */
    uint8_t press_to;   /* what a key's pressure does: 0 FILTER, 1 VIBRATO, 2 OFF */
    uint8_t phones;     /* 0.5.1: 1 LEFT FIX, with headphones in: for a jack whose left side hears L - R */
    uint8_t tips_off;   /* 0.5.1: the help line under the scope (0: shown) */
    uint8_t leds;       /* 0.6.5: 0 NORMAL, 1 NIGHT, 2 NIGHT+ (the unlit LEDs glow) */
    uint8_t rsv[8];
} fm313_setup_t;

struct fm313_app_s {
    fm313_voice_t   v;
    fm313_seq_t     q;
    fm313_pattern_t pat[FM313_NPAT];
    fm313_pattern_t undo, clip;
    fm313_sound_t   s;
    fm313_setup_t   su;
    /* ui */
    int mode, fx_page, shift, cursor, kb_oct;
    int cur;                       /* pattern on screen / being edited */
    int queued;                    /* pattern waiting for the bar end, -1 none */
    int held_key;                  /* live note key held, -1 none */
    uint8_t key_down[FM313_NKEYS];
    uint32_t edit_down_ms, msg_ms;
    char msg[24];
    char msg_name[24], msg_val_text[24];
    int msg_val, msg_has_val, msg_ladder, msg_red, scope_live;
    uint32_t msg_hold;
    uint32_t tap[4]; int ntap;
    uint32_t seed;
    int dirty_top, dirty_grid, dirty_knobs, dirty_msg;
    int last_step_drawn;
    uint32_t last_draw_ms, fx_ms, boot_until, touch_ms;
    int panel_dirty;
    uint32_t key_bits;             /* soft keys to redraw */
    uint32_t upd_ms;               /* SETUP: SAVE pressed once */
    uint32_t t_scope, t_field;     /* longest scope / other field draw in the last second, us */    /* SETUP: the trace off (a test: does the crackle follow the drawing?) */
    const void *touched;           /* the value turned last: its soft key lights */
    /* 0.5: hold layers (a button held: the keys and knobs do its things), latches, the keys' touch */
    int layer, lay_used;           /* L_*: the layer of the button held last; it did something meanwhile */
    uint32_t lay_down_ms;
    int menu;                      /* the menu a tap left open (L_FX .. L_LFO), L_NONE: none; one at a time */
    int bank;                      /* SEQ: the bank the white keys pick from (A..D) */
    int seq_first;                 /* SEQ: the white key held first (a chain to the next one pressed), -1 */
    uint8_t key_vel[FM313_NKEYS], key_p[FM313_NKEYS];
    int env_key, env_dec_key;      /* ENV ribbon: the keys holding env mod / decay, -1 */
    uint32_t save_ms; int save_held;
    int hp_in;                     /* headphones plugged (the jack's switch, debounced) */
    char msg_sub[40];              /* a second line under a message: what the button also does */
    int su_cur;                    /* SETUP's cursor */
    int midi_held, midi_sc;        /* audio side: MIDI notes held; SCATTER on by CC 102 */
    /* 0.4, after the TB-3's feature list */
    int rt_rec, rec_erase;         /* realtime record armed (SEL+REC); EDIT held: erasing */
    int rec_key, rec_last;         /* the key being recorded, -1 none; the step it was last written to */
    int wr_key, wr_prev;
    uint32_t hear_ms;              /* a step heard while set (stopped): its note ends after a moment */           /* step write: the key that wrote last (held: the next one slides), its step */
    int rec_step_seen;             /* the play step the record pass last looked at */
    int chain_n;                   /* patterns chained from chain_start (1: none) */
    int chain_start;
    int home_down, sc_latched, sc_used;   /* SCATTER: sc_latched = running (a kind picked in the menu) */
    int tone_pending;              /* a pattern with a sound started: load it (main loop) */
    const fm313_pattern_t *audio_pat;   /* audio side: the pattern whose tune is applied */
    uint32_t mo_on, mo_off; int mo_note;   /* MIDI out bookkeeping (audio side) */
    float clk_acc; int clk_was;
    uint32_t store_seq;
    /* audio side */
    volatile uint32_t ev_w, ev_r;
    fm313_ev_t ev[32];
    volatile int play_step;
    volatile int playing;
    float mono[64];
};

fm313_app_t fm313;
static fm313_fx_t fx __attribute__((section(".pool")));

/* ---------------------------------------------------------------- helpers */
static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

static void put_num(char *d, int v, int digits)   /* zero-padded */
{
    for (int i = digits - 1; i >= 0; i--) { d[i] = (char)('0' + v % 10); v /= 10; }
    d[digits] = 0;
}
static char *cat(char *d, const char *s) { while (*d) d++; while (*s) *d++ = *s++; *d = 0; return d; }
static void cat_int(char *d, int v)
{
    char b[12]; int n = 0, neg = v < 0;
    if (neg) v = -v;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (*d) d++;
    if (neg) *d++ = '-';
    while (n) *d++ = b[--n];
    *d = 0;
}
static void message(const char *a, int val, int show_val)
{
    fm313.msg[0] = 0;
    cat(fm313.msg, a);
    if (show_val) { cat(fm313.msg, " "); cat_int(fm313.msg, val); }
    fm313.msg_ms = hw_ms();
    fm313.dirty_msg = 1;
    /* the PLAY panel: a short name over a big value; a long text ("CLOCK: INTERNAL") splits at its colon */
    fm313.msg_name[0] = fm313.msg_val_text[0] = 0;
    fm313.msg_val = val; fm313.msg_has_val = show_val; fm313.msg_ladder = 0; fm313.msg_red = 0;
    fm313.msg_hold = 1500u;
    fm313.msg_sub[0] = 0;
    const char *c = a; while (*c && *c != ':') c++;
    if (*c == ':' && !show_val) {
        int n = (int)(c - a); if (n > 23) n = 23;
        for (int i = 0; i < n; i++) fm313.msg_name[i] = a[i];
        fm313.msg_name[n] = 0;
        c++; while (*c == ' ') c++;
        cat(fm313.msg_val_text, c);
    } else if (show_val) cat(fm313.msg_name, a);
    else cat(fm313.msg_val_text, a);
}

void fm313_app_notice(const char *what, int value) { message(what, value, 1); }
static void msg_sub(const char *t)                 /* the line under the message: what else the button does */
{
    fm313.msg_sub[0] = 0; cat(fm313.msg_sub, t);
    fm313.msg_hold = 3200u; fm313.dirty_msg = 1;
}

static void post(int type, int a, int b, int c)
{
    uint32_t w = fm313.ev_w;
    if (w - fm313.ev_r >= 32u) return;               /* full: drop (never blocks the UI) */
    fm313_ev_t *e = &fm313.ev[w & 31u];
    e->type = (uint8_t)type; e->a = (uint8_t)a; e->b = (uint8_t)b; e->c = (uint8_t)c;
    __asm__ volatile("" ::: "memory");
    fm313.ev_w = w + 1u;
}

/* knobs -> engine (floats: written whole, read by the ISR as they come) */
static float preset_gain(void);
static void apply_sound(void)
{
    fm313_sound_t *s = &fm313.s;
    fm313_params_t *p = &fm313.v.p;
    p->cutoff = s->cutoff; p->reso = s->reso; p->envmod = s->envmod; p->decay = s->decay;
    p->accent = s->accent; p->wave = s->wave; p->level = s->level*preset_gain();
    p->tune = s->tune + 0.1f*(float)fm313.pat[fm313.cur].tune10;   /* master + the pattern's tune (audio: as patterns change) */
    p->dist_type = s->dtype; p->dist_drive = s->drive; p->dist_tone = s->tone;
    fx.delay_mix = s->dmix; fx.delay_fb = s->dfb; fx.delay_div = s->ddiv;
    fx.rev_mix = s->rmix; fx.rev_time = s->rtime;
    fx.bpm = s->bpm;
    fm313.q.bpm = s->bpm; fm313.q.shuffle = s->shuffle;
    fm313.q.ext_clock = s->ext_clock;
}

/* ---------------------------------------------------------------- default patterns (original) */
static const char *const DEMO[] = {
    "C, C C'A C, D#S F C,A C' C, A#,S C C'A G, C C,A D#'S",
    "C,A C C, C'S C' C C,A D# C,S F#,S G, C C'A C, A#, G,A",
    "C, C,S C _ C' - C,A D#S F G,A C - A#,S C' C,A -",
    "C,A C,A C,A C,A - C,A C,A C,A C'A C'A - C,A C,A D#,A C,A G,AS",
    "C, C C, G, A#, C _ C, C,A G,S A#, C' - C, D#S F",
    "F,A F, F G#,S F, C F,A - F, D#'S F F,A - C G#, F",
    "C, - C,A C,S C - D#,A C, G,S G, C, A#,A - C, F,S G,",
    "D,A D D, - D,S A, D C'A D, D,S D' D, F,A D - G,S",
};

static void parse_pattern(fm313_pattern_t *p, const char *t)
{
    static const char NM[] = "C D EF G A B";      /* sharps follow with '#' */
    fm313_pattern_clear(p);
    int n = 0;
    while (*t && n < FM313_STEPS) {
        while (*t == ' ') t++;
        if (!*t) break;
        fm313_step_t *s = &p->s[n++];
        s->type = FM313_NOTE; s->oct = 0; s->flags = 0; s->note = 0;
        if (*t == '-') { s->type = FM313_REST; t++; }
        else if (*t == '_') { s->type = FM313_TIE; t++; }
        else {
            for (int i = 0; i < 12; i++) if (NM[i] == *t) s->note = (uint8_t)i;
            t++;
        }
        while (*t && *t != ' ') {
            if (*t == '#') s->note++;
            else if (*t == '\'') s->oct++;
            else if (*t == ',') s->oct--;
            else if (*t == 'A') s->flags |= FM313_F_ACC;
            else if (*t == 'S') s->flags |= FM313_F_SLIDE;
            t++;
        }
        if (s->note > 11) { s->note -= 12; s->oct++; }
        if (s->oct > 1) s->oct = 1;
        if (s->oct < -1) s->oct = -1;
    }
    p->len = (uint8_t)(n ? n : 16);
}

/* SOUND: FM-313's own presets (not the TB-3's): the knobs, the wave and the effects in one go. */
typedef struct { const char *name; float cut, res, env, dec, acc; uint8_t wave, dtype; float drive, tone, dmix, dfb; uint8_t ddiv; float rmix, rtime; float gain; } preset_t;
static const preset_t PRESETS[] = {
    { "CLASSIC",   0.35f, 0.72f, 0.55f, 0.45f, 0.70f, FM313_SAW,    FM313_DIST_OFF,  0.40f, 0.50f, 0.00f, 0.45f, 3, 0.00f, 0.50f, 0.98f },
    { "SQUARE",    0.40f, 0.65f, 0.50f, 0.40f, 0.65f, FM313_SQUARE, FM313_DIST_OFF,  0.40f, 0.50f, 0.00f, 0.45f, 3, 0.00f, 0.50f, 0.92f },
    { "SQUELCH",   0.22f, 0.92f, 0.80f, 0.55f, 0.85f, FM313_SAW,    FM313_DIST_OFF,  0.40f, 0.50f, 0.00f, 0.45f, 3, 0.00f, 0.50f, 0.40f },
    { "DEEP",      0.15f, 0.40f, 0.30f, 0.70f, 0.40f, FM313_SAW,    FM313_DIST_OFF,  0.30f, 0.30f, 0.00f, 0.45f, 3, 0.10f, 0.40f, 1.47f },
    { "RUBBER",    0.30f, 0.55f, 0.45f, 0.25f, 0.60f, FM313_SQUARE, FM313_DIST_OD,   0.35f, 0.40f, 0.00f, 0.45f, 3, 0.00f, 0.50f, 0.54f },
    { "ACID DRIVE",0.32f, 0.85f, 0.65f, 0.50f, 0.80f, FM313_SAW,    FM313_DIST_OD,   0.60f, 0.55f, 0.00f, 0.45f, 3, 0.00f, 0.50f, 0.43f },
    { "HARD DIST", 0.30f, 0.80f, 0.60f, 0.45f, 0.75f, FM313_SAW,    FM313_DIST_DIST, 0.75f, 0.60f, 0.00f, 0.45f, 3, 0.00f, 0.50f, 0.49f },
    { "FUZZ BASS", 0.25f, 0.60f, 0.50f, 0.50f, 0.60f, FM313_SQUARE, FM313_DIST_FUZZ, 0.65f, 0.45f, 0.00f, 0.45f, 3, 0.00f, 0.50f, 0.60f },
    { "CRUNCH",    0.38f, 0.75f, 0.60f, 0.40f, 0.70f, FM313_SAW,    FM313_DIST_CRUNCH,0.55f,0.55f, 0.00f, 0.45f, 3, 0.00f, 0.50f, 0.54f },
    { "DUB DELAY", 0.30f, 0.78f, 0.55f, 0.45f, 0.70f, FM313_SAW,    FM313_DIST_OFF,  0.40f, 0.50f, 0.45f, 0.60f, 3, 0.10f, 0.50f, 0.77f },
    { "PING PONG", 0.40f, 0.70f, 0.50f, 0.35f, 0.65f, FM313_SQUARE, FM313_DIST_OD,   0.30f, 0.50f, 0.35f, 0.50f, 1, 0.00f, 0.50f, 0.46f },
    { "SPACE",     0.35f, 0.82f, 0.60f, 0.60f, 0.70f, FM313_SAW,    FM313_DIST_OFF,  0.40f, 0.50f, 0.25f, 0.55f, 4, 0.40f, 0.80f, 0.49f },
    { "4-OSC LEAD",0.55f, 0.55f, 0.45f, 0.55f, 0.60f, FM313_SUPER,  FM313_DIST_OD,   0.35f, 0.60f, 0.25f, 0.40f, 3, 0.20f, 0.60f, 0.67f },
    { "SUPER BASS",0.25f, 0.45f, 0.40f, 0.50f, 0.55f, FM313_SUPER,  FM313_DIST_OFF,  0.40f, 0.50f, 0.00f, 0.45f, 3, 0.05f, 0.40f, 1.92f },
    /* 0.7.1: eight more (gains by tools/preset_loudness.py, matched to the fourteen above) */
    { "PLUCK",     0.28f, 0.50f, 0.70f, 0.12f, 0.60f, FM313_SQUARE, FM313_DIST_OFF,  0.40f, 0.50f, 0.00f, 0.45f, 3, 0.00f, 0.50f, 1.15f },
    { "LIQUID",    0.26f, 0.90f, 0.70f, 0.65f, 0.90f, FM313_SAW,    FM313_DIST_OFF,  0.40f, 0.50f, 0.20f, 0.40f, 3, 0.15f, 0.50f, 0.40f },
    { "HOLLOW",    0.50f, 0.30f, 0.20f, 0.60f, 0.50f, FM313_SQUARE, FM313_DIST_OFF,  0.40f, 0.50f, 0.00f, 0.45f, 3, 0.12f, 0.45f, 1.15f },
    { "SUB",       0.10f, 0.20f, 0.15f, 0.80f, 0.40f, FM313_SQUARE, FM313_DIST_OFF,  0.30f, 0.30f, 0.00f, 0.45f, 3, 0.00f, 0.50f, 1.29f },
    { "WOW",       0.18f, 0.88f, 0.90f, 0.75f, 0.95f, FM313_SAW,    FM313_DIST_OFF,  0.40f, 0.50f, 0.00f, 0.45f, 3, 0.00f, 0.50f, 0.38f },
    { "ACID FUZZ", 0.30f, 0.85f, 0.70f, 0.50f, 0.80f, FM313_SAW,    FM313_DIST_FUZZ, 0.60f, 0.55f, 0.00f, 0.45f, 3, 0.00f, 0.50f, 0.49f },
    { "WAREHOUSE", 0.32f, 0.78f, 0.60f, 0.50f, 0.75f, FM313_SAW,    FM313_DIST_DIST, 0.60f, 0.50f, 0.00f, 0.45f, 3, 0.35f, 0.70f, 0.48f },
    { "4-OSC ACID",0.30f, 0.80f, 0.65f, 0.50f, 0.75f, FM313_SUPER,  FM313_DIST_OD,   0.45f, 0.55f, 0.00f, 0.45f, 3, 0.00f, 0.50f, 0.40f },
};
#define NPRESETS ((int)(sizeof PRESETS/sizeof PRESETS[0]))
static void load_preset(int i)                      /* i: 0-based */
{
    const preset_t *p = &PRESETS[clampi(i, 0, NPRESETS - 1)];
    fm313_sound_t *s = &fm313.s;
    s->cutoff = p->cut; s->reso = p->res; s->envmod = p->env; s->decay = p->dec; s->accent = p->acc;
    s->wave = p->wave; s->dtype = p->dtype; s->drive = p->drive; s->tone = p->tone;
    s->dmix = p->dmix; s->dfb = p->dfb; s->ddiv = p->ddiv; s->rmix = p->rmix; s->rtime = p->rtime;
    s->preset = (uint8_t)(i + 1);
}
/* the presets' loudness evened out by ear-weighted loudness, not RMS (0.5.5: ITU-R BS.1770 integrated loudness over
 * the demo patterns A01-A03, the gains iterated until each sits at -13 LUFS of the app's output, CLASSIC at -12.5;
 * by RMS the distorted ones had been 2-3 dB louder to the ear) */
static float preset_gain(void) { return fm313.s.preset ? PRESETS[fm313.s.preset - 1].gain : 1.f; }

static void defaults(void)
{
    fm313_sound_t *s = &fm313.s;
    s->cutoff = 0.35f; s->reso = 0.72f; s->envmod = 0.55f; s->decay = 0.45f; s->accent = 0.7f;
    s->tune = 0.f; s->level = 0.8f;
    s->drive = 0.4f; s->tone = 0.5f; s->dmix = 0.f; s->dfb = 0.45f; s->rmix = 0.f; s->rtime = 0.5f;
    s->bpm = 126.f; s->shuffle = 0.f;
    s->wave = FM313_SAW; s->dtype = FM313_DIST_OFF; s->ddiv = 3; s->pattern = 0;
    s->midi_ch = 0; s->ext_clock = 0; s->kb_oct = 1;
    s->preset = 1; s->midi_out = 0; s->sc_type = SC_REVERSE; s->sc_depth = 5;
    fm313.su.touch = 1; fm313.su.vel_thr = 90; fm313.su.local_off = 0; fm313.su.scope_mode = 0; fm313.su.press_to = 0;
    for (int i = 0; i < FM313_NPAT; i++) {
        if (i < (int)(sizeof DEMO/sizeof DEMO[0])) parse_pattern(&fm313.pat[i], DEMO[i]);
        else fm313_pattern_clear(&fm313.pat[i]);
    }
}

/* ---------------------------------------------------------------- storage
 * Two banks of 8 KiB at 0xD0000 / 0xD2000 (inside Felucca's data area, which
 * FM-313 does not otherwise use). Each save goes to the older bank: a cut save
 * leaves the other bank whole. */
#define ST_BANK0 0x000D0000u
#define ST_BANK_LEN 0x2000u
#define ST_MAGIC 0x33313346u              /* "F313": the patterns, the sound, SETUP's choices */

typedef struct { uint32_t magic, seq, len, crc; } st_hdr_t;
#define ST_PAT_BYTES (FM313_NPAT*(FM313_STEPS*2 + 4))
#define ST_BODY (ST_PAT_BYTES + (uint32_t)sizeof(fm313_sound_t) + (uint32_t)sizeof(fm313_setup_t))
static uint8_t st_buf[(ST_BODY + 3u) & ~3u];

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}

static void st_pack(void)
{
    uint8_t *d = st_buf;
    for (int i = 0; i < FM313_NPAT; i++) {
        for (int j = 0; j < FM313_STEPS; j++) {
            uint16_t v = fm313_step_pack(&fm313.pat[i].s[j]);
            *d++ = (uint8_t)v; *d++ = (uint8_t)(v >> 8);
        }
        *d++ = fm313.pat[i].len; *d++ = fm313.pat[i].pflags; *d++ = (uint8_t)fm313.pat[i].tune10; *d++ = fm313.pat[i].tone;
    }
    const uint8_t *s = (const uint8_t *)&fm313.s;
    for (uint32_t k = 0; k < sizeof(fm313_sound_t); k++) *d++ = s[k];
    const uint8_t *u = (const uint8_t *)&fm313.su;
    for (uint32_t k = 0; k < sizeof(fm313_setup_t); k++) *d++ = u[k];
}

static int st_unpack(void)
{
    const uint8_t *d = st_buf;
    for (int i = 0; i < FM313_NPAT; i++) {
        for (int j = 0; j < FM313_STEPS; j++) {
            fm313_step_unpack(&fm313.pat[i].s[j], (uint16_t)(d[0] | d[1] << 8));
            d += 2;
        }
        fm313_pattern_t *pp = &fm313.pat[i];
        pp->len = (uint8_t)clampi(d[0], 1, FM313_STEPS);
        pp->pflags = (uint8_t)(d[1] & (FM313_P_TRIP | FM313_P_LOCK));
        pp->tune10 = (int8_t)clampi((int8_t)d[2], -70, 70);
        pp->tone = (uint8_t)(d[3] <= NPRESETS ? d[3] : 0);
        d += 4;
    }
    uint8_t *s = (uint8_t *)&fm313.s;
    for (uint32_t k = 0; k < sizeof(fm313_sound_t); k++) s[k] = *d++;
    { uint8_t *u = (uint8_t *)&fm313.su; for (uint32_t k = 0; k < sizeof(fm313_setup_t); k++) u[k] = *d++; }
    /* sanitize everything that came from flash */
    fm313_sound_t *x = &fm313.s;
    float *f = &x->cutoff;
    for (int i = 0; i < 13; i++) if (!(f[i] >= -12.f && f[i] <= 12.f)) return -1;   /* NaN too */
    x->cutoff = clampf(x->cutoff, 0, 1); x->reso = clampf(x->reso, 0, 1);
    x->envmod = clampf(x->envmod, 0, 1); x->decay = clampf(x->decay, 0, 1);
    x->accent = clampf(x->accent, 0, 1); x->tune = clampf(x->tune, -12, 12);
    x->level = clampf(x->level, 0, 1); x->drive = clampf(x->drive, 0, 1); x->tone = clampf(x->tone, 0, 1);
    x->dmix = clampf(x->dmix, 0, 1); x->dfb = clampf(x->dfb, 0, 1); x->rmix = clampf(x->rmix, 0, 1);
    x->rtime = clampf(x->rtime, 0, 1);
    if (!(x->bpm >= 40.f && x->bpm <= 300.f)) x->bpm = 126.f;
    x->shuffle = clampf(x->shuffle, 0, 1);
    if (x->wave > FM313_SUPER) x->wave = FM313_SAW;
    if (x->dtype >= FM313_DIST_NUM) x->dtype = 0;
    if (x->ddiv > 4) x->ddiv = 3;
    if (x->pattern >= FM313_NPAT) x->pattern = 0;
    if (x->midi_ch > 16) x->midi_ch = 0;
    x->ext_clock = x->ext_clock ? 1 : 0;
    if (x->kb_oct > 2) x->kb_oct = 1;
    if (x->preset > NPRESETS) x->preset = 0;
    x->midi_out = x->midi_out ? 1 : 0;
    if (x->sc_type < 1 || x->sc_type >= SC_NUM) x->sc_type = SC_REVERSE;
    x->sc_depth = (uint8_t)clampi(x->sc_depth, 1, 16);
    x->tune = clampf(x->tune, -0.40f, 0.40f);          /* 0.4: SETUP's master tune, 430-450 Hz */
    fm313_setup_t *u = &fm313.su;
    u->touch = u->touch ? 1 : 0; u->local_off = u->local_off ? 1 : 0;
    if (u->vel_thr < 20 || u->vel_thr > 127) u->vel_thr = 90;
    if (u->scope_mode > 2) u->scope_mode = 0;
    if (u->press_to > 2) u->press_to = 0;
    if (u->leds > 2) u->leds = 0;
    if (u->phones > 1) u->phones = 0;
    if (u->tips_off > 1) u->tips_off = 0;
    return 0;
}

static int st_bank_ok(uint32_t off, st_hdr_t *h)
{
    if (hw_flash_read(off, h, sizeof *h)) return 0;
    if (h->magic != ST_MAGIC || h->len != ST_BODY) return 0;
    if (hw_flash_read(off + sizeof *h, st_buf, h->len)) return 0;
    if (crc32(st_buf, h->len) != h->crc) return 0;
    return 1;
}

int fm313_app_store_load(void)
{
    st_hdr_t h0, h1;
    int ok0 = st_bank_ok(ST_BANK0, &h0);
    int ok1 = st_bank_ok(ST_BANK0 + ST_BANK_LEN, &h1);
    if (!ok0 && !ok1) return -1;
    int use1 = ok1 && (!ok0 || (int32_t)(h1.seq - h0.seq) > 0);
    st_bank_ok(use1 ? ST_BANK0 + ST_BANK_LEN : ST_BANK0, use1 ? &h1 : &h0);   /* reload the chosen body */
    fm313.store_seq = use1 ? h1.seq : h0.seq;
    return st_unpack();
}

int fm313_app_store_save(void)
{
    st_hdr_t h0, h1;
    int ok0 = st_bank_ok(ST_BANK0, &h0);
    int ok1 = st_bank_ok(ST_BANK0 + ST_BANK_LEN, &h1);
    uint32_t bank = ST_BANK0;
    if (ok0 && (!ok1 || (int32_t)(h0.seq - h1.seq) > 0)) bank = ST_BANK0 + ST_BANK_LEN;
    st_pack();
    st_hdr_t h = { ST_MAGIC, fm313.store_seq + 1u, ST_BODY, crc32(st_buf, ST_BODY) };
    for (uint32_t o = 0; o < ST_BANK_LEN; o += 0x1000u)
        if (hw_flash_erase4k(bank + o)) return -1;
    if (hw_flash_write(bank + sizeof h, st_buf, ST_BODY)) return -2;
    if (hw_flash_write(bank, &h, sizeof h)) return -3;      /* the header last: commits the bank */
    fm313.store_seq++;
    return 0;
}

/* SETUP's choices alone (0.7.2: on leaving SETUP, so PHONES, LEDS and the rest survive a restart without a SAVE):
 * the newest saved body with only its SETUP part replaced, written to the other bank. Patterns and sound stay as
 * they were last saved. Nothing saved yet: a first save of everything */
static int setup_save(void)
{
    st_hdr_t h0, h1;
    int ok0 = st_bank_ok(ST_BANK0, &h0);
    int ok1 = st_bank_ok(ST_BANK0 + ST_BANK_LEN, &h1);
    if (!ok0 && !ok1) return fm313_app_store_save();     /* nothing saved yet: the first save, everything */
    int use1 = ok1 && (!ok0 || (int32_t)(h1.seq - h0.seq) > 0);
    uint32_t seq = use1 ? h1.seq : h0.seq, bank = use1 ? ST_BANK0 : ST_BANK0 + ST_BANK_LEN;
    if (!st_bank_ok(use1 ? ST_BANK0 + ST_BANK_LEN : ST_BANK0, use1 ? &h1 : &h0)) return -1;   /* its body again */
    const uint8_t *u = (const uint8_t *)&fm313.su;
    uint8_t *d = st_buf + ST_PAT_BYTES + sizeof(fm313_sound_t);
    for (uint32_t k = 0; k < sizeof(fm313_setup_t); k++) d[k] = u[k];
    st_hdr_t h = { ST_MAGIC, seq + 1u, ST_BODY, crc32(st_buf, ST_BODY) };
    for (uint32_t o = 0; o < ST_BANK_LEN; o += 0x1000u)
        if (hw_flash_erase4k(bank + o)) return -1;
    if (hw_flash_write(bank + sizeof h, st_buf, ST_BODY)) return -2;
    if (hw_flash_write(bank, &h, sizeof h)) return -3;      /* the header last: commits the bank */
    fm313.store_seq = seq + 1u;
    return 0;
}

static void draw_boot(void);

/* ---------------------------------------------------------------- init */
void fm313_app_init(void)
{
    fm313_init(&fm313.v);
    fm313_fx_init(&fx);
    fm313.seed = 0x9e3779b9u ^ hw_ms();
    defaults();
    if (fm313_app_store_load() != 0) defaults();   /* nothing saved yet: the demo patterns */
    else message("LOADED", 0, 0);
    fm313.cur = fm313.s.pattern;
    fm313.kb_oct = fm313.s.kb_oct;
    fm313.queued = -1; fm313.held_key = -1;
    fm313.wr_key = fm313.wr_prev = -1;
    fm313.rec_key = -1; fm313.rec_last = -1; fm313.rec_step_seen = -1; fm313.chain_n = 1; fm313.mo_note = -1;
    fm313.seq_first = -1; fm313.env_key = fm313.env_dec_key = -1; fm313.bank = fm313.cur/16;
    fm313_seq_init(&fm313.q, &fm313.pat[fm313.cur], fm313.s.bpm);
    apply_sound();
    fm313.dirty_top = fm313.dirty_grid = fm313.dirty_knobs = fm313.dirty_msg = 1;
    fm313.last_step_drawn = -2;
    draw_boot();
    fm313.boot_until = hw_ms() + 1200u;
}

/* ---------------------------------------------------------------- audio side */
static void handle_event(const fm313_ev_t *e)
{
    switch (e->type) {
    case EV_NOTE_ON:
        if (!fm313.q.playing) {
            fm313_note_on(&fm313.v, e->a, e->b, e->c);
            fm313.q.on_note = e->a; fm313.q.on_acc = e->b; fm313.q.on_slide = e->c; fm313.q.n_on++;
        }
        break;
    case EV_NOTE_OFF:
        if (!fm313.q.playing) { if (fm313.v.gate) fm313.q.n_off++; fm313_gate_off(&fm313.v); }
        break;
    case EV_START:
        fm313.q.pat = &fm313.pat[fm313.cur]; fm313.q.next_pat = 0;
        fm313_seq_start(&fm313.q);
        break;
    case EV_STOP:
        fm313_seq_stop(&fm313.q, &fm313.v);
        fm313.q.transpose = 0;
        break;
    case EV_PATTERN:
        if (fm313.q.playing) fm313.q.next_pat = &fm313.pat[e->a];
        else fm313.q.pat = &fm313.pat[e->a];
        break;
    case EV_TRANSPOSE:
        fm313.q.transpose = (int)(int8_t)e->a;
        break;
    case EV_CHAIN:
        fm313.q.chain_base = &fm313.pat[e->a]; fm313.q.chain_n = e->b ? e->b : 1;
        fm313.q.chain_i = 0;
        for (int i = 0; i < fm313.q.chain_n; i++) if (fm313.q.pat == fm313.q.chain_base + i) fm313.q.chain_i = i;
        break;
    case EV_SCATTER:
        fm313_seq_scatter(&fm313.q, e->a, e->b);
        break;
    case EV_MIDI:                                      /* LOCAL OFF: a key straight out */
        hw_midi_out((uint8_t)(e->a | (fm313.s.midi_ch ? fm313.s.midi_ch - 1 : 0)), e->b, e->c);
        break;
    }
}

static void rec_note(int midi, int acc, int legato);
void fm313_app_midi(uint8_t st, uint8_t d1, uint8_t d2)   /* audio context */
{
    if (st >= 0xF8u) {
        if (st == 0xF8u) fm313_seq_midi_clock(&fm313.q, &fm313.v);
        else if (st == 0xFAu && fm313.s.ext_clock) { fm313.q.pat = &fm313.pat[fm313.cur]; fm313_seq_start(&fm313.q); }
        else if (st == 0xFBu && fm313.s.ext_clock) fm313.q.playing = 1;
        else if (st == 0xFCu && fm313.s.ext_clock) fm313_seq_stop(&fm313.q, &fm313.v);
        return;
    }
    uint8_t ch = st & 15u, ty = st & 0xF0u;
    if (fm313.s.midi_ch && ch != fm313.s.midi_ch - 1u) return;
    if (ty == 0x90u && d2) {
        int acc = d2 >= fm313.su.vel_thr;
        if (fm313.q.playing && fm313.rt_rec && fm313.mode == M_PLAY) rec_note(d1, acc, fm313.midi_held > 0);   /* External Record */
        else if (fm313.q.playing) fm313.q.transpose = clampi((int)d1 - 60, -24, 24);
        else {
            fm313_note_on(&fm313.v, d1, acc, fm313.v.gate);   /* overlapping notes slide */
            fm313.q.on_note = d1; fm313.q.on_acc = acc; fm313.q.n_on++;
        }
        if (fm313.midi_held < 16) fm313.midi_held++;
    } else if (ty == 0x80u || (ty == 0x90u && !d2)) {
        if (fm313.midi_held) fm313.midi_held--;
        if (!fm313.q.playing && (int)fm313.v.pitch_target == d1) { if (fm313.v.gate) fm313.q.n_off++; fm313_gate_off(&fm313.v); }
    } else if (ty == 0xA0u || ty == 0xD0u) {           /* aftertouch: as the keys' pressure */
        float p = (float)(ty == 0xD0u ? d1 : d2)*(1.f/127.f);
        fm313.v.p.touch_oct = fm313.su.press_to == 0 ? 2.2f*p : 0.f;
        fm313.v.p.vib = fm313.su.press_to == 1 ? 0.6f*p : 0.f;
    } else if (ty == 0xB0u && (d1 == 1u || (d1 >= 102u && d1 <= 104u))) {
        if (d1 == 1u) fm313.v.p.vib = (float)d2*(0.7f/127.f);                       /* mod wheel: vibrato */
        else {                                                                       /* SCATTER, as the TB-3's CCs */
            if (d1 == 103u) fm313.s.sc_type = (uint8_t)(1u + (uint32_t)d2*8u/128u);
            if (d1 == 104u) fm313.s.sc_depth = (uint8_t)(1u + (uint32_t)d2*16u/128u);
            if (d1 == 102u) fm313.midi_sc = d2 >= 64u;
            fm313_seq_scatter(&fm313.q, fm313.midi_sc || fm313.sc_latched ? fm313.s.sc_type : SC_OFF, fm313.s.sc_depth);
            fm313.dirty_top = 1;
        }
    } else if (ty == 0xB0u) {
        float v = (float)d2*(1.f/127.f);
        switch (d1) {
        case 74: fm313.s.cutoff = v; break;
        case 71: fm313.s.reso = v; break;
        case 75: fm313.s.envmod = v; break;
        case 72: fm313.s.decay = v; break;
        case 73: fm313.s.accent = v; break;
        case 12: fm313.s.drive = v; break;
        case 91: fm313.s.rmix = v; break;
        case 94: fm313.s.dmix = v; break;
        default: return;
        }
        apply_sound();
        fm313.dirty_knobs = 1;
    }
}

/* MIDI out (SETUP > MIDI OUT): what plays, as notes on the MIDI channel (ALL: 1), and the clock with
 * start / stop when FM-313 is the master. A slide's new note comes before the old one's note off. */
static void mo_send(uint8_t st, uint8_t d1, uint8_t d2) { hw_midi_out(st, d1, d2); }
static void midi_out(int n)
{
    fm313_seq_t *q = &fm313.q;
    uint8_t ch = (uint8_t)(fm313.s.midi_ch ? fm313.s.midi_ch - 1 : 0);
    if (!fm313.s.midi_out) { fm313.mo_on = q->n_on; fm313.mo_off = q->n_off; fm313.clk_was = q->playing; return; }
    if (fm313.mo_on != q->n_on) {
        int old = fm313.mo_note;
        fm313.mo_on = q->n_on;
        fm313.mo_note = q->on_note;
        mo_send((uint8_t)(0x90u | ch), (uint8_t)clampi(q->on_note, 0, 127), (uint8_t)(q->on_acc ? 127 : 100));
        if (old >= 0 && old != q->on_note) mo_send((uint8_t)(0x80u | ch), (uint8_t)old, 0);
        if (old == q->on_note) {}                    /* the same note again: its on retriggers it */
    }
    if (fm313.mo_off != q->n_off) {
        fm313.mo_off = q->n_off;
        if (fm313.mo_note >= 0) mo_send((uint8_t)(0x80u | ch), (uint8_t)fm313.mo_note, 0);
        fm313.mo_note = -1;
    }
    if (!fm313.s.ext_clock) {
        if (q->playing && !fm313.clk_was) { mo_send(0xFA, 0, 0); fm313.clk_acc = 0.f; }
        if (!q->playing && fm313.clk_was) mo_send(0xFC, 0, 0);
        if (q->playing) {
            float tick = FM313_FS*60.f/(q->bpm*24.f);
            fm313.clk_acc -= (float)n;
            while (fm313.clk_acc <= 0.f) { mo_send(0xF8, 0, 0); fm313.clk_acc += tick; }
        }
    }
    fm313.clk_was = q->playing;
}

void fm313_app_audio(float *out_lr, int n)
{
    while (fm313.ev_r != fm313.ev_w) {
        handle_event(&fm313.ev[fm313.ev_r & 31u]);
        fm313.ev_r++;
    }
    int done = 0;
    while (done < n) {
        int m = n - done;
        if (m > 64) m = 64;
        m = fm313_seq_advance(&fm313.q, &fm313.v, m);
        fm313_render(&fm313.v, fm313.mono, m);
        fm313_fx_process(&fx, fm313.mono, out_lr + 2*done, m);
        done += m;
    }
    if (fm313.su.phones && fm313.hp_in) {
        /* SETUP > PHONES LEFT FIX. Reported on one FM-1: with the dry (mono, L = R) sound only the right ear plays,
         * with delay or reverb (L != R) both do. That fits headphones whose common return sits on the R output:
         * left ear = L - R, right ear = -R. Sending L' = l - r, R' = -r gives back left = l, right = r. With headphones
         * only (PB5): the speaker keeps the plain signal. */
        /* 0.7.2: at half the level; 0.7.4: at 0.7 (half was reported as far too quiet). Reported: much louder with the fix on (one ear before, two now, and l - r of a
         * wide delay or reverb reaches twice the level of either side) */
        for (int i = 0; i < n; i++) {
            float l = out_lr[2*i], rr = out_lr[2*i + 1];
            out_lr[2*i] = 0.7f*(l - rr); out_lr[2*i + 1] = -0.7f*rr;
        }
    }
    fm313.play_step = fm313.q.playing ? fm313.q.step : -1;
    fm313.playing = fm313.q.playing;
    if (fm313.q.pat != fm313.audio_pat) {                /* a new pattern sounds: its tune, its sound */
        fm313.audio_pat = fm313.q.pat;
        fm313.v.p.tune = fm313.s.tune + 0.1f*(float)fm313.q.pat->tune10;
        if (fm313.q.pat->tone && fm313.q.playing) fm313.tone_pending = fm313.q.pat->tone;
    }
    midi_out(n);
    if (fm313.q.pat != &fm313.pat[fm313.cur] && fm313.q.playing && !fm313.q.next_pat) {
        /* the queued pattern has started */
        for (int i = 0; i < FM313_NPAT; i++) if (fm313.q.pat == &fm313.pat[i]) { fm313.cur = i; break; }
        fm313.queued = -1;
        fm313.dirty_top = fm313.dirty_grid = 1;
    }
}

/* ---------------------------------------------------------------- controls (0.5)
 * The panel, after three rules: the FM-1's labels mean what they say; a button held is a layer (its map on the
 * screen, as Felucca's quick layers), tapped it does its own one thing; the four knobs are always the sound.
 *
 *   KNOB 1-4  cutoff, resonance, env mod, decay      ALGORITHM accent      SELECT tempo (SEL: shuffle)
 *   PRESETS   the sound preset                       MASTER   volume
 *   keys      play (velocity: a hard press is an accent; pressure opens the filter), or transpose the pattern
 *   PLAY / REC  start-stop / step write (SEL+REC: realtime record)      OCT-/+  the keys' octave
 *   SAVE      save (held 1 s: undo)       HOME  back to the scope       GLO  SETUP
 *   FX   tap: the effect knobs stay;  held: the same while held
 *   SEQ  tap: next pattern (SEL: previous);  held: white keys = patterns 1-16 of the bank, black keys 1-4 = the bank,
 *        a white key held and a second pressed = a chain; knobs = tune, pitch shift, length, chain; ALGORITHM shuffle;
 *        PLAY = triplets, SAVE = lock, ENV = the sound to the pattern
 *   EDIT held: white keys = copy, paste, clear, random, modify, shift down, shift up, lock, sound, undo
 *   ARP  held: SCATTER, white keys 1-8 its kind, 9-16 its depth (ALGORITHM too); SEL+ARP latches it
 *   ENV  tap: the wave;  held (SEL+ENV latches): the keys are a strip, white = env mod, black = decay
 *   LFO  tap: tap tempo; held (SEL+LFO latches): XY play, a long glide between keys, pressure = vibrato
 * Step write: keys = notes (a hard press writes an accent), FX rest, LFO tie, ARP accent, SEQ slide, EDIT ends the
 * pattern at the cursor, SELECT moves the cursor. */
static const char *const DT_NAME[FM313_DIST_NUM] = { "OFF", "OD", "DIST", "FUZZ", "CRNCH" };
static const char *const DIV_NAME[5] = { "1/16", "1/8T", "1/8", "1/8.", "1/4" };
static const char *const WAVE_NAME[3] = { "SAW", "SQUARE", "4-OSC" };
static const char *const SC_NAME[SC_NUM] = { "OFF", "REVERSE", "REPEAT", "HALF", "RATCHET", "GATE", "SKIP", "RANDOM", "STUTTER" };
enum { L_NONE, L_FX, L_SEQ, L_EDIT, L_ARP, L_ENV, L_LFO, L_NUM };
static const int LAYER_BTN[L_NUM] = { -1, B_FX, B_SEQ, B_EDIT, B_ARP, B_ENV, B_LFO };
/* the 16 white and 11 black keys of F3..G5 */
static const int8_t WHITE[16] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
static const int8_t BLACK[11] = { 1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25 };
static int white_of(int k) { for (int i = 0; i < 16; i++) if (WHITE[i] == k) return i; return -1; }
static int black_of(int k) { for (int i = 0; i < 11; i++) if (BLACK[i] == k) return i; return -1; }
enum { ED_COPY, ED_PASTE, ED_CLEAR, ED_RANDOM, ED_MODIFY, ED_SHDN, ED_SHUP, ED_LOCK, ED_SOUND, ED_UNDO, ED_NUM };
static const char *const ED_NAME[ED_NUM] = { "COPY", "PASTE", "CLEAR", "RANDOM", "MODIFY", "SHIFT -1", "SHIFT +1",
                                             "LOCK", "SOUND", "UNDO" };

static float step100(float x, int steps)          /* the screen shows 0..100: a detent is one number */
{
    int n = (int)(x*100.f + 0.5f) + steps;
    return (float)clampi(n, 0, 100)*0.01f;
}
static void touch(const void *id)                 /* this value was turned: its soft key lights */
{
    fm313.touched = id; fm313.touch_ms = hw_ms() | 1u;
    fm313.dirty_knobs = 1;
}
static void knob(float *x, int steps, const char *name)
{
    *x = step100(*x, steps);
    apply_sound();
    message(name, (int)(*x*100.f + 0.5f), 1);
    fm313.msg_ladder = 1; fm313.msg_red = x == &fm313.s.accent; fm313.msg_hold = 1000u;   /* the scope back 1 s after */
    touch(x);
}
static int locked(void)                           /* a locked pattern takes no edit */
{
    if (!(fm313.pat[fm313.cur].pflags & FM313_P_LOCK)) return 0;
    message("PATTERN LOCKED", 0, 0);
    return 1;
}
static void sound_msg(const char *what, int idx)  /* idx: 1-based preset, 0 none */
{
    char b[32] = ""; cat(b, what); cat(b, ": ");
    if (idx) { char n[4]; put_num(n, idx, 2); cat(b, n); cat(b, " "); cat(b, PRESETS[idx - 1].name); }
    else cat(b, "NONE");
    message(b, 0, 0);
}
/* what the panel, keys and knobs serve: the button held, else the menu left open */
static int view(void) { return fm313.mode != M_PLAY ? L_NONE : fm313.layer != L_NONE ? fm313.layer : fm313.menu; }
static void scatter_apply(void)                   /* runs once a kind is picked, until the same kind again (or HOME) */
{
    int on = fm313.sc_latched;
    post(EV_SCATTER, on ? fm313.s.sc_type : SC_OFF, fm313.s.sc_depth, 0);
    fm313.dirty_top = 1;
}
static float hz_of(float tune) { return 440.f*fm313_exp2(tune*(1.f/12.f)); }
static float tune_of(float hz)                    /* 12 log2(hz/440), |hz/440 - 1| < 3 % here */
{
    float x = hz/440.f - 1.f;
    return 12.f*1.44269504f*(x - 0.5f*x*x + x*x*x*(1.f/3.f));
}
static void pat_name(char *b, int p)              /* "A01": bank and number, as the TB-3's banks */
{
    b[0] = (char)('A' + p/16); put_num(b + 1, p % 16 + 1, 2);
}

static void select_pattern(int p)
{
    p = clampi(p, 0, FM313_NPAT - 1);
    fm313.s.pattern = (uint8_t)p;
    fm313.bank = p/16;
    if (fm313.playing) {
        fm313.queued = p;
        post(EV_PATTERN, p, 0, 0);
    } else {
        fm313.cur = p;
        post(EV_PATTERN, p, 0, 0);
        fm313.dirty_grid = 1;
        if (fm313.pat[p].tone) { load_preset(fm313.pat[p].tone - 1); fm313.dirty_knobs = 1; }
        apply_sound();
    }
    if (fm313.chain_n > 1) { fm313.chain_n = 1; post(EV_CHAIN, p, 1, 0); }   /* a pattern chosen: the chain ends */
    char b[24] = "PATTERN: "; char n[5]; pat_name(n, p); cat(b, n);
    if (fm313.playing) cat(b, " NEXT");
    message(b, 0, 0);
    fm313.dirty_top = 1;
}
static void set_chain(int first, int n)
{
    int mx = FM313_NPAT - first; if (mx > 16) mx = 16;
    fm313.chain_n = clampi(n, 1, mx);
    fm313.chain_start = first;
    post(EV_CHAIN, first, fm313.chain_n, 0);
    if (fm313.chain_n > 1) {
        char b[24] = "CHAIN: "; char x[5]; pat_name(x, first); cat(b, x);
        cat(b, "-"); pat_name(x, first + fm313.chain_n - 1); cat(b, x); message(b, 0, 0);
    } else message("CHAIN: OFF", 0, 0);
    fm313.dirty_top = 1;
}

/* a fast turn moves further, only when really spun */
static int accel(int e, int steps)
{
    static uint32_t last[E_NUM];
    uint32_t now = hw_ms(), dt = now - last[e];
    last[e] = now;
    int m = dt < 20u ? 3 : dt < 40u ? 2 : 1;
    if (steps > 2 || steps < -2) m = 1;
    return steps*m;
}

static void edit_action(int a)                    /* EDIT + a white key */
{
    fm313_pattern_t *p = &fm313.pat[fm313.cur];
    switch (a) {
    case ED_COPY: fm313.clip = *p; message("COPIED", 0, 0); return;
    case ED_LOCK:
        p->pflags ^= FM313_P_LOCK;
        message((p->pflags & FM313_P_LOCK) ? "PATTERN LOCKED" : "PATTERN UNLOCKED", 0, 0);
        fm313.dirty_top = 1; return;
    case ED_SOUND:
        p->tone = (uint8_t)(p->tone && p->tone == fm313.s.preset ? 0 : fm313.s.preset);
        sound_msg("PATTERN SOUND", p->tone); fm313.dirty_top = 1; return;
    case ED_UNDO: { fm313_pattern_t t = *p; *p = fm313.undo; fm313.undo = t; message("UNDO", 0, 0); fm313.dirty_grid = 1; return; }
    default: break;
    }
    if (locked()) return;
    fm313.undo = *p;
    switch (a) {
    case ED_PASTE: *p = fm313.clip; message("PASTED", 0, 0); break;
    case ED_CLEAR: fm313_pattern_clear(p); message("CLEARED", 0, 0); break;
    case ED_RANDOM: fm313_pattern_random(p, &fm313.seed, p->len ? p->len : 16); message("RANDOM PATTERN", 0, 0); break;
    case ED_MODIFY: fm313_pattern_mutate(p, &fm313.seed); message("MODIFIED", 0, 0); break;
    case ED_SHDN: case ED_SHUP: {
        int f = fm313_pattern_shift(p, a == ED_SHUP ? 1 : -1);
        message(f ? "PITCH SHIFT: FOLDED" : (a == ED_SHUP ? "PITCH SHIFT: UP" : "PITCH SHIFT: DOWN"), 0, 0); break; }
    default: break;
    }
    fm313.dirty_grid = fm313.dirty_top = 1;
}

/* the keys' pressure, the strongest held: what it does (SETUP > PRESSURE), and the ENV strip's */
static void apply_touch(void)
{
    int pmax = 0;
    for (int i = 0; i < FM313_NKEYS; i++) if (fm313.key_down[i] && fm313.key_p[i] > pmax) pmax = fm313.key_p[i];
    float p = (float)pmax*(1.f/127.f);
    fm313_params_t *v = &fm313.v.p;
    int xy = view() == L_LFO;
    if (xy) { v->touch_oct = 0.8f*p; v->vib = 0.7f*p; }
    else if (fm313.su.press_to == 0) { v->touch_oct = 2.2f*p; v->vib = 0.f; }
    else if (fm313.su.press_to == 1) { v->touch_oct = 0.f; v->vib = 0.6f*p; }
    else { v->touch_oct = 0.f; v->vib = 0.f; }
    v->glide = xy ? 0.09f : 0.f;
    int env = view() == L_ENV;
    if (env && fm313.env_key >= 0) {                     /* the strip: where the finger is, pressed in deeper */
        int w = white_of(fm313.env_key);
        v->envmod = clampf((float)w*(1.f/15.f) + 0.3f*p, 0.f, 1.f);
    } else v->envmod = fm313.s.envmod;
    if (env && fm313.env_dec_key >= 0) v->decay = (float)black_of(fm313.env_dec_key)*(1.f/10.f);
    else v->decay = fm313.s.decay;
}
void fm313_app_pressure(int k, int p)
{
    if (k < 0 || k >= FM313_NKEYS) return;
    fm313.key_p[k] = (uint8_t)p;
    apply_touch();
    if (view() == L_ENV || view() == L_LFO) fm313.panel_dirty = 1;
}
int fm313_app_touch_pref(void) { return fm313.su.touch; }

/* ---- encoders */
enum { SU_MIDICH, SU_CLOCK, SU_MIDIOUT, SU_LOCAL, SU_TUNE, SU_LEVEL, SU_ACCENT, SU_PRESS, SU_SCOPE,
       SU_TIPS, SU_LEDS, SU_PHONES, SU_UPDATE, SU_NUM };
static void setup_turn(int steps);
static void fx_turn(int e, int steps, int fast)
{
    fm313_sound_t *s = &fm313.s;
    switch (e) {
    case E_K1: s->drive = step100(s->drive, fast); touch(&s->drive); break;
    case E_K2: s->tone = step100(s->tone, fast); touch(&s->tone); break;
    case E_K3: s->dmix = step100(s->dmix, fast); touch(&s->dmix); break;
    case E_K4: if (fm313.shift) { s->rtime = step100(s->rtime, fast); touch(&s->rtime); }
               else { s->rmix = step100(s->rmix, fast); touch(&s->rmix); } break;
    case E_SELECT: s->dtype = (uint8_t)clampi(s->dtype + steps, 0, FM313_DIST_NUM - 1); touch(&s->dtype); break;
    case E_ALGO: s->ddiv = (uint8_t)clampi(s->ddiv + steps, 0, 4); touch(&s->ddiv); break;
    case E_PRESETS: s->dfb = step100(s->dfb, fast); touch(&s->dfb); break;
    }
    apply_sound();
    fm313.panel_dirty = 1;
    if (fm313.layer == L_FX) fm313.lay_used = 1;
}
static void seq_turn(int e, int steps, int fast)
{
    fm313_pattern_t *p = &fm313.pat[fm313.cur];
    fm313.lay_used = 1;
    switch (e) {
    case E_K1: p->tune10 = (int8_t)clampi(p->tune10 + fast, -70, 70); apply_sound();
               message("PATTERN TUNE CT", p->tune10*10, 1); touch(&p->tune10); break;
    case E_K2: if (locked()) return;
               fm313.undo = *p; {
                   int f = fm313_pattern_shift(p, steps);
                   message(f ? "PITCH SHIFT: FOLDED" : (steps > 0 ? "PITCH SHIFT: UP" : "PITCH SHIFT: DOWN"), 0, 0); }
               fm313.dirty_grid = 1; break;
    case E_K3: if (locked()) return;
               p->len = (uint8_t)clampi(p->len + steps, 1, FM313_STEPS);
               if (fm313.cursor >= p->len) fm313.cursor = p->len - 1;
               message("LENGTH", p->len, 1); fm313.dirty_grid = 1; touch(&p->len); break;
    case E_K4: set_chain(fm313.cur, fm313.chain_n + steps); touch(&fm313.chain_n); break;
    case E_ALGO: case E_SELECT:
               fm313.s.shuffle = clampf(fm313.s.shuffle + (float)steps*(1.f/32.f), 0, 1); apply_sound();
               message("SHUFFLE", (int)(fm313.s.shuffle*100.f + 0.5f), 1); touch(&fm313.s.shuffle); break;
    case E_PRESETS: select_pattern((fm313.queued >= 0 ? fm313.queued : fm313.cur) + steps); break;
    }
    fm313.panel_dirty = 1;
}
/* ---- step write: the knobs are the step under the cursor (KNOB 1 / SELECT which, 2 its note, 3 accent, 4 slide,
 * ALGORITHM its octave); semitones s = oct*12 + note, -12 .. 23 */
static int step_s(const fm313_step_t *st) { return st->oct*12 + st->note; }
static void step_set_s(fm313_step_t *st, int v)
{
    v = clampi(v, -12, 23);
    st->type = FM313_NOTE;
    if (v < 0) { st->oct = -1; st->note = (uint8_t)(v + 12); }
    else if (v <= 12) { st->oct = 0; st->note = (uint8_t)v; }
    else { st->oct = 1; st->note = (uint8_t)(v - 12); }
}
static void step_hear(const fm313_step_t *st)          /* stopped: the step sounds as it is set */
{
    if (fm313.playing || st->type != FM313_NOTE) return;
    post(EV_NOTE_ON, 36 + step_s(st), (st->flags & FM313_F_ACC) != 0, 0);
    fm313.hear_ms = hw_ms() | 1u;
}
static void write_turn(int e, int steps)
{
    fm313_pattern_t *p = &fm313.pat[fm313.cur];
    fm313_step_t *st = &p->s[fm313.cursor];
    int len = p->len;
    if (e == E_SELECT || e == E_K1) {
        fm313.cursor = (fm313.cursor + steps) % len; if (fm313.cursor < 0) fm313.cursor += len;
        fm313.wr_prev = -1;
    } else {
        if (locked()) return;
        switch (e) {
        case E_K2: step_set_s(st, (st->type == FM313_NOTE ? step_s(st) : 0) + steps); step_hear(st); break;
        case E_ALGO: step_set_s(st, (st->type == FM313_NOTE ? step_s(st) : 0) + (steps > 0 ? 12 : -12)); step_hear(st); break;
        case E_K3: if (steps > 0) st->flags |= FM313_F_ACC; else st->flags &= (uint8_t)~FM313_F_ACC; break;
        case E_K4: if (steps > 0) st->flags |= FM313_F_SLIDE; else st->flags &= (uint8_t)~FM313_F_SLIDE; break;
        default: return;
        }
    }
    fm313.dirty_grid = fm313.dirty_top = fm313.dirty_knobs = 1; fm313.panel_dirty = 1;
}
static uint32_t rnd(void) { uint32_t x = fm313.seed; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return fm313.seed = x ? x : 1u; }
static int dice_pool(int *pool)                       /* the pattern's own notes, else C minor pentatonic */
{
    const fm313_pattern_t *p = &fm313.pat[fm313.cur];
    int n = 0;
    for (int i = 0; i < p->len; i++) {
        if (p->s[i].type != FM313_NOTE) continue;
        int v = step_s(&p->s[i]), seen = 0;
        for (int j = 0; j < n; j++) if (pool[j] == v) seen = 1;
        if (!seen && n < 16) pool[n++] = v;
    }
    if (n < 3) { static const int PENTA[6] = { 0, 3, 5, 7, 10, 12 }; for (n = 0; n < 6; n++) pool[n] = PENTA[n]; }
    return n;
}
static void dice_step(void)                           /* ARP: this step a note of the pattern's own (another one) */
{
    int pool[16], n = dice_pool(pool);
    fm313_step_t *st = &fm313.pat[fm313.cur].s[fm313.cursor];
    int was = st->type == FM313_NOTE ? step_s(st) : 999, v = was;
    for (int t = 0; t < 8 && v == was; t++) v = pool[rnd() % (uint32_t)n];
    step_set_s(st, v);
    step_hear(st);
    fm313.dirty_grid = fm313.dirty_top = fm313.dirty_knobs = 1; fm313.panel_dirty = 1;
}
static void dice_all(void)                            /* SEL+ARP: every note new, the rhythm, accents and slides kept */
{
    int pool[16], n = dice_pool(pool);
    fm313_pattern_t *p = &fm313.pat[fm313.cur];
    fm313.undo = *p;
    for (int i = 0; i < p->len; i++) if (p->s[i].type == FM313_NOTE) step_set_s(&p->s[i], pool[rnd() % (uint32_t)n]);
    message("DICE: ALL NOTES", 0, 0);
    fm313.dirty_grid = fm313.dirty_top = fm313.dirty_knobs = 1; fm313.panel_dirty = 1;
}
void fm313_app_encoder(int e, int steps)
{
    fm313_sound_t *s = &fm313.s;
    if (!steps) return;
    int fast = accel(e, steps);
    if (fm313.mode == M_GLOBAL) {
        if (e == E_SELECT) { fm313.su_cur = clampi(fm313.su_cur + steps, 0, SU_NUM - 1); fm313.panel_dirty = 1; }
        else setup_turn(steps);
        return;
    }
    if (view() == L_ARP && (e == E_SELECT || e == E_ALGO)) {           /* SCATTER's kind / depth (does not start it) */
        fm313.lay_used = 1;
        if (e == E_SELECT) s->sc_type = (uint8_t)clampi(s->sc_type + steps, 1, SC_NUM - 1);
        else s->sc_depth = (uint8_t)clampi(s->sc_depth + steps, 1, 16);
        if (fm313.layer == L_ARP) fm313.lay_used = 1;
        scatter_apply(); fm313.panel_dirty = 1;
        return;
    }
    if (view() == L_SEQ) { seq_turn(e, steps, fast); return; }
    if (view() == L_FX) { fx_turn(e, steps, fast); return; }
    if (fm313.mode == M_WRITE && e != E_PRESETS) { write_turn(e, steps); return; }
    switch (e) {
    case E_K1: knob(&s->cutoff, fast, "CUTOFF"); break;
    case E_K2: knob(&s->reso, fast, "RESONANCE"); break;
    case E_K3: knob(&s->envmod, fast, "ENV MOD"); break;
    case E_K4: knob(&s->decay, fast, "DECAY"); break;
    case E_ALGO: knob(&s->accent, fast, "ACCENT"); break;
    case E_SELECT:
        if (fm313.shift) {
            s->shuffle = clampf(s->shuffle + (float)steps*(1.f/32.f), 0, 1); apply_sound();
            message("SHUFFLE", (int)(s->shuffle*100.f + 0.5f), 1); fm313.dirty_top = 1;
        } else {
            s->bpm = clampf((float)(int)(s->bpm + 0.5f) + (float)fast, 40.f, 300.f);
            apply_sound(); message("TEMPO", (int)s->bpm, 1); fm313.dirty_top = 1;
        }
        break;
    case E_PRESETS: {
        int i = clampi((s->preset ? s->preset : 1) - 1 + steps, 0, NPRESETS - 1);
        load_preset(i); apply_sound(); fm313.dirty_knobs = fm313.dirty_top = 1;
        sound_msg("SOUND", i + 1);
        break; }
    }
}

/* ---- keys */
static void write_step(int type, int note, int oct)
{
    fm313_pattern_t *p = &fm313.pat[fm313.cur];
    fm313_step_t *st = &p->s[fm313.cursor];
    st->type = (uint8_t)type;
    if (type == FM313_NOTE) { st->note = (uint8_t)note; st->oct = (int8_t)oct; }
    fm313.cursor = (fm313.cursor + 1) % p->len;
    fm313.dirty_grid = fm313.dirty_top = 1; fm313.panel_dirty = 1;
}
static void key_to_step(int midi, int *note, int *oct)
{
    int rel = midi - 60;                         /* C4 key (the 8th) = step C, octave 0 */
    int o = rel < 0 ? -1 : (rel >= 12 ? 1 : 0);
    int nt = rel - 12*o;
    if (o == 1 && nt == 0) { o = 0; nt = 12; }  /* C' stays in the middle octave */
    while (nt > 12 || (o == 1 && nt > 11)) { nt -= 12; }
    while (nt < 0) nt += 12;
    *note = nt; *oct = o;
}
/* realtime record (keys, and notes from MIDI: the TB-3's External Record): into the nearer step */
static void rec_note(int midi, int acc, int legato)
{
    fm313_pattern_t *p = &fm313.pat[fm313.cur];
    if (p->pflags & FM313_P_LOCK) return;
    int len = fm313_seq_len(p), pos = fm313.q.pos < 0 ? 0 : fm313.q.pos;
    int at = fm313_seq_phase(&fm313.q) > 0.5f ? (pos + 1) % len : pos;
    int note, oct;
    key_to_step(midi, &note, &oct);
    if (legato && fm313.rec_last >= 0) p->s[fm313.rec_last].flags |= FM313_F_SLIDE;
    fm313_step_t *st = &p->s[at];
    st->type = FM313_NOTE; st->note = (uint8_t)note; st->oct = (int8_t)oct;
    st->flags = (uint8_t)(acc ? FM313_F_ACC : 0);
    fm313.rec_last = at; fm313.rec_step_seen = at;
    fm313.dirty_grid = 1;
}

void fm313_app_key(int k, int down, int vel)
{
    if (k < 0 || k >= FM313_NKEYS) return;
    fm313.key_down[k] = (uint8_t)down;
    if (down) fm313.key_vel[k] = (uint8_t)vel; else fm313.key_p[k] = 0;
    int midi = 53 + k + 12*(fm313.kb_oct - 1);
    int w = white_of(k), bl = black_of(k);
    int acc = fm313.shift;                         /* SEL held: an accent (0.6.3: the keys have no velocity) */
    /* the layers first: the keys are their buttons */
    if (view() == L_SEQ) {
        fm313.lay_used = 1;
        if (!down) { if (w >= 0 && w == fm313.seq_first) fm313.seq_first = -1; return; }
        if (bl >= 0 && bl < 4) { fm313.bank = bl; char b[16] = "BANK: "; char c[2] = { (char)('A' + bl), 0 }; cat(b, c); message(b, 0, 0); fm313.panel_dirty = 1; return; }
        if (w < 0) return;
        int pat = fm313.bank*16 + w;
        if (fm313.seq_first >= 0 && fm313.seq_first != w) {   /* a key held and another pressed: a chain from-to */
            int a = fm313.bank*16 + fm313.seq_first, b = pat;
            if (b < a) { int t = a; a = b; b = t; }
            if (!fm313.playing) { fm313.cur = a; post(EV_PATTERN, a, 0, 0); fm313.dirty_grid = 1; }
            set_chain(a, b - a + 1);
        } else {
            fm313.seq_first = w;
            select_pattern(pat);
        }
        fm313.panel_dirty = 1;
        return;
    }
    if (view() == L_EDIT) {
        fm313.lay_used = 1;
        if (down && w >= 0 && w < ED_NUM) edit_action(w);
        fm313.panel_dirty = 1;
        return;
    }
    if (view() == L_ARP) {          /* the SCATTER menu: black keys 1-8 start a kind (the same again: stop), white 1-16 depth */
        if (fm313.layer == L_ARP) fm313.lay_used = 1;
        if (!down) return;
        if (bl >= 0 && bl < 8) {
            if (fm313.sc_latched && fm313.s.sc_type == bl + 1) fm313.sc_latched = 0;
            else { fm313.s.sc_type = (uint8_t)(bl + 1); fm313.sc_latched = 1; }
        } else if (w >= 0) fm313.s.sc_depth = (uint8_t)(w + 1);   /* white key n: depth n */
        else return;
        scatter_apply(); fm313.panel_dirty = 1;
        return;
    }
    if (view() == L_ENV) {                            /* the strip */
        if (fm313.layer == L_ENV) fm313.lay_used = 1;
        if (down) { if (w >= 0) fm313.env_key = k; else fm313.env_dec_key = k; }
        else { if (k == fm313.env_key) fm313.env_key = -1; if (k == fm313.env_dec_key) fm313.env_dec_key = -1; }
        apply_touch(); fm313.panel_dirty = 1;
        return;
    }
    if (fm313.layer == L_LFO) fm313.lay_used = 1;
    if (fm313.mode == M_WRITE) {                   /* step write: a key is the note of the step under the cursor */
        if (!down) {
            if (k == fm313.wr_key) { fm313.wr_key = -1; if (!fm313.playing) post(EV_NOTE_OFF, 0, 0, 0); }
            return;
        }
        if (locked()) return;
        fm313_pattern_t *p = &fm313.pat[fm313.cur];
        int note, oct, legato = fm313.wr_key >= 0 && fm313.key_down[fm313.wr_key] && fm313.wr_prev >= 0;
        key_to_step(midi, &note, &oct);
        if (legato) p->s[fm313.wr_prev].flags |= FM313_F_SLIDE;   /* played over the last one: that one slides here */
        fm313_step_t *st = &p->s[fm313.cursor];
        st->flags = (uint8_t)(acc ? FM313_F_ACC : 0);        /* SEL held: accent; a new note slides only if played so */
        if (!fm313.playing) post(EV_NOTE_ON, 36 + 12*oct + note, acc, legato);
        fm313.wr_prev = fm313.cursor; fm313.wr_key = k;
        write_step(FM313_NOTE, note, oct);
        fm313.dirty_knobs = 1;
        return;
    }
    if (fm313.playing && fm313.rt_rec) {
        if (!down) { if (k == fm313.rec_key) fm313.rec_key = -1; return; }
        int legato = fm313.rec_key >= 0 && fm313.key_down[fm313.rec_key];
        rec_note(midi, acc, legato);
        fm313.rec_key = k;
        return;
    }
    if (fm313.playing) {                          /* keys transpose the running pattern (C4 = none); pressure still acts */
        if (down) {
            post(EV_TRANSPOSE, (uint8_t)(int8_t)(midi - 60), 0, 0);
            message("TRANSPOSE", midi - 60, 1);
            msg_sub(midi == 60 ? "C4 PLAYS THE PATTERN AS WRITTEN" : "PRESS C4 TO GO BACK TO 0");
        }
        apply_touch();
        return;
    }
    if (fm313.su.local_off) {                       /* LOCAL OFF: the keys go out only */
        post(EV_MIDI, down ? 0x90 : 0x80, midi - 24, down ? (acc ? 127 : 100) : 0);
        return;
    }
    if (down) {
        int legato = fm313.held_key >= 0;
        post(EV_NOTE_ON, midi - 24, acc, legato);   /* overlap: slide */
        fm313.held_key = k;
    } else if (k == fm313.held_key) {
        int other = -1;
        for (int i = FM313_NKEYS - 1; i >= 0; i--) if (fm313.key_down[i]) { other = i; break; }
        if (other >= 0) { post(EV_NOTE_ON, 53 + other + 12*(fm313.kb_oct - 1) - 24, 0, 1); fm313.held_key = other; }
        else { post(EV_NOTE_OFF, 0, 0, 0); fm313.held_key = -1; }
    }
    apply_touch();
}

static void tap_tempo(void)
{
    uint32_t now = hw_ms();
    if (fm313.ntap && now - fm313.tap[(fm313.ntap - 1) & 3] > 2000u) fm313.ntap = 0;
    fm313.tap[fm313.ntap & 3] = now; fm313.ntap++;
    if (fm313.ntap >= 2) {
        int k = fm313.ntap > 4 ? 4 : fm313.ntap;
        uint32_t span = fm313.tap[(fm313.ntap - 1) & 3] - fm313.tap[(fm313.ntap - k) & 3];
        float bpm = 60000.f*(float)(k - 1)/(float)span;
        fm313.s.bpm = clampf((float)(int)(bpm + 0.5f), 40.f, 300.f);
        apply_sound();
        fm313.dirty_top = 1;
        message("TAP TEMPO", (int)fm313.s.bpm, 1);
    }
}

/* ---- buttons */
static int layer_of(int b) { for (int l = 1; l < L_NUM; l++) if (LAYER_BTN[l] == b) return l; return L_NONE; }
static void layer_set(int l)
{
    if (l == fm313.layer) return;
    int was = fm313.layer;
    fm313.layer = l;
    if (was == L_ARP || l == L_ARP) scatter_apply();
    if (was == L_SEQ) fm313.seq_first = -1;
    if (was == L_ENV && fm313.menu != L_ENV) { fm313.env_key = fm313.env_dec_key = -1; }
    apply_touch();
    fm313.dirty_knobs = 1; fm313.panel_dirty = 1; fm313.dirty_top = 1;
}
static void menu_set(int l)                         /* the menu left open: one at a time */
{
    if (fm313.menu == L_ENV && l != L_ENV) fm313.env_key = fm313.env_dec_key = -1;
    fm313.menu = l;
    apply_touch();
    fm313.dirty_knobs = fm313.dirty_top = 1; fm313.panel_dirty = 1;
}
static void layer_tap(int l)                      /* a layer's button pressed and let go, nothing done meanwhile */
{
    menu_set(fm313.menu == l ? L_NONE : l);
}

void fm313_app_button(int b, int down)
{
    fm313_sound_t *s = &fm313.s;
    if (b == B_SEL) { fm313.shift = down; return; }
    /* step write: the buttons write */
    if (fm313.mode == M_WRITE && down) {
        fm313_step_t *st = &fm313.pat[fm313.cur].s[fm313.cursor];
        switch (b) {
        case B_FX: if (!locked()) { st->flags = 0; write_step(FM313_REST, 0, 0); fm313.wr_prev = -1; fm313.dirty_knobs = 1; } return;
        case B_LFO: if (!locked()) { st->flags = 0; write_step(FM313_TIE, 0, 0); fm313.dirty_knobs = 1; } return;
        case B_ARP: if (!locked()) { if (fm313.shift) dice_all(); else dice_step(); } return;   /* the dice: optional */
        case B_SEQ: return;
        case B_EDIT: if (!locked()) { fm313.pat[fm313.cur].len = (uint8_t)(fm313.cursor + 1); message("LENGTH", fm313.cursor + 1, 1);
                     fm313.dirty_grid = 1; fm313.panel_dirty = 1; } return;
        case B_ENV: s->wave = (uint8_t)((s->wave + 1) % 3); apply_sound(); message(WAVE_NAME[s->wave], 0, 0); fm313.dirty_top = 1; return;
        default: break;
        }
    }
    /* the layer buttons (PLAY mode) */
    int l = layer_of(b);
    if (l != L_NONE && fm313.mode == M_GLOBAL && down) {   /* in SETUP: a menu button leaves it for that menu at once */
        fm313.mode = M_PLAY; fm313.upd_ms = 0; fm313.menu = L_NONE;
        fm313.dirty_top = 1; fm313.panel_dirty = 1;
    }
    if (l == L_ENV && fm313.layer == L_SEQ) {            /* SEQ held + ENV: the sound to the pattern */
        if (down) { fm313.lay_used = 1; edit_action(ED_SOUND); }
        return;
    }
    if (l != L_NONE && fm313.mode == M_PLAY) {
        if (down) {
            if (fm313.shift) {                               /* SEL + it: its quick one-press thing */
                switch (l) {
                case L_ARP: fm313.sc_latched = !fm313.sc_latched; scatter_apply();          /* the last kind on / off */
                            { char b[28] = "SCATTER: "; cat(b, fm313.sc_latched ? SC_NAME[fm313.s.sc_type] : "OFF"); message(b, 0, 0); }
                            break;
                case L_ENV: fm313.s.wave = (uint8_t)((fm313.s.wave + 1) % 3); apply_sound();
                            { char b[24] = "WAVE: "; cat(b, WAVE_NAME[fm313.s.wave]); message(b, 0, 0); } break;
                case L_LFO: tap_tempo(); if (fm313.ntap < 2) message("TAP TEMPO: TAP IN TIME", 0, 0); break;
                case L_SEQ: select_pattern((fm313.queued >= 0 ? fm313.queued : fm313.cur) + 1); break;
                case L_EDIT: edit_action(ED_UNDO); break;
                default: break;
                }
                fm313.dirty_top = fm313.dirty_knobs = 1;
                return;
            }
            fm313.lay_used = 0; fm313.lay_down_ms = hw_ms();
            layer_set(l);
        } else if (fm313.layer == l) {
            int tap = !fm313.lay_used && hw_ms() - fm313.lay_down_ms < 450u;
            layer_set(L_NONE);
            if (tap) layer_tap(l);
        }
        return;
    }
    if (b == B_SAVE) {                                 /* tap: save; held 1 s: undo (as Felucca) */
        if (fm313.layer == L_SEQ && down) {
            fm313.lay_used = 1;
            fm313.pat[fm313.cur].pflags ^= FM313_P_LOCK;
            message((fm313.pat[fm313.cur].pflags & FM313_P_LOCK) ? "PATTERN LOCKED" : "PATTERN UNLOCKED", 0, 0);
            fm313.dirty_top = 1; return;
        }
        if (fm313.mode == M_GLOBAL) {
            if (!down) return;
            if (fm313.upd_ms && hw_ms() - fm313.upd_ms < 3000u) { fm313_app_store_save(); hw_enter_update(); return; }
            fm313.upd_ms = hw_ms() | 1u; fm313.panel_dirty = 1;
            message("SAVE AGAIN: UPDATE MODE", 0, 0);
            return;
        }
        if (down) { fm313.save_ms = hw_ms() | 1u; fm313.save_held = 0; return; }
        if (fm313.save_ms && !fm313.save_held) {
            int rc = fm313_app_store_save();
            message(rc ? "SAVE FAILED" : "SAVED", rc, rc != 0);
            if (!rc) msg_sub("HOLD SAVE 1 S: UNDO THE LAST EDIT");
        }
        fm313.save_ms = 0;
        return;
    }
    if (!down) return;
    switch (b) {
    case B_PLAY:
        if (fm313.layer == L_SEQ) {                      /* SEQ + PLAY: the grid */
            fm313.lay_used = 1;
            if (locked()) break;
            fm313.pat[fm313.cur].pflags ^= FM313_P_TRIP;
            message((fm313.pat[fm313.cur].pflags & FM313_P_TRIP) ? "GRID: TRIPLETS" : "GRID: 16THS", 0, 0);
            fm313.dirty_top = 1; fm313.panel_dirty = 1; break;
        }
        if (s->ext_clock) { message("CLOCK IS MIDI", 0, 0); break; }
        post(fm313.playing ? EV_STOP : EV_START, 0, 0, 0);
        fm313.playing = !fm313.playing;           /* the ISR confirms */
        fm313.queued = -1;
        fm313.dirty_top = 1;
        break;
    case B_REC:
        if (fm313.shift && fm313.mode == M_PLAY) {         /* realtime record: arm / disarm */
            if (!fm313.rt_rec && locked()) break;
            fm313.rt_rec = !fm313.rt_rec;
            if (fm313.rt_rec) { fm313.undo = fm313.pat[fm313.cur]; fm313.rec_key = -1; fm313.rec_step_seen = -1; fm313.rec_last = -1; }
            message(fm313.rt_rec ? "REALTIME REC: ON" : "REALTIME REC: OFF", 0, 0);
            if (fm313.rt_rec) msg_sub(fm313.playing ? "PLAY THE KEYS  HOLD EDIT: ERASE" : "PRESS PLAY, THEN PLAY THE KEYS");
            fm313.dirty_top = 1; break;
        }
        if (fm313.mode != M_WRITE && locked()) break;
        fm313.rt_rec = 0;
        fm313.mode = fm313.mode == M_WRITE ? M_PLAY : M_WRITE;
        if (fm313.mode == M_WRITE) { fm313.cursor = 0; fm313.wr_key = fm313.wr_prev = -1; fm313.undo = fm313.pat[fm313.cur]; layer_set(L_NONE); menu_set(L_NONE); }
        fm313.dirty_top = fm313.dirty_grid = 1;
        break;
    case B_HOME:                                       /* back to the scope: every layer and latch off */
        fm313.mode = M_PLAY; fm313.menu = L_NONE;
        fm313.env_key = fm313.env_dec_key = -1; layer_set(L_NONE); apply_touch();
        if (fm313.sc_latched) { fm313.sc_latched = 0; scatter_apply(); }
        if (fm313.q.transpose) post(EV_TRANSPOSE, 0, 0, 0);
        fm313.msg[0] = 0;
        fm313.dirty_top = fm313.dirty_grid = fm313.dirty_knobs = 1; fm313.panel_dirty = 1;
        break;
    case B_GLO:
        fm313.mode = fm313.mode == M_GLOBAL ? M_PLAY : M_GLOBAL;
        layer_set(L_NONE);
        fm313.dirty_top = 1; fm313.panel_dirty = 1;
        break;
    case B_OCTDN: case B_OCTUP:
        if (fm313.layer == L_SEQ) {                      /* SEQ + OCT: the bank */
            fm313.lay_used = 1;
            fm313.bank = clampi(fm313.bank + (b == B_OCTUP ? 1 : -1), 0, FM313_NPAT/16 - 1);
            fm313.panel_dirty = 1; break;
        }
        fm313.kb_oct = clampi(fm313.kb_oct + (b == B_OCTUP ? 1 : -1), 0, 2);
        s->kb_oct = (uint8_t)fm313.kb_oct;
        message("KEY OCTAVE", fm313.kb_oct - 1, 1);
        break;
    }
}

/* ---- SETUP: SELECT moves, ALGORITHM / the knobs change */
static const char *const SU_NAME[SU_NUM] = { "MIDI CH", "CLOCK", "MIDI OUT", "LOCAL", "TUNE", "LEVEL",
                                             "ACCENT AT", "AFTERTOUCH", "SCOPE", "TIPS",
                                             "LEDS", "PHONES", "UPDATE" };
/* what the row under the cursor is for, at the panel's foot */
static const char *const SU_HELP[SU_NUM] = { "MIDI IN AND OUT  (ALL: ANY CHANNEL)", "MIDI: TEMPO AND START FROM USB",
    "NOTES AND CLOCK OUT OVER USB", "OFF: THE KEYS ONLY SEND MIDI", "THE PITCH OF A4", "THE SOUND BEFORE MASTER",
    "MIDI NOTES THIS LOUD: ACCENT", "MIDI AFTERTOUCH MOVES", "THE SCREEN ABOVE THE STEPS",
    "THE HELP LINE UNDER THE SCOPE", "NIGHT: THE DARK LEDS GLOW FAINTLY", 0, 0 };
static void su_value(int i, char *b)
{
    const fm313_sound_t *s = &fm313.s;
    static const char *const SM[3] = { "WAVE", "LEVEL", "OFF" };
    static const char *const PT[3] = { "FILTER", "VIBRATO", "OFF" };
    b[0] = 0;
    switch (i) {
    case SU_MIDICH: if (s->midi_ch) cat_int(b, s->midi_ch); else cat(b, "ALL"); break;
    case SU_CLOCK: cat(b, s->ext_clock ? "MIDI" : "INTERNAL"); break;
    case SU_MIDIOUT: cat(b, s->midi_out ? "ON" : "OFF"); break;
    case SU_LOCAL: cat(b, fm313.su.local_off ? "OFF" : "ON"); break;
    case SU_TUNE: { int t = (int)(hz_of(s->tune)*10.f + 0.5f); cat_int(b, t/10); cat(b, "."); cat_int(b, t % 10); cat(b, " HZ"); break; }
    case SU_LEVEL: cat_int(b, (int)(s->level*100.f + 0.5f)); break;
    case SU_ACCENT: cat(b, "VEL "); cat_int(b, fm313.su.vel_thr); break;
    case SU_PRESS: cat(b, PT[fm313.su.press_to]); break;
    case SU_SCOPE: cat(b, SM[fm313.su.scope_mode]); break;
    case SU_TIPS: cat(b, fm313.su.tips_off ? "OFF" : "ON"); break;
    case SU_LEDS: { static const char *const LM[3] = { "NORMAL", "NIGHT", "NIGHT+" }; cat(b, LM[fm313.su.leds % 3]); break; }
    case SU_PHONES: cat(b, fm313.su.phones ? "LEFT FIX" : "NORMAL"); break;
    case SU_UPDATE: cat(b, fm313.upd_ms && hw_ms() - fm313.upd_ms < 3000u ? "SAVE AGAIN" : "SAVE 2X"); break;
    }
}
static void setup_turn(int steps)
{
    fm313_sound_t *s = &fm313.s;
    int d = steps > 0 ? 1 : -1;
    switch (fm313.su_cur) {
    case SU_MIDICH: s->midi_ch = (uint8_t)clampi(s->midi_ch + steps, 0, 16); break;
    case SU_CLOCK: s->ext_clock = steps > 0; apply_sound(); break;
    case SU_MIDIOUT: s->midi_out = steps > 0; break;
    case SU_LOCAL: fm313.su.local_off = steps > 0 ? 0 : 1; break;
    case SU_TUNE: {
        float hz = (float)(int)(hz_of(s->tune)*2.f + 0.5f)*0.5f + 0.5f*(float)steps;
        s->tune = tune_of(clampf(hz, 430.f, 450.f)); apply_sound(); break; }
    case SU_LEVEL: s->level = step100(s->level, steps); apply_sound(); break;
    case SU_ACCENT: fm313.su.vel_thr = (uint8_t)clampi(fm313.su.vel_thr + steps, 20, 127); break;
    case SU_PRESS: fm313.su.press_to = (uint8_t)clampi(fm313.su.press_to + d, 0, 2); apply_touch(); break;
    case SU_SCOPE: fm313.su.scope_mode = (uint8_t)clampi(fm313.su.scope_mode + d, 0, 2); fm313.scope_live = 0; break;
    case SU_TIPS: fm313.su.tips_off = steps > 0 ? 0 : 1; fm313.scope_live = 0; break;
    case SU_LEDS: fm313.su.leds = (uint8_t)clampi(fm313.su.leds + d, 0, 2); break;
    case SU_PHONES: fm313.su.phones = steps > 0; break;
    default: break;
    }
    fm313.panel_dirty = 1;
}

/* ---------------------------------------------------------------- screen
 * A phosphor oscilloscope, one grammar for every field:
 *   HEAD   y   0.. 24  pattern (bank + number) and its marks, tempo, state
 *   PANEL  y  24..152  the trace; or what is being set (a value, a layer's map, SETUP, step write), always in the
 *                      same frame, a title at the top left, its controls' hint at the top right
 *   STEPS  y 152..190  16 dots with their notes: red = accent, ring = slide, white = now
 *   KEYS   y 190..240  five soft keys: what the four knobs and ALGORITHM do right now
 * Text is placed by the capitals' ink (tools/gen_fonts.py cap_y / cap_h), so a line is centred where it looks
 * centred, whatever the font's cell. */
#define K_BG     FM313_RGB(0, 0, 0)
#define K_HOT    FM313_RGB(210, 255, 220)      /* the beam's core */
#define K_GREEN  FM313_RGB(24, 255, 80)
#define K_MID    FM313_RGB(0, 190, 60)
#define K_DIM    FM313_RGB(0, 120, 38)
#define K_GLOW   FM313_RGB(0, 96, 28)
#define K_FRAME  FM313_RGB(0, 120, 36)
#define K_GRID   FM313_RGB(0, 70, 22)
#define K_RED    FM313_RGB(255, 24, 40)
#define K_REDG   FM313_RGB(120, 0, 12)
#define K_LIT    FM313_RGB(0, 44, 14)
static const char *const NOTE_NAME[13] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", "C" };

static int trk(const fm313_font_t *f) { return f->h <= 13u ? 1 : 0; }   /* the small sizes a pixel apart */
static int tw(const fm313_font_t *f, const char *s) { return gfx_text_w(f, s, trk(f)); }
static int ty(const fm313_font_t *f, int cy) { return cy - (int)f->cap_y - (int)f->cap_h/2; }
static int txt(int x, int cy, const fm313_font_t *f, const char *s, uint16_t c) { return gfx_text_over(x, ty(f, cy), f, s, c, trk(f), 1.f); }
static int hint(int x, int cy, const char *s, uint16_t c) { return gfx_text_over(x, ty(&FM313_FONT_XS, cy), &FM313_FONT_XS, s, c, 0, 1.f); }   /* tight */
static int hint_w(const char *s) { return gfx_text_w(&FM313_FONT_XS, s, 0); }
static void txt_c(int cx, int cy, const fm313_font_t *f, const char *s, uint16_t c) { txt(cx - tw(f, s)/2, cy, f, s, c); }
static void txt_r(int rx, int cy, const fm313_font_t *f, const char *s, uint16_t c) { txt(rx - tw(f, s), cy, f, s, c); }

static void frame_grid(int x, int y, int w, int h)
{
    gfx_frame(x, y, w, h, 1, K_FRAME);
    for (int i = 1; i < 8; i++)                       /* dotted divisions: 8 across, 4 down */
        for (int yy = y + 3; yy < y + h - 2; yy += 4) hw_box_rect(x + i*w/8, yy, 1, 1, K_GRID);
    for (int i = 1; i < 4; i++)
        for (int xx = x + 3; xx < x + w - 2; xx += 4) hw_box_rect(xx, y + i*h/4, 1, 1, K_GRID);
}
/* a panel: the frame, its title, its hint; the content below y 22 */
#define P_X0 6
#define P_W 228
#define P_TITLE_CY 12
static void panel_begin(const char *title, const char *hint, int grid)
{
    hw_box_begin(240, 128, K_BG);
    if (grid) frame_grid(P_X0, 2, P_W, 124); else gfx_frame(P_X0, 2, P_W, 124, 1, K_FRAME);
    if (title) txt(14, P_TITLE_CY, &FM313_FONT_S, title, K_GREEN);
    if (hint) txt_r(226, P_TITLE_CY, &FM313_FONT_XS, hint, K_DIM);
}
static const char *menu_hint(int l)                    /* a menu's top-right corner: how it goes away */
{
    static const char *const T[L_NUM] = { "", "HOME OR FX: OUT", "HOME OR SEQ: OUT", "HOME OR EDIT: OUT",
                                          "HOME OR ARP: OUT", "HOME OR ENV: OUT", "HOME OR LFO: OUT" };
    return fm313.menu == l ? T[l] : "WHILE HELD";
}
static void panel_end(void) { hw_box_blit(0, 24); fm313.scope_live = 0; }
static void ladder(int x, int y, int n, int h, float v, uint16_t on)   /* a level as n ticks, 5 px apart */
{
    int lit = (int)(clampf(v, 0.f, 1.f)*(float)n + 0.5f);
    for (int i = 0; i < n; i++) hw_box_rect(x + i*5, y, 3, h, i < lit ? on : K_GRID);
}

/* ---- HEAD */
static void draw_head(void)
{
    char b[24];
    hw_box_begin(240, 24, K_BG);
    const int cy = 12;
    if (fm313.mode == M_WRITE) {
        b[0] = 0; cat(b, "STEP "); { char n[4]; put_num(n, fm313.cursor + 1, 2); cat(b, n); }
        cat(b, "/"); cat_int(b, fm313.pat[fm313.cur].len);
        txt(6, cy, &FM313_FONT_M, b, K_HOT);
        gfx_disc(176.f, (float)cy, 4.f, K_RED);
        txt(186, cy, &FM313_FONT_S, "WRITE", K_RED);
    } else {
        pat_name(b, fm313.cur);
        if (fm313.queued >= 0) { char n[5]; cat(b, ">"); pat_name(n, fm313.queued); cat(b, n); }
        int px = txt(6, cy, &FM313_FONT_M, b, K_HOT);
        {                                              /* the pattern's marks: chain, triplets, lock, sound */
            const fm313_pattern_t *p = &fm313.pat[fm313.cur];
            char f[24] = "";
            if (fm313.q.transpose) { cat(f, fm313.q.transpose > 0 ? "+" : ""); cat_int(f, fm313.q.transpose); }
            if (fm313.chain_n > 1) { cat(f, f[0] ? " x" : "x"); cat_int(f, fm313.chain_n); }
            if (p->pflags & FM313_P_TRIP) cat(f, f[0] ? " 3" : "3");
            if (p->pflags & FM313_P_LOCK) cat(f, f[0] ? " L" : "L");
            if (p->tone) cat(f, f[0] ? " S" : "S");
            while (f[0] && px + 5 + tw(&FM313_FONT_XS, f) >= 92) {   /* too long: the last mark goes */
                char *e = f; while (*e) e++;
                while (e > f && *e != ' ') e--;
                *e = 0;
            }
            if (f[0]) txt(px + 5, cy, &FM313_FONT_XS, f, fm313.q.transpose ? K_GREEN : K_MID);
        }
        b[0] = 0;
        if (fm313.s.ext_clock) cat(b, "EXT");
        else { int t10 = (int)(fm313.s.bpm*10.f + 0.5f); cat_int(b, t10/10); cat(b, "."); cat_int(b, t10 % 10); }
        int x = txt(96, cy, &FM313_FONT_M, b, K_HOT);
        txt(x + 4, cy, &FM313_FONT_XS, fm313.s.ext_clock ? "CLK" : "BPM", K_MID);
        if (fm313.mode == M_GLOBAL) txt_r(234, cy, &FM313_FONT_S, "SETUP", K_GREEN);
        else if (fm313.q.sc_type != SC_OFF) {            /* SCATTER: its kind and depth, red */
            static const char *const SH[SC_NUM] = { "", "REV", "RPT", "HALF", "RAT", "GATE", "SKIP", "RND", "STUT" };
            char c[12] = ""; cat(c, SH[fm313.q.sc_type]); cat(c, " "); cat_int(c, fm313.s.sc_depth);
            txt_r(234, cy, &FM313_FONT_S, c, K_RED);
        }
        else {                                         /* the state: an icon 6 px before its word */
            const char *w = fm313.rt_rec ? "REC" : fm313.playing ? "RUN" : "STOP";
            int x = 234 - tw(&FM313_FONT_S, w);
            if (fm313.rt_rec) { gfx_disc((float)(x - 10), (float)cy, 4.f, K_RED); txt(x, cy, &FM313_FONT_S, w, K_RED); }
            else if (fm313.playing) { gfx_tri_right(x - 15, cy - 5, 9, 11, K_GREEN); txt(x, cy, &FM313_FONT_S, w, K_GREEN); }
            else { hw_box_rect(x - 14, cy - 4, 9, 9, K_MID); txt(x, cy, &FM313_FONT_S, w, K_MID); }
        }
    }
    hw_box_blit(0, 0);
}

/* ---- the scope (after Felucca's ui_graph.c graph_scope): one min/max a column, one line with a glow; the frame
 * drawn once, then only the columns' changed pixels, straight in the screen buffer, the band they touch sent */
#define SC_X0 8
#define SC_N 224
#define SC_TOP 20
static int sc_bot = 122, sc_cy = 71, sc_amp = 46;    /* with the help line: 105, 62, 40 */
static int16_t sc_lo[SC_N], sc_hi[SC_N];
static int sc_label = -1;
static uint16_t grid_at(int x, int y)                 /* frame_grid(6, 2, 228, 124)'s dots, field coordinates */
{
    for (int i = 1; i < 8; i++)
        if (x == 6 + i*228/8 && y >= 5 && y < 124 && ((y - 5) & 3) == 0) return K_GRID;
    for (int i = 1; i < 4; i++)
        if (y == 2 + i*124/4 && x >= 9 && x < 232 && ((x - 9) & 3) == 0) return K_GRID;
    return K_BG;
}
/* ---- the help line under the scope: what the panel does now, a tip every 4.5 s */
static const char *const TIP_STOP[] = {
    "KEYS PLAY    SEL + KEY: ACCENT", "PLAY: START   REC: WRITE STEPS", "TAP A BUTTON: ITS MENU  HOME: OUT",
    "TAP FX: THE KNOBS SET THE EFFECTS", "PRESETS: SOUND    SELECT: TEMPO", "SEL+ENV: NEXT WAVE",
    "TAP ARP: SCATTER MENU", "SEL+LFO IN TIME: TAP TEMPO" };
static const char *const TIP_RUN[] = {
    "KEYS TRANSPOSE    C4: BACK TO 0", "TAP SEQ: THE PATTERNS ON THE KEYS",
    "TAP ARP: SCATTER MENU", "SEL+ARP: SCATTER ON / OFF", "SEL+REC: RECORD FROM THE KEYS", "TAP LFO: XY PLAY   SEL+LFO: TEMPO",
    "TAP ENV: THE KEYS SET THE ENVELOPE", "TAP FX: THE KNOBS SET THE EFFECTS" };
static int tips_on(void) { return !fm313.su.tips_off && fm313.su.scope_mode != 1; }
static const char *tip_now(uint32_t now)
{
    if (fm313.rt_rec) return fm313.playing ? "RECORDING: PLAY  HOLD EDIT: ERASE" : "REC IS ARMED: PRESS PLAY";
    if (fm313.sc_latched) return "SCATTER RUNS  ARP: MENU  HOME: OFF";
    if (fm313.playing) return TIP_RUN[(now/4500u) % (sizeof TIP_RUN/sizeof *TIP_RUN)];
    return TIP_STOP[(now/4500u) % (sizeof TIP_STOP/sizeof *TIP_STOP)];
}
static const char *tip_drawn;
static int tip_update(uint32_t now)                    /* 1: drawn (the line changed) */
{
    if (!tips_on()) return 0;
    const char *t = tip_now(now);
    if (t == tip_drawn) return 0;
    hw_box_begin(240, 18, K_BG);                       /* field rows 107..124, inside the frame */
    hw_box_rect(6, 0, 1, 18, K_FRAME); hw_box_rect(233, 0, 1, 18, K_FRAME);
    for (int x = 10; x < 231; x += 4) hw_box_rect(x, 0, 2, 1, K_GRID);
    hint(120 - hint_w(t)/2, 10, t, K_MID);
    hw_box_blit(0, 24 + 107);
    tip_drawn = t;
    return 1;
}
static void scope_bg(void)
{
    const fm313_sound_t *s = &fm313.s;
    hw_box_begin(240, 128, K_BG);
    frame_grid(6, 2, 228, 124);
    hw_box_rect(7, 3, 226, 16, K_BG);
    char b[24] = "CH1 "; cat(b, WAVE_NAME[s->wave]);
    txt(12, P_TITLE_CY, &FM313_FONT_XS, b, K_MID);
    if (s->preset) { char n[24] = ""; cat(n, PRESETS[s->preset - 1].name); txt_r(228, P_TITLE_CY, &FM313_FONT_XS, n, K_MID); }
    hw_box_blit(0, 24);
    if (tips_on()) { sc_bot = 105; sc_cy = 62; sc_amp = 40; } else { sc_bot = 122; sc_cy = 71; sc_amp = 46; }
    tip_drawn = 0; tip_update(hw_ms());
    for (int c = 0; c < SC_N; c++) { sc_lo[c] = -1; sc_hi[c] = -2; }
    sc_label = s->wave*64 + s->preset;
    fm313.scope_live = 1;
}
static void draw_scope(void)
{
    const fm313_voice_t *v = &fm313.v;
    const fm313_sound_t *s = &fm313.s;
    if (!fm313.scope_live || sc_label != s->wave*64 + s->preset) { scope_bg(); return; }
    if (!hw_fb_idle()) return;                        /* rows still going out: changed now, they would tear */
    if (tip_update(hw_ms())) return;
    float hz = 440.f*fm313_exp2((v->pitch + s->tune - 69.f)*(1.f/12.f));
    int W = (int)(2.f*FM313_FS/hz + 0.5f);              /* two cycles across */
    if (W < 112) W = 112;
    if (W > 1800) W = 1800;
    uint32_t cnt = v->out_count, wp = v->wrap_pos;
    if (cnt - wp > 2048u - (uint32_t)W || cnt - wp > 4000u) wp = cnt - 2u;   /* no trigger: free run */
    uint32_t start = wp - (uint32_t)W;
    static int sc_peak = 8000;                        /* auto-scaled, slowly */
    int pk = 1500;
    for (int k = 0; k < W; k++) { int q = v->scope[(start + (uint32_t)k) & 2047u]; if (q < 0) q = -q; if (q > pk) pk = q; }
    sc_peak = pk > sc_peak ? pk : sc_peak + (pk - sc_peak)/8;
    const int A = sc_amp, P = sc_peak;
    int prev = sc_cy, top = 999, bot = -1;
    for (int c = 0; c < SC_N; c++) {
        int a = c*W/SC_N, e = (c + 1)*W/SC_N;
        if (e <= a) e = a + 1;
        int mn = 32767, mx = -32768;
        for (int k = a; k < e; k++) {
            int q = v->scope[(start + (uint32_t)k) & 2047u];
            if (q < mn) mn = q;
            if (q > mx) mx = q;
        }
        int yt = sc_cy - mx*A/P, yb = sc_cy - mn*A/P;
        if (prev < yt) yt = prev;
        if (prev > yb) yb = prev;
        prev = sc_cy - (mn + mx)/2*A/P;
        yb += 1;
        if (yt < SC_TOP + 1) yt = SC_TOP + 1;
        if (yb > sc_bot - 1) yb = sc_bot - 1;
        if (yt > yb) yt = yb;
        int gt = yt, gb = yb;                         /* 0.7.4: a plain line, no glow */
        int ol = sc_lo[c], oh = sc_hi[c], x = SC_X0 + c;
        if (ol == gt && oh == gb) continue;
        for (int y = ol; y <= oh; y++)
            if (y < gt || y > gb) hw_fb_vspan(x, 24 + y, 24 + y, grid_at(x, y));
        hw_fb_vspan(x, 24 + yt, 24 + yb, K_GREEN);
        int t = ol >= 0 && ol < gt ? ol : gt, u = oh > gb ? oh : gb;
        if (t < top) top = t;
        if (u > bot) bot = u;
        sc_lo[c] = (int16_t)gt; sc_hi[c] = (int16_t)gb;
    }
    if (bot >= top) hw_fb_dirty(0, 24 + top, 240, bot - top + 1);
}
static void draw_levels(void)                         /* SETUP > SCOPE LEVEL */
{
    const fm313_voice_t *v = &fm313.v;
    if (!fm313.scope_live) {
        panel_begin(0, 0, 1);
        txt(14, 42, &FM313_FONT_S, "OUT", K_GREEN);
        txt(14, 86, &FM313_FONT_S, "FILTER", K_GREEN);
        hw_box_blit(0, 24);
        fm313.scope_live = 1;
    }
    int pk = 0;
    for (int k = 0; k < 512; k++) { int q = v->scope[(v->out_count - (uint32_t)k) & 2047u]; if (q < 0) q = -q; if (q > pk) pk = q; }
    float lv[2] = { (float)pk*(1.f/16384.f), (v->oct_now - 7.f)*(1.f/7.f) };
    for (int j = 0; j < 2; j++) {
        hw_box_begin(240, 16, K_BG);
        hw_box_rect(6, 0, 1, 16, K_FRAME); hw_box_rect(233, 0, 1, 16, K_FRAME);
        int lit = (int)(clampf(lv[j], 0.f, 1.f)*43.f + 0.5f);
        for (int i = 0; i < 43; i++) hw_box_rect(14 + i*5, 1, 3, 14, i < lit ? (j ? K_GREEN : (i > 37 ? K_RED : K_HOT)) : K_GRID);
        hw_box_blit(0, 24 + 52 + j*44);
    }
}

/* ---- a value or a message, in the panel while it is fresh */
static int str_cmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a != *b; }
static char mp_name[24];
static int mp_live;
static void draw_msg_panel(void)
{
    int same = mp_live && fm313.msg_has_val && fm313.msg_ladder && !str_cmp(mp_name, fm313.msg_name);
    if (!same) {
        panel_begin(fm313.msg_name[0] ? fm313.msg_name : 0, 0, 0);
        if (!fm313.msg_has_val && fm313.msg_val_text[0]) {
            const fm313_font_t *f = tw(&FM313_FONT_L, fm313.msg_val_text) <= 210 ? &FM313_FONT_L : &FM313_FONT_M;
            txt(14, fm313.msg_name[0] ? 66 : 64, f, fm313.msg_val_text, K_HOT);
        }
    } else hw_box_begin(240, 84, K_BG);              /* the same knob again: its number and ladder only */
    int oy = same ? 26 : 0;
    if (same) { hw_box_rect(6, 0, 1, 84, K_FRAME); hw_box_rect(233, 0, 1, 84, K_FRAME); }
    if (fm313.msg_has_val) {
        char b[12]; b[0] = 0; cat_int(b, fm313.msg_val);
        txt(14, 58 - oy, &FM313_FONT_XL, b, K_HOT);
        if (fm313.msg_ladder) ladder(14, 94 - oy, 43, 14, (float)fm313.msg_val*0.01f, fm313.msg_red ? K_RED : K_GREEN);
    }
    if (!same && fm313.msg_sub[0]) {                     /* what else the button does: a rule above it */
        for (int x = 14; x < 226; x += 4) hw_box_rect(x, 100, 2, 1, K_GRID);
        hint(14, 113, fm313.msg_sub, K_MID);
    }
    hw_box_blit(0, 24 + oy);
    mp_name[0] = 0; cat(mp_name, fm313.msg_name);
    mp_live = 1;
    fm313.scope_live = 0;
}

/* ---- the layers' maps */
static void draw_fx_panel(void)
{
    const fm313_sound_t *s = &fm313.s;
    panel_begin("EFFECTS", menu_hint(L_FX), 0);
    static const char *const LB[8] = { "DRIVE", "TONE", "DELAY", "REVERB", "TYPE", "TIME", "FEEDBK", "SIZE" };
    static const char *const CT[8] = { "K1", "K2", "K3", "K4", "SEL", "ALGO", "PRE", "SEL K4" };
    float vv[8] = { s->drive, s->tone, s->dmix, s->rmix, -1.f, -1.f, s->dfb, s->rtime };
    const void *id[8] = { &s->drive, &s->tone, &s->dmix, &s->rmix, &s->dtype, &s->ddiv, &s->dfb, &s->rtime };
    for (int i = 0; i < 8; i++) {
        int col = i % 4, row = i / 4, x = 12 + col*55, y = 24 + row*50, w = 51;
        int on = fm313.touch_ms && fm313.touched == id[i];
        if (on) hw_box_rect(x, y, w, 46, K_LIT);
        gfx_frame(x, y, w, 46, 1, on ? K_GREEN : K_GRID);
        txt(x + 4, y + 8, &FM313_FONT_XS, LB[i], K_MID);
        hint(x + w - 4 - hint_w(CT[i]), y + 38, CT[i], K_DIM);
        char b[12] = "";
        if (i == 4) cat(b, DT_NAME[s->dtype]);
        else if (i == 5) cat(b, DIV_NAME[s->ddiv]);
        else cat_int(b, (int)(vv[i]*100.f + 0.5f));
        txt(x + 4, y + 24, &FM313_FONT_M, b, on ? K_HOT : (i < 2 ? K_RED : K_GREEN));
    }
    panel_end();
}
/* ---- the key menus: the screen draws the keyboard itself (16 white keys F3..G5, the black ones over the joints),
 * each key showing what it does in this menu. One look tells which key: no numbers to count. */
#define KB_X0 16
#define KB_W 13
#define KB_YT 44
#define KB_YB 121
static const uint8_t BLACK_AFTER[16] = { 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 0 };   /* F# G# A# - C# D# - ... */
static int kb_x(int i) { return KB_X0 + i*KB_W; }
static int kb_bx(int j)                                 /* black key j (0..10): its left edge */
{
    for (int i = 0, n = 0; i < 15; i++) if (BLACK_AFTER[i]) { if (n == j) return KB_X0 + (i + 1)*KB_W - 4; n++; }
    return 0;
}
static void kb_white(int i, uint16_t fill, uint16_t frame)
{
    int x = kb_x(i);
    if (fill != K_BG) hw_box_rect(x + 1, KB_YT + 1, KB_W - 1, KB_YB - KB_YT - 1, fill);
    gfx_frame(x, KB_YT, KB_W + 1, KB_YB - KB_YT + 1, 1, frame);
}
static void kb_word(int i, const char *w, uint16_t c)  /* a white key's word, read upwards from its foot */
{
    gfx_text_up(kb_x(i) + KB_W/2 - 6, KB_YB - 3, &FM313_FONT_XS, w, c, 0);
}
static void kb_black(int j, uint16_t fill, uint16_t frame, const char *lab, uint16_t ink)
{
    int x = kb_bx(j);
    hw_box_rect(x, KB_YT, 9, 20, fill);
    gfx_frame(x, KB_YT, 9, 20, 1, frame);
    if (lab) hint(x + 2, KB_YT + 12, lab, ink);
}
static void kb_blacks(void) { for (int j = 0; j < 11; j++) kb_black(j, K_BG, K_GRID, 0, 0); }
static void kb_heads(const char *l, const char *r, uint16_t c)   /* the two words over the keys, a rule under each */
{
    if (l) txt(KB_X0, 31, &FM313_FONT_XS, l, c);
    if (r) txt_r(KB_X0 + 16*KB_W, 31, &FM313_FONT_XS, r, c);
}

static void draw_seq_panel(void)
{
    panel_begin("PATTERNS", menu_hint(L_SEQ), 0);
    {
        char l[24] = "BANK "; char c[2] = { (char)('A' + fm313.bank), 0 }; cat(l, c); 
        kb_heads(l, fm313.layer == L_SEQ ? "PLAY:3  SAVE:LOCK" : "HOLD 1, PRESS 2: CHAIN", K_MID);
    }
    for (int i = 0; i < 16; i++) {
        int p = fm313.bank*16 + i, cur = p == fm313.cur, q = p == fm313.queued;
        int ch = fm313.chain_n > 1 && p >= fm313.chain_start && p < fm313.chain_start + fm313.chain_n;
        kb_white(i, cur ? K_GREEN : ch ? K_LIT : K_BG, q ? K_RED : cur ? K_GREEN : K_FRAME);
        char n[4]; put_num(n, i + 1, 2);
        int x = kb_x(i);
        kb_word(i, n, cur ? K_BG : K_HOT);
        if (ch) hw_box_rect(x + 3, KB_YB - 22, KB_W - 5, 2, K_RED);
        if (fm313.pat[p].pflags & FM313_P_LOCK) hw_box_rect(x + 5, KB_YB - 30, 3, 3, cur ? K_BG : K_MID);
    }
    for (int j = 0; j < 11; j++) {                     /* black keys 1-4: the banks */
        char b[2] = { (char)('A' + j), 0 };
        if (j < 4) kb_black(j, j == fm313.bank ? K_GREEN : K_BG, j == fm313.bank ? K_GREEN : K_MID, b, j == fm313.bank ? K_BG : K_MID);
        else kb_black(j, K_BG, K_GRID, 0, 0);
    }
    panel_end();
}
static void draw_edit_panel(void)
{
    panel_begin("EDIT", menu_hint(L_EDIT), 0);
    kb_heads("THE PATTERN", "SEL+EDIT: UNDO", K_MID);
    for (int i = 0; i < 16; i++) {
        kb_white(i, K_BG, i < ED_NUM ? (i == ED_CLEAR ? K_RED : K_FRAME) : K_GRID);
        if (i < ED_NUM) kb_word(i, ED_NAME[i], i == ED_CLEAR ? K_RED : K_HOT);
    }
    kb_blacks();
    panel_end();
}
static void draw_arp_panel(void)                       /* no keyboard drawn: black keys = kinds, white keys = depth */
{
    const int run = fm313.sc_latched, kind = fm313.s.sc_type, d = fm313.s.sc_depth;
    panel_begin("SCATTER", menu_hint(L_ARP), 0);
    txt(12, 31, &FM313_FONT_XS, "KIND: BLACK KEYS 1-8", K_MID);
    txt_r(228, 31, &FM313_FONT_XS, run ? "RUNNING" : "OFF", run ? K_RED : K_DIM);
    for (int i = 0; i < 8; i++) {                      /* in the black keys' order: 1-4 above, 5-8 below */
        int x = 12 + (i % 4)*54, y = 38 + (i / 4)*20, on = run && i + 1 == kind;
        if (on) hw_box_rect(x, y, 52, 17, K_RED); else gfx_frame(x, y, 52, 17, 1, K_GRID);
        char n[2] = { (char)('1' + i), 0 };
        hint(x + 2, y + 9, n, on ? K_BG : K_DIM);
        hint(x + 9 + (42 - hint_w(SC_NAME[i + 1]))/2, y + 9, SC_NAME[i + 1], on ? K_BG : K_HOT);
    }
    txt(12, 85, &FM313_FONT_XS, "DEPTH: WHITE KEYS 1-16", K_MID);
    { char v[4] = ""; cat_int(v, d); txt_r(228, 85, &FM313_FONT_S, v, run ? K_RED : K_HOT); }
    for (int j = 0; j < 16; j++) {                     /* a cell a white key, lit up to the depth, the chosen one full */
        int x = 12 + (j % 8)*27, y = 94 + (j / 8)*16, on = j + 1 == d;   /* 1-8 above, 9-16 below */
        if (j + 1 <= d) hw_box_rect(x, y, 25, 14, on ? K_RED : K_REDG); else gfx_frame(x, y, 25, 14, 1, K_GRID);
        char n[4] = ""; cat_int(n, j + 1);
        hint(x + (25 - hint_w(n))/2 + 1, y + 7, n, on ? K_BG : K_HOT);
    }
    panel_end();
}
static void draw_touch_panel(int env)                 /* ENV strip / XY play */
{
    const fm313_params_t *p = &fm313.v.p;
    if (env) {                                          /* white keys: env mod rising; black keys: decay rising */
        panel_begin("ENV STRIP", menu_hint(L_ENV), 0);
        char l[20] = "ENV MOD "; cat_int(l, (int)(p->envmod*100.f + 0.5f));
        char r[20] = "BLACK: DECAY "; cat_int(r, (int)(p->decay*100.f + 0.5f));
        kb_heads(l, r, K_MID);
        int ek = fm313.env_key >= 0 ? white_of(fm313.env_key) : -1, dk = fm313.env_dec_key >= 0 ? black_of(fm313.env_dec_key) : -1;
        for (int i = 0; i < 16; i++) {
            int h = 4 + i*3;
            kb_white(i, i == ek ? K_LIT : K_BG, i == ek ? K_GREEN : K_FRAME);
            hw_box_rect(kb_x(i) + 3, KB_YB - 3 - h, KB_W - 5, h, i == ek ? K_HOT : K_DIM);
        }
        for (int j = 0; j < 11; j++) {
            int h = 2 + j*16/10;
            kb_black(j, j == dk ? K_GREEN : K_BG, j == dk ? K_GREEN : K_MID, 0, 0);
            hw_box_rect(kb_bx(j) + 2, KB_YT + 18 - h, 5, h, j == dk ? K_BG : K_DIM);
        }
    } else {                                            /* XY play: the keys held, the glide, the pressure */
        panel_begin("XY PLAY", menu_hint(L_LFO), 0);
        char l[24] = "GLIDE  NOW "; cat(l, NOTE_NAME[(int)(fm313.v.pitch_target + 0.5f) % 12]);
        kb_heads(l, 0, K_MID);
        for (int i = 0; i < 16; i++) {
            int dn = fm313.key_down[WHITE[i]];
            kb_white(i, dn ? K_GREEN : K_BG, dn ? K_GREEN : K_FRAME);
        }
        for (int j = 0; j < 11; j++) { int dn = fm313.key_down[BLACK[j]]; kb_black(j, dn ? K_GREEN : K_BG, dn ? K_GREEN : K_GRID, 0, 0); }
    }
    panel_end();
}

/* ---- step write */
static void draw_write_panel(void)                     /* step write: the pattern as a little piano roll */
{
    const fm313_pattern_t *p = &fm313.pat[fm313.cur];
    const fm313_step_t *cs = &p->s[fm313.cursor];
    char t[32] = "";
    if (cs->type == FM313_NOTE) {
        cat(t, NOTE_NAME[cs->note]);
        if (cs->oct) { cat(t, cs->oct > 0 ? " +" : " "); cat_int(t, cs->oct); }
        if (cs->flags & FM313_F_ACC) cat(t, "  ACC");
        if (cs->flags & FM313_F_SLIDE) cat(t, "  SLIDE");
    } else cat(t, cs->type == FM313_TIE ? "TIE" : "REST");
    panel_begin(t, "SEL+KEY: ACC", 0);
    const int page = fm313.cursor/16, x0 = 16, cw = 13, yt = 24, yb = 106;
    int lo = 99, hi = -99;                             /* the pitch range shown: the notes used, at least an octave */
    for (int i = 0; i < p->len; i++) if (p->s[i].type == FM313_NOTE) { int v = step_s(&p->s[i]); if (v < lo) lo = v; if (v > hi) hi = v; }
    if (lo > hi) { lo = 0; hi = 12; }
    if (hi - lo < 12) { int m = (lo + hi)/2; lo = m - 6; hi = lo + 12; }
    const int span = hi - lo + 1, ph = (yb - yt - 6)/span > 5 ? 5 : (yb - yt - 6)/span;
    const int ybase = yb - 4 - ((yb - yt - 6) - span*ph)/2;   /* the lowest pitch's row, the range centred */
    for (int v = lo; v <= hi; v++)                     /* the C rows, dotted */
        if (((v % 12) + 12) % 12 == 0) for (int x = x0; x < x0 + 16*cw; x += 3) hw_box_rect(x, ybase - (v - lo)*ph, 1, 1, K_GRID);
    int prev_y = -1, prev_on = 0;
    for (int c = 0; c < 16; c++) {
        int i = page*16 + c, x = x0 + c*cw;
        if (i >= p->len) { hw_box_rect(x + cw/2 - 1, yb - 2, 2, 2, K_GRID); prev_on = 0; continue; }
        const fm313_step_t *st = &p->s[i];
        int cur = i == fm313.cursor;
        if (cur) { hw_box_rect(x, yt, cw - 1, yb - yt + 1, K_LIT); gfx_frame(x, yt, cw - 1, yb - yt + 1, 1, K_GREEN); }
        if (i == fm313.play_step) gfx_tri_up(x + 2, yt + 1, cw - 5, 4, K_HOT, 1);
        if (st->type == FM313_NOTE) {
            int y = ybase - (step_s(st) - lo)*ph;
            uint16_t c0 = cur ? K_HOT : (st->flags & FM313_F_ACC) ? K_RED : K_GREEN;
            hw_box_rect(x + 1, y - 1, cw - 3, ph > 3 ? 4 : 3, c0);
            if (st->flags & FM313_F_ACC) hw_box_rect(x + 1, y - 4, cw - 3, 1, K_RED);
            if (prev_on && prev_y >= 0 && p->s[i - 1].flags & FM313_F_SLIDE)   /* a slide: a line from the last note */
                gfx_line_aa((float)(x - 2), (float)prev_y, (float)(x + 1), (float)y, 1.2f, K_HOT);
            prev_y = y; prev_on = 1;
        } else if (st->type == FM313_TIE && prev_on && prev_y >= 0) {   /* the note held on */
            hw_box_rect(x - 2, prev_y - 1, cw, 3, cur ? K_HOT : K_MID);
        } else { hw_box_rect(x + cw/2 - 1, yb - 3, 2, 2, cur ? K_HOT : K_DIM); prev_on = 0; prev_y = -1; }
        if (st->type == FM313_REST) prev_on = 0;
    }
    hint(14, 116, "FX REST  LFO TIE  ARP DICE  EDIT END", K_MID);
    panel_end();
}

/* ---- SETUP: a list, SELECT moves the cursor, ALGORITHM (or any knob) changes the value */
static void draw_setup(void)
{
    panel_begin("SETUP", "SELECT: ROW  ALGO: SET", 0);
    const int rows = 5, rh = 17, y0 = 22;
    int first = clampi(fm313.su_cur - rows/2, 0, SU_NUM - rows);
    for (int r = 0; r < rows; r++) {
        int i = first + r, cy = y0 + r*rh + rh/2, on = i == fm313.su_cur;
        char b[24];
        su_value(i, b);
        if (on) { hw_box_rect(10, y0 + r*rh, 220, rh - 1, K_LIT); gfx_tri_right(13, cy - 4, 5, 9, K_GREEN); }
        txt(22, cy, &FM313_FONT_S, SU_NAME[i], on ? K_HOT : K_MID);
        txt_r(224, cy, &FM313_FONT_S, b, on ? K_HOT : K_GREEN);
    }
    for (int x = 12; x < 228; x += 4) hw_box_rect(x, 108, 2, 1, K_GRID);
    if (fm313.su_cur == SU_UPDATE) {                    /* the version and the load */
        char b[40] = "V" FM313_VERSION "  A"; int tp, lm, un; hw_load(&tp, &lm, &un); (void)tp;
        cat_int(b, hw_cpu_pct()); cat(b, " L"); cat_int(b, lm); cat(b, " S"); cat_int(b, (int)(fm313.t_scope/1000u));
        cat(b, " U"); cat_int(b, un);
        hint(120 - hint_w(b)/2, 117, b, K_DIM);
    } else if (fm313.su_cur == SU_PHONES) {             /* and whether the jack sees the plug */
        const char *t = fm313.hp_in ? "LEFT SILENT? LEFT FIX  JACK: IN" : "LEFT SILENT? LEFT FIX  JACK: OUT";
        hint(120 - hint_w(t)/2, 117, t, K_MID);
    } else hint(120 - hint_w(SU_HELP[fm313.su_cur])/2, 117, SU_HELP[fm313.su_cur], K_MID);
    panel_end();
}

/* ---- the steps */
static int step_page(void)
{
    int k = fm313.mode == M_WRITE ? fm313.cursor : fm313.play_step;
    return k >= 16 ? 1 : 0;
}
static void draw_steps(int play)
{
    const fm313_pattern_t *p = &fm313.pat[fm313.cur];
    int base = step_page()*16;
    hw_box_begin(240, 38, K_BG);
    for (int i = 0; i < 16; i++) {
        int k = base + i;
        float cx = 8.f + (float)i*14.9f;
        if (k >= p->len) { gfx_disc(cx, 12.f, 1.f, K_GRID); continue; }
        const fm313_step_t *s = &p->s[k];
        int now = k == play || (fm313.mode == M_WRITE && k == fm313.cursor);
        uint16_t c = now ? K_HOT : s->type == FM313_REST ? K_GRID : s->type == FM313_TIE ? K_MID : (s->flags & FM313_F_ACC) ? K_RED : K_GREEN;
        float r = s->type == FM313_REST ? 1.6f : 4.2f;
        if (s->type != FM313_REST) gfx_disc(cx, 12.f, r + 2.5f, (s->flags & FM313_F_ACC) && !now ? K_REDG : K_GLOW);
        gfx_disc(cx, 12.f, r, c);
        if ((s->flags & FM313_F_SLIDE) && s->type != FM313_REST) gfx_ring(cx, 12.f, 7.6f, 6.4f, K_GREEN);
        const char *nm = s->type == FM313_NOTE ? NOTE_NAME[s->note] : s->type == FM313_TIE ? "-" : "";   /* (no ~ in the font) */
        if (nm[0]) hint((int)(cx + 0.5f) - hint_w(nm)/2, 29, nm, now ? K_HOT : K_MID);
    }
    if (p->len > 16) txt_r(238, 4, &FM313_FONT_XS, step_page() ? "2" : "1", K_DIM);
    hw_box_blit(0, 152);
}

/* ---- the soft keys: what the four knobs and ALGORITHM do now */
typedef struct { const char *lab; char txt[8]; float lvl; int red; const void *id; } sk_t;
static int softkeys(sk_t *k)
{
    fm313_sound_t *s = &fm313.s;
    int v = view(), ctx = fm313.mode == M_WRITE ? 4 : v == L_SEQ ? 2 : v == L_FX ? 1 : v == L_ARP ? 3 : 0;
    for (int i = 0; i < 5; i++) { k[i].txt[0] = 0; k[i].lvl = -1.f; k[i].red = 0; k[i].id = 0; }
    if (ctx == 1) {
        static const char *const L[5] = { "DRIVE", "TONE", "DELAY", "REVRB", "TIME" };
        float v[4] = { s->drive, s->tone, s->dmix, s->rmix };
        const void *id[5] = { &s->drive, &s->tone, &s->dmix, &s->rmix, &s->ddiv };
        for (int i = 0; i < 5; i++) { k[i].lab = L[i]; k[i].id = id[i]; }
        for (int i = 0; i < 4; i++) { k[i].lvl = v[i]; cat_int(k[i].txt, (int)(v[i]*100.f + 0.5f)); }
        cat(k[4].txt, DIV_NAME[s->ddiv]);
        k[0].red = k[1].red = 1;
    } else if (ctx == 4) {                             /* step write: the step under the cursor */
        const fm313_step_t *st = &fm313.pat[fm313.cur].s[fm313.cursor];
        static const char *const L[5] = { "STEP", "NOTE", "ACC", "SLIDE", "OCT" };
        for (int i = 0; i < 5; i++) k[i].lab = L[i];
        cat_int(k[0].txt, fm313.cursor + 1);
        cat(k[1].txt, st->type == FM313_NOTE ? NOTE_NAME[st->note] : st->type == FM313_TIE ? "TIE" : "REST");
        cat(k[2].txt, (st->flags & FM313_F_ACC) ? "ON" : "-"); k[2].red = 1;
        cat(k[3].txt, (st->flags & FM313_F_SLIDE) ? "ON" : "-");
        if (st->type == FM313_NOTE) { if (st->oct > 0) cat(k[4].txt, "+"); cat_int(k[4].txt, st->oct); } else cat(k[4].txt, "-");
    } else if (ctx == 2) {
        fm313_pattern_t *p = &fm313.pat[fm313.cur];
        static const char *const L[5] = { "TUNE", "SHIFT", "LEN", "CHAIN", "SWING" };
        for (int i = 0; i < 5; i++) k[i].lab = L[i];
        cat_int(k[0].txt, p->tune10*10); k[0].id = &p->tune10;
        cat(k[1].txt, "-/+");
        cat_int(k[2].txt, p->len); k[2].lvl = (float)p->len/32.f; k[2].id = &p->len;
        cat_int(k[3].txt, fm313.chain_n); k[3].id = &fm313.chain_n;
        cat_int(k[4].txt, (int)(s->shuffle*100.f + 0.5f)); k[4].lvl = s->shuffle; k[4].id = &s->shuffle;
    } else {
        static const char *const L[5] = { "CUT", "RES", "ENV", "DEC", "ACC" };
        float *v[5] = { &s->cutoff, &s->reso, &s->envmod, &s->decay, &s->accent };
        for (int i = 0; i < 5; i++) { k[i].lab = L[i]; k[i].id = v[i]; k[i].lvl = *v[i]; cat_int(k[i].txt, (int)(*v[i]*100.f + 0.5f)); }
        k[4].red = 1;
        if (ctx == 3) {                                /* the SCATTER menu: ALGORITHM is the depth */
            k[4].lab = "DEPTH"; k[4].txt[0] = 0; cat_int(k[4].txt, s->sc_depth);
            k[4].lvl = (float)s->sc_depth*(1.f/16.f); k[4].id = &s->sc_depth;
        }
    }
    return ctx;
}
static int key_of(const void *id)
{
    sk_t k[5];
    softkeys(k);
    for (int i = 0; i < 5; i++) if (k[i].id && k[i].id == id) return i;
    return -1;
}
static void draw_key(int i)                           /* one soft key, 44 x 46 */
{
    sk_t k[5];
    softkeys(k);
    hw_box_begin(44, 46, K_BG);                       /* no highlight: the number moves, the panel above shows it big */
    gfx_frame(0, 2, 44, 44, 1, K_FRAME);
    txt(5, 10, &FM313_FONT_XS, k[i].lab, K_MID);
    uint16_t c = k[i].red ? K_RED : K_GREEN;
    const fm313_font_t *f = tw(&FM313_FONT_M, k[i].txt) <= 36 ? &FM313_FONT_M : &FM313_FONT_S;
    txt(5, 28, f, k[i].txt, c);
    if (k[i].lvl >= 0.f) {
        int lit = (int)(clampf(k[i].lvl, 0.f, 1.f)*34.f + 0.5f);
        hw_box_rect(5, 41, 34, 1, K_GRID);
        hw_box_rect(5, 41, lit, 1, c);
    }
    hw_box_blit(6 + i*46, 190);
}
static void draw_keys(void)
{
    hw_box_begin(240, 50, K_BG);
    hw_box_blit(0, 190);
    for (int i = 0; i < 5; i++) draw_key(i);
}

/* the boot screen: the name as a dot matrix (5 x 7 cells a letter, lit cells over the unlit ones), a pattern row
 * under it (accents red, as on the home screen), the filter's ring on top */
static void draw_boot(void)
{
    static const uint8_t GLYPH[6][7] = {
        { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },   /* F */
        { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },   /* M */
        { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 },   /* - */
        { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E },   /* 3 */
        { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },   /* 1 */
        { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E },   /* 3 */
    };
    static const uint8_t STEP[16] = { 1, 1, 2, 1, 0, 1, 2, 1, 1, 0, 1, 2, 1, 1, 2, 1 };   /* 0 rest, 1 note, 2 accent */
    const int cw = 6, ch = 8, lx = 15, ly = 70;
    for (int half = 0; half < 2; half++) {
        int oy = half*120;
        hw_box_begin(240, 120, K_BG);
        frame_grid(6, 6 - oy, 228, 228);
        int prev = 38;
        for (int x = 0; x < 210; x++) {                 /* a decaying resonant ring: the filter's voice */
            float t = (float)x/210.f;
            float env = fm313_exp2(-t*3.5f);
            float ph = t*9.f;
            float w = 2.f*(ph - (float)(int)ph) - 1.f;
            int y = 38 - (int)(env*w*16.f);
            int a = y < prev ? y : prev, b = y < prev ? prev : y;
            hw_box_rect(lx + x - 1, a - 1 - oy, 3, b - a + 3, K_GLOW);
            hw_box_rect(lx + x, a - oy, 1, b - a + 1, K_GREEN);
            prev = y;
        }
        for (int g = 0; g < 6; g++)
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++) {
                    int x = lx + (g*6 + c)*cw, y = ly + r*ch - oy;
                    if (GLYPH[g][r] & (0x10 >> c)) {
                        hw_box_rect(x - 1, y - 1, cw + 1, ch + 1, g == 2 ? K_REDG : K_GLOW);
                        hw_box_rect(x, y, cw - 1, ch - 1, g == 2 ? K_RED : g < 2 ? K_GREEN : K_HOT);
                    } else hw_box_rect(x + 2, y + 3, 1, 1, K_GRID);
                }
        for (int i = 0; i < 16; i++) {
            int x = lx + i*13 + (i/4)*2, y = 142 - oy;
            if (STEP[i]) hw_box_rect(x, y, 10, 10, STEP[i] == 2 ? K_RED : K_MID);
            else gfx_frame(x, y, 10, 10, 1, K_GRID);
        }
        txt_c(120, 178 - oy, &FM313_FONT_S, "ACID BASS LINE", K_GREEN);
        txt_c(120, 208 - oy, &FM313_FONT_XS, "V" FM313_VERSION "  FOR THE M-VAVE FM-1", K_DIM);
        hw_box_blit(0, oy);
    }
}

static void update_leds(void)
{
    hw_led_btn(B_PLAY, fm313.playing);
    hw_led_btn(B_REC, fm313.mode == M_WRITE || (fm313.rt_rec && ((hw_ms() >> 8) & 1u)));
    hw_led_btn(B_GLO, fm313.mode == M_GLOBAL);
    hw_led_btn(B_SEL, fm313.shift);
    {                                                  /* a lit layer button: its menu is up (ARP: or SCATTER runs) */
        int v = view();
        hw_led_btn(B_FX, v == L_FX);
        hw_led_btn(B_ARP, v == L_ARP || fm313.sc_latched);
        hw_led_btn(B_ENV, v == L_ENV);
        hw_led_btn(B_LFO, v == L_LFO);
        hw_led_btn(B_SEQ, v == L_SEQ || fm313.chain_n > 1);
        hw_led_btn(B_EDIT, v == L_EDIT);
    }
    hw_led_btn(B_OCTUP, fm313.kb_oct > 1);               /* which way the keys are shifted */
    hw_led_btn(B_OCTDN, fm313.kb_oct < 1);
    /* the keys: in a layer, its choice; else the step as on the TB-3's 16 lights (the white keys) */
    static uint32_t lit;
    uint32_t want = 0;
    if (view() == L_SEQ) {
        if (fm313.cur/16 == fm313.bank) want |= 1u << WHITE[fm313.cur % 16];
        if (fm313.bank < 4) want |= 1u << BLACK[fm313.bank];
    } else if (view() == L_ARP) {
        if (fm313.sc_latched) want |= 1u << BLACK[fm313.s.sc_type - 1];   /* the running kind, the depth's white key */
        want |= 1u << WHITE[clampi(fm313.s.sc_depth - 1, 0, 15)];
    } else if (view() == L_EDIT) {
        for (int i = 0; i < ED_NUM; i++) want |= 1u << WHITE[i];
    } else {
        int step = fm313.mode == M_WRITE ? fm313.cursor : fm313.play_step;
        if (step >= 0) want |= 1u << WHITE[step % 16];
    }
    uint32_t ch = want ^ lit;
    for (int i = 0; i < FM313_NKEYS; i++) if (ch >> i & 1u) hw_led_key(i, (int)(want >> i & 1u));
    lit = want;
}

/* ---------------------------------------------------------------- the main loop's part */
void fm313_app_poll(void)
{
    uint32_t now = hw_ms();
    hw_box_pump();
    {
        static int leds_set = -1;                      /* SETUP > LEDS, applied (at boot and when changed) */
        if (fm313.su.leds != leds_set) { leds_set = fm313.su.leds; hw_leds(leds_set); }
    }
    {                                                   /* SETUP left with a choice changed: kept (setup_save) */
        static int was_setup;
        static fm313_setup_t at_entry;
        int in = fm313.mode == M_GLOBAL;
        if (in && !was_setup) at_entry = fm313.su;
        if (!in && was_setup) {
            const uint8_t *a = (const uint8_t *)&at_entry, *b = (const uint8_t *)&fm313.su;
            int diff = 0;
            for (uint32_t k = 0; k < sizeof(fm313_setup_t); k++) diff |= a[k] != b[k];
            if (diff && setup_save() == 0) message("SETUP KEPT", 0, 0);
        }
        was_setup = in;
    }
    {
        int hp = hw_phones();
        if (hp != fm313.hp_in) { fm313.hp_in = hp; if (fm313.mode == M_GLOBAL) fm313.panel_dirty = 1; }
    }
    if (fm313.tone_pending) {                           /* a pattern with its own sound has started */
        load_preset(fm313.tone_pending - 1); apply_sound();
        fm313.tone_pending = 0; fm313.dirty_knobs = fm313.dirty_top = 1;
    }
    if (fm313.rt_rec && fm313.playing && fm313.mode == M_PLAY) {   /* realtime record: ties while held, EDIT erases */
        fm313_pattern_t *p = &fm313.pat[fm313.cur];
        int pos = fm313.q.pos, len = fm313_seq_len(p);
        if (pos >= 0 && pos < len && pos != fm313.rec_step_seen) {
            fm313.rec_step_seen = pos;
            if (fm313.layer == L_EDIT && !(p->pflags & FM313_P_LOCK)) {
                p->s[pos].type = FM313_REST; p->s[pos].flags = 0; fm313.dirty_grid = 1; fm313.lay_used = 1;
            } else if (fm313.rec_key >= 0 && fm313.key_down[fm313.rec_key] && pos == (fm313.rec_last + 1) % len) {
                p->s[pos].type = FM313_TIE; p->s[pos].flags = 0; fm313.rec_last = pos; fm313.dirty_grid = 1;
            }
        }
    }
    if (fm313.hear_ms && now - fm313.hear_ms > 260u) { fm313.hear_ms = 0; if (!fm313.playing && fm313.wr_key < 0) post(EV_NOTE_OFF, 0, 0, 0); }
    if (fm313.save_ms && !fm313.save_held && now - fm313.save_ms > 1000u) {   /* SAVE held: undo */
        fm313.save_held = 1;
        fm313_pattern_t t = fm313.pat[fm313.cur]; fm313.pat[fm313.cur] = fm313.undo; fm313.undo = t;
        message("UNDO", 0, 0); fm313.dirty_grid = 1;
    }
    if (fm313.msg[0] && now - fm313.msg_ms > (fm313.msg_hold > 2500u ? fm313.msg_hold : 2500u)) { fm313.msg[0] = 0; fm313.dirty_msg = 1; }
    if (fm313.touch_ms && now - fm313.touch_ms > 3500u) {
        fm313.touch_ms = 0;
        int k = fm313.touched ? key_of(fm313.touched) : -1;
        if (k >= 0) fm313.key_bits |= 1u << k; else fm313.dirty_knobs = 1;
        fm313.panel_dirty = 1;
    }
    static uint32_t su_ms;
    if (fm313.mode == M_GLOBAL && now - su_ms > 500u) { su_ms = now; fm313.panel_dirty = 1; }
    if (fm313.mode == M_GLOBAL && fm313.upd_ms && now - fm313.upd_ms >= 3000u) { fm313.upd_ms = 0; fm313.panel_dirty = 1; }
    update_leds();
    if ((int32_t)(now - fm313.boot_until) < 0) return;
    if (fm313.boot_until) { fm313.boot_until = 0; fm313.dirty_top = fm313.dirty_grid = fm313.dirty_knobs = 1; fm313.panel_dirty = 1; }
    int ps = fm313.play_step;
    if (ps != fm313.last_step_drawn) { fm313.dirty_grid = 1; fm313.last_step_drawn = ps; }
    static int was_playing = -1, was_mode = -1, was_cur = -1;
    if (fm313.playing != was_playing) { was_playing = fm313.playing; fm313.dirty_top = 1; }
    if (fm313.cur != was_cur) { was_cur = fm313.cur; fm313.dirty_top = 1; if (view() == L_SEQ) fm313.panel_dirty = 1; }
    if (fm313.mode != was_mode) { was_mode = fm313.mode; fm313.dirty_top = fm313.dirty_grid = fm313.dirty_knobs = 1; fm313.panel_dirty = 1; fm313.scope_live = 0; }
    /* what fills the panel: SETUP, step write, a held layer's map (FX latched: its map), a fresh value, the scope */
    int msg_view = fm313.mode == M_PLAY && view() == L_NONE && fm313.msg[0] && now - fm313.msg_ms < fm313.msg_hold;
    int map = view() != L_NONE;
    int panel = fm313.mode != M_PLAY || map || msg_view;
    if (!msg_view) mp_live = 0;
    static uint32_t scope_ms, prof_ms, sc_max, fi_max;
    uint32_t t0 = hw_us(); int drew_scope = 0, drew = 1;
    if (fm313.key_bits) {                               /* a turned knob: its key, first */
        for (int i = 0; i < 5; i++) if (fm313.key_bits & (1u << i)) { fm313.key_bits &= ~(1u << i); draw_key(i); break; }
    } else if (fm313.dirty_knobs) {
        fm313.dirty_knobs = 0;
        int k = fm313.touched && fm313.touch_ms ? key_of(fm313.touched) : -1;
        static int ctx_drawn = -1;
        sk_t sk[5]; int ctx = softkeys(sk);
        if (k >= 0 && ctx == ctx_drawn) draw_key(k);   /* one key turned: that key */
        else { draw_keys(); ctx_drawn = ctx; }
        if (panel) fm313.panel_dirty = 1;
    } else if (fm313.dirty_msg) {
        fm313.dirty_msg = 0;
        fm313.panel_dirty = 1;
    } else if (fm313.dirty_top) { fm313.dirty_top = 0; draw_head(); }
    else if (fm313.dirty_grid) { fm313.dirty_grid = 0; draw_steps(ps); if (fm313.mode == M_WRITE) fm313.panel_dirty = 1; }
    else if (panel) {
        if (fm313.panel_dirty) {
            fm313.panel_dirty = 0;
            if (fm313.mode == M_GLOBAL) draw_setup();
            else if (fm313.mode == M_WRITE) draw_write_panel();
            else if (view() == L_SEQ) draw_seq_panel();
            else if (view() == L_EDIT) draw_edit_panel();
            else if (view() == L_ARP) draw_arp_panel();
            else if (view() == L_ENV) draw_touch_panel(1);
            else if (view() == L_LFO) draw_touch_panel(0);
            else if (view() == L_FX) draw_fx_panel();
            else draw_msg_panel();
        } else drew = 0;
    } else if (fm313.su.scope_mode == 2) {
        if (!fm313.scope_live) { panel_begin(0, 0, 1); hw_box_blit(0, 24); fm313.scope_live = 1; tip_drawn = 0; tip_update(now); }
        else if (!tip_update(now)) drew = 0;
    } else if (now - scope_ms >= 80u) {              /* 0.7.4: 12 frames a second (25 before) */
        scope_ms = now; drew_scope = 1;
        if (fm313.su.scope_mode == 1) draw_levels(); else draw_scope();
    }
    else drew = 0;
    if (drew) {
        uint32_t dt = hw_us() - t0;
        if (drew_scope) { if (dt > sc_max) sc_max = dt; } else if (dt > fi_max) fi_max = dt;
    }
    if (now - prof_ms >= 1000u) { prof_ms = now; fm313.t_scope = sc_max; fm313.t_field = fi_max; sc_max = fi_max = 0; }
    hw_box_pump();
}

#ifdef FM313_SIM
#include <string.h>
#include <stdio.h>
void sim_reset(void) { memset(&fm313, 0, sizeof fm313); }
int sim_field(const char *n)
{
    {                                                  /* sNtype, sNnote, sNflags: step N of the current pattern */
        int i; char w[8];
        if (sscanf(n, "s%d%7s", &i, w) == 2 && i >= 1 && i <= FM313_STEPS) {
            const fm313_step_t *st = &fm313.pat[fm313.cur].s[i - 1];
            if (!strcmp(w, "type")) return st->type;
            if (!strcmp(w, "note")) return st->note + 12*st->oct;
            if (!strcmp(w, "flags")) return st->flags;
        }
    }
    if (!strcmp(n, "cur")) return fm313.cur + 1;
    if (!strcmp(n, "playing")) return fm313.q.playing;
    if (!strcmp(n, "mode")) return fm313.mode;
    if (!strcmp(n, "cursor")) return fm313.cursor + 1;
    if (!strcmp(n, "len")) return fm313.pat[fm313.cur].len;
    if (!strcmp(n, "bpm")) return (int)(fm313.s.bpm + 0.5f);
    if (!strcmp(n, "wave")) return fm313.s.wave;
    if (!strcmp(n, "cut")) return (int)(fm313.s.cutoff*100.f + 0.5f);
    if (!strcmp(n, "s1type")) return fm313.pat[fm313.cur].s[0].type;
    if (!strcmp(n, "s1note")) return fm313.pat[fm313.cur].s[0].note + 12*fm313.pat[fm313.cur].s[0].oct;
    if (!strcmp(n, "s1flags")) return fm313.pat[fm313.cur].s[0].flags;
    if (!strcmp(n, "s2type")) return fm313.pat[fm313.cur].s[1].type;
    if (!strcmp(n, "s3type")) return fm313.pat[fm313.cur].s[2].type;
    if (!strcmp(n, "s2note")) return fm313.pat[fm313.cur].s[1].note + 12*fm313.pat[fm313.cur].s[1].oct;
    if (!strcmp(n, "transpose")) return fm313.q.transpose;
    if (!strcmp(n, "kboct")) return fm313.kb_oct;
    if (!strcmp(n, "menu")) return fm313.menu;
    if (!strcmp(n, "depth")) return fm313.s.sc_depth;
    if (!strcmp(n, "pitch")) return (int)(fm313.v.pitch_target + 0.5f);
    if (!strcmp(n, "gate")) return fm313.v.gate;
    if (!strcmp(n, "phones")) return fm313.su.phones;
    if (!strcmp(n, "tipsoff")) return fm313.su.tips_off;
    if (!strcmp(n, "step")) return fm313.q.step + 1;
    if (!strcmp(n, "dtype")) return fm313.s.dtype;
    if (!strcmp(n, "preset")) return fm313.s.preset;
    if (!strcmp(n, "acc")) return (int)(fm313.s.accent*100.f + 0.5f);
    if (!strcmp(n, "layer")) return fm313.layer;
    if (!strcmp(n, "drive")) return (int)(fm313.s.drive*100.f + 0.5f);
    if (!strcmp(n, "ftone")) return (int)(fm313.s.tone*100.f + 0.5f);
    if (!strcmp(n, "fxl")) return fm313.menu == L_FX;
    if (!strcmp(n, "s2flags")) return fm313.pat[fm313.cur].s[1].flags;
    if (!strcmp(n, "venv")) return (int)(fm313.v.p.envmod*100.f + 0.5f);
    if (!strcmp(n, "vtouch")) return (int)(fm313.v.p.touch_oct*100.f + 0.5f);
    if (!strcmp(n, "midich")) return fm313.s.midi_ch;
    if (!strcmp(n, "keys")) return fm313.su.touch;
    if (!strcmp(n, "tune10")) return fm313.pat[fm313.cur].tune10;
    if (!strcmp(n, "pflags")) return fm313.pat[fm313.cur].pflags;
    if (!strcmp(n, "tone")) return fm313.pat[fm313.cur].tone;
    if (!strcmp(n, "chain")) return fm313.q.chain_n;
    if (!strcmp(n, "scatter")) return fm313.q.sc_type;
    if (!strcmp(n, "rtrec")) return fm313.rt_rec;
    if (!strcmp(n, "non")) return fm313.q.n_on > 0;
    if (!strcmp(n, "notes")) { int c = 0; for (int i = 0; i < fm313.pat[fm313.cur].len; i++) c += fm313.pat[fm313.cur].s[i].type == FM313_NOTE; return c; }
    return -9999;
}
#endif
