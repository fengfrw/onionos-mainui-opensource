/* SPDX-License-Identifier: GPL-3.0-only
 * Video playback against the stub FFmpeg (tests/ffmpeg_stub.c), which
 * case_media_gallery.py puts on LD_LIBRARY_PATH under Onion's sonames.
 *
 *   fixture-video DIR LIBS play      frames in order, on time, repeating
 *   fixture-video DIR LIBS errors    unusable files and sizes show an error
 *   fixture-video DIR LIBS missing   no libraries, or one without a function
 *   fixture-video DIR LIBS cancel    closing while FFmpeg opens, probes or
 *                                    reads stops it at once
 *
 * DIR holds the stub video files; LIBS is the folder the libraries are
 * looked for in when not found by name, as Onion's parasyte folder, or "-"
 * for none. */
#include "platform/video.h"
#include "support.h"
#include <stdlib.h>
#include <string.h>

static SDL_Surface *screen(Uint32 r, Uint32 g, Uint32 b)
{
    SDL_Surface *surface = SDL_CreateRGBSurface(SDL_SWSURFACE, 640, 480, 32, r, g, b, 0);
    assert(surface);
    return surface;
}

/* The stub frame index a frame shows, from its first pixel's red. */
static int shown_index(SDL_Surface *frame)
{
    Uint8 r, g, b;
    SDL_GetRGB(*(Uint32 *)frame->pixels, frame->format, &r, &g, &b);
    assert(b == 100 && r + g == 255);
    return r / 40;
}

static const char *libraries;

static MainUIVideo *open_video(const char *dir, const char *name, SDL_Surface *target)
{
    char path[4096];
    TEST_PATH(path, "%s/%s", dir, name);
    MainUIVideo *video = mainui_video_open(path, libraries, target, 640, 480);
    assert(video);
    return video;
}

