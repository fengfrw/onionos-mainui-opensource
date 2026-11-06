/* SPDX-License-Identifier: GPL-3.0-only
 * A stand-in for the FFmpeg 2.4 libraries Onion ships, for host tests of the
 * media gallery's video playback. The tests link it under the four sonames.
 * A "video" is a text file:
 *
 *   STUB <width> <height> <frames> <fps> [nopts] [slow] [noseek] [nocodec]
 *        [lie] [nodims] [cover] [extra] [waitopen] [waitprobe] [waitread]
 *
 * Frame i decodes one frame late, as H.264 decoders hold frames, and
 * converts to a solid colour from i (stub_colour()). The codec context tells
 * the size before decoding, or 320x240 with lie, so only the decoded frames
 * show it, or nothing with nodims. cover adds cover art (an attached
 * picture, its size unknown), extra a second video stream of 4000x3000.
 * Probing reports, as an error, decoding any video stream not stated to be
 * within 1280x720, cover art included,
 * and decoding takes each picture's buffer through the codec context's
 * get_buffer2 at the size the stream really has, reporting as an error a
 * buffer taken for a picture over 1280x720 (rounded up to 16-pixel blocks).
 * grow<n> makes pictures from the nth on 4000x3000. waitopen, waitprobe and waitread hold
 * the open, the probe or the first read for 3 s unless the interrupt
 * callback stops them. Anything else fails to open. Built with
 * -DSTUB_WITHOUT_SCALE it lacks sws_scale. Every allocation is counted, and
 * the count left at unload is printed; errors are printed with "[ffstub]". */
#define _POSIX_C_SOURCE 200809L
#include "platform/ffmpeg_abi.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int live;

static void *allocate(size_t size)
{
    void *memory = calloc(1, size);
    live += memory != NULL;
    return memory;
}

static void release(void *memory)
{
    live -= memory != NULL;
    free(memory);
}

__attribute__((destructor)) static void report(void)
{
    if (live) {
        fprintf(stderr, "[ffstub] %d allocations left\n", live);
    }
}

typedef struct {
    MainUIAVFrame frame;
    int64_t best;
    uint8_t luma[16];
} StubFrame;

typedef struct {
    MainUIAVCodecContext context; /* first: the stream's codec points here */
    int width, height, frames, fps, pending, grow;
    int nopts, slow, noseek, nocodec, opened;
} StubCodec;

typedef struct {
    MainUIAVFormatContext context;
    MainUIAVStream stream, third;
    MainUIAVStream *streams[2];
    StubCodec codec;
    MainUIAVCodecContext third_codec; /* cover art or a second video */
    int third_width, third_height;    /* as decoded */
    int next;
    bool wait_probe, wait_read;
    uint8_t packet[16];
} StubInput;

/* Hold for up to 3 s, unless the interrupt callback stops it first. */
static bool interrupted_wait(MainUIAVFormatContext *context)
{
    for (int i = 0; i < 300; i++) {
        MainUIAVIOInterruptCB *check = &context->interrupt_callback;
        if (check->callback && check->callback(check->opaque)) {
            return true;
        }
        nanosleep(&(struct timespec){0, 10 * 1000 * 1000}, NULL);
    }
    return false;
}

#define STUB_AVERROR_EXIT (-1414092869)

int avcodec_default_get_buffer2(void *context, void *frame, int flags)
{
    (void)context;
    (void)frame;
    (void)flags;
    return 0;
}

/* A decoder taking a picture's buffer, as FFmpeg's do, through get_buffer2
 * when set. */
static int take_picture(MainUIAVCodecContext *context, int width, int height)
{
    MainUIAVFrame frame = {.width = width, .height = height};
    int result = context->get_buffer2 ? context->get_buffer2(context, &frame, 0) : 0;
    if (result >= 0 && (width > 1296 || height > 736)) {
        fprintf(stderr, "[ffstub] took a buffer for a %dx%d picture\n", width, height);
    }
    return result;
}

MainUIAVFormatContext *avformat_alloc_context(void)
{
    StubInput *input = allocate(sizeof *input);
    return input ? &input->context : NULL;
}

typedef struct {
    int format, width, height;
} StubScaler;

static const int decoder;

void av_register_all(void) {}

void av_log_set_level(int level)
{
    (void)level;
}

