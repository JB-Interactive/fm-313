/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-313: a 303-style bass line synthesizer and step sequencer for the M-VAVE FM-1.
 *
 * This file is the glue: the platform (platform.c: boot stub, update loader, USB, keys, screen; from TransitFM1,
 * Dinghy and Felucca) below, the engine (firmware/engine: voice, sequencer, effects, app) above. The engine is
 * hardware-free and runs the same code in the host simulator (host/sim.c).
 *
 *   audio   ALNK0 I2S, 44.1 kHz, half buffers of HALF_FRAMES; the ISR drains MIDI (USB) first, then renders
 *   input   TIMER5 scans the matrix (platform.c); the main loop turns edges / steps into app events,
 *           through the panel table (Felucca's default layout)
 *   flash   fm313_app.c keeps two 8 KiB banks at 0xD0000; the RAM flash driver runs with IRQs off, so the
 *           audio buffer is silenced first (no looping half while the ISR cannot run)
 */
#define APP_TAKES_MIDI 1
/* the installer's handshake (cmd 0x11, an installer's alone: no DAW sends it) goes to the main loop here, which
 * answers it and watches the answer leave (hs_poll below) */
#define APP_HANDSHAKE_SWITCH 1
static void app_ota_idle(void);
#define APP_OTA_IDLE app_ota_idle
#define LCD_BAUD 2u          /* SPI1 20 MHz (TransitFM1: 12). A full scope frame took 41 ms on the wire; the ST7789V takes 60+ */
#include "platform.c"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "fm1_jack.h"

#include "fm313_dsp.c"
#include "fm313_seq.c"
#include "fm313_fx.c"
#include "fm313_app.c"

#define HALF_FRAMES 128u                         /* 2.9 ms, as Felucca (core.h). 0.2.6-0.3.1 ran 256: the left channel went silent */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define OUT_SCALE 1509949.f                      /* 0.36 x 2^22 (-15 dBFS): 0.4.0 ran 2^22 (-6 dBFS, as Felucca), and the FM-1's
                                                 * output stage distorted from MASTER ~65 % up; the knob's top is now that level */

static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static float fbuf[2u * HALF_FRAMES];
static volatile uint32_t audio_late, audio_load_q8;
/* MASTER: the FM-1's volume pot is read by the ADC (PB6), not wired to the amp; square law, as Felucca */
static volatile float master_target = 0.5f;
static float master_now = 0.5f, master_knob = 700.f;
static int master_shown = -1;   /* render time / half time, x256, smoothed */

/* ---- panel: Felucca's default layout (labels -> matrix ids / encoder numbers) */
static const uint8_t BTN_ID[B_NUM] = { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1 };   /* B_FX .. B_OCTUP */
static const uint8_t ENC_ID[E_NUM] = { 0, 1, 6, 2, 3, 4, 5 };                         /* SELECT .. KNOB 4 */

/* ---- hardware services for the app */
uint32_t hw_ms(void) { return fm1_ms; }
uint32_t hw_us(void) { return fm1_ticks() / FM1_TICKS_PER_US; }
void hw_midi_out(uint8_t st, uint8_t d1, uint8_t d2)      /* audio ISR: the USB MIDI out ring (usb.c) */
{
    uint32_t cin = st >= 0xF8u ? 0xFu : (uint32_t)(st >> 4);
    midi_out_event(cin | (uint32_t)st << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24);
}
static void audio_silence(void);
/* ---- updates while FM-313 runs
 * As the firmwares of this family do it (Felucca, TransitFM1): the handshake is answered, and an installer's UPGRADE
 * command is served here as step 1 of the update (ota.c ota_session: the package checked, our loader staged, the
 * host's confirmation), then the FM-1 restarts into the loader, which writes. The sound stops for it.
 *
 * One fault is worked around. On the Windows 11 PC FM-313 was developed with, the host at times stopped collecting
 * what FM-313 sent (a packet stayed loaded for good: TxPktRdy never cleared, the queue behind it filled), and only
 * a restart helped. The platform's USB code is TransitFM1's, byte for byte; the cause is not found. So what FM-313
 * sends an installer is watched: an answer or a request the host has not taken after 200 ms, and FM-313 restarts
 * into its staged loader, with the update record in RAM only (ota.c ota_to_loader: nothing is written; a power
 * cycle plays again). The loader answers, and an installer goes on with it (at once, or at its next try). Not for a
 * mere handshake in the first minute after a warm boot: an installer's last look after an install must never send
 * the new firmware away again. SETUP > UPDATE restarts into the loader too.
 *
 * update_loop is the fallback when our loader is not staged (or the flash is unknown): the USB rescue's loop on the
 * running USB stack, the timer and the audio stopped at their sources (a flash read re-enables interrupts) */
static void update_loop(const char *what)
{
    audio_silence();
    fm1_irq_off();
    fm1_timer5_stop();                           /* TIMER5 stopped, masked, its pending flag cleared */
    fm1_audio_stop();
    fm1_irq_mask(FM1_IRQ_ALNK0);
    fm1_irq_on();                                /* (nothing left to come: as the USB rescue at power-on) */
    recovery_active = 1;
    recovery_last = recovery_usb_last = fm1_ticks();
    lcd_sync();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 72, 240, &FONT_S, what, C_WHITE, 1);
    draw_text_box(0, 104, 240, &FONT_S, "OPEN THE INSTALLER", C_WHITE, 1);
    draw_text_box(0, 136, 240, &FONT_S, "POWER OFF: BACK", C_WHITE, 1);
    lcd_sync();
    so_r = so_w;                                 /* a reply the app left unsent: dropped (the installer asks again) */
    sx_open = 0;
    for (;;) recovery_step();
}
static void to_loader(void)
{
    master_target = 0.f;                         /* the output glides to silence (the audio ISR, a few ms) */
    lcd_sync();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 96, 240, &FONT_S, APP_NAME " UPDATE MODE", C_WHITE, 1);
    draw_text_box(0, 128, 240, &FONT_S, "RESTARTING...", C_WHITE, 1);
    lcd_sync();
    fm1_delay_ms(200);
    if (flash_ok)
        ota_to_loader();                         /* returns only if our loader is not staged */
    update_loop(APP_NAME " UPDATE MODE");
}
/* warm: this boot follows a reset, not a power-on (an install, a cancelled update). The mark survives in RAM that no
 * boot clears (.noinit, above the loader's own RAM); the reset flags as platform.c reads them */
