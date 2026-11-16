// The audio renderer's Delay and I3DL2 reverb effects (runtime/services/audio_fx.h) on synthetic signals, with the
// parameter sets Kirby uses (its title screen and its first stage), and their cost per 240-sample audio frame.
//   clang -O2 -o test_audio_fx tools/test_audio_fx.c && ./test_audio_fx
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <time.h>

#include "../runtime/services/audio_fx.h"

#define FS 48000
#define FRAME 240
#define Q14(x) ((uint32_t)((x) * 16384.0f + 0.5f))

static float *L, *R, *OL, *OR;

// Run n samples of in (stereo, already in L/R) through fn into OL/OR, one audio frame at a time.
static void run(void (*fn)(void *, float *const *, float *const *, uint32_t), void *fx, int n) {
    for (int s = 0; s < n; s += FRAME) {
        float *in[2] = {L + s, R + s}, *out[2] = {OL + s, OR + s};
        fn(fx, in, out, FRAME);
    }
}
static void delay_fn(void *fx, float *const *in, float *const *out, uint32_t n) { fx_delay_process(fx, in, out, n); }
static void i3_fn(void *fx, float *const *in, float *const *out, uint32_t n) { fx_i3dl2_process(fx, in, out, n); }

static double energy(const float *a, int from, int to) {
    double e = 0;
    for (int i = from; i < to; i++) e += (double)a[i] * a[i];
    return e;
}
static float peak(const float *a, int n) {
    float p = 0;
    for (int i = 0; i < n; i++) {
        assert(a[i] == a[i]);  // no NaN
        if (fabsf(a[i]) > p) p = fabsf(a[i]);
    }
    return p;
}
static uint32_t rng = 1;
static float noise(void) { rng = rng * 1664525u + 1013904223u; return (int32_t)rng / 2147483648.0f; }

static void stereo_io(FxIo *io) {
    memset(io, 0, sizeof *io);
    io->input[0] = io->output[0] = 6, io->input[1] = io->output[1] = 7;
    io->ch_max = io->ch = 2;
}

static FxDelay make_delay(float in, float fb, float out, float dry, float spread, float lp) {
    FxDelay d = {0};
    stereo_io(&d.p.io);
    d.p.delay_max = 2000, d.p.delay = 160, d.p.sample_rate = 48000u << 14;
    d.p.in_gain = Q14(in), d.p.feedback = Q14(fb), d.p.out_gain = Q14(out), d.p.dry_gain = Q14(dry);
    d.p.spread = Q14(spread), d.p.lowpass = Q14(lp);
    fx_delay_init(&d);
    return d;
}

static FxI3dl2 make_i3(float room_hf, float hf_ref, float decay, float ratio, float room, float refl, float reverb,
                       float diffusion, float refl_delay, float reverb_delay, float density, float dry) {
    FxI3dl2 r = {0};
    stereo_io(&r.p.io);
    r.p.sample_rate = FS;
    r.p.room_hf = room_hf, r.p.hf_reference = hf_ref, r.p.decay_time = decay, r.p.hf_decay_ratio = ratio;
    r.p.room = room, r.p.reflections = refl, r.p.reverb = reverb, r.p.diffusion = diffusion;
    r.p.reflections_delay = refl_delay, r.p.reverb_delay = reverb_delay, r.p.density = density, r.p.dry_gain = dry;
    fx_i3dl2_init(&r);
    return r;
}

// Seconds for the tail's energy (Schroeder backward integral) to fall from -5 dB to -25 dB, times 3: T60.
static double t60(const float *a, const float *b, int from, int n) {
    double total = energy(a, from, n) + energy(b, from, n), left = total, t5 = -1, t25 = -1;
    for (int i = from; i < n; i++) {
        left -= (double)a[i] * a[i] + (double)b[i] * b[i];
        double db = 10 * log10(left / total + 1e-30);
        if (t5 < 0 && db <= -5) t5 = i;
        if (t25 < 0 && db <= -25) { t25 = i; break; }
    }
    assert(t5 > 0 && t25 > 0);
    return (t25 - t5) / FS * 3;
}

