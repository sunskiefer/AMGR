/* AMGR: touch on the waveforms. The Force's touchscreen is read directly from evdev, read-only and never grabbed (MPC keeps its touch), exactly as
 * reMixXXL's src/touch.c does it (tested on the Force 3.9.1: the device is /dev/input/event0 "ILI2117 Touchscreen", multitouch type B, range 0..2048 on
 * both axes, and MPC does not grab it). A finger that goes down on a lane takes that layer's position from its x on the lane and keeps it while
 * it moves, even out of the lane sideways or up and down, until it lifts. The finger's place is turned back through the display's rotation
 * (rot 3 on the Force) into the page's pixels with the geometry the live screen found (gr_overlay_geom), so it is exact wherever MPC puts the page.
 * It only acts while this instance's live screen shows one of our tabs (gr_overlay_page_shown >= 0). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "gr2.h"

#define SLOTS 10
typedef struct { int down, tracking_id, raw_x, raw_y, moved, lane, lx, ly, lpage; } slot_t;   /* moved: 2 just went down, 1 moved, 0 nothing new */

struct gr_touch {
    gr_overlay_t *ovl;
    gr_touch_cb cb;
    void *ctx;
    int fd, mt, running, available, ax_min, ax_max, ay_min, ay_max, cur_slot;
    char dev_name[128], dev_path[32];
    long events;
    pthread_t thread;
    slot_t slot[SLOTS];
    int logged;
};

/* MPC's landscape screen (lw x lh, u right, v down) turned into the display (x, y): backwards, in continuous coordinates */
static inline void fb_to_ui_f(int rot, int lw, int lh, float x, float y, float *u, float *v) {
    switch (rot) {
    case 0: *u = x; *v = y; break;
    case 1: *u = y; *v = (float)lh - x; break;
    case 2: *u = (float)lw - x; *v = (float)lh - y; break;
    default: *u = (float)lw - y; *v = x; break;
    }
}

/* the finger's place on the page; 0 when the page's place on the display is not known yet */
static int to_page(gr_touch_t *t, slot_t *s, int *px, int *py) {
    int rot, lw, lh, pox, poy;
    unsigned fbw, fbh;
    if (!gr_overlay_geom(t->ovl, &rot, &lw, &lh, &pox, &poy, &fbw, &fbh) || t->ax_max <= t->ax_min || t->ay_max <= t->ay_min) return 0;
    float fx = (float)(s->raw_x - t->ax_min) / (float)(t->ax_max - t->ax_min) * (float)fbw;
    float fy = (float)(s->raw_y - t->ay_min) / (float)(t->ay_max - t->ay_min) * (float)fbh;
    float u, v;
    fb_to_ui_f(rot, lw, lh, fx, fy, &u, &v);
    *px = (int)floorf(u - (float)pox);
    *py = (int)floorf(v - (float)poy);
    return 1;
}

static void sync_report(gr_touch_t *t) {
    int page = gr_overlay_page_shown(t->ovl);
    for (int i = 0; i < SLOTS; i++) {
        slot_t *s = &t->slot[i];
        if (s->down && s->tracking_id < 0) {
            s->down = 0;
            if (s->lane >= 0) t->cb(t->ctx, i, s->lpage, s->lx, s->ly, 0);
            s->lane = -1;
            continue;
        }
        if (!s->down || !s->moved) continue;
        int px, py;
        if (page < 0 || !to_page(t, s, &px, &py)) { s->moved = 0; continue; }
        if (s->moved == 2) {   /* just went down */
            s->lane = 1;       /* reported: its lift is reported too */
            if (t->logged < 60) { t->logged++; gr_log("touch: slot %d DOWN raw (%d,%d) page (%d,%d) tab %d", i, s->raw_x, s->raw_y, px, py, page); }
            t->cb(t->ctx, i, page, px, py, 2);
        } else if (s->lane >= 0) t->cb(t->ctx, i, page, px, py, 1);
        s->lx = px; s->ly = py; s->lpage = page;
        s->moved = 0;
    }
}

static void handle_event(gr_touch_t *t, const struct input_event *ev) {
    t->events++;
    if (ev->type == EV_SYN) { if (ev->code == SYN_REPORT) sync_report(t); return; }
    if (ev->type == EV_ABS) {
        if (t->mt) {
            if (ev->code == ABS_MT_SLOT) { t->cur_slot = ev->value >= 0 && ev->value < SLOTS ? ev->value : SLOTS - 1; return; }
            slot_t *s = &t->slot[t->cur_slot];
            if (ev->code == ABS_MT_TRACKING_ID) {
                if (ev->value >= 0 && !s->down) { s->down = 1; s->tracking_id = ev->value; s->moved = 2; s->lane = -1; }
                else if (ev->value < 0) s->tracking_id = -1;      /* up: finished in sync_report */
                return;
            }
            if (ev->code == ABS_MT_POSITION_X) { s->raw_x = ev->value; if (s->moved != 2) s->moved = 1; }
            else if (ev->code == ABS_MT_POSITION_Y) { s->raw_y = ev->value; if (s->moved != 2) s->moved = 1; }
        } else {
            slot_t *s = &t->slot[0];
            if (ev->code == ABS_X) { s->raw_x = ev->value; if (s->moved != 2) s->moved = 1; }
            else if (ev->code == ABS_Y) { s->raw_y = ev->value; if (s->moved != 2) s->moved = 1; }
        }
        return;
    }
    if (ev->type == EV_KEY && ev->code == BTN_TOUCH && !t->mt) {
        slot_t *s = &t->slot[0];
        if (ev->value && !s->down) { s->down = 1; s->tracking_id = 1; s->moved = 2; s->lane = -1; }
        else if (!ev->value && s->down) s->tracking_id = -1;
    }
}

