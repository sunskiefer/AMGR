/* AMGR engine: a four-layer granular instrument. Each layer is a sample (a .wav of the Samples folder, or a built-in test source) whose grains are
 * scattered around a position that you set by touching the waveform on the live screen. MIDI notes play all four layers at the note's pitch.
 *
 * Per layer: POS, SIZE (grain length 5 ms..1 s), DENS (grains a second per voice), PITCH (semitones), LEVEL, SPRD (scatter), PAN. Global: amp
 * ENVELOPE, a morphing state-variable FILTER, SHAPE (grain window), JITTER (timing), REVERSE (share of backwards grains), DELAY, REVERB, WIDTH.
 * 256 factory presets are generated in code (16 families x 16 variations); user presets are files in <plugin>/User, 999 slots.
 *
 * Threads: render() never allocates, locks or reads files. A worker thread loads the .wav files and builds the test sources, then swaps a pointer.
 *
 * Contract: wrapper/engine.h (44100 Hz, int16 stereo, up to 128 frames a call). The parameter order is tools/gen_amgr.py's (src/amgr_idx.h). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#include "wrapper/engine.h"
#include "params.h"
#include "gr2.h"
#include <sys/stat.h>
#include <sys/types.h>

/* ------------------------------------------------------------------------------------------------ the log */
static char g_log_path[GR_PATH];
void gr_log(const char *fmt, ...) {
    if (!g_log_path[0]) return;
    FILE *f = fopen(g_log_path, "a");
    if (!f) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "[%6ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ------------------------------------------------------------------------------------------------ the waveforms */
static gr_wave_t g_wave[GR_LAYERS];
static pthread_mutex_t g_wave_lock[GR_LAYERS] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};

void gr_wave_copy(int layer, gr_wave_t *dst) {
    if (layer < 0 || layer >= GR_LAYERS) { memset(dst, 0, sizeof *dst); return; }
    pthread_mutex_lock(&g_wave_lock[layer]);
    memcpy(dst, &g_wave[layer], sizeof *dst);
    pthread_mutex_unlock(&g_wave_lock[layer]);
}

static void wave_store(int layer, const gr_wave_t *w) {
    pthread_mutex_lock(&g_wave_lock[layer]);
    memcpy(&g_wave[layer], w, sizeof *w);
    pthread_mutex_unlock(&g_wave_lock[layer]);
}

/* a deterministic pseudo-random number 0..1 from an integer (so a test wave is the same every time) */
static float hash01(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return (x & 0xffffff) / 16777215.0f; }

/* ------------------------------------------------------------------------------------------------ sources */
#define SR 44100.0f
#define NVOICE 16                       /* as the GR-2: 16 voices */
#define NGRAIN 4000                     /* as the GR-2: up to 4000 grains in all */
#define VGRAIN 128                      /* as the GR-2: 128 grains a voice */
#define MAXBLK 128
#define MAX_SECONDS 300                 /* a layer holds up to 5 minutes */
#define MAXENT 1000
#define NFACTORY 256
#define NUSER 999

typedef struct { float *d; int n; double retired_at; int test; } audio_t;

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

/* built-in test sources, one per layer, ~6 s each, so the instrument sounds (and shows waves) with no files at all:
 * 1 drum loop, 2 pad, 3 rising sweep, 4 bells */
static audio_t *make_test_audio(int layer) {
    int n = (int)(6.0f * SR);
    audio_t *a = calloc(1, sizeof *a);
    float *d = calloc((size_t)n, sizeof(float));
    if (!a || !d) { free(a); free(d); return NULL; }
    uint32_t rs = 12345u + (uint32_t)layer * 777u;
    float lp = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR, v = 0;
        switch (layer) {
        case 0: {   /* a kick on every half second, a snare-ish noise burst on the off beats, closed hats between */
            float tb = fmodf(t, 0.5f);
            int beat = (int)(t / 0.5f);
            rs = rs * 1664525u + 1013904223u;
            float nz = ((rs >> 8) & 0xffff) / 32768.0f - 1.0f;
            v += sinf(6.2831853f * (50.0f * tb + 90.0f * (1.0f - expf(-18.0f * tb)) / 18.0f * 1.0f)) * expf(-9.0f * tb) * 0.9f;
            if (beat & 1) v += nz * expf(-16.0f * tb) * 0.6f;
            float th = fmodf(t + 0.25f, 0.25f);
            lp += 0.6f * (nz - lp);
            v += (nz - lp) * expf(-60.0f * th) * 0.25f;
            break; }
        case 1: {   /* a pad: detuned saws on A2 E3 A3 C4 with a slow swell */
            static const float f[4] = {110.0f, 164.81f, 220.0f, 261.63f};
            for (int k = 0; k < 4; k++)
                for (int dt = -1; dt <= 1; dt++) {
                    float fr = f[k] * (1.0f + dt * 0.004f), ph = fmodf(t * fr, 1.0f);
                    v += (2.0f * ph - 1.0f) * 0.07f;
                }
            lp += 0.12f * (v - lp); v = lp * 2.6f;
            float sw = 0.5f - 0.5f * cosf(6.2831853f * t / 6.0f);
            v *= 0.25f + 0.75f * sw;
            break; }
        case 2: {   /* a rising sweep with a few harmonics */
            float u = t / 6.0f, fr = 90.0f * powf(24.0f, u), ph = 6.2831853f * (90.0f * 6.0f / logf(24.0f) * (powf(24.0f, u) - 1.0f));
            (void)fr;
            v = sinf(ph) * 0.5f + sinf(2 * ph) * 0.2f + sinf(3 * ph) * 0.1f;
            v *= 0.2f + 0.8f * u;
            break; }
        default: {  /* bells: FM strikes */
            static const float st[3] = {0.3f, 2.1f, 3.9f}, fc[3] = {440.0f, 659.25f, 523.25f};
            for (int k = 0; k < 3; k++) {
                float tt = t - st[k];
                if (tt < 0) continue;
                float mod = sinf(6.2831853f * fc[k] * 3.5f * tt) * 2.2f * expf(-4.0f * tt);
                v += sinf(6.2831853f * fc[k] * tt + mod) * expf(-1.6f * tt) * 0.5f;
            }
        }
        }
        d[i] = v;
    }
    float pk = 0;
    for (int i = 0; i < n; i++) if (fabsf(d[i]) > pk) pk = fabsf(d[i]);
    if (pk > 0.0001f) for (int i = 0; i < n; i++) d[i] *= 0.9f / pk;
    /* a short fade at the ends so a looping read does not click */
    for (int i = 0; i < 220; i++) { float g = (float)i / 220.0f; d[i] *= g; d[n - 1 - i] *= g; }
    a->d = d; a->n = n; a->test = 1;
    return a;
}

/* the waveform columns and the display data of an audio buffer */
static void make_wave(const audio_t *a, const char *name, int is_test, gr_wave_t *w) {
    memset(w, 0, sizeof *w);
    float pk = 0.0001f;
    for (int i = 0; i < a->n; i += 7) if (fabsf(a->d[i]) > pk) pk = fabsf(a->d[i]);
    float sc = 30000.0f / (pk < 0.02f ? 0.02f : pk);
    unsigned char seen[GR_COLS];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < a->n; i++) {
        int col = (int)((long long)i * GR_COLS / a->n);
        float v = a->d[i] * sc;
        if (v > 32767) v = 32767;
        if (v < -32767) v = -32767;
        int16_t s = (int16_t)v;
        if (!seen[col]) { seen[col] = 1; w->mn[col] = w->mx[col] = s; }
        else { if (s < w->mn[col]) w->mn[col] = s; if (s > w->mx[col]) w->mx[col] = s; }
    }
    w->state = 2; w->is_test = is_test; w->seconds = (float)a->n / SR;
    snprintf(w->name, sizeof w->name, "%s", name);
    for (char *p = w->name; *p; p++) if ((unsigned char)*p < 32 || (unsigned char)*p > 126) *p = '?';
}

static int g_load_pct[GR_LAYERS] = {-1, -1, -1, -1};

static long mem_available_kb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[128];
    long kb = -1;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb;
}

/* a .wav or .flac -> mono 44.1 kHz float; at most MAX_SECONDS (*cut = 1 when the file was longer). *err: 1 not readable, 2 not enough memory.
 * The layer's loading percentage is kept in g_load_pct[L] for the screen. */
static audio_t *load_audio_file(const char *path, int L, int *cut, int *err) {
    drwav wav;
    drflac *fl = NULL;
    int is_flac = 0;
    size_t pl = strlen(path);
    *cut = 0; *err = 1;
    if (pl > 5 && !strcasecmp(path + pl - 5, ".flac")) { fl = drflac_open_file(path, NULL); if (!fl) return NULL; is_flac = 1; }
    else if (!drwav_init_file(&wav, path, NULL)) return NULL;
    unsigned ch = is_flac ? fl->channels : wav.channels;
    double rate = is_flac ? fl->sampleRate : wav.sampleRate;
    unsigned long long total = is_flac ? fl->totalPCMFrameCount : wav.totalPCMFrameCount;
    if (!ch) ch = 1;
    if (rate < 1) rate = 44100;
#define CLOSE_SRC() do { if (is_flac) drflac_close(fl); else drwav_uninit(&wav); } while (0)
    if (!total || ch > 16) { CLOSE_SRC(); return NULL; }
    unsigned long long maxin = (unsigned long long)(MAX_SECONDS * rate);
    if (total > maxin) { total = maxin; *cut = 1; }
    /* memory: the read buffer plus (when the rate differs) the resampled copy, and 150 MB left for MPC */
    double need_mb = (double)total * 4.0 / 1048576.0 * (fabs(rate - SR) < 1.0 ? 1.0 : 1.0 + SR / rate);
    long avail = mem_available_kb();
    if (avail >= 0 && need_mb + 150.0 > (double)avail / 1024.0) { CLOSE_SRC(); *err = 2; gr_log("%s: needs %.0f MB, %ld MB free", path, need_mb, avail / 1024); return NULL; }
    float *mono = malloc(sizeof(float) * (size_t)total);
    float *buf = malloc(sizeof(float) * 4096 * ch);
    if (!mono || !buf) { free(mono); free(buf); CLOSE_SRC(); *err = 2; return NULL; }
    unsigned long long done = 0;
    g_load_pct[L] = 0;
    while (done < total) {
        unsigned long long want = total - done < 4096 ? total - done : 4096;
        unsigned long long got = is_flac ? drflac_read_pcm_frames_f32(fl, want, buf) : drwav_read_pcm_frames_f32(&wav, want, buf);
        if (!got) break;
        for (unsigned long long i = 0; i < got; i++) {
            float sm = 0;
            for (unsigned c = 0; c < ch; c++) sm += buf[i * ch + c];
            mono[done + i] = sm / (float)ch;
        }
        done += got;
        g_load_pct[L] = (int)(done * 100 / total);
    }
    free(buf);
    CLOSE_SRC();
    if (done < 64) { free(mono); return NULL; }
    audio_t *a = calloc(1, sizeof *a);
    if (!a) { free(mono); *err = 2; return NULL; }
    if (fabs(rate - SR) < 1.0) { a->d = mono; a->n = (int)done; }
    else {   /* linear resample to 44.1 kHz */
        int n2 = (int)((double)done * SR / rate);
        float *o = malloc(sizeof(float) * (size_t)(n2 > 0 ? n2 : 1));
        if (!o || n2 < 64) { free(o); free(mono); free(a); *err = 2; return NULL; }
        double step = rate / SR;
        for (int i = 0; i < n2; i++) {
            double pp = i * step;
            unsigned long long i0 = (unsigned long long)pp;
            float f = (float)(pp - (double)i0);
            float s0 = mono[i0 < done ? i0 : done - 1], s1 = mono[i0 + 1 < done ? i0 + 1 : done - 1];
            o[i] = s0 + (s1 - s0) * f;
        }
        free(mono);
        a->d = o; a->n = n2;
    }
    *err = 0;
    return a;
}

static int cmp_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* ------------------------------------------------------------------------------------------------ the instance */
typedef struct { const float *d; int n; float pos, inc, wpos, wstep, gl, gr; int delay, len, age, layer, voice, active; } grain_t;
typedef struct { int active, note, gate, held; int stage[GR_LAYERS]; float vel, env, lenv[GR_LAYERS], rel_from[GR_LAYERS]; uint32_t order; float cnt[GR_LAYERS]; } voice_t;
typedef struct { char name[60]; unsigned char is_dir; } ent_t;
typedef struct { char box[GR_LAYERS][40], preset[56], ui_title[72], ui_row[GR_ROWS][56], lmsg[GR_LAYERS][40]; int ui_sel; } uitext_t;