int avformat_open_input(MainUIAVFormatContext **out, const char *path, void *format, void **options)
{
    (void)format;
    (void)options;
    char text[256] = "";
    FILE *file = fopen(path, "rb");
    size_t n = file ? fread(text, 1, sizeof text - 1, file) : 0;
    if (file) {
        fclose(file);
    }
    text[n] = 0;
    StubCodec codec = {.pending = -1};
    if (sscanf(text, "STUB %d %d %d %d", &codec.width, &codec.height, &codec.frames, &codec.fps) !=
            4 ||
        codec.fps <= 0 || (strstr(text, " waitopen") && *out && interrupted_wait(*out))) {
        release(*out);
        *out = NULL;
        return strstr(text, " waitopen") ? STUB_AVERROR_EXIT : -1;
    }
    codec.nopts = strstr(text, " nopts") != NULL;
    codec.slow = strstr(text, " slow") != NULL;
    codec.noseek = strstr(text, " noseek") != NULL;
    codec.nocodec = strstr(text, " nocodec") != NULL;
    const char *grow = strstr(text, " grow");
    codec.grow = grow ? atoi(grow + 5) : 0;
    bool lie = strstr(text, " lie") != NULL, nodims = strstr(text, " nodims") != NULL;
    codec.context.codec_type = MAINUI_AVMEDIA_TYPE_VIDEO;
    codec.context.codec_id = 28; /* H.264 */
    codec.context.width = nodims ? 0 : lie ? 320 : codec.width;
    codec.context.height = nodims ? 0 : lie ? 240 : codec.height;
    StubInput *input = *out ? (StubInput *)*out : allocate(sizeof *input);
    if (!input) {
        return -1;
    }
    MainUIAVIOInterruptCB check = input->context.interrupt_callback;
    *input = (StubInput){.context.interrupt_callback = check};
    input->wait_probe = strstr(text, " waitprobe") != NULL;
    input->wait_read = strstr(text, " waitread") != NULL;
    input->codec = codec;
    input->stream.codec = &input->codec.context;
    input->stream.time_base = (MainUIAVRational){1, 90000};
    input->streams[0] = &input->stream;
    input->context.nb_streams = 1;
    bool cover = strstr(text, " cover") != NULL, extra = strstr(text, " extra") != NULL;
    if (cover || extra) {
        input->third_codec.codec_type = MAINUI_AVMEDIA_TYPE_VIDEO;
        input->third_codec.codec_id = cover ? 7 : 28; /* MJPEG or H.264 */
        input->third_codec.width = cover ? 0 : 4000;
        input->third_codec.height = cover ? 0 : 3000;
        input->third_width = cover ? 3000 : 4000;
        input->third_height = 3000;
        input->third.disposition = cover ? MAINUI_AV_DISPOSITION_ATTACHED_PIC : 0;
        input->third.index = (int)input->context.nb_streams;
        input->third.codec = &input->third_codec;
        input->third.time_base = (MainUIAVRational){1, 90000};
        input->streams[input->context.nb_streams++] = &input->third;
    }
    input->context.streams = input->streams;
    *out = &input->context;
    return 0;
}

/* As FFmpeg's, which decodes streams whose size is not known yet. */
int avformat_find_stream_info(MainUIAVFormatContext *context, void **options)
{
    (void)options;
    StubInput *input = (StubInput *)context;
    if (input->wait_probe && interrupted_wait(context)) {
        return STUB_AVERROR_EXIT;
    }
    for (unsigned i = 0; i < context->nb_streams; i++) {
        const MainUIAVCodecContext *codec = context->streams[i]->codec;
        if (codec->codec_type == MAINUI_AVMEDIA_TYPE_VIDEO &&
            codec->codec_id != MAINUI_AV_CODEC_ID_NONE &&
            (codec->width <= 0 || codec->height <= 0 || codec->width > 1280 ||
             codec->height > 720)) {
            fprintf(stderr, "[ffstub] probing decoded stream %u, %dx%d\n", i, codec->width,
                    codec->height);
        }
    }
    /* It decodes a picture of each video stream, at the stream's own size. */
    for (unsigned i = 0; i < context->nb_streams; i++) {
        MainUIAVCodecContext *codec = (MainUIAVCodecContext *)context->streams[i]->codec;
        if (codec->codec_type != MAINUI_AVMEDIA_TYPE_VIDEO ||
            codec->codec_id == MAINUI_AV_CODEC_ID_NONE) {
            continue;
        }
        bool main = codec == &input->codec.context;
        take_picture(codec, main ? input->codec.width : input->third_width,
                     main ? input->codec.height : input->third_height);
    }
    return 0;
}

int av_find_best_stream(MainUIAVFormatContext *context, int type, int wanted, int related,
                        void **found, int flags)
{
    (void)context;
    (void)wanted;
    (void)related;
    (void)flags;
    if (type != MAINUI_AVMEDIA_TYPE_VIDEO) {
        return -1;
    }
    *found = (void *)&decoder;
    return 0;
}

void av_init_packet(MainUIAVPacket *packet)
{
    packet->pts = packet->dts = MAINUI_AV_NOPTS_VALUE;
    packet->stream_index = 0;
    packet->flags = 0;
}

void av_free_packet(MainUIAVPacket *packet)
{
    packet->data = NULL;
    packet->size = 0;
}

int av_read_frame(MainUIAVFormatContext *context, MainUIAVPacket *packet)
{
    StubInput *input = (StubInput *)context;
    if (input->wait_read) {
        input->wait_read = false;
        if (interrupted_wait(context)) {
            return STUB_AVERROR_EXIT;
        }
    }
    if (input->next >= input->codec.frames) {
        return -541478725; /* AVERROR_EOF */
    }
    int index = input->next++;
    memcpy(input->packet, &index, sizeof index);
    packet->data = input->packet;
    packet->size = (int)sizeof input->packet;
    packet->stream_index = 0;
    packet->pts = (int64_t)index * 90000 / input->codec.fps;
    return 0;
}

