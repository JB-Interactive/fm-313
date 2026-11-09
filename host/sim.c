/* SPDX-License-Identifier: GPL-3.0-only
 * Host simulator for the FM-313 app: runs scripted button/encoder/key input
 * against the real app code, renders the audio to a WAV and dumps screens
 * (framebuffer + text list) for tools/screen.py.
 *   sim script.txt out_prefix
 * script lines:  btn NAME 1|0 | enc NAME steps | key k 1|0 | wait ms | shot name
 *                midi b1 b2 b3 | power   (reboot: re-init from "flash")
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../firmware/engine/fm313_app.h"

static uint32_t now_ms;
static uint16_t fb[240*240];
static uint16_t box[240*240]; static int bw, bh;
static uint8_t flash[0x100000];
static int leds_btn[B_NUM], leds_key[FM313_NKEYS];

uint32_t hw_ms(void) { return now_ms; }
int hw_cpu_pct(void) { return 0; }
uint32_t hw_us(void) { return 0; }
void hw_enter_update(void) {}
int hw_phones(void) { return 1; }
void hw_leds(int mode) { (void)mode; }
static int sim_touch; int hw_touch(int on) { if (on >= 0) sim_touch = on; return sim_touch; }
static int midi_out_n; void hw_midi_out(uint8_t st, uint8_t d1, uint8_t d2) { (void)st; (void)d1; (void)d2; midi_out_n++; }
void hw_load(int *t, int *l, int *u) { *t = 0; *l = 0; *u = 0; }
void hw_box_begin(int w, int h, uint16_t bg) { bw = w; bh = h; for (int i = 0; i < w*h; i++) box[i] = bg; }
void hw_box_rect(int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++)
        if (i >= 0 && j >= 0 && i < bw && j < bh) box[j*bw + i] = c;
}
uint16_t hw_box_px(int x, int y) { return (x >= 0 && y >= 0 && x < bw && y < bh) ? box[y*bw + x] : 0; }
void hw_box_scanlines(void)
{
    for (int j = 1; j < bh; j += 2) for (int i = 0; i < bw; i++) {
        uint16_t c = box[j*bw + i];
        box[j*bw + i] = (uint16_t)((((c >> 11)*5/8) << 11) | ((((c >> 5) & 63)*5/8) << 5) | ((c & 31)*5/8));
    }
}
int hw_box_pump(void) { return 0; }
void hw_fb_vspan(int x, int y0, int y1, uint16_t c) { for (int y = y0; y <= y1; y++) if (x >= 0 && x < 240 && y >= 0 && y < 240) fb[y*240 + x] = c; }
int hw_fb_idle(void) { return 1; }
void hw_fb_dirty(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void hw_box_blit(int x, int y)
{
    for (int j = 0; j < bh; j++) for (int i = 0; i < bw; i++)
        if (x + i < 240 && y + j < 240) fb[(y + j)*240 + x + i] = box[j*bw + i];
}
void hw_led_btn(int b, int on) { leds_btn[b] = on; }
void hw_led_key(int k, int on) { leds_key[k] = on; }
int hw_flash_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, flash + off, n); return 0; }
int hw_flash_erase4k(uint32_t off) { if (off & 0xFFF) return -1; memset(flash + off, 0xFF, 4096); return 0; }
int hw_flash_write(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    for (uint32_t i = 0; i < n; i++) flash[off + i] &= s[i];   /* NOR: only 1 -> 0 */
    return 0;
}

static const char *BN[B_NUM] = { "FX","SEL","ENV","LFO","EDIT","GLO","HOME","SAVE","ARP","SEQ","PLAY","REC","OCT-","OCT+" };
static const char *EN[E_NUM] = { "SELECT","ALGO","PRESETS","K1","K2","K3","K4" };

static float *audio; static int alen, acap;
static void run_ms(int ms)
{
    for (int t = 0; t < ms; t++) {
        /* 1 ms ~ 44.1 frames: render in 32-frame blocks like the ISR, poll the UI each ms */
        static float acc;
        acc += 44.1f;
        while (acc >= 32.f) {
            if (alen + 32 > acap) { acap = acap ? acap*2 : 1 << 20; audio = realloc(audio, acap*2*sizeof(float)); }
            fm313_app_audio(audio + 2*alen, 32);
            alen += 32; acc -= 32.f;
        }
        now_ms++;
        fm313_app_poll();
    }
}