typedef struct {
    char plugin_dir[GR_PATH], samples_dir[GR_PATH], user_dir[GR_PATH];
    float val[NP];
    /* the layers' audio and the worker that loads it */
    audio_t *aud[GR_LAYERS], *test[GR_LAYERS];
    audio_t *retired[96];
    int nretired;
    pthread_mutex_t names_lock;      /* guards the browser's lists and the texts the screen shows */
    char dir[GR_LAYERS][GR_PATH];    /* each sample box's folder (remembered in amgr_last.txt) */
    char file[GR_LAYERS][64];        /* the file each layer holds ("" = its test source) */
    ent_t *ents[GR_LAYERS];
    int nents[GR_LAYERS], sel[GR_LAYERS];
    double pend_at[GR_LAYERS];       /* a file picked by scrolling loads when the scrolling has stopped: when, 0 = nothing pending */
    char req_path[GR_LAYERS][GR_PATH * 2], req_name[GR_LAYERS][64];
    int ui_open, ui_layer, ui_bank, cur_bank, cur_user;
    unsigned char user_has[NUSER + 1];
    uitext_t ui;
    double lmsg_until[GR_LAYERS];
    int grab[12];
    pthread_t loader;
    pthread_mutex_t lk;
    pthread_cond_t cv;
    int req[GR_LAYERS], loader_started, stop;
    /* screen and touch */
    gr_overlay_t *ovl;
    gr_touch_t *touch;
    pthread_mutex_t pub_lock;
    uint64_t dirty[2];
    int gnext;
    int fidx;
    float gov, load;   /* the load governor: 1 = every grain wanted; lower = thinned so the block is done in time; the smoothed share of a block's time used */
    float envb[NVOICE][GR_LAYERS][MAXBLK];
    char status[72], base_status[72];
    int msg_left;
    float act[GR_LAYERS][GR_ACT];
    int nvoices_on, pub_count;
    /* sound */
    voice_t v[NVOICE];
    grain_t g[NGRAIN];
    uint32_t rng, order;
    float win[513];
    float bend, mod;
    int sustain;
    float fi1[2], fi2[2];           /* filter state */
    float fcut;                     /* smoothed cutoff, Hz */
    float *dl[2];                   /* delay lines */
    int dw;
    float dcur, dlp[2];
    float *comb[2][8], *ap[2][4];
    int ci[2][8], ai[2][4], clen[2][8], alen[2][4];
    float cf[2][8];
} inst_t;

