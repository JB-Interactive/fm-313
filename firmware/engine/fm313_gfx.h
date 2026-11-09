/* SPDX-License-Identifier: GPL-3.0-only
 * FM-313 drawing on top of the three canvas services (hw_box_begin / rect / blit):
 * anti-aliased text in the Jost fonts (fm313_fonts.h), filled circles and triangles.
 * Everything is spans of hw_box_rect, so the simulator draws exactly what the FM-1 draws. */
#ifndef FM313_GFX_H
#define FM313_GFX_H
#include <stdint.h>

typedef struct {
    uint8_t h, pad, first, last;
    const uint8_t *adv, *bw;
    const uint32_t *off;
    const uint8_t *data;
    uint8_t cap_y, cap_h, base;   /* the capitals' ink: top and height in the cell; the baseline (tools/gen_fonts.py) */
} fm313_font_t;

#include "fm313_fonts.h"

void hw_box_rect(int x, int y, int w, int h, uint16_t c);
uint16_t hw_box_px(int x, int y);          /* what the canvas holds there (0 outside) */

static inline uint16_t gfx_mix(uint16_t fg, uint16_t bg, uint32_t a)   /* a 0..15; x*17/256 ~ x/15, no division */
{
    uint32_t na = 15u - a;
    uint32_t r = (((fg >> 11) * a + (bg >> 11) * na) * 17u) >> 8;
    uint32_t g = ((((fg >> 5) & 63u) * a + ((bg >> 5) & 63u) * na) * 17u) >> 8;
    uint32_t b = (((fg & 31u) * a + (bg & 31u) * na) * 17u) >> 8;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static inline uint32_t gfx_glyph(const fm313_font_t *f, uint32_t ch)
{
    if (ch >= 'a' && ch <= 'z') ch -= 32u;
    if (ch < f->first || ch > f->last) ch = '?';
    return ch - f->first;
}

static inline int gfx_text_w(const fm313_font_t *f, const char *s, int track)
{
    int w = 0;
    for (; *s; s++) w += f->adv[gfx_glyph(f, (uint8_t)*s)] + track;
    return w ? w - track : 0;
}

/* text with its top-left at (x, y), blended onto bg; returns the end x */
static inline int gfx_text(int x, int y, const fm313_font_t *f, const char *s, uint16_t fg, uint16_t bg, int track)
{
    uint16_t ramp[16];
    for (uint32_t a = 0; a < 16u; a++) ramp[a] = gfx_mix(fg, bg, a);
    for (; *s; s++) {
        uint32_t gi = gfx_glyph(f, (uint8_t)*s), w = f->bw[gi], bpr = (w + 1u)/2u;
        const uint8_t *gd = f->data + f->off[gi];
        for (uint32_t gy = 0; gy < f->h; gy++) {
            uint32_t gx = 0;
            while (gx < w) {                       /* runs of equal alpha: one rect each */
                uint32_t v = gd[gy*bpr + gx/2u];
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                uint32_t e = gx + 1u;
                while (e < w) {
                    uint32_t u = gd[gy*bpr + e/2u];
                    u = (e & 1u) ? (u & 15u) : (u >> 4);
                    if (u != v) break;
                    e++;
                }
                if (v) hw_box_rect(x - f->pad + (int)gx, y + (int)gy, (int)(e - gx), 1, ramp[v]);
                gx = e;
            }
        }
        x += f->adv[gi] + track;
    }
    return x;
}

static inline void gfx_text_c(int cx, int y, const fm313_font_t *f, const char *s, uint16_t fg, uint16_t bg, int track)
{
    gfx_text(cx - gfx_text_w(f, s, track)/2, y, f, s, fg, bg, track);
}
static inline void gfx_text_r(int rx, int y, const fm313_font_t *f, const char *s, uint16_t fg, uint16_t bg, int track)
{
    gfx_text(rx - gfx_text_w(f, s, track), y, f, s, fg, bg, track);
}

static inline void gfx_circle(int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1)*(dx + 1) + dy*dy <= r*r + r) dx++;
        hw_box_rect(cx - dx, cy + dy, 2*dx + 1, 1, c);
    }
}

/* a triangle pointing right (play, slide) or up / down (octave): apex and base */
static inline void gfx_tri_right(int x, int y, int w, int h, uint16_t c)   /* box x,y w*h, apex at the right */
{
    for (int i = 0; i < h; i++) {
        int d = i < h/2 ? i : h - 1 - i;
        int len = (2*d + 1)*w/(h | 1);
        if (len < 1) len = 1;
        hw_box_rect(x, y + i, len, 1, c);
    }
}
static inline void gfx_tri_up(int x, int y, int w, int h, uint16_t c, int down)
{
    for (int i = 0; i < h; i++) {
        int row = down ? h - 1 - i : i;
        int len = (row + 1)*w/h;
        if (len < 1) len = 1;
        hw_box_rect(x + (w - len)/2, y + i, len, 1, c);
    }
}
static inline void gfx_frame(int x, int y, int w, int h, int t, uint16_t c)
{
    hw_box_rect(x, y, w, t, c); hw_box_rect(x, y + h - t, w, t, c);
    hw_box_rect(x, y, t, h, c); hw_box_rect(x + w - t, y, t, h, c);
}