static void shot(const char *prefix, const char *name)
{
    char fn[256]; snprintf(fn, sizeof fn, "%s_%s.scr", prefix, name);
    FILE *f = fopen(fn, "wb");
    fwrite(fb, 2, 240*240, f);
    fprintf(f, "LEDS");
    for (int i = 0; i < B_NUM; i++) if (leds_btn[i]) fprintf(f, " %s", BN[i]);
    fprintf(f, "\n");
    fclose(f);
}

static void write_wav(const char *fn)
{
    FILE *f = fopen(fn, "wb");
    int sr = 44100, ch = 2, bytes = alen*4, v;
    short s;
    fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    s = 1; fwrite(&s, 2, 1, f); s = 2; fwrite(&s, 2, 1, f);
    fwrite(&sr, 4, 1, f); v = sr*ch*2; fwrite(&v, 4, 1, f);
    s = 4; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    for (int i = 0; i < alen*2; i++) {
        float y = audio[i]; if (y > 1) y = 1; if (y < -1) y = -1;
        short q = (short)lrintf(y*32767.f); fwrite(&q, 2, 1, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "sim script out_prefix\n"); return 1; }
    memset(flash, 0xFF, sizeof flash);
    fm313_app_init();
    FILE *sc = fopen(argv[1], "r");
    char line[256];
    int ln = 0, errors = 0;
    while (fgets(line, sizeof line, sc)) {
        ln++;
        char cmd[32], a[32] = "", b[32] = "", c[32] = "";
        if (line[0] == '#' || sscanf(line, "%31s %31s %31s %31s", cmd, a, b, c) < 1) continue;
        if (!strcmp(cmd, "btn")) {
            int i; for (i = 0; i < B_NUM; i++) if (!strcmp(a, BN[i])) break;
            if (i == B_NUM) { fprintf(stderr, "line %d: button %s?\n", ln, a); errors++; continue; }
            fm313_app_button(i, atoi(b)); run_ms(1);
        } else if (!strcmp(cmd, "press")) {
            int i; for (i = 0; i < B_NUM; i++) if (!strcmp(a, BN[i])) break;
            if (i == B_NUM) { fprintf(stderr, "line %d: button %s?\n", ln, a); errors++; continue; }
            fm313_app_button(i, 1); run_ms(30); fm313_app_button(i, 0); run_ms(1);
        } else if (!strcmp(cmd, "enc")) {
            int i; for (i = 0; i < E_NUM; i++) if (!strcmp(a, EN[i])) break;
            if (i == E_NUM) { fprintf(stderr, "line %d: encoder %s?\n", ln, a); errors++; continue; }
            fm313_app_encoder(i, atoi(b)); run_ms(1);
        } else if (!strcmp(cmd, "key")) {
            fm313_app_key(atoi(a), atoi(b), c[0] ? atoi(c) : 100); run_ms(1);
        } else if (!strcmp(cmd, "tap")) {
            fm313_app_key(atoi(a), 1, b[0] ? atoi(b) : 100); run_ms(60); fm313_app_key(atoi(a), 0, 0); run_ms(1);
        } else if (!strcmp(cmd, "pressure")) {
            fm313_app_pressure(atoi(a), atoi(b)); run_ms(1);
        } else if (!strcmp(cmd, "midi")) {
            fm313_app_midi((uint8_t)strtol(a, 0, 0), (uint8_t)strtol(b, 0, 0), (uint8_t)strtol(c, 0, 0));
        } else if (!strcmp(cmd, "wait")) {
            run_ms(atoi(a));
        } else if (!strcmp(cmd, "shot")) {
            run_ms(40); shot(argv[2], a);
        } else if (!strcmp(cmd, "power")) {
            { extern void sim_reset(void); sim_reset(); } memset(fb, 0, sizeof fb);
            fm313_app_init(); run_ms(40);
        } else if (!strcmp(cmd, "expect")) {   /* expect FIELD value */
            int v = atoi(b), got = 0;
            extern int sim_field(const char *);
            got = sim_field(a);
            if (got != v) { fprintf(stderr, "line %d: expect %s %d, got %d\n", ln, a, v, got); errors++; }
        }
    }
    char fn[256]; snprintf(fn, sizeof fn, "%s.wav", argv[2]);
    write_wav(fn);
    fprintf(stderr, "sim: %d errors, %.1f s audio\n", errors, alen/44100.0);
    return errors != 0;
}