static const int COMB_T[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const int AP_T[4] = {556, 441, 341, 225};
#define DMAX 66200

static float rnd01(inst_t *e) { e->rng ^= e->rng << 13; e->rng ^= e->rng >> 17; e->rng ^= e->rng << 5; return (e->rng & 0xffffff) / 16777216.0f; }
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* ---- value mappings (shared by the sound and the readouts) */
static float size_ms(float v) { return 5.0f * powf(200.0f, v); }
static float dens_gps(float v) { return 0.1f * powf(10000.0f, v); }   /* 0.1 .. 1000 grains a second */
static float env_s(float v) { return 0.001f * powf(4000.0f, v); }
static float rel_s(float v) { return 0.005f * powf(1200.0f, v); }
static float cut_hz(float v) { return 20.0f * powf(900.0f, v); }
static float dly_ms(float v) { return 20.0f * powf(60.0f, v); }

static void fmt_time(char *b, int n, float s) {
    if (s < 1.0f) snprintf(b, (size_t)n, "%dms", (int)(s * 1000.0f + 0.5f));
    else snprintf(b, (size_t)n, "%.1fs", s);
}

static int cmp_ent(const void *a, const void *b) {
    const ent_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return (int)y->is_dir - (int)x->is_dir;
    return strcasecmp(x->name, y->name);
}
static int is_audio_name(const char *n) {
    size_t l = strlen(n);
    return (l > 4 && !strcasecmp(n + l - 4, ".wav")) || (l > 5 && !strcasecmp(n + l - 5, ".flac"));
}
/* the folder of layer L: ".." first (unless at "/"), then the folders, then the .wav / .flac files, by name. Call with names_lock held. */
static void scan_dir(void *ep, int L);
static void strip_ext(char *n) { char *d = strrchr(n, '.'); if (d && d != n) *d = 0; }
/* the text of a parameter's value, as the knob shows it (at most about 9 characters: the knob's cell is narrow) */
static void fmt_value(inst_t *e, int i, float v, char *b, int n) {
    switch (AM_KIND[i]) {
    case K_FACT: snprintf(b, (size_t)n, "F%03d", (int)(v + 0.5f) + 1); break;
    case K_USER: snprintf(b, (size_t)n, "U%03d", (int)(v + 0.5f)); break;
    case K_SAVE: snprintf(b, (size_t)n, "SAVE"); break;
    case K_BEND: snprintf(b, (size_t)n, "%dst", (int)(v + 0.5f)); break;
    case K_WAV: {
        int L = i - P_WAV1, k = (int)(v + 0.5f);
        b[0] = 0;
        if (pthread_mutex_trylock(&e->names_lock) == 0) {
            if (k >= 0 && k < e->nents[L]) snprintf(b, (size_t)n, "%.10s", e->ents[L][k].name);
            pthread_mutex_unlock(&e->names_lock);
        }
        if (!b[0]) snprintf(b, (size_t)n, "-");
        break; }
    case K_ENVL: snprintf(b, (size_t)n, "LAYER %d", (int)(v + 0.5f)); break;
    case K_POS: case K_PCT: snprintf(b, (size_t)n, "%d%%", (int)(v * 100.0f + 0.5f)); break;
    case K_SIZE: fmt_time(b, n, size_ms(v) * 0.001f); break;
    case K_DENS: { float g = dens_gps(v); if (g < 10) snprintf(b, (size_t)n, "%.1f/s", g); else snprintf(b, (size_t)n, "%d/s", (int)(g + 0.5f)); break; }
    case K_ST: snprintf(b, (size_t)n, "%+dst", (int)(v + (v < 0 ? -0.5f : 0.5f))); break;
    case K_PAN: { int p = (int)((v - 0.5f) * 200.0f + (v < 0.5f ? -0.5f : 0.5f)); if (p < -2) snprintf(b, (size_t)n, "L%d", -p); else if (p > 2) snprintf(b, (size_t)n, "R%d", p); else snprintf(b, (size_t)n, "C"); break; }
    case K_ENV: fmt_time(b, n, env_s(v)); break;
    case K_REL: fmt_time(b, n, rel_s(v)); break;
    case K_CUT: { float h = cut_hz(v); if (h < 1000) snprintf(b, (size_t)n, "%dHz", (int)(h + 0.5f)); else snprintf(b, (size_t)n, "%.1fkHz", h / 1000.0f); break; }
    case K_FTYPE: snprintf(b, (size_t)n, v < 0.25f ? "LP" : v < 0.75f ? "BP" : "HP"); break;
    case K_DTIME: fmt_time(b, n, dly_ms(v) * 0.001f); break;
    default: snprintf(b, (size_t)n, "%.2f", v);
    }
}

/* ------------------------------------------------------------------------------------------------ presets */
/* 16 families x 16 variations = 256 factory presets, made by rule from a table, so they are always the same:
 * size, dens, sprd, pitch x4, level x4, atk, dec, sus, rel, cut, res, ftype, drive, shape, jitt, rev, dtime, dfb, dmix, vsize, vmix */
#define NFAM_V 27
static const char *FAMILY[16] = {"Frozen Glass", "Cloud Bed", "Shimmer Grain", "Tape Smear", "Freeze Pad", "Granular Rain", "Scan Drone", "Pulse Cloud",
                                 "Reverse Swell", "Stutter Kit", "Texture Bed", "Glitch Dust", "Bell Cloud", "Sub Bloom", "Wide Swarm", "Rhythm Grains"};
static const char *VARIANT[16] = {"Soft", "Wide", "Dark", "Bright", "Slow", "Fast", "Deep", "Thin", "Warm", "Cold", "Dense", "Sparse", "Long", "Short", "Odd", "Bold"};
static const float FAMV[16][NFAM_V] = {
    {.35f, .75f, .12f, 0, 12, 19, 24, .8f, .6f, .4f, .3f, .45f, .5f, .9f, .65f, .85f, .10f, 0, 0, .5f, .3f, 0, .55f, .45f, .2f, .8f, .35f},
    {.55f, .55f, .35f, 0, 0, -12, 7, .8f, .7f, .5f, .4f, .6f, .6f, .9f, .75f, .7f, .10f, 0, .05f, .35f, .5f, .1f, .6f, .4f, .15f, .75f, .3f},
    {.30f, .80f, .20f, 0, 12, 24, 19, .7f, .6f, .5f, .45f, .35f, .5f, .85f, .7f, .95f, .15f, 0, 0, .55f, .4f, .2f, .5f, .5f, .25f, .85f, .45f},
    {.60f, .45f, .08f, 0, -12, 0, 5, .9f, .5f, .4f, .3f, .1f, .4f, .85f, .5f, .55f, .2f, 0, .35f, .4f, .2f, 0, .7f, .55f, .3f, .4f, .15f},
    {.50f, .70f, .05f, 0, 7, 12, -12, .8f, .6f, .5f, .5f, .7f, .5f, .95f, .8f, .65f, .12f, 0, 0, .3f, .6f, 0, .4f, .3f, .1f, .9f, .4f},
    {.15f, .90f, .60f, 0, 5, 12, 17, .6f, .6f, .5f, .5f, .05f, .3f, .7f, .4f, .9f, .1f, 0, 0, .6f, .8f, .15f, .45f, .4f, .2f, .7f, .3f},
    {.65f, .60f, .02f, -12, 0, 7, -24, .8f, .7f, .5f, .6f, .8f, .6f, 1.0f, .85f, .5f, .25f, 0, .1f, .2f, .2f, 0, .65f, .5f, .2f, .85f, .35f},
    {.20f, .50f, .10f, 0, 0, 12, -12, .9f, .5f, .4f, .4f, .02f, .3f, .6f, .35f, .8f, .2f, 0, .15f, .7f, .05f, 0, .3f, .4f, .2f, .5f, .2f},
    {.50f, .65f, .15f, 0, 12, 0, -12, .8f, .6f, .5f, .4f, .75f, .5f, .9f, .7f, .75f, .1f, 0, 0, .4f, .35f, .9f, .55f, .45f, .2f, .8f, .35f},
    {.10f, .40f, .03f, 0, 0, -5, 7, .9f, .7f, .6f, .5f, 0, .25f, .5f, .2f, .9f, .15f, 0, .2f, .75f, 0, 0, .2f, .3f, .1f, .3f, .1f},
    {.40f, .70f, .45f, 0, -7, 12, -12, .7f, .7f, .6f, .5f, .4f, .5f, .85f, .6f, .6f, .2f, 0, .1f, .45f, .6f, .25f, .5f, .4f, .15f, .7f, .25f},
    {.08f, .85f, .80f, 0, 12, -12, 24, .6f, .5f, .5f, .4f, 0, .2f, .5f, .25f, .95f, .2f, 0, .25f, .8f, .9f, .3f, .25f, .45f, .25f, .4f, .15f},
    {.40f, .40f, .10f, 0, 12, 19, 7, .8f, .7f, .5f, .5f, .05f, .6f, .5f, .75f, .9f, .1f, 0, 0, .5f, .3f, 0, .55f, .5f, .2f, .8f, .4f},
    {.55f, .55f, .05f, -24, -12, 0, -17, .9f, .7f, .4f, .5f, .5f, .5f, .9f, .7f, .35f, .25f, 0, .3f, .3f, .2f, 0, .6f, .3f, .1f, .6f, .2f},
    {.30f, .95f, .55f, 0, 7, -5, 12, .7f, .7f, .7f, .7f, .3f, .5f, .8f, .5f, .8f, .1f, 0, .1f, .5f, .7f, .1f, .5f, .4f, .2f, .75f, .3f},
    {.12f, .55f, .05f, 0, 0, 7, 12, .9f, .6f, .5f, .4f, 0, .3f, .6f, .3f, .85f, .2f, 0, .1f, .65f, .1f, .05f, .4f, .35f, .2f, .45f, .15f},
};

static float h01(uint32_t x) { return hash01(x); }

/* fills v[] (indexed like val[]) with the sound parameters of factory preset n */
static void factory_fill(int n, float *v) {
    int f = (n >> 4) & 15, k = n & 15;
    const float *b = FAMV[f];
    float size = b[0], dens = b[1], sprd = b[2], pit[4] = {b[3], b[4], b[5], b[6]}, lvl[4] = {b[7], b[8], b[9], b[10]};
    float atk = b[11], dec = b[12], sus = b[13], rel = b[14], cut = b[15], res = b[16], ft = b[17], drive = b[18], shape = b[19], jitt = b[20],
          rev = b[21], dtime = b[22], dfb = b[23], dmix = b[24], vsize = b[25], vmix = b[26], vdamp = 0.4f, width = 0.8f, panw = 0.7f;
    switch (k) {
    case 0: atk += .15f; cut *= .8f; shape -= .1f; break;
    case 1: sprd += .2f; panw = 1.0f; width = 1.0f; vmix += .1f; break;
    case 2: cut *= .55f; res += .1f; pit[2] -= 12; pit[3] -= 12; vdamp = .7f; break;
    case 3: cut = 1.0f; lvl[2] *= 1.2f; lvl[3] *= 1.2f; pit[3] += 12; vdamp = .2f; break;
    case 4: dens *= .6f; size += .12f; atk += .15f; break;
    case 5: dens += .15f; size -= .1f; atk *= .3f; break;
    case 6: pit[0] -= 12; pit[1] -= 12; cut *= .8f; break;
    case 7: ft = .9f; cut = .45f; lvl[0] *= .8f; break;
    case 8: drive += .35f; cut *= .7f; ft = 0; vdamp = .6f; break;
    case 9: vmix += .15f; vsize += .1f; pit[2] += 12; break;
    case 10: dens += .25f; size -= .05f; break;
    case 11: dens *= .5f; jitt += .2f; break;
    case 12: rel += .2f; dfb += .2f; dmix += .1f; vsize += .1f; break;
    case 13: rel *= .45f; dmix *= .5f; vmix *= .5f; break;
    case 14: pit[0] = 0; pit[1] = 6; pit[2] = 10; pit[3] = -7; sprd += .1f; jitt += .3f; break;
    default: for (int i = 0; i < 4; i++) lvl[i] = clampf(lvl[i] * 1.15f, 0, 1); drive += .2f; sus = 1.0f; break;
    }
    /* every preset then gets its own variation on every parameter (from its number, so it is always the same): the family and the variation set the
     * character, the numbers below move each parameter around it. Levels, drive, feedback, mixes and the master stay in a safe range. */
#define VR(key, amt) ((h01((uint32_t)n * 2246822519u + (uint32_t)(key) * 3266489917u + 97u) - 0.5f) * 2.0f * (amt))
    {
        static const float IV[10] = {-12, -7, -5, 0, 5, 7, 12, 12, 19, 24};
        for (int i = 0; i < 4; i++) {
            float r = h01((uint32_t)n * 668265263u + (uint32_t)i * 374761393u + 5u);
            if (r < 0.55f) pit[i] = IV[(int)(h01((uint32_t)n * 3266489917u + (uint32_t)i * 2246822519u + 9u) * 10.0f) % 10];   /* a new interval for the layer */
            else if (r < 0.75f) pit[i] += (h01((uint32_t)n * 97u + (uint32_t)i * 31u) < 0.5f) ? 12.0f : -12.0f;
            lvl[i] = clampf(lvl[i] + VR(40 + i, 0.18f), 0.45f, 0.85f);
        }
        pit[(n >> 2) & 3] = 0;   /* one layer always sits on the note */
        size = clampf(size + VR(1, 0.22f), 0.03f, 0.95f);
        dens = clampf(dens + VR(2, 0.25f), 0.05f, 0.98f);
        sprd = clampf(sprd + VR(3, 0.22f), 0.0f, 0.95f);
        atk = clampf(atk + VR(4, 0.22f), 0.0f, 0.85f);
        dec = clampf(dec + VR(5, 0.25f), 0.1f, 0.95f);
        sus = clampf(sus + VR(6, 0.25f), 0.3f, 1.0f);
        rel = clampf(rel + VR(7, 0.25f), 0.1f, 0.95f);
        cut = clampf(cut + VR(8, 0.3f), 0.2f, 1.0f);
        res = clampf(res + VR(9, 0.25f), 0.0f, 0.7f);
        ft = clampf(ft + VR(10, 0.35f), 0.0f, 1.0f);
        if (ft > 0.6f && cut > 0.6f) cut = 0.6f;   /* a high-pass with the cutoff right up is silence */
        drive = clampf(drive + VR(11, 0.2f), 0.0f, 0.45f);
        shape = clampf(shape + VR(12, 0.3f), 0.05f, 1.0f);
        jitt = clampf(jitt + 0.12f + VR(13, 0.3f), 0.05f, 0.9f);   /* always some speed scatter */
        rev = clampf(rev + 0.2f + VR(14, 0.2f), 0.0f, 0.6f);      /* and some grains going backwards */
        dtime = clampf(dtime + VR(15, 0.35f), 0.05f, 0.95f);
        dfb = clampf(dfb + VR(16, 0.25f), 0.05f, 0.7f);
        dmix = clampf(dmix + VR(17, 0.2f), 0.0f, 0.5f);
        vsize = clampf(vsize + VR(18, 0.25f), 0.2f, 0.95f);
        vmix = clampf(vmix + VR(19, 0.2f), 0.0f, 0.5f);
        vdamp = clampf(vdamp + VR(20, 0.3f), 0.05f, 0.95f);
        width = clampf(width + VR(21, 0.25f), 0.4f, 1.0f);
        panw = clampf(panw + VR(22, 0.35f), 0.2f, 1.0f);
    }
    static const float pan0[4] = {0.30f, 0.70f, 0.15f, 0.85f};
    for (int i = 0; i < GR_LAYERS; i++) {
        uint32_t hs = (uint32_t)n * 8u + (uint32_t)i;
        float pos = 0.10f + 0.22f * i + (h01(hs * 2654435761u + 11) - 0.5f) * 0.30f;
        v[P_POS1 + i] = clampf(pos, 0.02f, 0.98f);
        v[P_SIZE1 + i] = clampf(size + VR(50 + i, 0.15f), 0.02f, 1);
        v[P_DENS1 + i] = clampf(((dens + (h01(hs * 977u + 3) - 0.5f) * 0.10f) * 1.7782f + 1.0f) * 0.25f, 0, 1);   /* the table is on the old 1..60/s scale */
        v[P_PITCH1 + i] = clampf(pit[i], -24, 24);
        v[P_LEVEL1 + i] = clampf(lvl[i], 0, 1);
        v[P_SPRD1 + i] = clampf(sprd + VR(80 + i, 0.1f), 0, 1);
        v[P_PAN1 + i] = clampf(0.5f + (pan0[i] - 0.5f) * (panw / 0.7f) + VR(30 + i, 0.08f), 0, 1);
    }
    for (int i = 0; i < GR_LAYERS; i++) {   /* each layer its own envelope: the later layers swell in and ring out a little longer */
        v[P_ATK1 + i] = clampf(atk + 0.04f * i + VR(60 + i, 0.1f), 0, 1); v[P_DEC1 + i] = clampf(dec + VR(64 + i, 0.1f), 0, 1);
        v[P_SUS1 + i] = clampf(sus + VR(68 + i, 0.1f), 0.2f, 1); v[P_REL1 + i] = clampf(rel + 0.03f * i + VR(72 + i, 0.1f), 0, 1);
    }
    v[P_CUT] = clampf(cut, 0, 1); v[P_RES] = clampf(res, 0, 1); v[P_FTYPE] = clampf(ft, 0, 1); v[P_DRIVE] = clampf(drive, 0, 1);
    v[P_VOL] = 0.8f;   /* the master is always the same safe level */ v[P_SHAPE] = clampf(shape, 0, 1); v[P_JITT] = clampf(jitt, 0, 1); v[P_REV] = clampf(rev, 0, 1);
    v[P_DTIME] = clampf(dtime, 0, 1); v[P_DFB] = clampf(dfb, 0, 1); v[P_DTONE] = clampf(0.6f + VR(23, 0.35f), 0.15f, 0.95f); v[P_DMIX] = clampf(dmix, 0, 1);
    v[P_VSIZE] = clampf(vsize, 0, 1); v[P_VDAMP] = vdamp; v[P_VMIX] = clampf(vmix, 0, 1); v[P_WIDTH] = width;
}
static void factory_name(int n, char *b, int len) { snprintf(b, (size_t)len, "%s %s", VARIANT[n & 15], FAMILY[(n >> 4) & 15]); }

/* the sound parameters: the ones a preset holds (not the preset/user/save/bend selectors and not the sample choice) */
#define SOUND_FIRST P_POS1
#define SOUND_LAST P_WIDTH

/* ------------------------------------------------------------------------------------------------ the screen's view */
static void dirty_set(void *ep, int i);
static void publish(inst_t *e) {
    gr_view_t v;
    memset(&v, 0, sizeof v);
    double tn = now_s();
    for (int L = 0; L < GR_LAYERS; L++) {
        audio_t *a = __atomic_load_n(&e->aud[L], __ATOMIC_ACQUIRE);
        float dur = a ? (float)a->n / SR : 1.0f;
        float sms = size_ms(e->val[P_SIZE1 + L]);
        v.pos[L] = e->val[P_POS1 + L];
        v.size_ms[L] = sms;
        v.dur[L] = dur;
        v.winw[L] = clampf(sms * 0.001f * powf(2.0f, e->val[P_PITCH1 + L] / 12.0f) / dur, 0, 1);
        v.sprd[L] = e->val[P_SPRD1 + L];
        v.dens[L] = dens_gps(e->val[P_DENS1 + L]);
        v.pitch[L] = (int)(e->val[P_PITCH1 + L] + (e->val[P_PITCH1 + L] < 0 ? -0.5f : 0.5f));
        for (int g = 0; g < GR_ACT; g++) v.act[L][g] = e->act[L][g];
        v.load_pct[L] = g_load_pct[L];
        if (tn < e->lmsg_until[L]) memcpy(v.lmsg[L], e->ui.lmsg[L], sizeof v.lmsg[L]);
        memcpy(v.box[L], e->ui.box[L], sizeof v.box[L]);
        v.box[L][sizeof v.box[L] - 1] = 0; v.lmsg[L][sizeof v.lmsg[L] - 1] = 0;
    }
    v.voices = e->nvoices_on;
    snprintf(v.status, sizeof v.status, "%s", e->status);
    snprintf(v.preset, sizeof v.preset, "%s", e->status);   /* the preset box shows the preset, or a message for a moment */
    v.bend = (int)(e->val[P_BEND] + 0.5f);
    v.env_layer = (int)(e->val[P_ENVL] + 0.5f) - 1;
    v.ui_open = e->ui_open; v.ui_layer = e->ui_layer; v.ui_bank = e->ui_bank;
    memcpy(v.ui_title, e->ui.ui_title, sizeof v.ui_title); v.ui_title[sizeof v.ui_title - 1] = 0;
    for (int r = 0; r < GR_ROWS; r++) { memcpy(v.ui_row[r], e->ui.ui_row[r], sizeof v.ui_row[r]); v.ui_row[r][sizeof v.ui_row[r] - 1] = 0; }
    v.ui_sel = e->ui.ui_sel;
    if (pthread_mutex_trylock(&e->pub_lock) == 0) {
        gr_overlay_publish(e->ovl, &v);
        pthread_mutex_unlock(&e->pub_lock);
    }
}

static void dirty_one(inst_t *e, int i) { __atomic_or_fetch(&e->dirty[i >> 6], 1ull << (i & 63), __ATOMIC_RELEASE); }
static void dirty_set(void *ep, int i) { dirty_one(ep, i); }
static void mark_dirty_all(inst_t *e) { __atomic_or_fetch(&e->dirty[0], ~0ull, __ATOMIC_RELEASE); __atomic_or_fetch(&e->dirty[1], ~0ull, __ATOMIC_RELEASE); }

/* the ENVELOPE page's four knobs show the selected layer's own values */
static void env_proxy_refresh(inst_t *e) {
    int L = (int)(e->val[P_ENVL] + 0.5f) - 1;
    if (L < 0) L = 0;
    if (L >= GR_LAYERS) L = GR_LAYERS - 1;
    e->val[P_ATK] = e->val[P_ATK1 + L]; e->val[P_DEC] = e->val[P_DEC1 + L]; e->val[P_SUS] = e->val[P_SUS1 + L]; e->val[P_REL] = e->val[P_REL1 + L];
    dirty_one(e, P_ATK); dirty_one(e, P_DEC); dirty_one(e, P_SUS); dirty_one(e, P_REL);
}

static void build_window(inst_t *e) {
    float ef = 0.5f * (1.0f - e->val[P_SHAPE]) + 0.02f;
    for (int i = 0; i <= 512; i++) {
        float t = (float)i / 512.0f, w;
        if (t < ef) w = 0.5f - 0.5f * cosf(3.14159265f * t / ef);
        else if (t > 1.0f - ef) w = 0.5f - 0.5f * cosf(3.14159265f * (1.0f - t) / ef);
        else w = 1.0f;
        e->win[i] = w;
    }
}

/* a message on the status line for a couple of seconds, then the preset's name again */
static void say(inst_t *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->status, sizeof e->status, fmt, ap);
    va_end(ap);
    e->msg_left = 110000;   /* frames: 2.5 s */
}

