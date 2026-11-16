// Audio renderer effects DSP: delay lines, the Delay effect (type 3) and the I3DL2 reverb (type 5), included by
// audio.c and by tools/test_audio_fx.c. The plain Reverb (type 4) stays in audio.c.
//
// Delay follows the public behaviour of the Switch renderer's delay as Ryujinx implements it (DelayCommand /
// DelayState, MIT): one delay line per channel, the delayed signal fed back through a channel-spread matrix and a
// one-pole low-pass. I3DL2 is a port of Ryujinx's Reverb3d (below).
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ delay lines (port of Ryujinx DelayLine)
typedef struct { float *buf; uint32_t sr, cur, last, count, max; } DelayLine;  // sr: samples per millisecond

static void dl_set(DelayLine *d, float ms) {
    uint32_t n = (uint32_t)roundf(d->sr * ms);
    d->count = n < d->max ? n : d->max;
    d->cur = 0;
    d->last = d->count ? d->count - 1 : 0;
}
static void dl_init(DelayLine *d, uint32_t sr, float ms_max) {
    d->sr = sr;
    d->max = (uint32_t)roundf(sr * ms_max);
    free(d->buf);
    d->buf = calloc(d->max + 1, sizeof(float));
    dl_set(d, ms_max);
}
static float dl_read(DelayLine *d) { return d->buf[d->cur]; }
// Below this a decaying tail is silence: flushed to 0 before it becomes denormal (a 10-40x slower float on x86).
#define FX_FLUSH(v) (fabsf(v) < 1e-20f ? 0.0f : (v))

static float dl_update(DelayLine *d, float v) {
    v = FX_FLUSH(v);
    float o = d->buf[d->cur];
    d->buf[d->cur++] = v;
    if (d->cur >= d->last) d->cur = 0;
    return o;
}
static float dl_tap_unsafe(DelayLine *d, uint32_t idx, int off) {
    int t = (int)d->cur - ((int)idx + off);
    if (t < 0) t += (int)d->count;
    if (t < 0 || (uint32_t)t > d->max) return 0;
    return d->buf[t];
}
static float dl_tap(DelayLine *d, uint32_t idx) {
    if (idx >= d->count) idx = d->count ? d->count - 1 : 0;
    return dl_tap_unsafe(d, idx, -1);
}

typedef struct { DelayLine dl; float rate; } DecayDelay;  // all-pass
static float dd_update(DecayDelay *d, float v) {
    float p = v - d->rate * dl_read(&d->dl);
    return dl_update(&d->dl, p) + p * d->rate;
}

// Every effect's parameters start with these (nn::audio, 6 channels at most).
typedef struct {
    uint8_t input[6], output[6];
    uint16_t ch_max, ch;
} FxIo;

// ------------------------------------------------------------------ Delay (type 3)
typedef struct {  // DelayParameter: times in ms, sample rate and gains Q14
    FxIo io;
    uint32_t delay_max, delay, sample_rate, in_gain, feedback, out_gain, dry_gain, spread, lowpass;
} DelayParam;

typedef struct {
    DelayParam p;
    DelayLine line[6];
    float z[6], fb_raw, fb_base, fb_cross, lp_fb, lp_base;
    uint32_t cur_delay;
} FxDelay;

static float fx_q14(uint32_t v) { return (int32_t)v / 16384.0f; }

static void fx_delay_update(FxDelay *d) {
    DelayParam *p = &d->p;
    if (p->delay != d->cur_delay) {
        for (int c = 0; c < 6; c++) dl_set(&d->line[c], (float)(p->delay < p->delay_max ? p->delay : p->delay_max));
        d->cur_delay = p->delay;
    }
    d->fb_raw = fx_q14(p->feedback);  // (mono and the LFE: their own feedback, unscaled)
    float fb = d->fb_raw * 0.98f, spread = fx_q14(p->spread);
    d->fb_base = (1 - spread) * fb;
    d->fb_cross = spread * fb * (p->io.ch == 4 || p->io.ch == 6 ? 0.5f : 1.0f);
    d->lp_fb = 0.95f * fx_q14(p->lowpass);
    d->lp_base = 1 - d->lp_fb;
}

static void fx_delay_init(FxDelay *d) {
    uint32_t sr = (uint32_t)(fx_q14(d->p.sample_rate) / 1000);
    if (!sr || sr > 192) sr = 48;
    uint32_t max = d->p.delay_max < 5000 ? d->p.delay_max : 5000;
    for (int c = 0; c < 6; c++) dl_init(&d->line[c], sr, (float)max);
    memset(d->z, 0, sizeof d->z);
    d->cur_delay = ~0u;
    fx_delay_update(d);
}