static double cost_us(void (*fn)(void *, float *const *, float *const *, uint32_t), void *fx) {
    int frames = 20000;
    for (int i = 0; i < FS * 2; i++) L[i] = noise() * 0.3f, R[i] = noise() * 0.3f;
    clock_t t0 = clock();
    for (int k = 0; k < frames; k++) {
        int s = (k % (FS * 2 / FRAME)) * FRAME;
        float *in[2] = {L + s, R + s}, *out[2] = {OL + s, OR + s};
        fn(fx, in, out, FRAME);
    }
    return (double)(clock() - t0) / CLOCKS_PER_SEC * 1e6 / frames;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int n = FS * 20;
    L = calloc(n, 4), R = calloc(n, 4), OL = calloc(n, 4), OR = calloc(n, 4);

    // Delay, the title screen's set: dry at once, echoes every 160 ms on both sides (spread), each quieter
    FxDelay d = make_delay(0.5f, 0.4f, 1.0f, 0.5f, 0.3f, 0.8f);
    L[0] = 1;
    run(delay_fn, &d, n);
    assert(fabsf(OL[0] - 0.5f) < 1e-6f && OR[0] == 0);
    int step = 160 * 48, prev_i = 0;
    double prev = 1e9;
    for (int k = 1; k <= 4; k++) {
        double e = energy(OL, k * step - 200, k * step + 2000) + energy(OR, k * step - 200, k * step + 2000);
        double between = energy(OL, (k - 1) * step + 2400, k * step - 200);
        printf("delay echo %d: energy %.3g (between echoes %.3g)\n", k, e, between);
        assert(e > 1e-6 && e < prev && between < e * 0.01);
        prev = e, prev_i = k;
    }
    assert(prev_i == 4 && energy(OR, step - 200, step + 2000) == 0 && energy(OR, 2 * step - 200, 2 * step + 2000) > 0);  // (spread: the other side from the second echo)
    assert(energy(OL, n - FS, n) + energy(OR, n - FS, n) < 1e-12);
    printf("delay: peak %.3f\n", peak(OL, n));
    assert(peak(OL, n) <= 1 && peak(OR, n) <= 1);

    // Delay, the first stage's set (in 0, feedback 0, dry 0): the bus goes quiet
    fx_delay_free(&d);
    d = make_delay(0, 0, 1.0f, 0, 0.3f, 0);
    for (int i = 0; i < FS; i++) L[i] = noise(), R[i] = noise();
    run(delay_fn, &d, FS);
    assert(peak(OL, FS) == 0 && peak(OR, FS) == 0);
    fx_delay_free(&d);

    // I3DL2, the title screen's set: a tail that lasts about DecayTime, no clipping, no blow-up
    memset(L, 0, n * 4), memset(R, 0, n * 4);
    FxI3dl2 r = make_i3(-1000, 6000, 2.0f, 0.8f, -1000, -700, -500, 50, 0.005f, 0.04f, 3.0f, 0.25f);
    L[0] = 1;
    run(i3_fn, &r, n);
    assert(fabsf(OL[0] - 0.25f) < 1e-6f);
    double tail = energy(OL, FS / 10, FS / 2) + energy(OR, FS / 10, FS / 2);
    double t = t60(OL, OR, FS / 10, n);
    printf("i3dl2: tail energy 0.1-0.5 s %.3g, T60 %.2f s (DecayTime 2.0), peak %.3f\n", tail, t, peak(OL, n));
    assert(tail > 1e-4 && t > 1.4 && t < 2.6);
    assert(energy(OL, n - FS, n) + energy(OR, n - FS, n) < tail * 1e-9);
    // 20 s on, the tail is flushed to zeros, not denormals (each one costs tens of times a normal float on x86)
    for (int i = 0; i < 4; i++)
        for (uint32_t k = 0; k <= r.fdn[i].max; k++) assert(r.fdn[i].buf[k] == 0 || fabsf(r.fdn[i].buf[k]) > 1e-30f);
    for (uint32_t k = 0; k <= r.pre.max; k++) assert(r.pre.buf[k] == 0 || fabsf(r.pre.buf[k]) > 1e-30f);
    fx_i3dl2_free(&r);

    // I3DL2: high frequencies die sooner with DecayHFRatio 0.3 than with 1.0 (energy above HFReference late on)
    double hf[2];
    for (int k = 0; k < 2; k++) {
        r = make_i3(0, 5000, 2.0f, k ? 0.3f : 1.0f, 0, 0, 0, 100, 0, 0, 100, 0);
        memset(L, 0, n * 4), memset(R, 0, n * 4), L[0] = 1;
        run(i3_fn, &r, FS * 2);
        double e = 0;
        for (int i = FS / 2; i < FS * 2 - 1; i++) e += (OL[i + 1] - OL[i]) * (OL[i + 1] - OL[i]);  // (a first difference: mostly HF)
        hf[k] = e / energy(OL, FS / 2, FS * 2);
        fx_i3dl2_free(&r);
    }
    printf("i3dl2: late HF share %.3g (ratio 1.0) vs %.3g (ratio 0.3)\n", hf[0], hf[1]);
    assert(hf[1] < hf[0] * 0.5);

    // I3DL2, the first stage's set (the SDK's minimum times, 0 mB levels, dry 0), with loud noise: bounded output (the
    // 20 early taps at 0 mB sum to 11.4 times the summed input) that dies away within 0.5 s
    r = make_i3(0, 20, 0.1f, 0.1f, 0, 0, 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < FS * 2; i++) L[i] = noise() * 0.5f, R[i] = noise() * 0.5f;
    memset(L + FS * 2, 0, (n - FS * 2) * 4), memset(R + FS * 2, 0, (n - FS * 2) * 4);
    run(i3_fn, &r, FS * 4);
    printf("i3dl2 short: peak %.3f, after 0.5 s of silence %.3g\n", peak(OL, FS * 4), peak(OL + FS * 2 + FS / 2, FS));
    assert(peak(OL, FS * 4) < 11.4f && peak(OL + FS * 2 + FS / 2, FS) < 1e-4f);
    fx_i3dl2_free(&r);

    // all wet levels at -10000 mB: only the dry signal
    r = make_i3(0, 5000, 2.0f, 0.8f, -10000, -10000, -10000, 50, 0.01f, 0.02f, 50, 1.0f);
    for (int i = 0; i < FS; i++) L[i] = noise(), R[i] = noise();
    run(i3_fn, &r, FS);
    for (int i = 0; i < FS; i++) assert(fabsf(OL[i] - L[i]) < 1e-4f && fabsf(OR[i] - R[i]) < 1e-4f);
    fx_i3dl2_free(&r);

    d = make_delay(0.5f, 0.4f, 1.0f, 0.5f, 0.3f, 0.8f);
    r = make_i3(-1000, 6000, 2.0f, 0.8f, -1000, -700, -500, 50, 0.005f, 0.04f, 3.0f, 0.25f);
    printf("cost per 240-sample stereo frame (5 ms of audio): delay %.2f us, i3dl2 %.2f us\n", cost_us(delay_fn, &d),
           cost_us(i3_fn, &r));
    printf("audio fx: ok\n");
    return 0;
}