static void set_status_base(inst_t *e, const char *s) { snprintf(e->base_status, sizeof e->base_status, "%s", s); snprintf(e->status, sizeof e->status, "%s", s); e->msg_left = 0; }

static void load_factory(inst_t *e, int n) {
    if (n < 0) n = 0;
    if (n >= NFACTORY) n = NFACTORY - 1;
    float v[NP];
    memset(v, 0, sizeof v);
    factory_fill(n, v);
    for (int i = SOUND_FIRST; i <= SOUND_LAST; i++) e->val[i] = v[i];
    e->val[P_PRESET] = (float)n; e->fidx = n;
    e->cur_bank = 0;
    env_proxy_refresh(e);
    char nm[48], s[72];
    factory_name(n, nm, sizeof nm);
    snprintf(s, sizeof s, "F%03d  %s", n + 1, nm);
    set_status_base(e, s);
    build_window(e);
    mark_dirty_all(e);
    gr_log("factory preset %d: %s", n + 1, nm);
}

static void user_path(inst_t *e, int slot, char *p, size_t n) { snprintf(p, n, "%s/u%03d.amgr", e->user_dir, slot); }

static int load_user(inst_t *e, int slot) {
    char p[GR_PATH * 2 + 32], line[160];
    user_path(e, slot, p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int got = 0;
    while (fgets(line, sizeof line, f)) {
        char key[32];
        float v;
        if (sscanf(line, "%31s %f", key, &v) != 2) continue;
        static const char *old4[4] = {"atk", "dec", "sus", "rel"};   /* a 0.1.0 preset: one envelope, given to all four layers */
        static const int new4[4] = {P_ATK1, P_DEC1, P_SUS1, P_REL1};
        for (int k = 0; k < 4; k++) if (!strcmp(key, old4[k])) for (int L = 0; L < GR_LAYERS; L++) e->val[new4[k] + L] = clampf(v, 0, 1);
        for (int i = SOUND_FIRST; i <= SOUND_LAST; i++)
            if (!strcmp(key, PARAMS[i].key)) { e->val[i] = clampf(v, PARAMS[i].min, PARAMS[i].max); got++; break; }
    }
    fclose(f);
    char s[72];
    snprintf(s, sizeof s, "U%03d  USER %d", slot, slot);
    set_status_base(e, s);
    e->cur_bank = 1; e->cur_user = slot;
    env_proxy_refresh(e);
    build_window(e);
    mark_dirty_all(e);
    gr_log("user preset %d loaded (%d values)", slot, got);
    return 1;
}

static void save_user(inst_t *e, int slot) {
    mkdir(e->user_dir, 0777);
    char p[GR_PATH * 2 + 32];
    user_path(e, slot, p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) { say(e, "SAVE FAILED (U%03d)", slot); gr_log("save user %d: cannot write %s", slot, p); return; }
    fprintf(f, "# AMGR user preset %d\n", slot);
    for (int i = SOUND_FIRST; i <= SOUND_LAST; i++) fprintf(f, "%s %.6f\n", PARAMS[i].key, e->val[i]);
    fclose(f);
    snprintf(e->base_status, sizeof e->base_status, "U%03d  USER %d", slot, slot);
    e->cur_bank = 1; e->cur_user = slot;
    if (slot >= 1 && slot <= NUSER) e->user_has[slot] = 1;
    say(e, "SAVED AS U%03d", slot);
    gr_log("user preset %d saved", slot);
}

/* ------------------------------------------------------------------------------------------------ the loader thread */
static void install(inst_t *e, int L, audio_t *a, const char *name) {
    gr_wave_t *w = malloc(sizeof *w);
    if (w) { make_wave(a, name, a->test, w); wave_store(L, w); free(w); }
    audio_t *old = __atomic_exchange_n(&e->aud[L], a, __ATOMIC_ACQ_REL);
    if (old && !old->test) {   /* a render may still read it for up to a grain: freed a few seconds later */
        old->retired_at = now_s();
        if (e->nretired < 96) e->retired[e->nretired++] = old; else { free(old->d); free(old); }
    }
    publish(e);
}

static void sweep_retired(inst_t *e, int all) {
    double t = now_s();
    int j = 0;
    for (int i = 0; i < e->nretired; i++) {
        audio_t *a = e->retired[i];
        if (all || t - a->retired_at > 4.0) { free(a->d); free(a); } else e->retired[j++] = a;
    }
    e->nretired = j;
}

/* ------------------------------------------------------------------------------------------------ the browser and the texts on the strip */
static void lane_say(inst_t *e, int L, const char *msg) { snprintf(e->ui.lmsg[L], sizeof e->ui.lmsg[L], "%s", msg); e->lmsg_until[L] = now_s() + 4.0; }

/* the DRIVES list (dir "" ): the plugin's Samples folder, the internal drive and every mounted USB / SD drive, as "name<TAB>path" */
static int list_drives(inst_t *e, ent_t *t, int max) {
    int n = 0;
    char seen[24][GR_PATH];
    int ns = 0;
    #define ADD(label, path) do { struct stat st_; char rp_[GR_PATH]; \
        if (n < max && stat(path, &st_) == 0 && S_ISDIR(st_.st_mode) && realpath(path, rp_)) { int dup_ = 0; \
            for (int q_ = 0; q_ < ns; q_++) if (!strcmp(seen[q_], rp_)) dup_ = 1; \
            if (!dup_) { if (ns < 24) snprintf(seen[ns++], GR_PATH, "%s", rp_); snprintf(t[n].name, sizeof t[n].name, "%.24s\t%.32s", label, path); t[n].is_dir = 1; n++; } } } while (0)
    ADD("SAMPLES (plugin)", e->samples_dir);
    ADD("INTERNAL", "/sdcard");
    FILE *f = fopen("/proc/mounts", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char dev[128], mp[256], fs[32];
            if (sscanf(line, "%127s %255s %31s", dev, mp, fs) != 3) continue;
            if (strcmp(fs, "vfat") && strcmp(fs, "exfat") && strcmp(fs, "ntfs") && strcmp(fs, "ntfs3") && strcmp(fs, "fuseblk") && strcmp(fs, "ext4") &&
                strcmp(fs, "ext3") && strcmp(fs, "ext2") && strcmp(fs, "f2fs") && strcmp(fs, "hfsplus")) continue;
            if (!strcmp(mp, "/") || !strncmp(mp, "/boot", 5) || !strncmp(mp, "/usr", 4) || !strncmp(mp, "/var", 4) || !strncmp(mp, "/opt", 4) || !strncmp(mp, "/system", 7)) continue;
            if (strncmp(mp, "/media", 6) && strncmp(mp, "/mnt", 4) && strncmp(mp, "/run/media", 10) && strncmp(mp, "/sdcard", 7) && strncmp(mp, "/storage", 8) && strncmp(mp, "/home", 5)) continue;
            for (char *q = mp; *q; q++) if (q[0] == '\\' && q[1] == '0' && q[2] == '4' && q[3] == '0') { *q = ' '; memmove(q + 1, q + 4, strlen(q + 4) + 1); }
            const char *base = strrchr(mp, '/');
            base = base && base[1] ? base + 1 : mp;
            ADD(base, mp);
        }
        fclose(f);
    }
    #undef ADD
    return n;
}

static void scan_dir(void *ep, int L) {
    inst_t *e = ep;
    int n = 0;
    ent_t *t = e->ents[L];
    if (!e->dir[L][0]) { e->nents[L] = list_drives(e, t, MAXENT); if (e->sel[L] >= e->nents[L]) e->sel[L] = 0; return; }
    snprintf(t[n].name, sizeof t[n].name, ".."); t[n].is_dir = 1; n++;   /* up: to the folder above, or to DRIVES from a drive's top */
    int first = n;
    DIR *d = opendir(e->dir[L]);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && n < MAXENT) {
            if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof t[n].name) continue;
            char full[GR_PATH * 2];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", strcmp(e->dir[L], "/") ? e->dir[L] : "", de->d_name);
            if (stat(full, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) { snprintf(t[n].name, sizeof t[n].name, "%s", de->d_name); t[n].is_dir = 1; n++; }
            else if (S_ISREG(st.st_mode) && is_audio_name(de->d_name)) { snprintf(t[n].name, sizeof t[n].name, "%s", de->d_name); t[n].is_dir = 0; n++; }
        }
        closedir(d);
    }
    qsort(t + first, (size_t)(n - first), sizeof *t, cmp_ent);
    e->nents[L] = n;
    if (e->sel[L] >= n) e->sel[L] = n > 0 ? n - 1 : 0;
}