static void play(const char *dir)
{
    /* 320x240 at 20 fps, 6 frames: shown 640x480, one every 50 ms. */
    SDL_Surface *target = screen(0xff0000, 0xff00, 0xff);
    MainUIVideo *video = open_video(dir, "ok.mp4", target);
    assert(!mainui_video_frame(video, SDL_GetTicks()));
    mainui_video_wait_first(video, 5000);
    SDL_Surface *frame = mainui_video_frame(video, SDL_GetTicks());
    assert(frame && frame->w == 640 && frame->h == 480 && !*mainui_video_error(video));
    assert(shown_index(frame) == 0);
    Uint32 start = SDL_GetTicks();
    int expected = 0, seen = 0, repeats = 0;
    while (seen < 15) {
        int due = mainui_video_due(video, SDL_GetTicks());
        if (due < 0) {
            SDL_Delay(2);
            assert(SDL_GetTicks() - start < 5000);
            continue;
        }
        SDL_Delay((Uint32)due);
        frame = mainui_video_frame(video, SDL_GetTicks());
        int index = shown_index(frame);
        if (index == expected) {
            continue;
        }
        /* The next frame, or a skipped one on a slow host; the sixth is
         * followed by the first again. */
        assert(index > expected || index == 0);
        repeats += index < expected;
        expected = index;
        seen++;
    }
    /* About 50 ms a frame: 15 frames in 0.75 s, give or take a slow host. */
    Uint32 took = SDL_GetTicks() - start;
    assert(repeats >= 1 && took >= 600 && took < 3000);
    mainui_video_close(video);
    /* Other screen layouts get their own byte order. */
    const Uint32 masks[][3] = {
        {0xff, 0xff00, 0xff0000}, {0xff000000, 0xff0000, 0xff00}, {0xff00, 0xff0000, 0xff000000}};
    for (int i = 0; i < 3; i++) {
        SDL_Surface *other = screen(masks[i][0], masks[i][1], masks[i][2]);
        video = open_video(dir, "ok.mp4", other);
        mainui_video_wait_first(video, 5000);
        frame = mainui_video_frame(video, SDL_GetTicks());
        assert(frame && shown_index(frame) == 0);
        mainui_video_close(video);
        SDL_FreeSurface(other);
    }

    /* A wide video fits the width, a tall one the height. */
    const struct {
        const char *name;
        int w, h;
    } shapes[] = {{"wide.mp4", 640, 160}, {"tall.mp4", 240, 480}};

    for (int i = 0; i < 2; i++) {
        video = open_video(dir, shapes[i].name, target);
        mainui_video_wait_first(video, 5000);
        frame = mainui_video_frame(video, SDL_GetTicks());
        assert(frame && frame->w == shapes[i].w && frame->h == shapes[i].h);
        mainui_video_close(video);
    }
    /* Without timestamps, frames still follow each other. */
    video = open_video(dir, "nopts.mp4", target);
    mainui_video_wait_first(video, 5000);
    assert(mainui_video_frame(video, SDL_GetTicks()));
    SDL_Delay(200);
    assert(shown_index(mainui_video_frame(video, SDL_GetTicks())) > 0);
    mainui_video_close(video);
    /* A video that cannot go back to its start keeps its last frame. */
    video = open_video(dir, "noseek.mp4", target);
    for (int i = 0; i < 40; i++) {
        frame = mainui_video_frame(video, SDL_GetTicks());
        SDL_Delay(20);
    }
    assert(frame && shown_index(frame) == 2 && !*mainui_video_error(video));
    assert(mainui_video_due(video, SDL_GetTicks()) == -1);
    mainui_video_close(video);
    /* B stops at once, also while decoding slowly and with frames waiting
     * to be shown, and closing twice in a row is harmless. */
    for (int i = 0; i < 20; i++) {
        video = open_video(dir, "slow.mp4", target);
        SDL_Delay((Uint32)(i * 7));
        Uint32 before = SDL_GetTicks();
        mainui_video_close(video);
        assert(SDL_GetTicks() - before < 200);
    }
    mainui_video_close(NULL);
    SDL_FreeSurface(target);
    /* Cover art in the file is not decoded (the stub reports it if it is). */
    target = screen(0xff0000, 0xff00, 0xff);
    MainUIVideo *covered = open_video(dir, "cover.mp4", target);
    mainui_video_wait_first(covered, 5000);
    assert(mainui_video_frame(covered, SDL_GetTicks()) && !*mainui_video_error(covered));
    mainui_video_close(covered);
    SDL_FreeSurface(target);
    puts("video frames play in order, on time and repeating; closing stops at once");
}

/* Closing while FFmpeg opens the file, probes it or reads it, each held for
 * 3 s by the stub, stops it through the interrupt callback at once. */
static void cancel(const char *dir)
{
    SDL_Surface *target = screen(0xff0000, 0xff00, 0xff);
    const char *names[] = {"waitopen.mp4", "waitprobe.mp4", "waitread.mp4"};
    for (int i = 0; i < 3; i++) {
        MainUIVideo *video = open_video(dir, names[i], target);
        SDL_Delay(150);
        assert(!mainui_video_frame(video, SDL_GetTicks()));
        Uint32 start = SDL_GetTicks();
        mainui_video_close(video);
        Uint32 took = SDL_GetTicks() - start;
        if (took > 300) {
            fprintf(stderr, "%s: closing took %u ms\n", names[i], (unsigned)took);
            abort();
        }
    }
    /* Not held up, they play. */
    MainUIVideo *video = open_video(dir, "waitread.mp4", target);
    mainui_video_wait_first(video, 5000);
    assert(mainui_video_frame(video, SDL_GetTicks()) && !*mainui_video_error(video));
    mainui_video_close(video);
    SDL_FreeSurface(target);
    puts("closing a video stops FFmpeg's opening, probing and reading at once");
}

/* A stream whose pictures grow past the limit midway (from the tenth):
 * the first ones show, then the larger picture is refused before its
 * buffer is taken (the stub reports otherwise), and the video stops. */
