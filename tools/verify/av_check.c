// S4's decoder check: decode whole movies and Nintendo Opus streams with the FFmpeg the runtime links, and say what
// the runtime's movie and audio code will meet. One line per file on stdout: "OK" or "FAIL", the path, the facts.
//   av_check movie <file>...    every stream is opened with the decoder the runtime would pick and decoded to the end
//   av_check opus <file>...     a Nintendo Opus file (header, then packets of u32be size, u32be range, data), as hwopus gets them
// Build: make build/verify/av_check.exe (tools/verify/verify.mk). Nothing is written.
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int64_t frames, samples; int errors; int fail; } Stats;

static void drain(AVCodecContext *ctx, AVFrame *fr, Stats *st, const char *kind, char *why) {
    for (;;) {
        int r = avcodec_receive_frame(ctx, fr);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
        if (r < 0) { st->errors++; return; }
        st->frames++;
        st->samples += fr->nb_samples;
        // what hle/movie.c assumes of a decoded frame: video is 8-bit 4:2:0 planes of even size, audio is planar float
        if (ctx->codec_type == AVMEDIA_TYPE_VIDEO && (fr->format != AV_PIX_FMT_YUV420P || (fr->width & 1) || (fr->height & 1)) && !st->fail) {
            st->fail = 1;
            snprintf(why, 160, "video frame %s %dx%d, the runtime reads yuv420p of even size", av_get_pix_fmt_name(fr->format), fr->width, fr->height);
        }
        if (ctx->codec_type == AVMEDIA_TYPE_AUDIO && fr->format != AV_SAMPLE_FMT_FLTP && !st->fail) {
            st->fail = 1;
            snprintf(why, 160, "audio frame %s, the runtime reads fltp", av_get_sample_fmt_name(fr->format));
        }
        (void)kind;
    }
}

static AVCodecContext *open_decoder(enum AVCodecID id, AVCodecParameters *par, char *why) {
    const AVCodec *c = avcodec_find_decoder(id);
    if (!c) { snprintf(why, 160, "this FFmpeg has no decoder for %s", avcodec_get_name(id)); return NULL; }
    AVCodecContext *ctx = avcodec_alloc_context3(c);
    if (par) avcodec_parameters_to_context(ctx, par);
    ctx->thread_count = 4;  // (as hle/movie.c's decoder)
    if (avcodec_open2(ctx, c, NULL) < 0) { snprintf(why, 160, "cannot open decoder %s", c->name); avcodec_free_context(&ctx); return NULL; }
    return ctx;
}

static int movie(const char *path) {
    AVFormatContext *fc = NULL;
    char why[160] = "", line[512];
    if (avformat_open_input(&fc, path, NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0) {
        printf("FAIL %s cannot open as a movie (the runtime's FFmpeg has the matroska and mov demuxers only)\n", path);
        return 1;
    }
    AVCodecContext *ctx[16] = {0};
    Stats st[16] = {0};
    int n = fc->nb_streams < 16 ? (int)fc->nb_streams : 16, bad = 0;
    for (int i = 0; i < n; i++) {
        AVCodecParameters *p = fc->streams[i]->codecpar;
        if (p->codec_type != AVMEDIA_TYPE_VIDEO && p->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        ctx[i] = open_decoder(p->codec_id, p, why);
        if (!ctx[i]) bad = 1;
    }
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    while (!bad && av_read_frame(fc, pkt) >= 0) {
        int i = pkt->stream_index;
        if (i < n && ctx[i]) {
            if (avcodec_send_packet(ctx[i], pkt) < 0) st[i].errors++;
            drain(ctx[i], fr, &st[i], "movie", why);
        }
        av_packet_unref(pkt);
    }
    int off = snprintf(line, sizeof line, "%s", fc->iformat->name);
    for (int i = 0; i < n; i++) {
        if (!ctx[i]) continue;
        avcodec_send_packet(ctx[i], NULL);
        drain(ctx[i], fr, &st[i], "movie", why);
        AVCodecParameters *p = fc->streams[i]->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO)
            off += snprintf(line + off, sizeof line - off, " | video %s %dx%d %s frames %lld", avcodec_get_name(p->codec_id), p->width, p->height,
                            av_get_pix_fmt_name(p->format), (long long)st[i].frames);
        else
            off += snprintf(line + off, sizeof line - off, " | audio %s %dch %dHz %s samples %lld", avcodec_get_name(p->codec_id),
                            p->ch_layout.nb_channels, p->sample_rate, av_get_sample_fmt_name(p->format), (long long)st[i].samples);
        if (st[i].errors) { bad = 1; snprintf(why, sizeof why, "stream %d: %d decode errors", i, st[i].errors); }
        if (st[i].fail) bad = 1;
        if (!st[i].frames) { bad = 1; snprintf(why, sizeof why, "stream %d decoded nothing", i); }
    }
    printf("%s %s %s%s%s\n", bad ? "FAIL" : "OK", path, line, why[0] ? " | " : "", why);
    return bad;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }

static int opus(const char *path) {
    FILE *f = fopen(path, "rb");
    char why[160] = "";
    if (!f) { printf("FAIL %s cannot open\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(size);
    if (fread(b, 1, size, f) != (size_t)size) { printf("FAIL %s short read\n", path); return 1; }
    fclose(f);
    int bad = 0, ch = 0, rate = 0;
    long pos = 0, packets = 0;
    Stats st = {0};
    if (size < 0x30 || le32(b) != 0x80000001u) { snprintf(why, sizeof why, "no Nintendo Opus header"); bad = 1; }
    else {
        ch = b[9];
        rate = (int)le32(b + 0xC);
        pos = le32(b + 0x10);
        if (pos + 8 > size || le32(b + pos) != 0x80000004u) { snprintf(why, sizeof why, "no data chunk at %ld", pos); bad = 1; }
        pos += 8;
    }
    AVCodecContext *ctx = bad ? NULL : open_decoder(AV_CODEC_ID_OPUS, NULL, why);
    if (!bad && !ctx) bad = 1;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    while (!bad && pos + 8 <= size) {
        uint32_t n = be32(b + pos);
        if (n == 0 || pos + 8 + (long)n > size) { snprintf(why, sizeof why, "packet %ld of %u bytes runs past the end", packets, n); bad = 1; break; }
        pkt->data = b + pos + 8;
        pkt->size = (int)n;
        if (avcodec_send_packet(ctx, pkt) < 0) st.errors++;
        drain(ctx, fr, &st, "opus", why);
        pos += 8 + n;
        packets++;
    }
    if (!bad && pos != size) { snprintf(why, sizeof why, "%ld stray bytes after the last packet", size - pos); bad = 1; }
    if (!bad && st.errors) { snprintf(why, sizeof why, "%d packets failed to decode", st.errors); bad = 1; }
    if (!bad && st.fail) bad = 1;
    printf("%s %s opus %dch %dHz packets %ld samples %lld%s%s\n", bad ? "FAIL" : "OK", path, ch, rate, packets, (long long)st.samples,
           why[0] ? " | " : "", why);
    return bad;
}

int main(int argc, char **argv) {
    if (argc < 3 || (strcmp(argv[1], "movie") && strcmp(argv[1], "opus"))) {
        fprintf(stderr, "usage: av_check movie|opus <file>...\n");
        return 2;
    }
    av_log_set_level(AV_LOG_FATAL);
    int bad = 0;
    for (int i = 2; i < argc; i++) bad += argv[1][0] == 'm' ? movie(argv[i]) : opus(argv[i]);
    fflush(stdout);
    return bad ? 1 : 0;
}
