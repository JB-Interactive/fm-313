/* SPDX-License-Identifier: GPL-3.0-only
 * FM-313 step sequencer: up to 32 steps, note/rest/tie, accent, slide,
 * shuffle, per-pattern length, triplet grid and tune, transpose, a chain of
 * adjacent patterns, SCATTER (eight ways of re-reading the pattern live, ten
 * depths), internal or MIDI clock.
 * Timing follows the classic bass line convention: a gate is high for the
 * first half of a step; a slide step keeps it high into the next step.
 * No doubles anywhere (the FM-1's FPU is single precision): time is a
 * countdown to the next event.
 */
#ifndef FM313_SEQ_H
#define FM313_SEQ_H
#include <stdint.h>
#include "fm313_dsp.h"

#define FM313_STEPS 32
enum { FM313_NOTE = 0, FM313_REST = 1, FM313_TIE = 2 };
enum { FM313_F_ACC = 1, FM313_F_SLIDE = 2 };
enum { FM313_P_TRIP = 1, FM313_P_LOCK = 2 };          /* pattern flags: 8th-triplet grid, locked against edits */
enum { SC_OFF, SC_REVERSE, SC_REPEAT, SC_HALF, SC_RATCHET, SC_GATE, SC_SKIP, SC_RANDOM, SC_STUTTER, SC_NUM };

typedef struct {
    uint8_t note;     /* 0..12 within the octave (C..C') */
    int8_t  oct;      /* -1, 0, +1 */
    uint8_t type;     /* FM313_NOTE / REST / TIE */
    uint8_t flags;    /* FM313_F_ACC | FM313_F_SLIDE */
} fm313_step_t;

typedef struct {
    fm313_step_t s[FM313_STEPS];
    uint8_t len;      /* 1..32 */
    uint8_t pflags;   /* FM313_P_TRIP | FM313_P_LOCK */
    int8_t  tune10;   /* pattern tune in 10-cent units, -70..70 */
    uint8_t tone;     /* the sound that comes with the pattern: 0 none, 1.. preset */
} fm313_pattern_t;

typedef struct {
    fm313_pattern_t *pat;
    fm313_pattern_t *next_pat;     /* queued: switches at the end of the pattern */
    fm313_pattern_t *chain_base;   /* a chain: chain_n patterns from chain_base, one after the other */
    int   chain_n, chain_i;
    float bpm, shuffle;          /* shuffle 0..1 */
    int   root;                  /* MIDI note of step note 0, oct 0 */
    int   transpose;             /* semitones, from the keys while playing */
    int   playing;
    int   pos;                   /* the logical step (what plays without SCATTER) */
    int   step;                  /* the step that sounds now (the screen) */
    int   sub_left;              /* ratchet hits still due in this step */
    float sub_len;
    float to_step;               /* samples until the next event */
    float to_gate_off;           /* samples until the gate falls, < 0: none */
    float cur_len;               /* length of the current step */
    int   prev_slide;
    int   ext_clock;             /* 1: steps come from MIDI clock */
    int   ext_ticks;
    float ext_tick_len;          /* measured samples per MIDI clock */
    float since_tick;
    int   sc_type, sc_depth;     /* SCATTER: type (SC_OFF = none), depth 1..16 (a white key each) */
    int   sc_anchor;             /* REPEAT: where the loop started */
    uint32_t rng;
    /* for MIDI out (the app reads them in audio context): a count per note on / gate off */
    uint32_t n_on, n_off;
    int   on_note, on_acc, on_slide;
} fm313_seq_t;

void fm313_seq_init(fm313_seq_t *q, fm313_pattern_t *p, float bpm);
void fm313_seq_start(fm313_seq_t *q);
void fm313_seq_stop(fm313_seq_t *q, fm313_voice_t *v);
/* advance by up to n output samples, firing due events into the voice;
 * returns how many samples to render before the next event (1..n) */
int  fm313_seq_advance(fm313_seq_t *q, fm313_voice_t *v, int n);
/* MIDI clock (24 ppqn) when ext_clock is set */
void fm313_seq_midi_clock(fm313_seq_t *q, fm313_voice_t *v);
void fm313_seq_scatter(fm313_seq_t *q, int type, int depth);
int  fm313_seq_len(const fm313_pattern_t *p);
/* where in the current step we are, 0..1 (realtime record rounds to the nearer step) */
float fm313_seq_phase(const fm313_seq_t *q);

void fm313_pattern_clear(fm313_pattern_t *p);
void fm313_pattern_random(fm313_pattern_t *p, uint32_t *seed, int len);
/* a few steps changed: notes moved within the scale, accents and slides flipped, a rest here and there */
void fm313_pattern_mutate(fm313_pattern_t *p, uint32_t *seed);
/* every note moved by semitones (octaves folded into -1..+1); returns how many notes had to be folded */
int  fm313_pattern_shift(fm313_pattern_t *p, int semis);
/* 2-byte packed step for storage */
uint16_t fm313_step_pack(const fm313_step_t *s);
void fm313_step_unpack(fm313_step_t *s, uint16_t v);
#endif