#define WARM_MAGIC 0x57333133u
uint32_t fm313_warm_mark __attribute__((section(".noinit")));
static uint8_t boot_warm;
static void warm_begin(void)
{
    uint32_t p3 = fm1_boot.p3_rst;
    boot_warm = fm313_warm_mark == WARM_MAGIC || (p3 & (4u | 0x40u)) || !(p3 & 1u);
    fm313_warm_mark = WARM_MAGIC;
}
static int hs_quiet(void) { return boot_warm && fm1_ms < 60000u; }
/* step 1, as TransitFM1's main loop serves it (platform.c platform_poll), with the sound off first: the flash
 * driver runs with interrupts off, and the DMA would loop a stale half. TIMER5 keeps polling USB. A session that
 * ends without a commit (refused, failed, the host gone) has said so on the screen: FM-313 then restarts */
static uint8_t in_session;
static void serve_update(void)
{
    master_target = 0.f;                         /* the output glides to silence (the audio ISR, a few ms) */
    fm1_delay_ms(30);
    fm1_irq_off();
    fm1_audio_stop();
    fm1_irq_mask(FM1_IRQ_ALNK0);
    fm1_irq_on();
    lcd_sync();
    usb.ota_req = 0;
    in_session = 1;
    if (flash_ok)
        ota_session();                           /* returns only if nothing was committed */
    in_session = 0;
    for (uint32_t t = fm1_ms; fm1_ms - t < 2500u;)
        fm1_wdt_feed();                          /* (the screen says why) */
    usb_detach();
    fm1_delay_ms(30);
    bootguard_clear(&bootguard);
    fm1_reboot();
}
static void app_ota_idle(void)                   /* ota.c waits (a request sent, its answer due) */
{
    if (in_session && tx_wait > 400u) {          /* the host does not take our request: the loader instead */
        in_session = 0;
        to_loader();
    }
}
static void hs_poll(void)                        /* the main loop */
{
    static uint32_t sent_ms;                     /* an answer is on its way since (0: none) */
    int stuck = tx_wait > 400u;                  /* loaded for 200 ms (2 kHz polls) and not taken */
    if (ota_hs_pending) {
        ota_hs_pending = 0;
        if (!stuck) {
            ota_reply_identity();
            sent_ms = fm1_ms | 1u;
        } else if (!hs_quiet())
            to_loader();
    }
    if (sent_ms && fm1_ms - sent_ms >= 400u) {
        sent_ms = 0;
        if ((so_w != so_r || tx_wait > 400u) && !hs_quiet())
            to_loader();
    }
    if (usb.ota_req) {
        if (stuck)
            to_loader();                         /* (the loader takes the installer's next try) */
        serve_update();
    }
}
void hw_enter_update(void) { to_loader(); }
int hw_cpu_pct(void) { return (int)(audio_load_q8 * 100u / 256u); }
/* SETUP's load line: the timer ISR's share and the main loop's longest pass in the last second */
static uint32_t loop_max_us, loop_max_shown, tim_pct_shown, load_win_t0, load_t5_0;
void hw_load(int *timer_pct, int *loop_ms, int *late)
{
    *late = (int)audio_late;
    *timer_pct = (int)tim_pct_shown;
    *loop_ms = (int)((loop_max_shown + 500u) / 1000u);
}
static void load_tick(uint32_t pass_us)
{
    uint32_t now = fm1_ticks();
    if (pass_us > loop_max_us) loop_max_us = pass_us;
    if (now - load_win_t0 >= 1000000u * FM1_TICKS_PER_US) {
        uint32_t busy = t5_busy - load_t5_0, span = now - load_win_t0;
        tim_pct_shown = busy * 100u / span;   /* (busy <= span = 24 M: fits 32 bit) */
        loop_max_shown = loop_max_us;
        loop_max_us = 0;
        load_win_t0 = now;
        load_t5_0 = t5_busy;
    }
}
/* The screen lives in RAM (fb, 240 x 240, as the panel takes it). A field is drawn into the canvas, copied into fb
 * and marked dirty; hw_box_pump() sends the dirty areas to the panel one DMA chunk at a time, small ones first, and
 * never waits for the SPI. 0.2.5-0.2.7 drew straight to the panel: a 240 x 128 scope held the bus for up to 41 ms
 * and a turned knob's value waited behind it. */
