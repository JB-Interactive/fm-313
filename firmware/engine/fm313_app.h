/* SPDX-License-Identifier: GPL-3.0-only
 * FM-313 app: everything between the FM-1's controls and the engine.
 * Hardware-independent; the firmware glue (fw/firmware/src/fm313.c) and the
 * host simulator (host/sim.c) supply fm313_hw.h.
 */
#ifndef FM313_APP_H
#define FM313_APP_H
#include <stdint.h>
#include "fm313_dsp.h"
#include "fm313_seq.h"
#include "fm313_fx.h"

#define FM313_NPAT 64
#define FM313_NKEYS 27            /* F3 .. G5 */

/* panel labels (the glue maps the matrix to these) */
enum { B_FX, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC,
       B_OCTDN, B_OCTUP, B_NUM };
enum { E_SELECT, E_ALGO, E_PRESETS, E_K1, E_K2, E_K3, E_K4, E_NUM };

/* hardware services, implemented by the glue */
uint32_t hw_ms(void);
void hw_box_begin(int w, int h, uint16_t bg);
void hw_box_rect(int x, int y, int w, int h, uint16_t c);
void hw_box_blit(int x, int y);
int  hw_box_pump(void);
/* straight into the screen (the scope): a column span y0..y1 in screen pixels, then the rows to send */
void hw_fb_vspan(int x, int y0, int y1, uint16_t c);
void hw_fb_dirty(int x, int y, int w, int h);
int  hw_fb_idle(void);           /* 1: no transfer pending, the screen buffer may change without tearing */                     /* send the next strip of a big field; 0 when none is left */
void hw_box_scanlines(void);    /* darken every other row of the canvas: the CRT */
void hw_led_btn(int b, int on);
void hw_led_key(int k, int on);
int  hw_cpu_pct(void);           /* audio render load, % of real time */
uint32_t hw_us(void);            /* microseconds (wraps) */
void hw_midi_out(uint8_t st, uint8_t d1, uint8_t d2);   /* audio context: one MIDI message out over USB */
void hw_enter_update(void);
void hw_leds(int mode);         /* 0 NORMAL, 1 NIGHT, 2 NIGHT+: the unlit LEDs glow (faint, brighter) */
int  hw_phones(void);            /* 1: headphones plugged (debounced) */
int  hw_touch(int on);           /* the keys' touch sampling on / off (-1: ask); returns the state */      /* the USB update mode, as OCT- at power-on gives it; does not return */
void hw_load(int *timer_pct, int *loop_ms, int *late);   /* key-scan ISR %, longest main-loop pass, audio underruns */
int  hw_flash_read(uint32_t off, void *dst, uint32_t n);
int  hw_flash_erase4k(uint32_t off);
int  hw_flash_write(uint32_t off, const void *src, uint32_t n);

#define FM313_RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* main loop side */
void fm313_app_init(void);
void fm313_app_button(int b, int down);
void fm313_app_encoder(int e, int steps);
void fm313_app_key(int k, int down, int vel);   /* vel 1..127 (the keys' touch; 100 without) */
void fm313_app_pressure(int k, int p);          /* a held key's pressure, 0..127 */
int  fm313_app_touch_pref(void);                /* SETUP > KEYS: 1 the touch on (the glue starts the scan's sampling) */
void fm313_app_midi(uint8_t st, uint8_t d1, uint8_t d2);   /* channel + realtime */
void fm313_app_poll(void);
void fm313_app_notice(const char *what, int value);   /* a line on the screen (the glue: MASTER) */        /* timers, screen */
/* audio ISR side: n frames of stereo Q-float into out_lr */
void fm313_app_audio(float *out_lr, int n);

/* for the simulator and tests */
typedef struct fm313_app_s fm313_app_t;
extern fm313_app_t fm313;
int fm313_app_store_save(void);
int fm313_app_store_load(void);
#endif