static void fx_delay_free(FxDelay *d) {
    for (int c = 0; c < 6; c++) free(d->line[c].buf), d->line[c].buf = NULL;
}

// Which channels feed each other's delay line: front pairs, sides and the centre (FL FR FC LFE BL BR).
static const int8_t fx_delay_nb[4][6][2] = {
    {{-1, -1}},
    {{1, -1}, {0, -1}},
    {{1, 2}, {0, 3}, {0, 3}, {1, 2}},
    {{2, 4}, {2, 5}, {0, 1}, {-1, -1}, {0, 5}, {1, 4}},
};

static void fx_delay_process(FxDelay *d, float *const *in, float *const *out, uint32_t n) {
    int ch = d->p.io.ch, k = ch == 1 ? 0 : ch == 2 ? 1 : ch == 4 ? 2 : ch == 6 ? 3 : -1;
    if (k < 0) return;
    float in_gain = fx_q14(d->p.in_gain), out_gain = fx_q14(d->p.out_gain), dry = fx_q14(d->p.dry_gain);
    for (uint32_t s = 0; s < n; s++) {
        float x[6], y[6];
        for (int c = 0; c < ch; c++) x[c] = in[c][s], y[c] = dl_read(&d->line[c]);
        for (int c = 0; c < ch; c++) {
            float fb = ch == 1 || (ch == 6 && c == 3) ? y[c] * d->fb_raw : y[c] * d->fb_base;
            for (int j = 0; j < 2; j++) if (fx_delay_nb[k][c][j] >= 0) fb += y[fx_delay_nb[k][c][j]] * d->fb_cross;
            d->z[c] = FX_FLUSH(d->lp_fb * d->z[c] + d->lp_base * (x[c] * in_gain + fb));
            dl_update(&d->line[c], d->z[c]);
            out[c][s] = x[c] * dry + y[c] * out_gain;
        }
    }
}

// ------------------------------------------------------------------ I3DL2 reverb (type 5)
// Port of Ryujinx's Reverb3dState / Reverb3dCommand / DelayLine3d (MIT, third_party/ryujinx/LICENSE.txt): early
// reflections from 20 taps of a pre-delay line, then a late reverb of 4 feedback delay lines, each through two
// decaying all-passes and a high-frequency shelf set from DecayTime, DecayHFRatio and HFReference.
typedef struct {  // I3dl2ReverbParameter: levels in mB, times in s, diffusion and density in %
    FxIo io;
    uint32_t reserved, sample_rate;  // Hz
    float room_hf, hf_reference, decay_time, hf_decay_ratio, room, reflections, reverb, diffusion, reflections_delay,
        reverb_delay, density, dry_gain;
} I3dl2Param;

// A delay line with separate read and write positions (DelayLine3d), so taps look back from the latest write.
typedef struct { float *buf; uint32_t sr, cur, last, count, max; } Dl3;

static void dl3_set(Dl3 *d, float ms) {
    uint32_t n = (uint32_t)roundf(d->sr * ms);
    if (n > d->max) return;
    d->count = n;
    d->last = (d->cur + n) % (d->max + 1);
}
static void dl3_init(Dl3 *d, uint32_t sr, float ms_max) {
    d->sr = sr;
    d->max = (uint32_t)roundf(sr * ms_max);
    free(d->buf);
    d->buf = calloc(d->max + 1, sizeof(float));
    d->cur = 0;
    dl3_set(d, ms_max);
}
static float dl3_read(Dl3 *d) { return d->buf[d->cur]; }
static float dl3_update(Dl3 *d, float v) {
    d->buf[d->last++] = FX_FLUSH(v);
    float o = d->buf[d->cur++];
    if (d->cur > d->max) d->cur = 0;  // (all max + 1 slots: Ryujinx wraps at max, so slot max keeps its first sample)
    if (d->last > d->max) d->last = 0;
    return o;
}
static float dl3_tap(Dl3 *d, uint32_t idx) {  // idx samples before the latest write
    int t = (int)d->last - (int)idx - 1;
    if (t < 0) t += (int)d->max + 1;
    return d->buf[t];
}

typedef struct { Dl3 dl; float rate; } Dl3Decay;  // all-pass (DecayDelay)
static float dl3_decay(Dl3Decay *d, float v) {
    float p = v - d->rate * dl3_read(&d->dl);
    return dl3_update(&d->dl, p) + p * d->rate;
}