static uint16_t fb[240u * 240u] __attribute__((section(".pool")));
typedef struct { uint16_t x, y, w, h, r0; } dirty_t;
#define NDIRTY 8
static dirty_t dq[NDIRTY];
static uint32_t ndq;
#define CHUNK_ROWS 24u                          /* 11.5 KB: ~4.6 ms at 20 MHz */
static int lcd_idle(void)                       /* the last transfer is done (no waiting) */
{
    if (!lcd_busy) return 1;
    if (!fm1_lcd_done()) return 0;
    lcd_sync();
    return 1;
}
static void dirty_add(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t i;
    for (i = 0; i < ndq; i++) {
        if (dq[i].x == x && dq[i].y == y && dq[i].w == w && dq[i].h == h) { dq[i].r0 = 0; return; }   /* again, from the top */
        if (w == 240u && dq[i].w == 240u && y <= (uint32_t)dq[i].y + dq[i].h && (uint32_t)dq[i].y <= y + h) {
            uint32_t a = y < dq[i].y ? y : dq[i].y, e = y + h > (uint32_t)dq[i].y + dq[i].h ? y + h : (uint32_t)dq[i].y + dq[i].h;
            dq[i].y = (uint16_t)a; dq[i].h = (uint16_t)(e - a); dq[i].r0 = 0;   /* full rows that touch: one band */
            return;
        }
    }
    if (ndq == NDIRTY) { ndq = 1; dq[0].x = 0; dq[0].y = 0; dq[0].w = 240; dq[0].h = 240; dq[0].r0 = 0; return; }
    dq[ndq].x = (uint16_t)x; dq[ndq].y = (uint16_t)y; dq[ndq].w = (uint16_t)w; dq[ndq].h = (uint16_t)h; dq[ndq].r0 = 0;
    ndq++;
}
int hw_box_pump(void)
{
    uint32_t i, best = 0, n, r;
    if (!ndq || !lcd_idle()) return ndq != 0;
    for (i = 1; i < ndq; i++)                   /* the smallest area first: a key before the scope */
        if ((uint32_t)dq[i].w * dq[i].h < (uint32_t)dq[best].w * dq[best].h) best = i;
    dirty_t *d = &dq[best];
    if (d->w == 240u) {                          /* full rows: contiguous in fb, one DMA, the CPU goes on */
        n = d->h - d->r0 < CHUNK_ROWS ? d->h - d->r0 : CHUNK_ROWS;
        lcd_blit(0, d->y + d->r0, 240, n, fb + (uint32_t)(d->y + d->r0) * 240u);
        d->r0 = (uint16_t)(d->r0 + n);
    } else {                                     /* a narrow box: one window, a transfer per row */
        lcd_window(d->x, d->y, d->x + d->w - 1u, d->y + d->h - 1u);
        for (r = 0; r < d->h; r++)
            lcd_data(fb + (uint32_t)(d->y + r) * 240u + d->x, (uint32_t)d->w * 2u);
        d->r0 = d->h;
    }
    if (d->r0 >= d->h) *d = dq[--ndq];
    return 1;
}
void hw_fb_vspan(int x, int y0, int y1, uint16_t c)
{
    uint16_t sw = swap16(c);
    if ((uint32_t)x >= 240u) return;
    if (y0 < 0) y0 = 0;
    if (y1 > 239) y1 = 239;
    for (int y = y0; y <= y1; y++) fb[(uint32_t)y * 240u + (uint32_t)x] = sw;
}
int hw_fb_idle(void) { return !ndq && lcd_idle(); }   /* nothing on its way to the panel */
void hw_fb_dirty(int x, int y, int w, int h) { if (w > 0 && h > 0) dirty_add((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h); }
void hw_box_begin(int w, int h, uint16_t bg)
{
    uint32_t i, n;
    uint16_t c = swap16(bg);
    if ((uint32_t)(w * h) > CV_MAX) h = (int)(CV_MAX / (uint32_t)w);
    cv_px = cv_mem; cv_w = (uint32_t)w; cv_h = (uint32_t)h; cv_oy = 0;
    n = cv_w * cv_h;
    for (i = 0; i < n; i++) cv_px[i] = c;      /* no lcd_sync: the panel reads fb, never the canvas */
}
void hw_box_rect(int x, int y, int w, int h, uint16_t c) { cv_rect(x, y, w, h, c); }
uint16_t hw_box_px(int x, int y)
{
    if (x < 0 || y < 0 || (uint32_t)x >= cv_w || (uint32_t)y >= cv_h) return 0;
    return swap16(cv_px[(uint32_t)y * cv_w + (uint32_t)x]);
}
void hw_box_scanlines(void)                    /* every other row at 5/8: the CRT */
{
    uint32_t j, i;
    for (j = 1; j < cv_h; j += 2)
        for (i = 0; i < cv_w; i++) {
            uint16_t c = swap16(cv_px[j * cv_w + i]);
            c = (uint16_t)((((c >> 11) * 5u / 8u) << 11) | ((((c >> 5) & 63u) * 5u / 8u) << 5) | ((c & 31u) * 5u / 8u));
            cv_px[j * cv_w + i] = swap16(c);
        }
}
void hw_box_blit(int x, int y)
{
    uint32_t r, w = cv_w, h = cv_h;
    if (x < 0 || y < 0) return;
    if ((uint32_t)x + w > 240u) w = 240u - (uint32_t)x;
    if ((uint32_t)y + h > 240u) h = 240u - (uint32_t)y;
    if (!lcd_idle()) lcd_sync();                /* fb rows may be on their way out: at most one chunk */
    for (r = 0; r < h; r++) {                  /* halfwords (libc's memcpy goes byte by byte) */
        uint16_t *d = fb + ((uint32_t)y + r) * 240u + (uint32_t)x;
        const uint16_t *q = cv_px + r * cv_w;
        for (uint32_t i = 0; i < w; i++) d[i] = q[i];
    }
    dirty_add((uint32_t)x, (uint32_t)y, w, h);
    hw_box_pump();
}
/* LEDs. SETUP > LEDS NIGHT / NIGHT+: the LEDs lit stay fully lit; every other one glows, as an outline of the panel in
 * the dark. NIGHT: fm1_input.h's glow pulse (~1.6 us a frame, ~910 Hz: ~1/60, Felucca's LEDS DIM LO), the faintest
 * steady light the scan gives. NIGHT+: its mid level (lit 1 frame in 12, ~76 Hz: ~1/12), brighter, a whole lit tick.
 * An LED in both maps is simply lit. */
static void led_set(uint32_t id, int on) { fm1_led_key(id, on); }
void hw_led_btn(int b, int on) { if (b >= 0 && b < B_NUM) led_set(BTN_ID[b], on); }
void hw_led_key(int k, int on) { if (k >= 0 && k < FM313_NKEYS) led_set(14u + (uint32_t)k, on); }
void hw_leds(int mode)                           /* 0 NORMAL, 1 NIGHT, 2 NIGHT+: what the unlit LEDs do */
{
    uint8_t all[FM1_NCOL];
    for (uint32_t c = 0; c < FM1_NCOL; c++) {         /* every position with an LED: the buttons and the keys */
        all[c] = 0;
        for (uint32_t r = 1; r < 5u; r++) if (FM1_KEYMAP[r][c] >= 0 && FM1_KEYMAP[r][c] < 41) all[c] |= (uint8_t)(1u << r);
    }
    fm1_led_dim_ns(FM1_LED_DIM_LO_NS);
    fm1__mid_n = 12u;
    for (uint32_t c = 0; c < FM1_NCOL; c++) {
        fm1_led_dim[c] = mode == 1 ? all[c] : 0u;
        fm1_led_mid[c] = mode == 2 ? all[c] : 0u;
    }
}

static void audio_silence(void)
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
}
int hw_flash_read(uint32_t off, void *dst, uint32_t n) { return flash_ok ? st_read(off, dst, n) : -1; }
int hw_flash_erase4k(uint32_t off)
{
    uint32_t took;
    if (!flash_ok) return -1;
    audio_silence();
    return fl_erase4k(off, &took);
}
int hw_flash_write(uint32_t off, const void *src, uint32_t n)
{
    if (!flash_ok) return -1;
    audio_silence();
    return fl_write(off, (const uint8_t *)src, n);
}