/* where each box last looked and what it holds: one line a layer, "folder<TAB>file" */
static void last_write(inst_t *e) {
    char p[GR_PATH * 2];
    snprintf(p, sizeof p, "%s/amgr_last.txt", e->plugin_dir);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int L = 0; L < GR_LAYERS; L++) fprintf(f, "%s\t%s\n", e->dir[L], e->file[L]);
    fclose(f);
}
static void last_read(inst_t *e) {
    char p[GR_PATH * 2], line[GR_PATH + 128];
    snprintf(p, sizeof p, "%s/amgr_last.txt", e->plugin_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    for (int L = 0; L < GR_LAYERS && fgets(line, sizeof line, f); L++) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        struct stat st;
        if (line[0] == '/' && line[1] && stat(line, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(e->dir[L], sizeof e->dir[L], "%s", line);
            snprintf(e->file[L], sizeof e->file[L], "%s", tab + 1);
        }
    }
    fclose(f);
}

/* the texts the screen shows on the strip. Call with names_lock held. */
static void ui_refresh(inst_t *e) {
    for (int L = 0; L < GR_LAYERS; L++) {
        char nm[64];
        if (e->pend_at[L] > 0 && e->sel[L] < e->nents[L]) { snprintf(nm, sizeof nm, "%s", e->ents[L][e->sel[L]].name); strip_ext(nm); }   /* scrolling: the one about to load */
        else if (e->file[L][0]) { snprintf(nm, sizeof nm, "%s", e->file[L]); strip_ext(nm); } else snprintf(nm, sizeof nm, "TEST %d", L + 1);
        snprintf(e->ui.box[L], sizeof e->ui.box[L], "%.38s", nm);
    }
    for (int r = 0; r < GR_ROWS; r++) e->ui.ui_row[r][0] = 0;
    e->ui.ui_sel = -1;
    if (e->ui_open == 1) {
        int L = e->ui_layer, n = e->nents[L], sel = e->sel[L];
        int top = sel - 1;
        if (top > n - GR_ROWS) top = n - GR_ROWS;
        if (top < 0) top = 0;
        const char *dn = e->dir[L][0] ? e->dir[L] : "DRIVES";
        size_t dl = strlen(dn);
        snprintf(e->ui.ui_title, sizeof e->ui.ui_title, "LAYER %d   %s%s", L + 1, dl > 46 ? ".." : "", dl > 46 ? dn + dl - 44 : dn);
        for (int r = 0; r < GR_ROWS && top + r < n; r++) {
            const ent_t *t = &e->ents[L][top + r];
            if (!e->dir[L][0]) snprintf(e->ui.ui_row[r], sizeof e->ui.ui_row[r], "[%.*s]", (int)strcspn(t->name, "\t"), t->name);
            else if (t->is_dir) snprintf(e->ui.ui_row[r], sizeof e->ui.ui_row[r], "[%.50s]", t->name);
            else snprintf(e->ui.ui_row[r], sizeof e->ui.ui_row[r], "%.54s", t->name);
            if (top + r == sel) e->ui.ui_sel = r;
        }
        if (n == 0) snprintf(e->ui.ui_row[0], sizeof e->ui.ui_row[0], "(empty)");
    } else if (e->ui_open == 2) {
        int n = e->ui_bank ? NUSER : NFACTORY, sel = e->ui_bank ? (int)e->val[P_USER] - 1 : (int)e->val[P_PRESET];
        int top = sel - 1;
        if (top > n - GR_ROWS) top = n - GR_ROWS;
        if (top < 0) top = 0;
        snprintf(e->ui.ui_title, sizeof e->ui.ui_title, e->ui_bank ? "USER PRESETS   %d slots" : "FACTORY PRESETS   %d", n);
        for (int r = 0; r < GR_ROWS && top + r < n; r++) {
            int k = top + r;
            if (e->ui_bank) snprintf(e->ui.ui_row[r], sizeof e->ui.ui_row[r], "U%03d   %s", k + 1, e->user_has[k + 1] ? "USER PRESET" : "(empty)");
            else { char nm[48]; factory_name(k, nm, sizeof nm); snprintf(e->ui.ui_row[r], sizeof e->ui.ui_row[r], "F%03d   %s", k + 1, nm); }
            if (k == sel) e->ui.ui_sel = r;
        }
    }
}
static void ui_update(inst_t *e) { pthread_mutex_lock(&e->names_lock); ui_refresh(e); pthread_mutex_unlock(&e->names_lock); publish(e); }

/* ------------------------------------------------------------------------------------------------ the loader thread */
static void do_load(inst_t *e, int L, const char *path, const char *name) {
    if (!path[0]) {
        if (!e->test[L]) e->test[L] = make_test_audio(L);
        if (e->test[L]) { char nm[32]; snprintf(nm, sizeof nm, "TEST %d", L + 1); install(e, L, e->test[L], nm); }
        pthread_mutex_lock(&e->names_lock); e->file[L][0] = 0; ui_refresh(e); pthread_mutex_unlock(&e->names_lock);
        publish(e);
        return;
    }
    int cut = 0, err = 0;
    publish(e);
    audio_t *a = load_audio_file(path, L, &cut, &err);
    g_load_pct[L] = -1;
    if (!a) {
        gr_log("layer %d: %s could not be loaded (%s)", L + 1, path, err == 2 ? "not enough memory" : "not readable");
        lane_say(e, L, err == 2 ? "NOT ENOUGH MEMORY" : "CANNOT READ FILE");
        if (!e->aud[L]) do_load(e, L, "", "");
        publish(e);
        return;
    }
    gr_log("layer %d: %s (%.1f s%s)", L + 1, path, (double)a->n / SR, cut ? ", cut at 5:00" : "");
    install(e, L, a, name);
    if (cut) lane_say(e, L, "CUT AT 5:00");
    pthread_mutex_lock(&e->names_lock);
    snprintf(e->file[L], sizeof e->file[L], "%s", name);
    ui_refresh(e);
    last_write(e);
    pthread_mutex_unlock(&e->names_lock);
    publish(e);
}

static void request_load(inst_t *e, int L, const char *path, const char *name) {
    pthread_mutex_lock(&e->lk);
    snprintf(e->req_path[L], sizeof e->req_path[L], "%s", path);
    snprintf(e->req_name[L], sizeof e->req_name[L], "%s", name);
    e->req[L] = 1;
    pthread_cond_signal(&e->cv);
    pthread_mutex_unlock(&e->lk);
}

/* loads the browser's highlighted entry of layer L when it is a file. Call with names_lock held. */
static void load_selected(inst_t *e, int L) {
    e->pend_at[L] = 0;
    if (e->sel[L] < 0 || e->sel[L] >= e->nents[L] || e->ents[L][e->sel[L]].is_dir) return;
    const char *nm = e->ents[L][e->sel[L]].name;
    if (!strcmp(nm, e->file[L]) && e->aud[L] && !e->aud[L]->test) return;   /* already in the lane */
    char path[GR_PATH * 2];
    snprintf(path, sizeof path, "%s/%s", strcmp(e->dir[L], "/") ? e->dir[L] : "", nm);
    request_load(e, L, path, nm);
}

static void *loader_main(void *p) {
    inst_t *e = p;
    int tick = 0;
    pthread_mutex_lock(&e->lk);
    while (!e->stop) {
        int did = 0;
        for (int L = 0; L < GR_LAYERS && !e->stop; L++) {
            if (e->req[L] < 0) continue;
            char path[GR_PATH * 2], name[64];
            snprintf(path, sizeof path, "%s", e->req_path[L]);
            snprintf(name, sizeof name, "%s", e->req_name[L]);
            e->req[L] = -1;
            pthread_mutex_unlock(&e->lk);
            do_load(e, L, path, name);
            pthread_mutex_lock(&e->lk);
            did = 1;
        }
        if (!did) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_nsec -= 1000000000; ts.tv_sec++; }
            pthread_cond_timedwait(&e->cv, &e->lk, &ts);
            pthread_mutex_unlock(&e->lk);
            /* a file picked by scrolling (Q-Link, data wheel) loads once the scrolling has rested for half a second */
            double tn = now_s();
            pthread_mutex_lock(&e->names_lock);
            for (int L = 0; L < GR_LAYERS; L++) if (e->pend_at[L] > 0 && tn >= e->pend_at[L]) load_selected(e, L);
            pthread_mutex_unlock(&e->names_lock);
            if (++tick >= 20) { tick = 0; sweep_retired(e, 0); publish(e); }
            pthread_mutex_lock(&e->lk);
        }
    }
    pthread_mutex_unlock(&e->lk);
    return NULL;
}

/* the browser's highlight moved (touch, Q-Link, wheel): keep the parameter, the list and the pending load in step. Call with names_lock held. */
static void sel_set(inst_t *e, int L, int k, int from_param) {
    if (k >= e->nents[L]) k = e->nents[L] - 1;
    if (k < 0) k = 0;
    e->sel[L] = k;
    if (!from_param || (int)(e->val[P_WAV1 + L] + 0.5f) != k) { e->val[P_WAV1 + L] = (float)k; dirty_one(e, P_WAV1 + L); }
    e->pend_at[L] = (k < e->nents[L] && !e->ents[L][k].is_dir) ? now_s() + 0.5 : 0;
    ui_refresh(e);
}

/* into a folder of layer L's list (".." = up). Call with names_lock held. */
static void dir_enter(inst_t *e, int L, const char *name) {
    char prev[64] = "";
    if (!strcmp(name, "..")) {
        /* up one folder; from a drive's top (or anything not below a drive) back to the DRIVES list */
        char rp[GR_PATH];
        int at_top = 1;
        if (realpath(e->dir[L], rp)) {
            ent_t dr[24];
            int nd = list_drives(e, dr, 24);
            for (int i = 0; i < nd; i++) {
                char dp[GR_PATH], *tab = strchr(dr[i].name, '\t');
                if (tab && realpath(tab + 1, dp) && strncmp(rp, dp, strlen(dp)) == 0 && strlen(rp) > strlen(dp)) at_top = 0;
            }
        }
        char *sl = strrchr(e->dir[L], '/');
        if (at_top || !sl) {
            snprintf(prev, sizeof prev, "%s", e->dir[L]);
            e->dir[L][0] = 0;
            e->sel[L] = 0;
            scan_dir(e, L);
            for (int i = 0; i < e->nents[L]; i++) { char *tab = strchr(e->ents[L][i].name, '\t'); if (tab && !strcmp(tab + 1, prev)) e->sel[L] = i; }
            e->val[P_WAV1 + L] = (float)e->sel[L]; dirty_one(e, P_WAV1 + L);
            e->pend_at[L] = 0;
            ui_refresh(e);
            return;
        }
        snprintf(prev, sizeof prev, "%s", sl + 1);
        if (sl == e->dir[L]) e->dir[L][1] = 0; else *sl = 0;
    } else if (!e->dir[L][0]) {   /* a drive from the DRIVES list */
        const char *tab = strchr(name, '\t');
        if (!tab) return;
        snprintf(e->dir[L], sizeof e->dir[L], "%s", tab + 1);
    } else {
        size_t l = strlen(e->dir[L]);
        if (l + strlen(name) + 2 >= sizeof e->dir[L]) return;
        snprintf(e->dir[L] + l, sizeof e->dir[L] - l, "%s%s", l > 1 ? "/" : "", name);
    }
    e->sel[L] = 0;
    scan_dir(e, L);
    for (int i = 0; i < e->nents[L]; i++) if (prev[0] && !strcmp(e->ents[L][i].name, prev)) e->sel[L] = i;   /* coming up: stay on the folder you left */
    e->val[P_WAV1 + L] = (float)e->sel[L]; dirty_one(e, P_WAV1 + L);
    e->pend_at[L] = 0;
    last_write(e);
    ui_refresh(e);
}

static int first_free_user(inst_t *e) { for (int i = 1; i <= NUSER; i++) if (!e->user_has[i]) return i; return NUSER; }

static int load_user(inst_t *e, int slot);
static void save_user(inst_t *e, int slot);
static void load_factory(inst_t *e, int n);

/* ------------------------------------------------------------------------------------------------ touch */
static int in_box(int x, int y, int bx, int by, int bw, int bh) { return x >= bx && x < bx + bw && y >= by && y < by + bh; }
static int lane_at(int x, int y) {
    for (int L = 0; L < GR_LAYERS; L++) {
        int ly = GR_LANE_Y0 + L * GR_LANE_PITCH;
        if (x >= 0 && x < GR_SKIN_W && y >= ly && y < ly + GR_LANE_H) return L;
    }
    return -1;
}

/* a tap on the strip under the lanes */
static void ui_tap(inst_t *e, int page, int x, int y) {
    pthread_mutex_lock(&e->names_lock);
    int open = (page == PG_SAMPLES && e->ui_open == 1) || (page == PG_PRESETS && e->ui_open == 2);
    if (open && in_box(x, y, LIST_X, ROW_Y0, LIST_W, ROW_H * GR_ROWS)) {
        int r = (y - ROW_Y0) / ROW_H;
        if (e->ui_open == 1) {
            int L = e->ui_layer, n = e->nents[L], top = e->sel[L] - 1;
            if (top > n - GR_ROWS) top = n - GR_ROWS;
            if (top < 0) top = 0;
            if (top + r < n) {
                if (top + r == e->sel[L] && e->ents[L][e->sel[L]].is_dir) dir_enter(e, L, e->ents[L][e->sel[L]].name);   /* a second tap on a folder goes in */
                else sel_set(e, L, top + r, 0);
            }
        } else {
            int n = e->ui_bank ? NUSER : NFACTORY, sel = e->ui_bank ? (int)e->val[P_USER] - 1 : (int)e->val[P_PRESET], top = sel - 1;
            if (top > n - GR_ROWS) top = n - GR_ROWS;
            if (top < 0) top = 0;
            int k = top + r;
            if (k < n) {
                if (e->ui_bank) { e->val[P_USER] = (float)(k + 1); dirty_one(e, P_USER); if (!load_user(e, k + 1)) say(e, "U%03d  (EMPTY)", k + 1); }
                else load_factory(e, k);
            }
            ui_refresh(e);
        }
    } else if (open && e->ui_open == 1 && in_box(x, y, LBTN_X(0), TILE_Y, LBTN_X(3) + LBTN_W - LBTN_X(0), TILE_H)) {
        int L = e->ui_layer, b = (x - LBTN_X(0)) / 112;
        if (b == 0) dir_enter(e, L, "..");
        else if (b == 1) { if (e->sel[L] < e->nents[L] && e->ents[L][e->sel[L]].is_dir) dir_enter(e, L, e->ents[L][e->sel[L]].name); }
        else if (b == 2) { if (e->sel[L] < e->nents[L] && !e->ents[L][e->sel[L]].is_dir) { load_selected(e, L); e->ui_open = 0; } }
        else e->ui_open = 0;
        if (!e->ui_open) for (int o = 0; o < GR_LAYERS; o++) { e->val[P_WAV1 + o] = (float)e->sel[o]; dirty_one(e, P_WAV1 + o); }
        ui_refresh(e);
    } else if (open && e->ui_open == 2 && in_box(x, y, LBTN_X(0), TILE_Y, LBTN_X(2) + LBTN_W - LBTN_X(0), TILE_H)) {
        int b = (x - LBTN_X(0)) / 112;
        if (b == 0) e->ui_open = 0; else e->ui_bank = b - 1;
        e->val[P_PRESET] = (e->ui_open == 2 && e->ui_bank == 1) ? 128.0f : (float)e->fidx; dirty_one(e, P_PRESET);   /* the USER list moves slots: the control sits mid-range so both turns count */
        ui_refresh(e);
    } else if (!open && page == PG_SAMPLES && y >= TILE_Y && y < TILE_Y + TILE_H) {
        for (int L = 0; L < GR_LAYERS; L++)
            if (x >= SBOX_X(L) && x < SBOX_X(L) + SBOX_W) {
                e->ui_open = 1; e->ui_layer = L;
                scan_dir(e, L);
                for (int i = 0; i < e->nents[L]; i++) if (e->file[L][0] && !strcmp(e->ents[L][i].name, e->file[L])) e->sel[L] = i;
                e->val[P_WAV1 + L] = (float)e->sel[L]; dirty_one(e, P_WAV1 + L);
                for (int o = 0; o < GR_LAYERS; o++) if (o != L) { e->val[P_WAV1 + o] = 500.0f; dirty_one(e, P_WAV1 + o); }
                ui_refresh(e);
            }
    } else if (!open && page == PG_PRESETS && y >= TILE_Y && y < TILE_Y + TILE_H) {
        if (x >= PBOX_X && x < PBOX_X + PBOX_W) { e->ui_open = 2; e->ui_bank = e->cur_bank; e->val[P_PRESET] = e->ui_bank == 1 ? 128.0f : (float)e->fidx; dirty_one(e, P_PRESET); ui_refresh(e); }
        else if (x >= SAVE_X && x < SAVE_X + BTN_W) {   /* SAVE: into the next free user slot */
            int slot = first_free_user(e);
            e->val[P_USER] = (float)slot; dirty_one(e, P_USER);
            save_user(e, slot);
        } else if (x >= BEND_X && x < BEND_X + BTN_W) {   /* BEND: the pitch wheel's range, one tap = the next step */
            static const int st[6] = {1, 2, 5, 7, 12, 24};
            int cur = (int)(e->val[P_BEND] + 0.5f), nx = st[0];
            for (int i = 0; i < 6; i++) if (st[i] > cur) { nx = st[i]; break; }
            e->val[P_BEND] = (float)nx; dirty_one(e, P_BEND);
        }
    } else if (page == PG_ENVELOPE) {
        for (int i = 0; i < GR_LAYERS; i++)
            if (in_box(x, y, ET_X(i), ET_Y(i), ET_W, ET_H)) { e->val[P_ENVL] = (float)(i + 1); dirty_one(e, P_ENVL); env_proxy_refresh(e); }
    }
    pthread_mutex_unlock(&e->names_lock);
    publish(e);
}