static void grows(const char *dir)
{
    SDL_Surface *target = screen(0xff0000, 0xff00, 0xff);
    MainUIVideo *video = open_video(dir, "grow.mp4", target);
    mainui_video_wait_first(video, 5000);
    assert(mainui_video_frame(video, SDL_GetTicks()));
    for (Uint32 start = SDL_GetTicks();
         !*mainui_video_error(video) && SDL_GetTicks() - start < 5000;) {
        mainui_video_frame(video, SDL_GetTicks());
        SDL_Delay(5);
    }
    assert(!strcmp(mainui_video_error(video), "Video too large to play"));
    mainui_video_close(video);
    SDL_FreeSurface(target);
}

static void expect_error(const char *dir, const char *name, const char *error)
{
    SDL_Surface *target = screen(0xff0000, 0xff00, 0xff);
    MainUIVideo *video = open_video(dir, name, target);
    /* Read as the UI does while the decoder fails: "" or the whole text. */
    for (Uint32 start = SDL_GetTicks(); SDL_GetTicks() - start < 5000;) {
        const char *seen = mainui_video_error(video);
        if (*seen) {
            assert(!strcmp(seen, error) || !strcmp(seen, "Cannot play this video") ||
                   !strncmp(seen, "Video", 5));
            break;
        }
    }
    mainui_video_wait_first(video, 5000);
    if (strcmp(mainui_video_error(video), error)) {
        fprintf(stderr, "%s: \"%s\", not \"%s\"\n", name, mainui_video_error(video), error);
        abort();
    }
    assert(!mainui_video_frame(video, SDL_GetTicks()) && mainui_video_due(video, 0) == -1);
    mainui_video_close(video);
    SDL_FreeSurface(target);
}

int main(int argc, char **argv)
{
    assert(argc == 4 && SDL_Init(SDL_INIT_TIMER) == 0);
    libraries = strcmp(argv[2], "-") ? argv[2] : NULL;
    if (!strcmp(argv[3], "play")) {
        play(argv[1]);
    }
    else if (!strcmp(argv[3], "cancel")) {
        cancel(argv[1]);
    }
    else if (!strcmp(argv[3], "errors")) {
        expect_error(argv[1], "text.mp4", "Cannot play this video");
        expect_error(argv[1], "missing.mp4", "Cannot play this video");
        expect_error(argv[1], "empty.mp4", "Cannot play this video");
        expect_error(argv[1], "nocodec.mp4", "Cannot play this video");
        expect_error(argv[1], "huge.mp4", "Video too large to play");
        /* Refused from the file's own size, before the decoder is opened
         * (this one cannot be), and from the size the stream itself gives,
         * before a picture buffer of that size is taken. */
        expect_error(argv[1], "hugenocodec.mp4", "Video too large to play");
        expect_error(argv[1], "liar.mp4", "Video too large to play");
        /* Before probing: a stream stating no size, or a second one too
         * large, is refused (the stub reports any probe that decodes them). */
        expect_error(argv[1], "nodims.mp4", "Cannot play this video");
        expect_error(argv[1], "extra.mp4", "Video too large to play");
        grows(argv[1]);
        SDL_Surface *paletted = SDL_CreateRGBSurface(SDL_SWSURFACE, 640, 480, 8, 0, 0, 0, 0);
        char path[4096];
        TEST_PATH(path, "%s/ok.mp4", argv[1]);
        MainUIVideo *video = mainui_video_open(path, libraries, paletted, 640, 480);
        assert(video && !strcmp(mainui_video_error(video), "Cannot play video on this screen"));
        mainui_video_close(video);
        SDL_FreeSurface(paletted);
        assert(!mainui_video_open(path, libraries, NULL, 640, 480));
        puts("unusable videos, sizes and screens show why they cannot play");
    }
    else {
        expect_error(argv[1], "ok.mp4", "Video needs Onion's FFmpeg libraries");
        puts("without usable FFmpeg libraries videos show why they cannot play");
    }
    SDL_Quit();
    return 0;
}