/* ---- audio */
static void midi_take(void)                      /* USB MIDI ring (usb.c) -> the app, in audio context */
{
    while (mi_r != mi_w) {
        uint32_t pkt = midi_in_q[mi_r % MQ];
        mi_r++;
        uint32_t cin = pkt & 0xFu;
        uint8_t st = (uint8_t)(pkt >> 8), d1 = (uint8_t)(pkt >> 16), d2 = (uint8_t)(pkt >> 24);
        if (cin == 0xFu) fm313_app_midi(st, 0, 0);
        else fm313_app_midi(st, d1, d2);
    }
}

void fm1_alnk0_irq(void)                         /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), i, t0 = fm1_ticks();
        {                                        /* a call that came late (an IRQ held off) counts as well */
            static uint32_t last;
            if (last && t0 - last > (HALF_FRAMES * 1000000u / 44100u) * 3u / 2u * FM1_TICKS_PER_US) audio_late++;
            last = t0;
        }
        int32_t *o = &abuf[half * HALF_WORDS];
        midi_take();
        fm313_app_audio(fbuf, (int)HALF_FRAMES);
        {
            float g = master_now, gt = master_target;
            for (i = 0; i < 2u * HALF_FRAMES; i += 2u) {     /* a short glide: no zipper noise */
                g += (gt - g) * 0.004f;
                o[i] = (int32_t)(fbuf[i] * OUT_SCALE * g);
                o[i + 1u] = (int32_t)(fbuf[i + 1u] * OUT_SCALE * g);
            }
            master_now = g;
        }
        fm1_audio_ack_half();
        {
            uint32_t us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
            uint32_t q8 = us * 256u / (HALF_FRAMES * 1000000u / 44100u);
            audio_load_q8 = (audio_load_q8 * 15u + q8) / 16u;
        }
        if (fm1_audio_free_half() != half)
            audio_late++;                        /* the DMA moved on while we rendered */
    }
}
extern void isr_alnk0(void);