/* a finger: on a waveform that layer's position follows it until it lifts (the wrapper is told, HAS_DIRTY, so the host's knob, the Q-Link and
 * automation follow too); on the strip under the lanes it is a tap on a tile or a list row */
static void touch_ev(void *ctx, int slot, int page, int x, int y, int phase) {
    inst_t *e = ctx;
    if (slot < 0 || slot >= 12) return;
    if (phase == 0) { e->grab[slot] = -1; return; }
    if (phase == 2) {
        e->grab[slot] = lane_at(x, y);
        if (e->grab[slot] < 0 && y >= UI_Y) { ui_tap(e, page, x, y); return; }
    }
    int L = e->grab[slot];
    if (L < 0) return;
    float pos = clampf((float)(x - GR_LANE_X) / (float)(GR_LANE_W - 2), 0, 1);
    e->val[P_POS1 + L] = pos;
    dirty_one(e, P_POS1 + L);
    publish(e);
}

/* ------------------------------------------------------------------------------------------------ lifecycle */
static void *create(const char *data_dir) {
    inst_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    if (data_dir && data_dir[0]) {
        snprintf(e->samples_dir, sizeof e->samples_dir, "%s", data_dir);
        snprintf(e->plugin_dir, sizeof e->plugin_dir, "%s", data_dir);
        char *sl = strrchr(e->plugin_dir, '/');
        if (sl && sl != e->plugin_dir) *sl = 0; else snprintf(e->plugin_dir, sizeof e->plugin_dir, ".");
    } else { snprintf(e->samples_dir, sizeof e->samples_dir, "."); snprintf(e->plugin_dir, sizeof e->plugin_dir, "."); }
    if (e->samples_dir[0] != '/') {   /* no absolute data folder given: beside this plugin file, found from the loaded library */
        Dl_info di;
        if (dladdr((void *)create, &di) && di.dli_fname && di.dli_fname[0] == '/') {
            snprintf(e->plugin_dir, sizeof e->plugin_dir, "%s", di.dli_fname);
            char *sl = strrchr(e->plugin_dir, '/');
            if (sl && sl != e->plugin_dir) *sl = 0;
            snprintf(e->samples_dir, sizeof e->samples_dir, "%s/Samples", e->plugin_dir);
        }
    }
    snprintf(e->user_dir, sizeof e->user_dir, "%s/User", e->plugin_dir);
    if (!g_log_path[0]) {
        snprintf(g_log_path, sizeof g_log_path, "%s/amgr.log", e->plugin_dir);
        FILE *f = fopen(g_log_path, "w");
        if (f) fclose(f); else g_log_path[0] = 0;
    }
    gr_log("AMGR 0.2.1: create (plugin dir %s, samples %s)", e->plugin_dir, e->samples_dir);
    pthread_mutex_init(&e->names_lock, NULL);
    pthread_mutex_init(&e->pub_lock, NULL);
    pthread_mutex_init(&e->lk, NULL);
    pthread_cond_init(&e->cv, NULL);
    e->rng = 2463534242u;
    e->gov = 1.0f;
    for (int i = 0; i < NP; i++) e->val[i] = PARAMS[i].def;
    for (int i = 0; i < 12; i++) e->grab[i] = -1;
    for (int i = 1; i <= NUSER; i++) { char up[GR_PATH * 2 + 32]; user_path(e, i, up, sizeof up); e->user_has[i] = access(up, F_OK) == 0; }
    /* each box starts where it last looked (amgr_last.txt), else in the plugin's Samples folder on its Nth file */
    for (int L = 0; L < GR_LAYERS; L++) {
        e->req[L] = -1;
        e->ents[L] = calloc(MAXENT + 1, sizeof(ent_t));
        if (!e->ents[L]) { free(e); return NULL; }
        snprintf(e->dir[L], sizeof e->dir[L], "%s", e->samples_dir);
    }
    int had_last = 0;
    { char lp[GR_PATH * 2]; snprintf(lp, sizeof lp, "%s/amgr_last.txt", e->plugin_dir); had_last = access(lp, F_OK) == 0; }
    last_read(e);
    for (int L = 0; L < GR_LAYERS; L++) {
        scan_dir(e, L);
        int pick = -1, nth = 0;
        for (int i = 0; i < e->nents[L]; i++) {
            if (e->ents[L][i].is_dir) continue;
            if (had_last ? (e->file[L][0] && !strcmp(e->ents[L][i].name, e->file[L])) : nth == L) { pick = i; break; }
            nth++;
        }
        e->file[L][0] = 0;
        if (pick >= 0) {
            char path[GR_PATH * 2];
            e->sel[L] = pick;
            snprintf(path, sizeof path, "%s/%s", strcmp(e->dir[L], "/") ? e->dir[L] : "", e->ents[L][pick].name);
            snprintf(e->req_path[L], sizeof e->req_path[L], "%s", path);
            snprintf(e->req_name[L], sizeof e->req_name[L], "%s", e->ents[L][pick].name);
        } else { e->req_path[L][0] = 0; e->req_name[L][0] = 0; }
        e->req[L] = 1;
        e->val[P_WAV1 + L] = (float)e->sel[L];
    }
    for (int sd = 0; sd < 2; sd++) {
        e->dl[sd] = calloc(DMAX, sizeof(float));
        for (int c = 0; c < 8; c++) { e->clen[sd][c] = COMB_T[c] + sd * 23; e->comb[sd][c] = calloc((size_t)e->clen[sd][c], sizeof(float)); }
        for (int a = 0; a < 4; a++) { e->alen[sd][a] = AP_T[a] + sd * 23; e->ap[sd][a] = calloc((size_t)e->alen[sd][a], sizeof(float)); }
    }
    e->fcut = cut_hz(e->val[P_CUT]);
    e->dcur = dly_ms(e->val[P_DTIME]) * 0.001f * SR;
    load_factory(e, 0);
    e->dirty[0] = e->dirty[1] = 0;
    for (int L = 0; L < GR_LAYERS; L++) for (int g = 0; g < GR_ACT; g++) e->act[L][g] = -1.0f;
    char font[GR_PATH * 2];
    snprintf(font, sizeof font, "%s/Fonts/TitilliumWeb-Regular.ttf", e->plugin_dir);
    gr_screen_set_font(font);
    for (int L = 0; L < GR_LAYERS; L++) { gr_wave_t w; memset(&w, 0, sizeof w); wave_store(L, &w); }
    int ok = 0;
    e->ovl = gr_overlay_start(e->plugin_dir, &ok);
    ui_update(e);
    e->touch = gr_touch_start(e->ovl, touch_ev, e);
    if (pthread_create(&e->loader, NULL, loader_main, e) == 0) e->loader_started = 1;
    return e;
}

static void destroy(void *p) {
    inst_t *e = p;
    pthread_mutex_lock(&e->lk);
    e->stop = 1;
    pthread_cond_signal(&e->cv);
    pthread_mutex_unlock(&e->lk);
    if (e->loader_started) pthread_join(e->loader, NULL);
    gr_touch_stop(e->touch);
    gr_overlay_stop(e->ovl);
    sweep_retired(e, 1);
    for (int L = 0; L < GR_LAYERS; L++) {
        if (e->aud[L] && !e->aud[L]->test) { free(e->aud[L]->d); free(e->aud[L]); }
        if (e->test[L]) { free(e->test[L]->d); free(e->test[L]); }
        free(e->ents[L]);
    }
    for (int sd = 0; sd < 2; sd++) {
        free(e->dl[sd]);
        for (int c = 0; c < 8; c++) free(e->comb[sd][c]);
        for (int a = 0; a < 4; a++) free(e->ap[sd][a]);
    }
    gr_log("destroy");
    free(e);
}

/* ------------------------------------------------------------------------------------------------ parameters */
static int find_key(const char *key) {
    for (int i = 0; i < NP; i++) if (!strcmp(key, PARAMS[i].key)) return i;
    return -1;
}

/* the whole instrument as text, for the project: every value, and each layer's folder and file by name */
static int state_get(inst_t *e, char *buf, int len) {
    size_t cap = 256 + (size_t)NP * 40 + GR_LAYERS * (GR_PATH + 96), n = 0;
    char *t = malloc(cap);
    if (!t) return 0;
    n += (size_t)snprintf(t + n, cap - n, "AMGR 2\n");
    pthread_mutex_lock(&e->names_lock);
    for (int L = 0; L < GR_LAYERS; L++) n += (size_t)snprintf(t + n, cap - n, "@dir%d %s\n@file%d %s\n", L + 1, e->dir[L], L + 1, e->file[L]);
    pthread_mutex_unlock(&e->names_lock);
    n += (size_t)snprintf(t + n, cap - n, "@bank %d %d\n", e->cur_bank, e->cur_user);
    for (int i = 0; i < NP; i++) if (i != P_SAVE && !(i >= P_WAV1 && i <= P_WAV4)) n += (size_t)snprintf(t + n, cap - n, "%s %.6f\n", PARAMS[i].key, e->val[i]);
    if ((int)n + 1 <= len) memcpy(buf, t, n + 1);
    free(t);
    return (int)n + 1;
}

static void state_set(inst_t *e, const char *txt) {
    if (strncmp(txt, "AMGR ", 5)) return;
    char line[GR_PATH + 128], dirs[GR_LAYERS][GR_PATH], files[GR_LAYERS][64];
    int have[GR_LAYERS] = {0};
    const char *q = txt;
    while (*q) {
        size_t l = strcspn(q, "\n");
        snprintf(line, sizeof line, "%.*s", (int)(l < sizeof line - 1 ? l : sizeof line - 1), q);
        q += l + (q[l] ? 1 : 0);
        if (line[0] == '@') {
            int L = 0, a = 0, b = 0;
            if (sscanf(line, "@dir%d", &L) == 1 && L >= 1 && L <= GR_LAYERS && strlen(line) > 6) { snprintf(dirs[L - 1], sizeof dirs[0], "%s", line + 6); have[L - 1] |= 1; }
            else if (sscanf(line, "@file%d", &L) == 1 && L >= 1 && L <= GR_LAYERS) { snprintf(files[L - 1], sizeof files[0], "%s", strlen(line) > 7 ? line + 7 : ""); have[L - 1] |= 2; }
            else if (sscanf(line, "@bank %d %d", &a, &b) == 2) { e->cur_bank = a ? 1 : 0; e->cur_user = b; }
            continue;
        }
        char key[32];
        float v;
        if (sscanf(line, "%31s %f", key, &v) != 2) continue;
        int i = find_key(key);
        if (i >= 0 && i != P_SAVE && !(i >= P_WAV1 && i <= P_WAV4)) e->val[i] = clampf(v, PARAMS[i].min, PARAMS[i].max);
    }
    pthread_mutex_lock(&e->names_lock);
    for (int L = 0; L < GR_LAYERS; L++) {
        if (have[L] != 3) continue;
        struct stat st;
        if (dirs[L][0] != '/' || stat(dirs[L], &st) != 0 || !S_ISDIR(st.st_mode)) { gr_log("state: layer %d's folder %s is not there; the layer keeps what it has", L + 1, dirs[L]); continue; }
        snprintf(e->dir[L], sizeof e->dir[L], "%s", dirs[L]);
        e->sel[L] = 0;
        scan_dir(e, L);
        int found = -1;
        for (int i = 0; i < e->nents[L]; i++) if (files[L][0] && !e->ents[L][i].is_dir && !strcmp(e->ents[L][i].name, files[L])) found = i;
        if (found >= 0) { e->sel[L] = found; load_selected(e, L); }
        else if (!files[L][0]) request_load(e, L, "", "");
        else gr_log("state: layer %d's file %s is not in %s", L + 1, files[L], dirs[L]);
        e->val[P_WAV1 + L] = (float)e->sel[L];
    }
    char nm[48], st2[72];
    if (e->cur_bank) snprintf(st2, sizeof st2, "U%03d  USER %d", e->cur_user, e->cur_user);
    else { factory_name((int)e->val[P_PRESET], nm, sizeof nm); snprintf(st2, sizeof st2, "F%03d  %s", (int)e->val[P_PRESET] + 1, nm); }
    set_status_base(e, st2);
    ui_refresh(e);
    pthread_mutex_unlock(&e->names_lock);
    env_proxy_refresh(e);
    build_window(e);
    mark_dirty_all(e);
    publish(e);
}

