/* AMGR live screen drawing: four waveform lanes in the page buffer (ARGB8888, premultiplied; everything not drawn stays transparent so the skin's
 * knobs show through). Each lane: the layer's waveform (a min/max column per pixel), a white position line, the window box (SIZE) from the position,
 * the layer number and file name, and a readout. Text is Titillium Web at 26 px and up (the plugin family's rule: never smaller). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "gr2.h"
#include "amgr_tile.h"

#define TXT_PX 26
#define RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))
#define COL_WAVE RGB(0xc9, 0x6a, 0x3c)       /* the skin's accent */
#define COL_WAVE_IN RGB(0xee, 0xa8, 0x78)    /* the lighter core of each column */
#define COL_AXIS RGB(0x46, 0x46, 0x46)
#define COL_POS RGB(0xff, 0xff, 0xff)
#define COL_WIN RGB(0x42, 0xd4, 0xf4)
#define COL_INK RGB(0xe4, 0xe2, 0xda)
#define COL_GREY RGB(0xbe, 0xbe, 0xba)
static const uint32_t LCOL[GR_LAYERS] = {0xff8a3d, 0x3dd6f0, 0x7ae05a, 0xe06af0};

static uint8_t *B;
static int BW, BH, BP;
static font_target_t g_ft;
static char g_font_path[512];
static int g_font_tried;

void gr_screen_set_font(const char *ttf_path) { snprintf(g_font_path, sizeof g_font_path, "%s", ttf_path ? ttf_path : ""); g_font_tried = 0; }

