/* SPDX-License-Identifier: GPL-3.0-only */
#include "platform/video.h"
#include "platform/ffmpeg_abi.h"
#include <dlfcn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One frame shown and one decoded ahead: the decoder waits for a free one. */
enum {
    SLOTS = 2,
    LIBRARIES = 9
};

typedef enum {
    SLOT_FREE,
    SLOT_READY,
    SLOT_SHOWN
} SlotState;

/* Onion starts MainUI with an LD_LIBRARY_PATH without .tmp_update/lib/
 * parasyte, where its FFmpeg is, so the libraries are loaded from there by
 * path, each after the ones it needs: a library's dependencies are found
 * among those already loaded. Each is tried by name first, so one already
 * loaded or on the path (zlib, bzip2 in miyoo/lib) is not loaded twice. The
 * four FFmpeg libraries must load; the others only help them to. */
static const struct {
    const char *soname;
    int ffmpeg; /* index used by the symbol table, or -1 */
} libraries[LIBRARIES] = {
    {"libz.so.1", -1},      {"libbz2.so.1.0", -1},   {"liblzma.so.5", -1},
    {"libx264.so.152", -1}, {"libavutil.so.54", 0},  {"libswresample.so.1", -1},
    {"libswscale.so.3", 1}, {"libavcodec.so.56", 2}, {"libavformat.so.56", 3}};

static const char *const ffmpeg_names[] = {"libavutil.so.54", "libswscale.so.3", "libavcodec.so.56",
                                           "libavformat.so.56"};

/* One symbols[] row. clang-format 14 and 17 lay this macro out
 * differently, so it is kept as it is. */
/* clang-format off */
#define SYMBOL(library, name) {#name, library, offsetof(MainUIFFmpeg, name)}

/* clang-format on */

static const struct {
    const char *name;
    int library;
    size_t offset;
} symbols[] = {
    SYMBOL(0, av_frame_alloc),
    SYMBOL(0, av_frame_free),
    SYMBOL(0, av_frame_get_best_effort_timestamp),
    SYMBOL(0, av_log_set_level),
    SYMBOL(1, sws_getCachedContext),
    SYMBOL(1, sws_scale),
    SYMBOL(1, sws_freeContext),
    SYMBOL(2, avcodec_default_get_buffer2),
    SYMBOL(2, avcodec_open2),
    SYMBOL(2, avcodec_close),
    SYMBOL(2, avcodec_decode_video2),
    SYMBOL(2, avcodec_flush_buffers),
    SYMBOL(2, av_init_packet),
    SYMBOL(2, av_free_packet),
    SYMBOL(3, av_register_all),
    SYMBOL(3, avformat_alloc_context),
    SYMBOL(3, avformat_open_input),
    SYMBOL(3, avformat_find_stream_info),
    SYMBOL(3, av_find_best_stream),
    SYMBOL(3, av_read_frame),
    SYMBOL(3, av_seek_frame),
    SYMBOL(3, avformat_close_input),

#undef SYMBOL
};

struct MainUIVideo {
    char path[4096], library_dir[4096];
    Uint32 rmask, gmask, bmask;
    int pixel_format, max_width, max_height;
    SDL_Thread *thread;
    SDL_mutex *lock;
    SDL_cond *changed;
    atomic_bool stop;
    atomic_bool oversized; /* a decoder asked for a picture over the limit */
    void *handles[LIBRARIES], *ffmpeg[4];
    MainUIFFmpeg ff;
    /* Shared, under lock. Frames are created by the decoder before the first
     * one is ready and freed only on close; the decoder writes FREE ones and
     * the UI reads READY and SHOWN ones. due is the frame's time in
     * milliseconds from the start of the video, counting repeats. */
    SDL_Surface *frames[SLOTS];
    SlotState state[SLOTS];
    Uint32 due[SLOTS];
    bool clock_set;
    Uint32 clock; /* ticks at which the video's time 0 was, once shown */
    char error[160];
};

static void wake_ui(void)
{
    SDL_Event event = {.type = SDL_USEREVENT};
    SDL_PushEvent(&event);
}

