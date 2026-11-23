/* AMGR: what the engine, the live waveform screen and the drawing share. */
#pragma once
#include <stdint.h>

#define GR_PATH 512
#define GR_LAYERS 4
#include "amgr_idx.h"
#define GR_PAGES GR_PAGES_N              /* the tabs: the same lanes are drawn on every one */
#define GR_SKIN_W 1280                  /* the plugin page, as the skin draws it */
#define GR_SKIN_H 628
#define GR_COLS 1240                    /* one waveform column per pixel of a lane */
#define GR_LANE_X 4
#define GR_LANE_W 1272
#define GR_LANE_Y0 6
#define GR_LANE_H 100
#define GR_LANE_PITCH 105

/* the strip under the lanes: tiles and lists the live screen draws and the touch reader hits (one place for both) */
#define UI_Y 426
#define TILE_Y 482
#define TILE_H 84
#define SBOX_X(L) (22 + (L) * 310)
#define SBOX_W 292
#define PBOX_X 22
#define PBOX_W 780
#define SAVE_X 830
#define BEND_X 1040
#define BTN_W 200
#define LIST_X 14
#define LIST_Y 432
#define LIST_W 800
#define LIST_H 190
#define ROW_Y0 472
#define ROW_H 36
#define GR_ROWS 4
#define LBTN_X(i) (826 + (i) * 112)
#define LBTN_W 104
#define ET_X(i) (6 + ((i) & 1) * 58)
#define ET_Y(i) (438 + ((i) >> 1) * 94)
#define ET_W 54
#define ET_H 86

/* the marker blocks the skin bakes into every tab's page background (tools/skin_post.py): a 2 x 2 block at the top-left corner and one at the
 * bottom-right corner, colours that carry the tab number (XYZ's recipe, proven on the Force 3.9.1) */
#define GR_MARK_TL_X 0
#define GR_MARK_TL_Y 0
#define GR_MARK_BR_X (GR_SKIN_W - 2)
#define GR_MARK_BR_Y (GR_SKIN_H - 2)
#include "amgr_marks.h"

/* what the screen draws, published by the engine on every parameter change (seqlock copy) */
#define GR_ACT 24                       /* grains shown per lane */
typedef struct {
    float pos[GR_LAYERS];               /* 0..1 along the sample */
    float winw[GR_LAYERS];              /* one grain's length as a fraction of the sample (the box) */
    float sprd[GR_LAYERS];              /* the scatter, 0..1 */
    float size_ms[GR_LAYERS], dens[GR_LAYERS], dur[GR_LAYERS];
    int pitch[GR_LAYERS];
    float act[GR_LAYERS][GR_ACT];       /* where the playing grains are, 0..1; below 0 = none */
    int voices;
    char status[72];                    /* the preset's name, or a short message */
    int load_pct[GR_LAYERS];            /* 0..100 while a file is being read, else -1 */
    char lmsg[GR_LAYERS][40];           /* a short message on a lane (too long, no memory), "" = none */
    char box[GR_LAYERS][40];            /* the sample tiles' text */
    char preset[56];                    /* the preset box's text */
    int bend, env_layer;                /* bend range in semitones; the ENVELOPE page's layer 0..3 */
    int ui_open, ui_layer, ui_bank;     /* 0 closed, 1 the sample browser of ui_layer, 2 the preset list (bank 0 factory, 1 user) */
    char ui_title[72];
    char ui_row[GR_ROWS][56];
    int ui_sel;                         /* the highlighted row, -1 none */
} gr_view_t;

/* one layer's waveform: per column the lowest and highest sample (int16 scale) */
typedef struct {
    int state;                          /* 0 empty, 1 loading, 2 ready */
    int is_test;                        /* 1: a generated test wave (no sample file for this layer) */
    float seconds;                      /* length of the sample (0 for a test wave) */
    char name[64];
    int16_t mn[GR_COLS], mx[GR_COLS];
} gr_wave_t;

void gr_log(const char *fmt, ...);
void gr_wave_copy(int layer, gr_wave_t *dst);     /* a consistent copy (taken under the layer's lock) */

/* the live screen (gr2_overlay.c) */
typedef struct gr_overlay gr_overlay_t;
gr_overlay_t *gr_overlay_start(const char *plugin_dir, int *ok);
void gr_overlay_stop(gr_overlay_t *o);
void gr_overlay_publish(gr_overlay_t *o, const gr_view_t *v);

/* the drawing (gr2_screen.c): the page buffer is ARGB8888 little endian, premultiplied; alpha 0 = the skin shows through */
void gr_screen_set_font(const char *ttf_path);
void gr_draw(int page, const gr_view_t *v, uint8_t *buf, int w, int h, int pitch, double now);

/* where the page is on the display, for touch (gr2_overlay.c): rot = how MPC's landscape screen lies in the display, lw x lh = that screen,
 * ox, oy = the page's top-left on it, fbw x fbh = the display. 0: not known. gr_overlay_page_shown: the tab shown (0/1), or -1 when this
 * instance's live screen is not showing a page of ours. */
int gr_overlay_geom(gr_overlay_t *o, int *rot, int *lw, int *lh, int *ox, int *oy, unsigned *fbw, unsigned *fbh);
int gr_overlay_page_shown(gr_overlay_t *o);

/* touch on the waveforms (gr2_touch.c): a finger on a lane sets that layer's position; on_pos(ctx, layer, 0..1) is called from the reader thread */
typedef struct gr_touch gr_touch_t;
/* every finger: phase 2 = went down, 1 = moved, 0 = lifted; x, y are page pixels, page the tab shown */
typedef void (*gr_touch_cb)(void *ctx, int slot, int page, int x, int y, int phase);
gr_touch_t *gr_touch_start(gr_overlay_t *o, gr_touch_cb cb, void *ctx);
void gr_touch_stop(gr_touch_t *t);