typedef struct {
    I3dl2Param p;
    Dl3 fdn[4], pre, fc;
    Dl3Decay decay1[4], decay2[4];
    uint32_t early_time[20], reflection_time;
    float dry_gain, prev_pre, prev_pre_gain, target_pre_gain, early_gain, late_gain;
    float direct_gain[4], current_gain[4], output_gain[4], prev_fb[4];
} FxI3dl2;

static const float i3_fdn_min[4] = {5.0f, 6.0f, 13.0f, 14.0f}, i3_fdn_max[4] = {45.704f, 82.782f, 149.94f, 271.58f};
static const float i3_decay_max1[4] = {17.0f, 13.0f, 9.0f, 7.0f}, i3_decay_max2[4] = {19.0f, 11.0f, 10.0f, 6.0f};
static const float i3_early_times[20] = {0.017136f, 0.059154f, 0.16173f, 0.39019f, 0.42526f, 0.45541f, 0.68974f,
    0.74591f, 0.83384f, 0.8595f, 0.0f, 0.075024f, 0.16879f, 0.2999f, 0.33744f, 0.3719f, 0.59901f, 0.71674f,
    0.81786f, 0.85166f};
static const float i3_early_gains[20] = {0.67096f, 0.61027f, 1.0f, 0.35680f, 0.68361f, 0.65978f, 0.51939f,
    0.24712f, 0.45945f, 0.45021f, 0.64196f, 0.54879f, 0.92925f, 0.38270f, 0.72867f, 0.69794f, 0.5464f, 0.24563f,
    0.45214f, 0.44042f};

// 10^x as the console computes it: 1 at and above 0, 0 below -5.3
static float i3_pow10(float x) { return x >= 0 ? 1.0f : x <= -5.3f ? 0.0f : powf(10.0f, x); }
static float i3_cosd(float deg) { return cosf(deg * (float)M_PI / 180.0f); }
static float i3_sind(float deg) { return sinf(deg * (float)M_PI / 180.0f); }

static void fx_i3dl2_update(FxI3dl2 *r) {
    I3dl2Param *p = &r->p;
    uint32_t sr = r->pre.sr;
    float fs = sr * 1000.0f;
    r->dry_gain = p->dry_gain;
    memset(r->prev_fb, 0, sizeof r->prev_fb);
    r->prev_pre = 0;
    r->early_gain = i3_pow10(fminf(p->room + p->reflections, 5000.0f) / 2000.0f);
    r->late_gain = i3_pow10(fminf(p->room + p->reverb, 5000.0f) / 2000.0f);
    float hf_room = i3_pow10(p->room_hf / 2000.0f);  // RoomHF: a low-pass on what enters the pre-delay
    if (hf_room < 1.0f) {
        float a = 1.0f - hf_room, b = 2.0f - 2.0f * hf_room * i3_cosd(256.0f * p->hf_reference / fs);
        float c = sqrtf(fmaxf(0, b * b - 4.0f * a * a));
        r->prev_pre_gain = (b - c) / (2.0f * a);
        r->target_pre_gain = 1.0f - r->prev_pre_gain;
    } else {
        r->prev_pre_gain = 0;
        r->target_pre_gain = 1;
    }
    r->reflection_time = (uint32_t)roundf(sr * 1000.0f * (p->reflections_delay + p->reverb_delay));
    if (r->reflection_time > r->pre.max) r->reflection_time = r->pre.max;
    float diffusion = 0.6f * p->diffusion * 0.01f;
    for (int i = 0; i < 4; i++) {
        dl3_set(&r->fdn[i], i3_fdn_min[i] + p->density / 100 * (i3_fdn_max[i] - i3_fdn_min[i]));
        float len = (float)(r->fdn[i].count + r->decay1[i].dl.count + r->decay2[i].dl.count);
        float a = -60.0f * len / (p->decay_time * fs), b = a / p->hf_decay_ratio;  // dB per loop: all, and at HF
        float c = i3_cosd(64.0f * p->hf_reference / fs) / i3_sind(64.0f * p->hf_reference / fs);
        float d = i3_pow10((b - a) / 40.0f), e = i3_pow10((b + a) / 40.0f) * 0.7071f;
        r->direct_gain[i] = e * (d * c + 1.0f) / (c + d);
        r->current_gain[i] = e * (1.0f - d * c) / (c + d);
        r->output_gain[i] = (c - d) / (c + d);
        r->decay1[i].rate = diffusion;
        r->decay2[i].rate = diffusion * -0.9f;
    }
    for (int i = 0; i < 20; i++) {
        uint32_t n = (uint32_t)roundf(sr * (p->reflections_delay * 1000.0f +
                                            i3_early_times[i] * 1000.0f * (p->reverb_delay * 0.9998f + 0.02f)));
        r->early_time[i] = n < r->pre.max ? n : r->pre.max;
    }
}