/* Only the first reason is kept; the log gets the details. */
static void fail(MainUIVideo *video, const char *shown, const char *format, ...)
{
    char detail[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(detail, sizeof detail, format, arguments);
    va_end(arguments);
    fprintf(stderr, "[video] %.300s: %s\n", video->path, detail);
    SDL_mutexP(video->lock);
    if (!*video->error) {
        snprintf(video->error, sizeof video->error, "%s", shown);
    }
    SDL_mutexV(video->lock);
    wake_ui();
}

static bool load(MainUIVideo *video)
{
    for (int i = 0; i < LIBRARIES; i++) {
        /* Closing asked for: no more libraries (each load can take a while). */
        if (atomic_load(&video->stop)) {
            return false;
        }
        void *handle = dlopen(libraries[i].soname, RTLD_NOW | RTLD_LOCAL);
        char path[4096 + 32];
        if (!handle && *video->library_dir &&
            snprintf(path, sizeof path, "%s/%s", video->library_dir, libraries[i].soname) <
                (int)sizeof path) {
            handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        }
        video->handles[i] = handle;
        if (libraries[i].ffmpeg < 0) {
            continue;
        }
        if (!handle) {
            const char *why = dlerror();
            fail(video, "Video needs Onion's FFmpeg libraries", "%s (also looked in %s)",
                 why ? why : libraries[i].soname,
                 *video->library_dir ? video->library_dir : "no folder");
            return false;
        }
        video->ffmpeg[libraries[i].ffmpeg] = handle;
    }
    for (size_t i = 0; i < sizeof symbols / sizeof *symbols; i++) {
        void *symbol = dlsym(video->ffmpeg[symbols[i].library], symbols[i].name);
        if (!symbol) {
            fail(video, "Video needs Onion's FFmpeg libraries", "%s has no %s",
                 ffmpeg_names[symbols[i].library], symbols[i].name);
            return false;
        }
        memcpy((char *)&video->ff + symbols[i].offset, &symbol, sizeof symbol);
    }
    return true;
}

/* A free frame to write, or -1 once stopping. */
static int free_slot(MainUIVideo *video)
{
    int slot = -1;
    SDL_mutexP(video->lock);
    while (!atomic_load(&video->stop)) {
        for (int i = 0; i < SLOTS && slot < 0; i++) {
            if (video->state[i] == SLOT_FREE) {
                slot = i;
            }
        }
        if (slot >= 0) {
            break;
        }
        SDL_CondWait(video->changed, video->lock);
    }
    SDL_mutexV(video->lock);
    return atomic_load(&video->stop) ? -1 : slot;
}

/* A frame's due time is already past by more than a frame: skip it. */
static bool late(MainUIVideo *video, Uint32 due, Uint32 frame_ms)
{
    SDL_mutexP(video->lock);
    bool behind =
        video->clock_set && (Sint32)(SDL_GetTicks() - (video->clock + due)) > (Sint32)frame_ms;
    SDL_mutexV(video->lock);
    return behind;
}

typedef struct {
    MainUIAVFormatContext *input;
    void *codec, *scaler;
    MainUIAVFrame *frame;
    int stream;
    MainUIAVRational base;
    int64_t start;         /* first timestamp, the video's time 0 */
    Uint32 last, frame_ms; /* last frame's due time and the spacing of frames */
    Uint32 offset;         /* added on each repeat */
    int width, height;     /* of the frames shown */
    bool any;              /* a frame was decoded */
} Decoder;

static bool show(MainUIVideo *video, Decoder *d)
{
    MainUIFFmpeg *ff = &video->ff;
    MainUIAVFrame *frame = d->frame;
    if (frame->width <= 0 || frame->height <= 0 || frame->width > MAINUI_VIDEO_MAX_WIDTH ||
        frame->height > MAINUI_VIDEO_MAX_HEIGHT) {
        fail(video, "Video too large to play", "%dx%d is over %dx%d", frame->width, frame->height,
             MAINUI_VIDEO_MAX_WIDTH, MAINUI_VIDEO_MAX_HEIGHT);
        return false;
    }
    int64_t stamp = ff->av_frame_get_best_effort_timestamp(frame);
    Uint32 due = d->last + d->frame_ms;
    if (stamp != MAINUI_AV_NOPTS_VALUE && d->base.den > 0 && d->base.num > 0) {
        if (!d->any) {
            d->start = stamp;
        }
        int64_t ms = (stamp - d->start) * 1000 * d->base.num / d->base.den;
        if (ms >= 0 && ms < 24 * 3600 * 1000) {
            due = d->offset + (Uint32)ms;
        }
    }
    if (d->any && due > d->last && due - d->last < 1000) {
        d->frame_ms = due - d->last;
    }
    else if (d->any && due <= d->last) {
        due = d->last + d->frame_ms;
    }
    bool first = !d->any;
    d->any = true;
    d->last = due;
    if (first) {
        /* Fit the screen, keeping the shape. */
        int max_w = video->max_width, max_h = video->max_height;
        if ((int64_t)frame->width * max_h > (int64_t)frame->height * max_w) {
            d->width = max_w;
            d->height = (int)((int64_t)frame->height * max_w / frame->width);
        }
        else {
            d->height = max_h;
            d->width = (int)((int64_t)frame->width * max_h / frame->height);
        }
        d->width = d->width < 1 ? 1 : d->width;
        d->height = d->height < 1 ? 1 : d->height;
        for (int i = 0; i < SLOTS; i++) {
            SDL_Surface *surface =
                SDL_CreateRGBSurface(SDL_SWSURFACE, d->width, d->height, 32, video->rmask,
                                     video->gmask, video->bmask, 0);
            SDL_mutexP(video->lock);
            video->frames[i] = surface;
            SDL_mutexV(video->lock);
            if (!surface) {
                fail(video, "Not enough memory for this video", "no %dx%d frame", d->width,
                     d->height);
                return false;
            }
        }
    }
    else if (late(video, due, d->frame_ms)) {
        return true;
    }
    d->scaler = ff->sws_getCachedContext(d->scaler, frame->width, frame->height, frame->format,
                                         d->width, d->height, video->pixel_format,
                                         MAINUI_SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!d->scaler) {
        fail(video, "Cannot play this video", "no conversion from pixel format %d", frame->format);
        return false;
    }
    int slot = free_slot(video);
    if (slot < 0) {
        return false;
    }
    SDL_Surface *target = video->frames[slot];
    uint8_t *planes[4] = {target->pixels, NULL, NULL, NULL};
    int pitches[4] = {target->pitch, 0, 0, 0};
    ff->sws_scale(d->scaler, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height,
                  planes, pitches);
    SDL_mutexP(video->lock);
    video->state[slot] = SLOT_READY;
    video->due[slot] = due;
    SDL_mutexV(video->lock);
    wake_ui();
    return true;
}

/* Decode one packet; a NULL-data packet drains the decoder's delayed frames.
 * Returns false to stop, with *got whether a frame came out. */
static bool too_large(MainUIVideo *video);

static bool decode(MainUIVideo *video, Decoder *d, MainUIAVPacket *packet, bool *got)
{
    int picture = 0;
    *got = false;
    int used = video->ff.avcodec_decode_video2(d->codec, d->frame, &picture, packet);
    if (too_large(video)) {
        return false;
    }
    if (used < 0 || !picture) {
        return true;
    }
    *got = true;
    return show(video, d);
}

static void play(MainUIVideo *video, Decoder *d)
{
    MainUIFFmpeg *ff = &video->ff;
    int unplayable = 0;
    while (!atomic_load(&video->stop)) {
        MainUIAVPacket packet;
        ff->av_init_packet(&packet);
        packet.data = NULL;
        packet.size = 0;
        bool got = false, go_on = true;
        if (ff->av_read_frame(d->input, &packet) >= 0) {
            if (packet.stream_index == d->stream) {
                go_on = decode(video, d, &packet, &got);
            }
            ff->av_free_packet(&packet);
            /* Many packets and still no picture: not a video this can show. */
            if (go_on && !d->any && ++unplayable > 300) {
                fail(video, "Cannot play this video", "no picture in 300 packets");
                return;
            }
            if (!go_on) {
                return;
            }
            continue;
        }
        /* The end: the frames the decoder still holds, then from the start. */
        do {
            ff->av_init_packet(&packet);
            packet.data = NULL;
            packet.size = 0;
            go_on = decode(video, d, &packet, &got);
        } while (go_on && got && !atomic_load(&video->stop));
        if (!go_on) {
            return;
        }
        if (!d->any) {
            fail(video, "Cannot play this video", "no picture decoded");
            return;
        }
        if (ff->av_seek_frame(d->input, d->stream, 0, MAINUI_AVSEEK_FLAG_BACKWARD) < 0) {
            fprintf(stderr, "[video] %.300s: cannot seek to the start; the last frame stays\n",
                    video->path);
            return;
        }
        ff->avcodec_flush_buffers(d->codec);
        d->offset = d->last + d->frame_ms;
    }
}

/* FFmpeg's reads and its stream probing ask this: closing stops them. */
static int interrupted(void *context)
{
    MainUIVideo *video = context;
    return atomic_load(&video->stop);
}

/* Every picture buffer a video decoder takes, while probing too: one over
 * the limit (the size the stream itself gives, which can differ from what
 * the file states, or change midway) is refused before it is allocated. */
static int picture_buffer(void *context, void *picture, int flags)
{
    MainUIAVCodecContext *codec = context;
    MainUIAVFrame *frame = picture;
    MainUIVideo *video = codec->opaque;
    /* Decoders round the size up to whole 16-pixel blocks. */
    if (frame->width > MAINUI_VIDEO_MAX_WIDTH + 16 ||
        frame->height > MAINUI_VIDEO_MAX_HEIGHT + 16) {
        if (!atomic_exchange(&video->oversized, true)) {
            fprintf(stderr, "[video] %.300s: refused a %dx%d picture\n", video->path, frame->width,
                    frame->height);
        }
        return -1094995529; /* AVERROR_INVALIDDATA */
    }
    return video->ff.avcodec_default_get_buffer2(context, picture, flags);
}

/* Before probing the streams, which can decode them: every video stream it
 * could decode must state its size, within the limit, and gets the picture
 * buffer guard above. Cover art (an attached picture) is left undecoded
 * instead: it is never shown here. A decoder's smaller tables, sized by the
 * stream, are not limited here. */
static bool sizes_allowed(MainUIVideo *video, MainUIAVFormatContext *input)
{
    for (unsigned i = 0; i < input->nb_streams; i++) {
        MainUIAVStream *stream = input->streams[i];
        MainUIAVCodecContext *codec = stream ? stream->codec : NULL;
        if (!codec || codec->codec_type != MAINUI_AVMEDIA_TYPE_VIDEO ||
            codec->codec_id == MAINUI_AV_CODEC_ID_NONE) {
            continue;
        }
        if (stream->disposition & MAINUI_AV_DISPOSITION_ATTACHED_PIC) {
            codec->codec_id = MAINUI_AV_CODEC_ID_NONE;
            continue;
        }
        if (codec->width <= 0 || codec->height <= 0) {
            fail(video, "Cannot play this video", "stream %u states no size", i);
            return false;
        }
        if (codec->width > MAINUI_VIDEO_MAX_WIDTH || codec->height > MAINUI_VIDEO_MAX_HEIGHT) {
            fail(video, "Video too large to play", "stream %u is %dx%d, over %dx%d", i,
                 codec->width, codec->height, MAINUI_VIDEO_MAX_WIDTH, MAINUI_VIDEO_MAX_HEIGHT);
            return false;
        }
        codec->opaque = video;
        codec->get_buffer2 = picture_buffer;
    }
    return true;
}

/* A decoder was refused a picture over the limit: the video stops. */
static bool too_large(MainUIVideo *video)
{
    if (!atomic_load(&video->oversized)) {
        return false;
    }
    fail(video, "Video too large to play", "a picture over %dx%d", MAINUI_VIDEO_MAX_WIDTH,
         MAINUI_VIDEO_MAX_HEIGHT);
    return true;
}

static int run(void *context)
{
    MainUIVideo *video = context;
    if (!load(video)) {
        return 0;
    }
    MainUIFFmpeg *ff = &video->ff;
    ff->av_log_set_level(MAINUI_AV_LOG_QUIET);
    ff->av_register_all();
    Decoder d = {.stream = -1, .frame_ms = 40};
    void *decoder = NULL;
    /* Each step can take a while on a slow card; closing stops FFmpeg's
     * reads and probing through the interrupt, and the steps between. */
    d.input = ff->avformat_alloc_context();
    if (d.input) {
        d.input->interrupt_callback = (MainUIAVIOInterruptCB){interrupted, video};
    }
    if (!d.input) {
        fail(video, "Not enough memory for this video", "no format context");
    }
    else if (ff->avformat_open_input(&d.input, video->path, NULL, NULL) < 0) {
        d.input = NULL; /* freed by FFmpeg */
        if (!atomic_load(&video->stop)) {
            fail(video, "Cannot play this video", "cannot open it");
        }
    }
    else if (atomic_load(&video->stop) || !sizes_allowed(video, d.input)) {
        /* Closing, or refused: nothing more. */
    }
    else if (ff->avformat_find_stream_info(d.input, NULL) < 0 || atomic_load(&video->stop)) {
        if (!atomic_load(&video->stop)) {
            fail(video, "Cannot play this video", "no stream information");
        }
    }
    else if (too_large(video)) {
        /* Refused while probing. */
    }
    else if ((d.stream = ff->av_find_best_stream(d.input, MAINUI_AVMEDIA_TYPE_VIDEO, -1, -1,
                                                 &decoder, 0)) < 0 ||
             !decoder || (unsigned)d.stream >= d.input->nb_streams) {
        d.stream = -1;
        fail(video, "Cannot play this video", "no video stream it can decode");
    }
    else {
        MainUIAVStream *stream = d.input->streams[d.stream];
        d.base = stream->time_base;
        int width = stream->codec->width, height = stream->codec->height;
        /* Again as probed, before this decoder opens; decoded frames, which
         * could change size midway, are checked too. */
        if (width > MAINUI_VIDEO_MAX_WIDTH || height > MAINUI_VIDEO_MAX_HEIGHT) {
            fail(video, "Video too large to play", "%dx%d is over %dx%d", width, height,
                 MAINUI_VIDEO_MAX_WIDTH, MAINUI_VIDEO_MAX_HEIGHT);
        }
        else if (ff->avcodec_open2(stream->codec, decoder, NULL) < 0) {
            fail(video, "Cannot play this video", "the decoder did not open");
        }
        else {
            d.codec = stream->codec;
            d.frame = ff->av_frame_alloc();
            if (d.frame) {
                play(video, &d);
            }
            else {
                fail(video, "Not enough memory for this video", "no frame");
            }
        }
    }
    if (d.frame) {
        ff->av_frame_free(&d.frame);
    }
    if (d.codec) {
        ff->avcodec_close(d.codec);
    }
    if (d.input) {
        ff->avformat_close_input(&d.input);
    }
    if (d.scaler) {
        ff->sws_freeContext(d.scaler);
    }
    return 0;
}

static int pixel_format(const SDL_PixelFormat *format)
{
    if (SDL_BYTEORDER != SDL_LIL_ENDIAN || format->BitsPerPixel != 32) {
        return -1;
    }
    /* Little-endian bytes in memory, lowest first. */
    if (format->Rmask == 0x00ff0000 && format->Gmask == 0x0000ff00 && format->Bmask == 0x000000ff) {
        return MAINUI_AV_PIX_FMT_BGRA;
    }
    if (format->Rmask == 0x000000ff && format->Gmask == 0x0000ff00 && format->Bmask == 0x00ff0000) {
        return MAINUI_AV_PIX_FMT_RGBA;
    }
    if (format->Rmask == 0xff000000 && format->Gmask == 0x00ff0000 && format->Bmask == 0x0000ff00) {
        return MAINUI_AV_PIX_FMT_ABGR;
    }
    if (format->Rmask == 0x0000ff00 && format->Gmask == 0x00ff0000 && format->Bmask == 0xff000000) {
        return MAINUI_AV_PIX_FMT_ARGB;
    }
    return -1;
}

MainUIVideo *mainui_video_open(const char *path, const char *library_dir, const SDL_Surface *screen,
                               int max_width, int max_height)
{
    if (!path || !screen || max_width < 1 || max_height < 1 ||
        strlen(path) >= sizeof((MainUIVideo *)0)->path ||
        (library_dir && strlen(library_dir) >= sizeof((MainUIVideo *)0)->library_dir)) {
        return NULL;
    }
    MainUIVideo *video = calloc(1, sizeof *video);
    if (!video) {
        return NULL;
    }
    strcpy(video->path, path);
    strcpy(video->library_dir, library_dir ? library_dir : "");
    video->rmask = screen->format->Rmask;
    video->gmask = screen->format->Gmask;
    video->bmask = screen->format->Bmask;
    video->pixel_format = pixel_format(screen->format);
    video->max_width = max_width;
    video->max_height = max_height;
    atomic_init(&video->stop, false);
    atomic_init(&video->oversized, false);
    video->lock = SDL_CreateMutex();
    video->changed = SDL_CreateCond();
    if (!video->lock || !video->changed) {
        mainui_video_close(video);
        return NULL;
    }
    if (video->pixel_format < 0) {
        fail(video, "Cannot play video on this screen", "unsupported screen pixel format");
        return video;
    }
    video->thread = SDL_CreateThread(run, video);
    if (!video->thread) {
        mainui_video_close(video);
        return NULL;
    }
    return video;
}

SDL_Surface *mainui_video_frame(MainUIVideo *video, Uint32 now)
{
    if (!video) {
        return NULL;
    }
    SDL_mutexP(video->lock);
    int chosen = -1;
    for (int i = 0; i < SLOTS; i++) {
        if (video->state[i] != SLOT_READY) {
            continue;
        }
        /* The first frame starts the clock; then the newest one due. */
        if (!video->clock_set) {
            if (chosen < 0 || video->due[i] < video->due[chosen]) {
                chosen = i;
            }
        }
        else if ((Sint32)(now - (video->clock + video->due[i])) >= 0 &&
                 (chosen < 0 || video->due[i] > video->due[chosen])) {
            chosen = i;
        }
    }
    if (chosen >= 0) {
        if (!video->clock_set) {
            video->clock = now - video->due[chosen];
            video->clock_set = true;
        }
        for (int i = 0; i < SLOTS; i++) {
            if (i != chosen &&
                (video->state[i] == SLOT_SHOWN ||
                 (video->state[i] == SLOT_READY && video->due[i] < video->due[chosen]))) {
                video->state[i] = SLOT_FREE;
            }
        }
        video->state[chosen] = SLOT_SHOWN;
        SDL_CondSignal(video->changed);
    }
    SDL_Surface *shown = NULL;
    for (int i = 0; i < SLOTS; i++) {
        if (video->state[i] == SLOT_SHOWN) {
            shown = video->frames[i];
        }
    }
    SDL_mutexV(video->lock);
    return shown;
}

const char *mainui_video_error(MainUIVideo *video)
{
    if (!video) {
        return "";
    }
    SDL_mutexP(video->lock);
    /* Written once, under the lock, and never again after: once seen set it
     * can be read after unlocking. Unset, it may be written meanwhile, so
     * "" is returned rather than the buffer. */
    const char *error = *video->error ? video->error : "";
    SDL_mutexV(video->lock);
    return error;
}

int mainui_video_due(MainUIVideo *video, Uint32 now)
{
    if (!video) {
        return -1;
    }
    int wait = -1;
    SDL_mutexP(video->lock);
    for (int i = 0; i < SLOTS && !*video->error; i++) {
        if (video->state[i] != SLOT_READY) {
            continue;
        }
        Sint32 until = video->clock_set ? (Sint32)(video->clock + video->due[i] - now) : 0;
        int ms = until < 0 ? 0 : (int)until;
        if (wait < 0 || ms < wait) {
            wait = ms;
        }
    }
    SDL_mutexV(video->lock);
    return wait;
}

void mainui_video_wait_first(MainUIVideo *video, Uint32 timeout_ms)
{
    Uint32 start = SDL_GetTicks();
    while (video && SDL_GetTicks() - start < timeout_ms) {
        SDL_mutexP(video->lock);
        bool settled = *video->error || video->state[0] != SLOT_FREE ||
                       video->state[1] != SLOT_FREE || !video->thread;
        SDL_mutexV(video->lock);
        if (settled) {
            return;
        }
        SDL_Delay(5);
    }
}

void mainui_video_close(MainUIVideo *video)
{
    if (!video) {
        return;
    }
    atomic_store(&video->stop, true);
    if (video->thread) {
        SDL_mutexP(video->lock);
        SDL_CondBroadcast(video->changed);
        SDL_mutexV(video->lock);
        SDL_WaitThread(video->thread, NULL);
    }
    for (int i = 0; i < SLOTS; i++) {
        if (video->frames[i]) {
            SDL_FreeSurface(video->frames[i]);
        }
    }
    for (int i = LIBRARIES - 1; i >= 0; i--) {
        if (video->handles[i]) {
            dlclose(video->handles[i]);
        }
    }
    if (video->changed) {
        SDL_DestroyCond(video->changed);
    }
    if (video->lock) {
        SDL_DestroyMutex(video->lock);
    }
    free(video);
}