int av_seek_frame(MainUIAVFormatContext *context, int stream, int64_t timestamp, int flags)
{
    (void)stream;
    (void)timestamp;
    (void)flags;
    StubInput *input = (StubInput *)context;
    if (input->codec.noseek) {
        return -1;
    }
    input->next = 0;
    return 0;
}

void avformat_close_input(MainUIAVFormatContext **context)
{
    release(*context);
    *context = NULL;
}

int avcodec_open2(void *context, const void *codec, void **options)
{
    (void)options;
    StubCodec *stub = context;
    if (codec != &decoder || stub->nocodec) {
        return -1;
    }
    stub->opened++;
    return 0;
}

int avcodec_close(void *context)
{
    StubCodec *stub = context;
    stub->opened--;
    if (stub->opened) {
        fprintf(stderr, "[ffstub] codec closed %d times too few\n", stub->opened);
    }
    return 0;
}

void avcodec_flush_buffers(void *context)
{
    ((StubCodec *)context)->pending = -1;
}

int avcodec_decode_video2(void *context, MainUIAVFrame *frame, int *got,
                          const MainUIAVPacket *packet)
{
    StubCodec *codec = context;
    *got = 0;
    int out = codec->pending;
    if (packet && packet->size > 0) {
        memcpy(&codec->pending, packet->data, sizeof codec->pending);
    }
    else {
        codec->pending = -1;
    }
    if (out < 0) {
        return packet ? packet->size : 0;
    }
    if (codec->slow) {
        nanosleep(&(struct timespec){0, 30 * 1000 * 1000}, NULL);
    }
    int width = codec->grow && out >= codec->grow ? 4000 : codec->width;
    int height = codec->grow && out >= codec->grow ? 3000 : codec->height;
    int taken = take_picture(&codec->context, width, height);
    if (taken < 0) {
        return taken;
    }
    StubFrame *stub = (StubFrame *)frame;
    stub->luma[0] = (uint8_t)out;
    frame->data[0] = stub->luma;
    frame->linesize[0] = width;
    frame->width = width;
    frame->height = height;
    frame->format = 0; /* YUV420P */
    stub->best = codec->nopts ? MAINUI_AV_NOPTS_VALUE : (int64_t)out * 90000 / codec->fps;
    *got = 1;
    return packet ? packet->size : 0;
}

MainUIAVFrame *av_frame_alloc(void)
{
    StubFrame *frame = allocate(sizeof *frame);
    return frame ? &frame->frame : NULL;
}

void av_frame_free(MainUIAVFrame **frame)
{
    release(*frame);
    *frame = NULL;
}

int64_t av_frame_get_best_effort_timestamp(const MainUIAVFrame *frame)
{
    return ((const StubFrame *)frame)->best;
}

void *sws_getCachedContext(void *context, int src_w, int src_h, int src_format, int dst_w,
                           int dst_h, int dst_format, int flags, void *src_filter, void *dst_filter,
                           const double *param)
{
    (void)src_w;
    (void)src_h;
    (void)src_format;
    (void)flags;
    (void)src_filter;
    (void)dst_filter;
    (void)param;
    if (dst_format < MAINUI_AV_PIX_FMT_ARGB || dst_format > MAINUI_AV_PIX_FMT_BGRA) {
        return NULL;
    }
    StubScaler *scaler = context ? context : allocate(sizeof *scaler);
    if (scaler) {
        *scaler = (StubScaler){dst_format, dst_w, dst_h};
    }
    return scaler;
}

#ifndef STUB_WITHOUT_SCALE
int sws_scale(void *context, const uint8_t *const source[], const int source_pitch[], int y,
              int height, uint8_t *const target[], const int target_pitch[])
{
    (void)source_pitch;
    (void)y;
    StubScaler *scaler = context;
    int index = source[0][0];
    uint8_t r = (uint8_t)(index * 40), g = (uint8_t)(255 - index * 40), b = 100;
    uint8_t pixel[4];
    switch (scaler->format) {
    case MAINUI_AV_PIX_FMT_BGRA:
        memcpy(pixel, (uint8_t[4]){b, g, r, 255}, 4);
        break;
    case MAINUI_AV_PIX_FMT_RGBA:
        memcpy(pixel, (uint8_t[4]){r, g, b, 255}, 4);
        break;
    case MAINUI_AV_PIX_FMT_ABGR:
        memcpy(pixel, (uint8_t[4]){255, b, g, r}, 4);
        break;
    default:
        memcpy(pixel, (uint8_t[4]){255, r, g, b}, 4);
        break;
    }
    for (int row = 0; row < scaler->height; row++) {
        for (int x = 0; x < scaler->width; x++) {
            memcpy(target[0] + row * target_pitch[0] + x * 4, pixel, 4);
        }
    }
    return height;
}
#endif

void sws_freeContext(void *context)
{
    release(context);
}