/* ---- anti-aliased forms, blended onto what the canvas already holds ---- */
static inline float gfx_sqrt(float x)
{
    if (x <= 0.f) return 0.f;
    union { float f; int32_t i; } u; u.f = x;
    u.i = 0x1fbd1df5 + (u.i >> 1);
    float y = u.f;
    y = 0.5f*(y + x/y); y = 0.5f*(y + x/y);
    return y;
}
static inline void gfx_blend(int x, int y, uint16_t c, float a)
{
    if (a <= 0.004f) return;
    if (a >= 0.996f) { hw_box_rect(x, y, 1, 1, c); return; }
    hw_box_rect(x, y, 1, 1, gfx_mix(c, hw_box_px(x, y), (uint32_t)(a*15.f + 0.5f)));
}
static inline float gfx_clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

/* a disc (r_in = 0) or a ring between r_in and r_out, centre in pixel units */
static inline void gfx_ring(float cx, float cy, float r_out, float r_in, uint16_t c)
{
    int y0 = (int)(cy - r_out - 1.f), y1 = (int)(cy + r_out + 2.f);
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy, dy2 = dy*dy;
        if (dy2 > (r_out + 1.f)*(r_out + 1.f)) continue;
        float ho = gfx_sqrt((r_out + 1.f)*(r_out + 1.f) - dy2);
        int x0 = (int)(cx - ho) - 1, x1 = (int)(cx + ho) + 1;
        /* the solid middle of a disc in one rect */
        float hi = r_in > 0.f ? -1.f : ((r_out - 1.f)*(r_out - 1.f) > dy2 ? gfx_sqrt((r_out - 1.f)*(r_out - 1.f) - dy2) : -1.f);
        int s0 = hi > 0.f ? (int)(cx - hi) + 1 : 1, s1 = hi > 0.f ? (int)(cx + hi) - 1 : 0;
        if (s1 >= s0) hw_box_rect(s0, y, s1 - s0 + 1, 1, c);
        for (int x = x0; x <= x1; x++) {
            if (x >= s0 && x <= s1) continue;
            float dx = (float)x + 0.5f - cx, d = gfx_sqrt(dx*dx + dy2);
            float a = gfx_clamp01(r_out + 0.5f - d);
            if (r_in > 0.f) a *= gfx_clamp01(d - r_in + 0.5f);
            gfx_blend(x, y, c, a);
        }
    }
}
static inline void gfx_disc(float cx, float cy, float r, uint16_t c) { gfx_ring(cx, cy, r, 0.f, c); }

/* a right-angled wedge: the vertical edge at x0 from base_y up h, the slope down to x0 + w */
static inline void gfx_wedge(int x0, int base_y, float w, float h, uint16_t c)
{
    for (int y = 0; y < (int)(h + 1.f); y++) {
        float row = (float)y + 0.5f;                     /* distance above the base */
        if (row > h) break;
        float len = w*(1.f - row/h);                     /* filled length on this row */
        int full = (int)len;
        if (full > 0) hw_box_rect(x0, base_y - 1 - y, full, 1, c);
        gfx_blend(x0 + full, base_y - 1 - y, c, len - (float)full);
    }
}

/* a line of thickness t (any angle), anti-aliased by distance */
static inline void gfx_line_aa(float xa, float ya, float xb, float yb, float t, uint16_t c)
{
    float dx = xb - xa, dy = yb - ya, L2 = dx*dx + dy*dy;
    if (L2 < 0.01f) return;
    int x0 = (int)((xa < xb ? xa : xb) - t - 1.f), x1 = (int)((xa > xb ? xa : xb) + t + 1.f);
    int y0 = (int)((ya < yb ? ya : yb) - t - 1.f), y1 = (int)((ya > yb ? ya : yb) + t + 1.f);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f - xa, py = (float)y + 0.5f - ya;
            float u = gfx_clamp01((px*dx + py*dy)/L2);
            float ex = px - u*dx, ey = py - u*dy;
            gfx_blend(x, y, c, gfx_clamp01(t*0.5f + 0.5f - gfx_sqrt(ex*ex + ey*ey)));
        }
}

/* text blended onto what the canvas holds, with an alpha scale (glow layers) */
static inline int gfx_text_over(int x, int y, const fm313_font_t *f, const char *s, uint16_t fg, int track, float alpha)
{
    for (; *s; s++) {
        uint32_t gi = gfx_glyph(f, (uint8_t)*s), w = f->bw[gi], bpr = (w + 1u)/2u;
        const uint8_t *gd = f->data + f->off[gi];
        for (uint32_t gy = 0; gy < f->h; gy++)
            for (uint32_t gx = 0; gx < w; gx++) {
                uint32_t v = gd[gy*bpr + gx/2u];
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                if (v) gfx_blend(x - f->pad + (int)gx, y + (int)gy, fg, (float)v*(1.f/15.f)*alpha);
            }
        x += f->adv[gi] + track;
    }
    return x;
}

/* text turned 90 degrees, read from bottom to top; (x, y) = the bottom-left corner of the run */
static inline int gfx_text_up(int x, int y, const fm313_font_t *f, const char *s, uint16_t fg, int track)
{
    for (; *s; s++) {
        uint32_t gi = gfx_glyph(f, (uint8_t)*s), w = f->bw[gi], bpr = (w + 1u)/2u;
        const uint8_t *gd = f->data + f->off[gi];
        for (uint32_t gy = 0; gy < f->h; gy++)
            for (uint32_t gx = 0; gx < w; gx++) {
                uint32_t v = gd[gy*bpr + gx/2u];
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                if (v) gfx_blend(x + (int)gy, y - (int)gx + f->pad, fg, (float)v*(1.f/15.f));
            }
        y -= f->adv[gi] + track;
    }
    return y;
}
#endif