static inline void put_opaque(uint8_t *d, uint32_t rgb) { d[0] = (uint8_t)rgb; d[1] = (uint8_t)(rgb >> 8); d[2] = (uint8_t)(rgb >> 16); d[3] = 255; }
static inline void blend_px(uint8_t *d, uint32_t rgb, int a) {
    int ia = 255 - a;
    d[0] = (uint8_t)(((rgb & 255) * a + d[0] * ia + 127) / 255);
    d[1] = (uint8_t)((((rgb >> 8) & 255) * a + d[1] * ia + 127) / 255);
    d[2] = (uint8_t)((((rgb >> 16) & 255) * a + d[2] * ia + 127) / 255);
    d[3] = (uint8_t)(a + (d[3] * ia + 127) / 255);
}
static void clip_rect(int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > BW) *w = BW - *x;
    if (*y + *h > BH) *h = BH - *y;
}
static void fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    clip_rect(&x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;
    uint8_t *row0 = B + (size_t)y * BP + x * 4;
    for (int i = 0; i < w; i++) put_opaque(row0 + i * 4, rgb);
    for (int j = 1; j < h; j++) memcpy(row0 + (size_t)j * BP, row0, (size_t)w * 4);
}
static void blend_rect(int x, int y, int w, int h, uint32_t rgb, int a) {
    clip_rect(&x, &y, &w, &h);
    if (w <= 0 || h <= 0 || a <= 0) return;
    for (int j = 0; j < h; j++) {
        uint8_t *d = B + (size_t)(y + j) * BP + x * 4;
        for (int i = 0; i < w; i++, d += 4) blend_px(d, rgb, a);
    }
}
static void vline(int x, int y0, int y1, uint32_t rgb, int a) {
    if (x < 0 || x >= BW) return;
    if (y0 < 0) y0 = 0;
    if (y1 > BH) y1 = BH;
    uint8_t *d = B + (size_t)y0 * BP + x * 4;
    if (a >= 255) for (int y = y0; y < y1; y++, d += BP) put_opaque(d, rgb);
    else for (int y = y0; y < y1; y++, d += BP) blend_px(d, rgb, a);
}
static void textp(int px, int x, int y, uint32_t rgb, const char *s, int cx, int cy, int cw, int ch) {
    font_clip_t c = {cx, cy, cw, ch};
    font_draw(&g_ft, x, y, px, rgb, 255, s, &c);
}
static inline uint32_t shade(uint32_t c, int num, int add) {   /* c * num / 100 + add, per channel, clamped */
    int r = (int)((c >> 16) & 255) * num / 100 + add, g = (int)((c >> 8) & 255) * num / 100 + add, b = (int)(c & 255) * num / 100 + add;
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    return RGB(r, g, b);
}
static void disc(int cx, int cy, int r, uint32_t rgb) {
    for (int y = -r; y <= r; y++) {
        int yy = cy + y;
        if (yy < 0 || yy >= BH) continue;
        int hw = (int)sqrtf((float)(r * r - y * y));
        int x0 = cx - hw, x1 = cx + hw;
        if (x0 < 0) x0 = 0;
        if (x1 >= BW) x1 = BW - 1;
        uint8_t *d = B + (size_t)yy * BP + x0 * 4;
        for (int x = x0; x <= x1; x++, d += 4) put_opaque(d, rgb);
    }
}
/* VARIANT's tile, stretched to w x h with its corners kept (nine-slice); bright: 100 = as is, more = the selected look */
static void tile(int x, int y, int w, int h, int bright) {
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= BH) continue;
        int sy = j < TILE_E ? j : j >= h - TILE_E ? TILE_SH - (h - j) : TILE_E + (j - TILE_E) * (TILE_SH - 2 * TILE_E) / (h - 2 * TILE_E > 0 ? h - 2 * TILE_E : 1);
        if (sy < 0) sy = 0; if (sy >= TILE_SH) sy = TILE_SH - 1;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= BW) continue;
            int sx = i < TILE_E ? i : i >= w - TILE_E ? TILE_SW - (w - i) : TILE_E + (i - TILE_E) * (TILE_SW - 2 * TILE_E) / (w - 2 * TILE_E > 0 ? w - 2 * TILE_E : 1);
            if (sx < 0) sx = 0; if (sx >= TILE_SW) sx = TILE_SW - 1;
            const unsigned char *t = TILE_PX + (sy * TILE_SW + sx) * 4;
            if (!t[3]) continue;
            uint32_t c = RGB(t[0], t[1], t[2]);
            if (bright != 100) c = shade(c, bright, 0);
            put_opaque(B + (size_t)yy * BP + xx * 4, c);
        }
    }
}
/* a label centred in a box: the size drops from px to 26 when it does not fit, then the text is cut with ".." */
static void label(int x, int y, int w, int h, int px, uint32_t rgb, const char *s) {
    char t[64];
    snprintf(t, sizeof t, "%s", s);
    int room = w - 16;
    while (px > 26 && font_width(px, t) > room) px -= 2;
    if (px < 26) px = 26;
    int n = (int)strlen(t);
    while (n > 3 && font_width(px, t) > room) { n--; t[n] = 0; t[n - 1] = '.'; t[n - 2] = '.'; }
    int tw = font_width(px, t), th = font_height(px);
    textp(px, x + (w - tw) / 2, y + (h - th) / 2, rgb, t, x, y, w, h);
}
static void tile_label(int x, int y, int w, int h, int px, const char *s, int bright) { tile(x, y, w, h, bright); label(x, y, w, h, px, COL_INK, s); }
static void clear_px(int x, int y) { if (x >= 0 && x < BW && y >= 0 && y < BH) memset(B + (size_t)y * BP + x * 4, 0, 4); }