static void set_param(void *p, const char *key, const char *val) {
    inst_t *e = p;
    if (!strcmp(key, "state")) { state_set(e, val); return; }
    int i = find_key(key);
    if (i < 0) return;
    float v = clampf((float)atof(val), PARAMS[i].min, PARAMS[i].max);
    if (PARAMS[i].int_display) v = floorf(v + 0.5f);
    float old = e->val[i];
    e->val[i] = v;
    switch (i) {
    case P_PRESET:
        if (v != old && e->ui_open == 2 && e->ui_bank == 1) {   /* the wheel on the USER list: it moves the user slot instead */
            int step = v > old ? 1 : -1, slot = (int)e->val[P_USER] + step;
            if (slot < 1) slot = 1;
            if (slot > NUSER) slot = NUSER;
            e->val[P_PRESET] = 128.0f; dirty_one(e, P_PRESET);
            if (slot != (int)e->val[P_USER]) { e->val[P_USER] = (float)slot; dirty_one(e, P_USER); if (!load_user(e, slot)) say(e, "U%03d  (EMPTY)", slot); }
            ui_update(e);
        } else if (v != old && e->ui_open == 2 && e->ui_bank == 0) {   /* the FACT list is open: one preset a click */
            int n = (int)old + (v > old ? 1 : -1);
            if (n < 0) n = 0;
            if (n > NFACTORY - 1) n = NFACTORY - 1;
            e->val[P_PRESET] = (float)n; dirty_one(e, P_PRESET);
            load_factory(e, n); ui_update(e);
        } else if (v != old) { load_factory(e, (int)v); ui_update(e); }
        break;
    case P_USER:
        if (v != old) { if (!load_user(e, (int)v)) say(e, "U%03d  (EMPTY)", (int)v); ui_update(e); }
        break;
    case P_SAVE: if (v > 0.5f && old <= 0.5f) { save_user(e, (int)e->val[P_USER]); ui_update(e); } break;
    case P_SHAPE: build_window(e); break;
    case P_ENVL: env_proxy_refresh(e); break;
    case P_ATK: case P_DEC: case P_SUS: case P_REL: {   /* an ENVELOPE knob: it is the selected layer's value */
        int L = (int)(e->val[P_ENVL] + 0.5f) - 1;
        static const int base[4] = {P_ATK1, P_DEC1, P_SUS1, P_REL1};
        if (L < 0) L = 0;
        if (L >= GR_LAYERS) L = GR_LAYERS - 1;
        e->val[base[i - P_ATK] + L] = v; dirty_one(e, base[i - P_ATK] + L);
        break; }
    case P_WAV1: case P_WAV2: case P_WAV3: case P_WAV4:   /* a sample box's Q-Link or the data wheel: scroll that layer's folder */
        if (v != old) {
            pthread_mutex_lock(&e->names_lock);
            int L = i - P_WAV1;
            /* a turn only says which way: one row per click (the host's step is a fraction of the 0..999 range, which used to jump to the end of the
             * list and clamp), and the value is kept at the row so the next click is relative to it. A list is open: it moves, whichever control has the wheel. */
            int tl = (e->ui_open == 1) ? e->ui_layer : L;
            if (tl != L) { e->val[i] = 500.0f; dirty_one(e, i); }   /* not the open list's own control: parked mid-range, so a turn either way is a move */
            sel_set(e, tl, e->sel[tl] + (v > old ? 1 : -1), 0);
            pthread_mutex_unlock(&e->names_lock);
        }
        break;
    default:
        if (i >= P_ATK1 && i <= P_REL4) { int L = (int)(e->val[P_ENVL] + 0.5f) - 1; if ((i - P_ATK1) % GR_LAYERS == L) env_proxy_refresh(e); }
        break;
    }
    publish(e);
}

static int get_param(void *p, const char *key, char *buf, int len) {
    inst_t *e = p;
    if (!strcmp(key, "state")) return state_get(e, buf, len);
    if (len <= 0) return 0;
    if (!strcmp(key, "dirty")) {   /* the parameters the engine moved itself (touch, a preset) since the wrapper last asked: "8 9 " */
        int n = 0;
        for (int w = 0; w < 2; w++) {
            uint64_t m = __atomic_exchange_n(&e->dirty[w], 0ull, __ATOMIC_ACQ_REL);
            for (int i = 0; i < 64; i++) if (((m >> i) & 1) && w * 64 + i < NP && n < len - 6) n += snprintf(buf + n, (size_t)(len - n), "%d ", w * 64 + i);
        }
        if (!n) return snprintf(buf, (size_t)len, " ") + 1;
        return n + 1;
    }
    if (!strncmp(key, "preset:", 7)) {   /* program n's name for MPC's PRESET menu */
        int n = atoi(key + 7);
        if (n < 0 || n >= NFACTORY) return 0;
        char nm[48];
        factory_name(n, nm, sizeof nm);
        return snprintf(buf, (size_t)len, "%s", nm) + 1;
    }
    size_t kl = strlen(key);
    if (kl > 8 && !strcmp(key + kl - 8, "_display")) {
        char k[32];
        snprintf(k, sizeof k, "%.*s", (int)(kl - 8), key);
        int i = find_key(k);
        if (i < 0) return 0;
        fmt_value(e, i, e->val[i], buf, len);
        return (int)strlen(buf) + 1;
    }
    int i = find_key(key);
    if (i < 0) return 0;
    return snprintf(buf, (size_t)len, "%.5f", e->val[i]) + 1;
}

/* ------------------------------------------------------------------------------------------------ MIDI and voices */
static void voice_on(inst_t *e, int note, int vel) {
    int vi = -1;
    for (int i = 0; i < NVOICE; i++) if (e->v[i].active && e->v[i].note == note && e->v[i].gate) { vi = i; break; }   /* the same note again: retrigger it */
    if (vi < 0) for (int i = 0; i < NVOICE; i++) if (!e->v[i].active) { vi = i; break; }
    if (vi < 0) {   /* steal: the quietest releasing voice, else the oldest */
        float best = 2.0f;
        for (int i = 0; i < NVOICE; i++) if (!e->v[i].gate && e->v[i].env < best) { best = e->v[i].env; vi = i; }
        if (vi < 0) { uint32_t o = 0xffffffffu; for (int i = 0; i < NVOICE; i++) if (e->v[i].order < o) { o = e->v[i].order; vi = i; } }
    }
    voice_t *v = &e->v[vi];
    memset(v, 0, sizeof *v);
    v->active = 1; v->note = note; v->gate = 1; v->vel = 0.35f + 0.65f * (float)vel / 127.0f; v->env = 0;
    v->order = ++e->order;
    for (int L = 0; L < GR_LAYERS; L++) v->cnt[L] = 0;
}

static void voice_release(voice_t *v) { v->gate = 0; v->held = 0; for (int L = 0; L < GR_LAYERS; L++) { v->stage[L] = 3; v->rel_from[L] = v->lenv[L]; } }

static void voice_off(inst_t *e, int note) {
    for (int i = 0; i < NVOICE; i++) {
        voice_t *v = &e->v[i];
        if (!v->active || !v->gate || v->note != note) continue;
        if (e->sustain) v->held = 1; else voice_release(v);
    }
}

static void midi(void *p, const uint8_t *msg, int len) {
    inst_t *e = p;
    if (len < 1) return;
    int st = msg[0] & 0xf0;
    if (st == 0x90 && len >= 3) { if (msg[2]) voice_on(e, msg[1], msg[2]); else voice_off(e, msg[1]); }
    else if (st == 0x80 && len >= 3) voice_off(e, msg[1]);
    else if (st == 0xe0 && len >= 3) e->bend = ((msg[1] | (msg[2] << 7)) - 8192) / 8192.0f;
    else if (st == 0xb0 && len >= 3) {
        if (msg[1] == 64) {
            int on = msg[2] >= 64;
            if (!on && e->sustain) for (int i = 0; i < NVOICE; i++) if (e->v[i].active && e->v[i].held) voice_release(&e->v[i]);
            e->sustain = on;
        } else if (msg[1] == 1) e->mod = msg[2] / 127.0f;
        else if (msg[1] == 120 || msg[1] == 123) for (int i = 0; i < NVOICE; i++) if (e->v[i].active) voice_release(&e->v[i]);
    }
}

/* ------------------------------------------------------------------------------------------------ the sound */
typedef struct { audio_t *a; float pos, sprd, gps, gain_base, ratio, pl, pr; int len; } lyr_t;

static inline float softclip(float x) {
    if (x > 3.0f) x = 3.0f;
    if (x < -3.0f) x = -3.0f;
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}
static inline float flush(float x) { return (x > -1e-18f && x < 1e-18f) ? 0.0f : x; }

static void spawn(inst_t *e, const lyr_t *ly, int L, int vi, int delay, float rev, float note_ratio, float vel, float mod) {
    grain_t *g = NULL;
    /* a free slot from where the last one was found; when none is near (the pool is full), the grain closest to its end among those looked at
     * is taken over, so a new grain is never dropped and the sound doesn't go hollow (a grain near its end is almost silent: no click) */
    int best = -1;
    float bestage = -1.0f;
    for (int k = 0; k < 48; k++) {
        int i = e->gnext + k;
        if (i >= NGRAIN) i -= NGRAIN;
        if (!e->g[i].active) { best = i; break; }
        float ar = (float)e->g[i].age / (float)(e->g[i].len > 0 ? e->g[i].len : 1);
        if (ar > bestage) { bestage = ar; best = i; }
    }
    if (best < 0) return;
    g = &e->g[best];
    e->gnext = best + 1 >= NGRAIN ? 0 : best + 1;
    float sprd = clampf(ly->sprd + mod * 0.5f, 0, 1);
    float p = ly->pos + (rnd01(e) * 2.0f - 1.0f) * sprd * 0.25f;
    p = clampf(p, 0.0f, 0.9999f);
    float inc = ly->ratio * note_ratio;
    inc *= exp2f((rnd01(e) * 2.0f - 1.0f) * e->val[P_JITT] * (2.0f / 12.0f));   /* each grain its own speed: up to +-2 semitones at full JITTER */
    int n = ly->a->n;
    g->d = ly->a->d; g->n = n; g->layer = L; g->voice = vi;
    float start = p * (float)n;
    if (rnd01(e) < rev) {   /* a backwards grain: starts at the far end of its span and reads toward the position */
        start = fmodf(start + (float)ly->len * inc, (float)n);
        g->inc = -inc;
    } else g->inc = inc;
    g->pos = start; g->len = ly->len; g->age = 0; g->wpos = 0; g->wstep = 512.0f / (float)ly->len;
    float gain = ly->gain_base * vel;
    g->gl = gain * ly->pl; g->gr = gain * ly->pr;
    g->delay = delay; g->active = 1;
}