static void fx_i3dl2_init(FxI3dl2 *r) {
    uint32_t sr = r->p.sample_rate / 1000;
    if (!sr || sr > 192) sr = 48;
    for (int i = 0; i < 4; i++) {
        dl3_init(&r->fdn[i], sr, i3_fdn_max[i]);
        dl3_init(&r->decay1[i].dl, sr, i3_decay_max1[i]);
        dl3_init(&r->decay2[i].dl, sr, i3_decay_max2[i]);
    }
    dl3_init(&r->pre, sr, 400);
    dl3_init(&r->fc, sr, 5);
    fx_i3dl2_update(r);
}

static void fx_i3dl2_free(FxI3dl2 *r) {
    Dl3 *all[] = {&r->pre, &r->fc, &r->fdn[0], &r->fdn[1], &r->fdn[2], &r->fdn[3], &r->decay1[0].dl,
                  &r->decay1[1].dl, &r->decay1[2].dl, &r->decay1[3].dl, &r->decay2[0].dl, &r->decay2[1].dl,
                  &r->decay2[2].dl, &r->decay2[3].dl};
    for (unsigned i = 0; i < sizeof all / sizeof *all; i++) free(all[i]->buf), all[i]->buf = NULL;
}

static void fx_i3dl2_process(FxI3dl2 *r, float *const *in, float *const *out, uint32_t n) {
    // per channel count: the output each early tap goes to (6 channels: Ryujinx adds each tap twice), and the late
    // line each output takes
    static const int8_t early1[20] = {0}, early2[20] = {0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1};
    static const int8_t early4[20] = {0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 0, 0, 0, 0, 3, 3, 3};
    static const int8_t early6[20] = {4, 5, 0, 5, 0, 5, 1, 5, 1, 5, 1, 5, 1, 5, 2, 5, 2, 5, 2, 5};
    static const int8_t late1[1] = {0}, late2[2] = {0, 1}, late4[4] = {0, 1, 2, 3}, late6[6] = {0, 1, 2, 3, -1, 3};
    int ch = r->p.io.ch;
    const int8_t *early, *late;
    switch (ch) {
    case 1: early = early1, late = late1; break;
    case 2: early = early2, late = late2; break;
    case 4: early = early4, late = late4; break;
    case 6: early = early6, late = late6; break;
    default: return;
    }
    for (uint32_t s = 0; s < n; s++) {
        float o[6] = {0}, x[6], fo[4], fb[4], v[4], sum = 0;
        float tap = dl3_tap(&r->pre, r->reflection_time);
        for (int i = 0; i < 20; i++) {
            float e = dl3_tap(&r->pre, r->early_time[i]) * i3_early_gains[i];
            o[early[i]] += ch == 6 ? 2 * e : e;
        }
        for (int c = 0; c < ch; c++) x[c] = in[c][s], sum += x[c], o[c] *= r->early_gain;
        r->prev_pre = FX_FLUSH(sum * r->target_pre_gain + r->prev_pre * r->prev_pre_gain);
        dl3_update(&r->pre, r->prev_pre);
        for (int i = 0; i < 4; i++) {
            float f = dl3_read(&r->fdn[i]);
            fo[i] = f * r->direct_gain[i] + r->prev_fb[i];
            r->prev_fb[i] = FX_FLUSH(f * r->current_gain[i] + fo[i] * r->output_gain[i]);
        }
        fb[0] = fo[2] + fo[1];
        fb[1] = -fo[0] - fo[3];
        fb[2] = fo[0] - fo[3];
        fb[3] = fo[1] - fo[2];
        for (int i = 0; i < 4; i++) {
            v[i] = dl3_decay(&r->decay2[i], dl3_decay(&r->decay1[i], tap * r->late_gain + fb[i]));
            dl3_update(&r->fdn[i], v[i]);
        }
        for (int c = 0; c < ch; c++)
            if (late[c] >= 0) out[c][s] = o[c] + v[late[c]] + x[c] * r->dry_gain;
        if (ch == 1) out[0][s] += v[1];
        if (ch == 6) out[4][s] = o[4] + dl3_update(&r->fc, (v[2] - v[3]) * 0.5f) + x[4] * r->dry_gain;
    }
}