/* ---- MASTER pot */
static void master_read(int boot)
{
    int32_t a = fm1_an.con ? (fm1_an.master ? (int32_t)fm1_an.master : -1) : fm1_adc_read(FM1_ADC_MASTER);
    if (a < 0) return;
    if (boot) master_knob = (float)a;
    else master_knob += ((float)a - master_knob) * 0.25f;
    /* the pot's track does not reach its rails without a firm turn (98-99 at the stop): the last and the first
     * 2.5 % of the travel count as the ends, so 0 and 100 come at the stops */
    float k = (master_knob - 12.f) * (1.f / (1023.f - 12.f - 26.f));
    k = k < 0.f ? 0.f : (k > 1.f ? 1.f : k);
    master_target = k * k;
    int pct = (int)(k * 100.f + 0.5f);
    if (boot) master_shown = pct;
    else if (pct - master_shown >= 3 || master_shown - pct >= 3 ||
             (pct != master_shown && (pct == 0 || pct == 100))) {   /* the knob moved (or reached an end): say so */
        master_shown = pct;
        fm313_app_notice("VOLUME", pct);
    }
}

/* ---- the main loop */
int hw_touch(int on) { (void)on; return 0; }      /* 0.6.3: no touch sensing (the keys are plain contacts) */
int hw_phones(void)                              /* PB5, the jack's switch: steady for 200 ms (a plug bounces ~150 ms) */
{
    static int st = -1, raw_last = -1;
    static uint32_t since;
    int raw = (int)fm1_hp_in();
    if (st < 0) { st = raw_last = raw; since = fm1_ms; }
    if (raw != raw_last) { raw_last = raw; since = fm1_ms; }
    else if (raw != st && fm1_ms - since >= 200u) st = raw;
    return st;
}