static void render(void *p, int16_t *out, int frames) {
    inst_t *e = p;
    if (frames > MAXBLK) frames = MAXBLK;
    if (frames < 1) return;
#if defined(__arm__)
    unsigned fpscr_old;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr_old));
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr_old | (1u << 24)));   /* flush denormals to zero for the block */
#endif
    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    float gov = e->gov;
    float mixL[MAXBLK] = {0}, mixR[MAXBLK] = {0};
    float (*envb)[GR_LAYERS][MAXBLK] = e->envb;
    /* this block's settings */
    lyr_t ly[GR_LAYERS];
    float rev = e->val[P_REV], jitt = e->val[P_JITT];
    float bendst = e->bend * e->val[P_BEND];
    for (int L = 0; L < GR_LAYERS; L++) {
        ly[L].a = __atomic_load_n(&e->aud[L], __ATOMIC_ACQUIRE);
        ly[L].pos = e->val[P_POS1 + L];
        ly[L].sprd = e->val[P_SPRD1 + L];
        float sms = size_ms(e->val[P_SIZE1 + L]);
        ly[L].len = (int)(sms * 0.001f * SR);
        if (ly[L].len < 48) ly[L].len = 48;
        ly[L].gps = dens_gps(e->val[P_DENS1 + L]) * gov;   /* thinned when the block runs short of time */
        float overlap = ly[L].gps * (float)ly[L].len / SR;
        if (overlap > (float)VGRAIN) overlap = (float)VGRAIN;
        ly[L].gain_base = e->val[P_LEVEL1 + L] * 0.55f / sqrtf(overlap > 1.0f ? overlap : 1.0f);
        ly[L].ratio = powf(2.0f, (e->val[P_PITCH1 + L] + bendst) / 12.0f);
        float pan = e->val[P_PAN1 + L];
        ly[L].pl = cosf(pan * 1.5707963f); ly[L].pr = sinf(pan * 1.5707963f);
    }
    /* envelopes, per frame: every layer has its own attack, decay, sustain and release */
    int nv = 0;
    for (int vi = 0; vi < NVOICE; vi++) {
        voice_t *v = &e->v[vi];
        if (!v->active) { memset(envb[vi], 0, sizeof envb[vi]); continue; }
        int alive = 0;
        float top = 0;
        for (int L = 0; L < GR_LAYERS; L++) {
            float atk = env_s(e->val[P_ATK1 + L]) * SR, dec = env_s(e->val[P_DEC1 + L]) * SR, sus = e->val[P_SUS1 + L], rel = rel_s(e->val[P_REL1 + L]) * SR;
            float en = v->lenv[L];
            int st = v->stage[L];
            float *eb = envb[vi][L];
            for (int i = 0; i < frames; i++) {
                switch (st) {
                case 0: en += 1.0f / (atk < 1 ? 1 : atk); if (en >= 1.0f) { en = 1.0f; st = 1; } break;
                case 1: en -= (1.0f - sus) / (dec < 1 ? 1 : dec); if (en <= sus) { en = sus; st = 2; } break;
                case 2: break;
                case 3: en -= v->rel_from[L] / (rel < 1 ? 1 : rel); if (en <= 0.0005f) { en = 0; st = 4; } break;
                default: en = 0; break;
                }
                eb[i] = en;
            }
            v->lenv[L] = en; v->stage[L] = st;
            if (st != 4) alive = 1;
            if (en > top) top = en;
        }
        v->env = top;
        if (!alive) v->active = 0; else nv++;
    }
    e->nvoices_on = nv;
    /* spawn grains for the voices that are sounding: at most VGRAIN grains a voice */
    int vg[NVOICE];
    memset(vg, 0, sizeof vg);
    int total = 0;
    for (int gi = 0; gi < NGRAIN; gi++) if (e->g[gi].active) { vg[e->g[gi].voice]++; total++; }
    /* under pressure (thinned, or the pool nearly full) the voices that are only fading out give up grains first: a quarter of the cap */
    int pressure = gov < 0.999f || total > (NGRAIN * 3) / 4;
    int vcap = (int)((float)VGRAIN * (gov > 0.25f ? gov : 0.25f));
    if (vcap < 8) vcap = 8;
    for (int vi = 0; vi < NVOICE; vi++) {
        voice_t *v = &e->v[vi];
        if (!v->active) continue;
        float note_ratio = powf(2.0f, (float)(v->note - 60) / 12.0f);
        for (int L = 0; L < GR_LAYERS; L++) {
            if (!ly[L].a || e->val[P_LEVEL1 + L] < 0.002f) { v->cnt[L] = 0; continue; }
            float interval = SR / ly[L].gps;
            while (v->cnt[L] < (float)frames) {
                int d = (int)v->cnt[L];
                if (d < 0) d = 0;
                if (v->stage[L] < 3 || (v->stage[L] == 3 && v->lenv[L] > 0.001f)) {
                    int cap = (pressure && v->stage[0] >= 3 && v->stage[1] >= 3 && v->stage[2] >= 3 && v->stage[3] >= 3) ? vcap / 4 : vcap;
                    if (vg[vi] < cap) { spawn(e, &ly[L], L, vi, d, rev, note_ratio, v->vel, e->mod); vg[vi]++; }
                }
                v->cnt[L] += interval * (1.0f + jitt * (rnd01(e) * 2.0f - 1.0f) * 0.9f);
            }
            v->cnt[L] -= (float)frames;
        }
    }
    /* the grains */
    for (int gi = 0; gi < NGRAIN; gi++) {
        grain_t *g = &e->g[gi];
        if (!g->active) continue;
        const float *d = g->d;
        int n = g->n;
        float pos = g->pos, inc = g->inc, wpos = g->wpos, wstep = g->wstep;
        const float *env = envb[g->voice][g->layer];
        int i = g->delay, age = g->age, len = g->len;
        float gl = g->gl, gr = g->gr;
        for (; i < frames && age < len; i++, age++) {
            int i0 = (int)pos, i1 = i0 + 1;
            if (i1 >= n) i1 = 0;
            float fr = pos - (float)i0;
            float s = d[i0] + (d[i1] - d[i0]) * fr;
            float w = e->win[(int)wpos] * s * env[i];
            mixL[i] += w * gl; mixR[i] += w * gr;
            pos += inc; wpos += wstep;
            if (pos >= (float)n) pos -= (float)n; else if (pos < 0) pos += (float)n;
        }
        g->delay = 0; g->age = age; g->pos = pos; g->wpos = wpos;
        if (age >= len) g->active = 0;
    }
    /* filter (state-variable, TPT), delay, reverb, master */
    float cut_target = cut_hz(e->val[P_CUT]);
    e->fcut += (cut_target - e->fcut) * 0.35f;
    float fc = e->fcut > 18000.0f ? 18000.0f : e->fcut;
    float gF = tanf(3.14159265f * fc / SR), kF = 2.0f - 1.96f * e->val[P_RES];
    float a1 = 1.0f / (1.0f + gF * (gF + kF)), a2 = gF * a1, a3 = gF * a2;
    float ft = e->val[P_FTYPE], dr = 1.0f + e->val[P_DRIVE] * 7.0f, drc = 1.0f / sqrtf(dr);
    float wl = ft < 0.5f ? 1.0f - ft * 2.0f : 0.0f, wb = ft < 0.5f ? ft * 2.0f : 2.0f - ft * 2.0f, wh = ft < 0.5f ? 0.0f : ft * 2.0f - 1.0f;
    float dtime_t = dly_ms(e->val[P_DTIME]) * 0.001f * SR, dfb = e->val[P_DFB] * 0.92f, dmix = e->val[P_DMIX];
    float tone = e->val[P_DTONE], dlpa = 0.04f + 0.95f * tone * tone;
    float vfb = 0.70f + 0.28f * e->val[P_VSIZE], vd = e->val[P_VDAMP] * 0.4f, vmix = e->val[P_VMIX] * 3.0f;
    float vol = e->val[P_VOL], sg = e->val[P_WIDTH] * 1.25f;
    for (int i = 0; i < frames; i++) {
        float in[2] = {mixL[i], mixR[i]}, o[2];
        for (int c = 0; c < 2; c++) {
            float sx = softclip(in[c] * dr) * drc;
            float v3 = sx - e->fi2[c];
            float v1 = a1 * e->fi1[c] + a2 * v3;
            float v2 = e->fi2[c] + a2 * e->fi1[c] + a3 * v3;
            e->fi1[c] = flush(2.0f * v1 - e->fi1[c]);
            e->fi2[c] = flush(2.0f * v2 - e->fi2[c]);
            o[c] = wl * v2 + wb * (v1 * kF) + wh * (sx - kF * v1 - v2);
        }
        /* ping-pong delay with a tone filter in the feedback */
        e->dcur += (dtime_t - e->dcur) * 0.0008f;
        float rp = (float)e->dw - e->dcur;
        if (rp < 0) rp += (float)DMAX;
        int r0 = (int)rp;
        float rf = rp - (float)r0;
        if (r0 >= DMAX) r0 -= DMAX;
        int r1 = r0 + 1 >= DMAX ? 0 : r0 + 1;
        float tl = e->dl[0][r0] + (e->dl[0][r1] - e->dl[0][r0]) * rf, tr = e->dl[1][r0] + (e->dl[1][r1] - e->dl[1][r0]) * rf;
        e->dlp[0] = flush(e->dlp[0] + dlpa * (tr - e->dlp[0]));
        e->dlp[1] = flush(e->dlp[1] + dlpa * (tl - e->dlp[1]));
        e->dl[0][e->dw] = flush(o[0] + dfb * e->dlp[0]);
        e->dl[1][e->dw] = flush(o[1] + dfb * e->dlp[1]);
        if (++e->dw >= DMAX) e->dw = 0;
        o[0] += tl * dmix; o[1] += tr * dmix;
        /* reverb: 8 combs and 4 all-passes a side (Freeverb's layout) */
        float rin = (o[0] + o[1]) * 0.015f, rs[2] = {0, 0};
        for (int s = 0; s < 2; s++) {
            for (int c = 0; c < 8; c++) {
                float *b = e->comb[s][c];
                int idx = e->ci[s][c];
                float ov = b[idx];
                e->cf[s][c] = flush(ov * (1.0f - vd) + e->cf[s][c] * vd);
                b[idx] = flush(rin + e->cf[s][c] * vfb);
                if (++idx >= e->clen[s][c]) idx = 0;
                e->ci[s][c] = idx;
                rs[s] += ov;
            }
            for (int a = 0; a < 4; a++) {
                float *b = e->ap[s][a];
                int idx = e->ai[s][a];
                float bo = b[idx], inp = rs[s];
                rs[s] = bo - inp;
                b[idx] = flush(inp + bo * 0.5f);
                if (++idx >= e->alen[s][a]) idx = 0;
                e->ai[s][a] = idx;
            }
        }
        o[0] += rs[0] * vmix; o[1] += rs[1] * vmix;
        float m = (o[0] + o[1]) * 0.5f, sd = (o[0] - o[1]) * 0.5f * sg;
        float ol = softclip((m + sd) * vol * 1.3f), orr = softclip((m - sd) * vol * 1.3f);
        out[i * 2] = (int16_t)(ol * 32760.0f);
        out[i * 2 + 1] = (int16_t)(orr * 32760.0f);
    }
    /* what the screen shows: where the grains are now, and the status line */
    {
        int cnt[GR_LAYERS] = {0};
        for (int gi = 0; gi < NGRAIN; gi++) {
            const grain_t *g = &e->g[gi];
            if (g->active && g->n > 0 && cnt[g->layer] < GR_ACT) e->act[g->layer][cnt[g->layer]++] = g->pos / (float)g->n;
        }
        for (int L = 0; L < GR_LAYERS; L++) for (int c = cnt[L]; c < GR_ACT; c++) e->act[L][c] = -1.0f;
    }
    {   /* the load governor: how much of the block's time this block took; over 65% it thins the grains a little each block, under 45% it recovers slowly */
        struct timespec ts1;
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        float used = ((float)(ts1.tv_sec - ts0.tv_sec) + (float)(ts1.tv_nsec - ts0.tv_nsec) * 1e-9f) / ((float)frames / SR);
        e->load += (used - e->load) * 0.15f;
        if (e->load > 0.65f || used > 0.9f) { e->gov *= used > 0.9f ? 0.95f : 0.97f; if (e->gov < 0.12f) e->gov = 0.12f; }
        else if (e->load < 0.45f && e->gov < 1.0f) { e->gov += 0.004f; if (e->gov > 1.0f) e->gov = 1.0f; }
    }
    if (e->msg_left > 0 && (e->msg_left -= frames) <= 0) { e->msg_left = 0; snprintf(e->status, sizeof e->status, "%s", e->base_status); }
    if ((e->pub_count += frames) >= 1764) { e->pub_count = 0; publish(e); }
#if defined(__arm__)
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr_old));
#endif
}

static const mpc_engine_t ENGINE = {create, destroy, midi, set_param, get_param, render, NULL};
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