static void *reader_main(void *arg) {
    gr_touch_t *t = arg;
    struct input_event evs[64];
    while (t->running) {
        struct pollfd p = { t->fd, POLLIN, 0 };
        int r = poll(&p, 1, 200);
        if (r < 0) { if (errno == EINTR) continue; gr_log("touch: poll: %s", strerror(errno)); break; }
        if (r == 0) continue;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) { gr_log("touch: device went away"); break; }
        ssize_t n = read(t->fd, evs, sizeof evs);
        if (n < 0) { if (errno == EAGAIN || errno == EINTR) continue; gr_log("touch: read: %s", strerror(errno)); break; }
        for (int i = 0; i < (int)(n / sizeof evs[0]); i++) handle_event(t, &evs[i]);
    }
    t->available = 0;
    return NULL;
}

#define NBITS(x) (((x) + 7) / 8)
static int bit_set(const unsigned char *bits, int b) { return (bits[b / 8] >> (b % 8)) & 1; }

static int probe_device(gr_touch_t *t, const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { gr_log("touch: %s: %s", path, strerror(errno)); return 0; }
    char name[128] = "?";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    unsigned char ev_bits[NBITS(EV_MAX + 1)] = {0}, abs_bits[NBITS(ABS_MAX + 1)] = {0}, key_bits[NBITS(KEY_MAX + 1)] = {0};
    ioctl(fd, EVIOCGBIT(0, sizeof ev_bits), ev_bits);
    int has_abs = bit_set(ev_bits, EV_ABS), has_key = bit_set(ev_bits, EV_KEY);
    if (has_abs) ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs_bits), abs_bits);
    if (has_key) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key_bits), key_bits);
    int mt = has_abs && bit_set(abs_bits, ABS_MT_POSITION_X) && bit_set(abs_bits, ABS_MT_POSITION_Y);
    int st = has_abs && bit_set(abs_bits, ABS_X) && bit_set(abs_bits, ABS_Y) && has_key && bit_set(key_bits, BTN_TOUCH);
    struct input_absinfo ai;
    int xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    if (mt || st) {
        if (ioctl(fd, EVIOCGABS(mt ? ABS_MT_POSITION_X : ABS_X), &ai) == 0) { xmin = ai.minimum; xmax = ai.maximum; }
        if (ioctl(fd, EVIOCGABS(mt ? ABS_MT_POSITION_Y : ABS_Y), &ai) == 0) { ymin = ai.minimum; ymax = ai.maximum; }
    }
    gr_log("touch: %s \"%s\": MT=%d single-touch=%d X %d..%d Y %d..%d", path, name, mt, st, xmin, xmax, ymin, ymax);
    if (!(mt || st)) { close(fd); return 0; }
    /* the grab test: EBUSY means MPC holds it exclusively and we would never see events; released at once on success */
    if (ioctl(fd, EVIOCGRAB, (void *)1) < 0) {
        gr_log("touch: %s EVIOCGRAB test: %s", path, strerror(errno));
        if (errno == EBUSY) { close(fd); return 0; }
    } else {
        ioctl(fd, EVIOCGRAB, (void *)0);
        gr_log("touch: %s grab test ok, released (MPC keeps its touch)", path);
    }
    t->fd = fd; t->mt = mt;
    t->ax_min = xmin; t->ax_max = xmax; t->ay_min = ymin; t->ay_max = ymax;
    snprintf(t->dev_name, sizeof t->dev_name, "%s", name);
    snprintf(t->dev_path, sizeof t->dev_path, "%s", path);
    return 1;
}

gr_touch_t *gr_touch_start(gr_overlay_t *o, gr_touch_cb cb, void *ctx) {
    gr_touch_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->ovl = o; t->cb = cb; t->ctx = ctx; t->fd = -1;
    for (int i = 0; i < SLOTS; i++) { t->slot[i].tracking_id = -1; t->slot[i].lane = -1; }
    int found = 0, seen = 0;
    for (int i = 0; i < 32 && !found; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        if (access(path, F_OK) != 0) continue;
        seen++;
        found = probe_device(t, path);
    }
    if (!found) { gr_log("touch: no usable touchscreen among %d input device(s)", seen); return t; }
    t->available = 1;
    t->running = 1;
    if (pthread_create(&t->thread, NULL, reader_main, t) != 0) { gr_log("touch: cannot start the reader thread: %s", strerror(errno)); t->running = 0; t->available = 0; close(t->fd); t->fd = -1; return t; }
    gr_log("touch: reading %s \"%s\" (%s), ranges X %d..%d Y %d..%d", t->dev_path, t->dev_name, t->mt ? "multitouch" : "single-touch", t->ax_min, t->ax_max, t->ay_min, t->ay_max);
    return t;
}

void gr_touch_stop(gr_touch_t *t) {
    if (!t) return;
    if (t->running) { t->running = 0; pthread_join(t->thread, NULL); }
    if (t->fd >= 0) close(t->fd);
    gr_log("touch: stopped after %ld events", t->events);
    free(t);
}