void gr_draw(int page, const gr_view_t *v, uint8_t *buf, int w, int h, int pitch, double now) {
    (void)now;
    B = buf; BW = w; BH = h; BP = pitch;
    g_ft.p = buf; g_ft.w = w; g_ft.h = h; g_ft.pitch = pitch;
    if (!g_font_tried) { g_font_tried = 1; if (g_font_path[0]) font_load(g_font_path); font_prepare(26); font_prepare(28); font_prepare(30); font_prepare(32); font_prepare(34); }
    /* the strip under the lanes starts see-through every frame, so nothing from another page can stay on the screen */
    for (int y = UI_Y - 4; y < h; y++) memset(buf + (size_t)y * pitch, 0, (size_t)w * 4);
    for (int L = 0; L < GR_LAYERS; L++) {
        gr_wave_t wv;
        gr_wave_copy(L, &wv);
        int lx = GR_LANE_X, ly = GR_LANE_Y0 + L * GR_LANE_PITCH, lw = GR_LANE_W, lh = GR_LANE_H, half = lh / 2;
        uint32_t col = LCOL[L], cout = shade(col, 78, 0), cin = shade(col, 100, 30);
        fill_rect(lx, ly, lw, lh, 0x000000);
        for (int x = 0; x < lw; x++) put_opaque(B + (size_t)(ly + half) * BP + (lx + x) * 4, COL_AXIS);
        if (wv.state == 2) {
            for (int x = 0; x < lw; x++) {
                int c = x * GR_COLS / lw;
                int a = (int)((long)wv.mx[c] * (half - 3) / 32767), b = (int)((long)(-wv.mn[c]) * (half - 3) / 32767);
                if (a < 0) a = 0;
                if (b < 0) b = 0;
                vline(lx + x, ly + half - a, ly + half + b + 1, cout, 255);
                int a2 = a / 2, b2 = b / 2;
                if (a2 + b2 > 1) vline(lx + x, ly + half - a2, ly + half + b2 + 1, cin, 255);
            }
        }
        /* the scatter band, the grain's length from the position, the position line, then the grains as balls on top of the waveform */
        float pos = v->pos[L];
        int px = lx + (int)(pos * (lw - 2));
        if (v->sprd[L] > 0.002f) {
            int sa = lx + (int)((pos - v->sprd[L] * 0.25f) * (lw - 2)), sb = lx + (int)((pos + v->sprd[L] * 0.25f) * (lw - 2));
            if (sa < lx) sa = lx;
            if (sb > lx + lw) sb = lx + lw;
            if (sb > sa) blend_rect(sa, ly, sb - sa, lh, 0xffffff, 24);
        }
        int ww = (int)(v->winw[L] * lw);
        if (ww < 4) ww = 4;
        if (px + ww > lx + lw) ww = lx + lw - px;
        blend_rect(px, ly, ww, lh, col, 50);
        vline(px, ly, ly + lh, COL_POS, 255);
        vline(px + 1, ly, ly + lh, COL_POS, 255);
        for (int g = 0; g < GR_ACT; g++) {
            float a = v->act[L][g];
            if (a < 0) continue;
            int gx = lx + 9 + (int)(a * (lw - 18));
            int gy = ly + 14 + (int)((unsigned)(g * 37 + L * 11) % (unsigned)(lh - 28));   /* each slot keeps its own height */
            int r = 6 + (g % 3) * 2;
            disc(gx, gy, r + 3, shade(col, 50, 0));
            disc(gx, gy, r, col);
            disc(gx, gy, r - 2, 0xffffff);
        }
        /* the lane's number (lit on the ENVELOPE page for the layer its knobs hold), the readout on a faint backing, a loading note */
        char s[96];
        snprintf(s, sizeof s, "%d", L + 1);
        int sel = page == PG_ENVELOPE && v->env_layer == L;
        if (sel) fill_rect(lx, ly, 6, lh, col);
        textp(34, lx + 12, ly + 2, sel ? col : 0xf0f0f0, s, lx, ly, lw, lh);
        float secs = pos * v->dur[L];
        int t = (int)(secs * 10);
        if (v->size_ms[L] >= 1000) snprintf(s, sizeof s, "%d:%02d.%d %.1fs %+dst", t / 600, (t / 10) % 60, t % 10, v->size_ms[L] / 1000, v->pitch[L]);
        else snprintf(s, sizeof s, "%d:%02d.%d %dms %+dst", t / 600, (t / 10) % 60, t % 10, (int)(v->size_ms[L] + 0.5f), v->pitch[L]);
        int tw = font_width(26, s), th = font_height(26);
        blend_rect(lx + lw - tw - 16, ly + 3, tw + 14, th, 0x000000, 64);
        textp(26, lx + lw - tw - 9, ly + 3, 0xe1e1e1, s, lx, ly, lw, lh);
        s[0] = 0;
        if (v->load_pct[L] >= 0) snprintf(s, sizeof s, "LOADING %d%%", v->load_pct[L]);
        else if (v->lmsg[L][0]) snprintf(s, sizeof s, "%s", v->lmsg[L]);
        if (s[0]) {
            tw = font_width(34, s); th = font_height(34);
            fill_rect(lx + (lw - tw) / 2 - 14, ly + (lh - th) / 2 - 4, tw + 28, th + 8, 0x000000);
            textp(34, lx + (lw - tw) / 2, ly + (lh - th) / 2, 0xffffff, s, lx, ly, lw, lh);
        }
        /* slightly round corners: the corner pixels go back to see-through */
        static const int cut[8] = {5, 3, 2, 1, 1, 0, 0, 0};
        for (int j = 0; j < 5; j++)
            for (int i = 0; i < cut[j]; i++) {
                clear_px(lx + i, ly + j); clear_px(lx + lw - 1 - i, ly + j);
                clear_px(lx + i, ly + lh - 1 - j); clear_px(lx + lw - 1 - i, ly + lh - 1 - j);
            }
    }
    /* the strip under the lanes: tiles and lists on the pages that have them (everything here is drawn on solid black, never on see-through pixels) */
    if (page == PG_SAMPLES || page == PG_PRESETS) {
        fill_rect(2, UI_Y, BW - 4, BH - UI_Y - 2, 0x000000);   /* not over the page markers in the corners */
        int open = (page == PG_SAMPLES && v->ui_open == 1) || (page == PG_PRESETS && v->ui_open == 2);
        if (open) {
            fill_rect(LIST_X, LIST_Y, LIST_W, 2, RGB(70, 70, 66)); fill_rect(LIST_X, LIST_Y + LIST_H - 2, LIST_W, 2, RGB(70, 70, 66));
            fill_rect(LIST_X, LIST_Y, 2, LIST_H, RGB(70, 70, 66)); fill_rect(LIST_X + LIST_W - 2, LIST_Y, 2, LIST_H, RGB(70, 70, 66));
            textp(26, LIST_X + 18, LIST_Y + 5, COL_GREY, v->ui_title, LIST_X + 4, LIST_Y + 2, LIST_W - 8, 34);
            fill_rect(LIST_X + 10, ROW_Y0 - 3, LIST_W - 20, 1, RGB(50, 50, 48));
            for (int r = 0; r < GR_ROWS; r++) {
                int ry = ROW_Y0 + r * ROW_H;
                if (r == v->ui_sel) fill_rect(LIST_X + 6, ry, LIST_W - 12, ROW_H - 1, RGB(52, 52, 50));
                textp(28, LIST_X + 22, ry + 1, r == v->ui_sel ? COL_INK : COL_GREY, v->ui_row[r], LIST_X + 6, ry, LIST_W - 12, ROW_H);
            }
            if (page == PG_SAMPLES) {
                static const char *bn[4] = {"BACK", "OPEN", "LOAD", "DONE"};
                for (int i = 0; i < 4; i++) tile_label(LBTN_X(i), TILE_Y, LBTN_W, TILE_H, 28, bn[i], 100);
            } else {
                tile_label(LBTN_X(0), TILE_Y, LBTN_W, TILE_H, 28, "DONE", 100);
                tile_label(LBTN_X(1), TILE_Y, LBTN_W, TILE_H, 28, "FACT", v->ui_bank == 0 ? 170 : 100);
                tile_label(LBTN_X(2), TILE_Y, LBTN_W, TILE_H, 28, "USER", v->ui_bank == 1 ? 170 : 100);
            }
        } else if (page == PG_SAMPLES) {
            for (int L = 0; L < GR_LAYERS; L++) tile_label(SBOX_X(L), TILE_Y, SBOX_W, TILE_H, 34, v->box[L], 100);
        } else {
            char s[40];
            tile_label(PBOX_X, TILE_Y, PBOX_W, TILE_H, 34, v->preset, 100);
            tile_label(SAVE_X, TILE_Y, BTN_W, TILE_H, 34, "SAVE", 100);
            snprintf(s, sizeof s, "BEND %dst", v->bend);
            tile_label(BEND_X, TILE_Y, BTN_W, TILE_H, 34, s, 100);
        }
    } else if (page == PG_ENVELOPE) {   /* the four layer tiles left of the knobs; the rest of the strip stays see-through for the knobs */
        for (int i = 0; i < GR_LAYERS; i++) {
            char s[4];
            snprintf(s, sizeof s, "%d", i + 1);
            tile(ET_X(i), ET_Y(i), ET_W, ET_H, v->env_layer == i ? 190 : 100);
            label(ET_X(i), ET_Y(i), ET_W, ET_H, 34, v->env_layer == i ? LCOL[i] : COL_GREY, s);
        }
    }
}