static void input_poll(void)
{
    uint32_t rel = 0, pr = fm1_input_edges(&rel), i;
    static uint32_t notes_prev;
    for (i = 0; i < B_NUM; i++) {
        uint32_t bit = 1u << BTN_ID[i];
        if (pr & bit) fm313_app_button((int)i, 1);
        if (rel & bit) fm313_app_button((int)i, 0);
    }
    for (i = 0; i < E_NUM; i++) {
        int32_t s = fm1_enc_take(ENC_ID[i]);
        if (s) fm313_app_encoder((int)i, (int)s);
    }
    {                                            /* the keys: plain contacts, at once (velocity 100) */
        uint32_t now = fm1_in.notes, ch = now ^ notes_prev;
        fm1_input_note_edges();                  /* (levels are enough here) */
        for (i = 0; i < FM313_NKEYS; i++)
            if (ch & (1u << i)) fm313_app_key((int)i, (int)((now >> i) & 1u), (now >> i) & 1u ? 100 : 0);
        notes_prev = now;
    }
}

static void app_main(void)
{
    warm_begin();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    fm313_app_init();
    fm1_adc_init();
    (void)fm1_adc_read(FM1_ADC_MASTER);               /* the first conversion after the pin setup: dropped */
    master_read(1);
    master_now = master_target;                       /* the first block plays at the knob's level */
    audio_silence();
    /* the vector table is write-protected once the platform has attached its own IRQs (fm1_guard_lock_top):
     * open it for the audio vector, IRQs off, and close it again. 0.2 attached it with the guard armed and
     * faulted at every boot (the boot guard's USB rescue followed) */
    fm1_irq_off();
    fm1_guard_unlock_top();
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);   /* below TIMER5 (4): the key scan nests into the render */
    fm1_guard_lock_top();
    fm1_irq_on();
    for (;;) {
        uint32_t pass0 = fm1_ticks();
        hs_poll();
        platform_poll();
        input_poll();
        {
            static uint32_t last;
            if (fm1_ms - last >= 40u) { last = fm1_ms; master_read(0); }
        }
        fm313_app_poll();
        load_tick((fm1_ticks() - pass0) / FM1_TICKS_PER_US);
    }
}
