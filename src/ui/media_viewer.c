/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui/media_viewer.h"
#include "platform/audio.h"
#include "ui/artwork.h"
#include "ui/drawing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* A regular file: a folder or a missing name is no media. */
static bool exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static bool add(MainUIMediaItem *items, int max, int *count, int kind, const char *root,
                const char *folder, const char *name, const char *suffix)
{
    if (*count >= max) {
        return false;
    }
    MainUIMediaItem *item = &items[*count];
    int n = snprintf(item->path, sizeof item->path, "%s/media/%s/%s%s", root, folder, name, suffix);
    if (n < 0 || (size_t)n >= sizeof item->path || !exists(item->path)) {
        return false;
    }
    item->kind = kind;
    ++*count;
    return true;
}

int mainui_media_find(const char *rom_root, const char *rom, MainUIMediaItem *items, int max)
{
    if (!rom_root || !*rom_root || !rom || !items || max <= 0) {
        return 0;
    }
    /* The ROM file's name without its extension, not the game's title. */
    const char *file = strrchr(rom, '/');
    file = file ? file + 1 : rom;
    char name[1024];
    snprintf(name, sizeof name, "%s", file);
    char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        *dot = 0;
    }
    if (!*name || strlen(file) >= sizeof name) {
        return 0;
    }
    int count = 0;
    add(items, max, &count, MAINUI_MEDIA_IMAGE, rom_root, "box2dfront", name, ".png");
    add(items, max, &count, MAINUI_MEDIA_IMAGE, rom_root, "box2dback", name, ".png");
    add(items, max, &count, MAINUI_MEDIA_IMAGE, rom_root, "box3d", name, ".png");
    /* <name>.png, then -1, -2 and on to the first missing number; a missing
     * -1 does not end them, as numbering may start at -2. */
    int shots = add(items, max, &count, MAINUI_MEDIA_IMAGE, rom_root, "screenshot", name, ".png");
    for (int i = 1; shots < MAINUI_MEDIA_SCREENSHOTS; i++) {
        char suffix[24];
        snprintf(suffix, sizeof suffix, "-%d.png", i);
        if (add(items, max, &count, MAINUI_MEDIA_IMAGE, rom_root, "screenshot", name, suffix)) {
            shots++;
        }
        else if (i > 1) {
            break;
        }
    }
    add(items, max, &count, MAINUI_MEDIA_VIDEO, rom_root, "videos", name, ".mp4");
    return count;
}

static void release(MainUIMediaViewer *viewer)
{
    /* The mixer must stop asking the video for sound before it goes. */
    if (viewer->hooked) {
        mainui_audio_unhook();
        viewer->hooked = false;
    }
    mainui_video_close(viewer->video);
    viewer->video = NULL;
    if (viewer->image) {
        SDL_FreeSurface(viewer->image);
        viewer->image = NULL;
    }
    viewer->image_failed = false;
}

/* The image scaled to fit the screen, keeping its shape, on black, in the
 * screen's format: shown with one plain copy. */
static SDL_Surface *fitted(const char *path, const SDL_Surface *screen)
{
    SDL_Surface *loaded = mainui_artwork_load(path);
    SDL_Surface *image = loaded ? mainui_artwork_software(loaded) : NULL;
    if (loaded) {
        SDL_FreeSurface(loaded);
    }
    if (!image) {
        return NULL;
    }
    int width = screen->w, height = (int)((int64_t)image->h * screen->w / image->w);
    if (height > screen->h) {
        height = screen->h;
        width = (int)((int64_t)image->w * screen->h / image->h);
    }
    width = width < 1 ? 1 : width;
    height = height < 1 ? 1 : height;
    SDL_Surface *scaled = SDL_CreateRGBSurface(
        SDL_SWSURFACE, width, height, image->format->BitsPerPixel, image->format->Rmask,
        image->format->Gmask, image->format->Bmask, image->format->Amask);
    SDL_Surface *result = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height,
                                               screen->format->BitsPerPixel, screen->format->Rmask,
                                               screen->format->Gmask, screen->format->Bmask, 0);
    bool ok = scaled && result && SDL_SoftStretch(image, NULL, scaled, NULL) == 0;
    if (ok) {
        SDL_FillRect(result, NULL, SDL_MapRGB(result->format, 0, 0, 0));
        mainui_artwork_blend(scaled);
        ok = SDL_BlitSurface(scaled, NULL, result, NULL) == 0;
    }
    SDL_FreeSurface(image);
    if (scaled) {
        SDL_FreeSurface(scaled);
    }
    if (!ok && result) {
        SDL_FreeSurface(result);
        result = NULL;
    }
    return result;
}

static void show(MainUIMediaViewer *viewer, SDL_Surface *screen)
{
    release(viewer);
    const MainUIMediaItem *item = &viewer->items[viewer->index];
    viewer->shown_at = SDL_GetTicks();
    if (item->kind == MAINUI_MEDIA_VIDEO) {
        /* With sound when the theme's mixer can take it, in place of the
         * theme's music, which goes on when the video is left. */
        int rate = mainui_audio_rate();
        viewer->video =
            mainui_video_open(item->path, *viewer->library_dir ? viewer->library_dir : NULL, screen,
                              screen->w, screen->h, rate);
        viewer->image_failed = !viewer->video;
        viewer->hooked =
            viewer->video && rate && mainui_audio_hook(mainui_video_sound, viewer->video);
        if (viewer->sleeping) {
            mainui_video_hold(viewer->video, true, SDL_GetTicks());
        }
    }
    else {
        viewer->image = fitted(item->path, screen);
        viewer->image_failed = !viewer->image;
        if (viewer->image_failed) {
            fprintf(stderr, "[media] cannot show %.300s\n", item->path);
        }
    }
}

bool mainui_media_open(MainUIMediaViewer *viewer, const char *rom_root, const char *rom,
                       const char *sd, SDL_Surface *screen)
{
    mainui_media_close(viewer);
    if (sd) {
        snprintf(viewer->library_dir, sizeof viewer->library_dir, "%s/.tmp_update/lib/parasyte",
                 sd);
    }
    MainUIMediaItem *items = calloc(MAINUI_MEDIA_MAX, sizeof *items);
    int count = items ? mainui_media_find(rom_root, rom, items, MAINUI_MEDIA_MAX) : 0;
    if (!count) {
        free(items);
        return false;
    }
    viewer->items = items;
    viewer->count = count;
    viewer->index = 0;
    viewer->open = true;
    show(viewer, screen);
    return true;
}

void mainui_media_key(MainUIMediaViewer *viewer, SDLKey key, SDL_Surface *screen)
{
    if (!viewer->open) {
        return;
    }
    /* Left from the first item goes back to Game Info, as B does. */
    if (key == SDLK_ESCAPE || (key == SDLK_LEFT && viewer->index == 0)) {
        mainui_media_close(viewer);
    }
    else if (key == SDLK_LEFT) {
        viewer->index--;
        show(viewer, screen);
    }
    /* Right on the last item does nothing; B goes back. */
    else if (key == SDLK_RIGHT && viewer->index + 1 < viewer->count) {
        viewer->index++;
        show(viewer, screen);
    }
    /* A video: A pauses and goes on, L and R jump 5 s back and forward. */
    else if (key == SDLK_RETURN && viewer->video) {
        mainui_video_pause(viewer->video, !mainui_video_paused(viewer->video), SDL_GetTicks());
    }
    else if ((key == SDLK_PAGEUP || key == SDLK_PAGEDOWN) && viewer->video) {
        mainui_video_seek(viewer->video,
                          key == SDLK_PAGEUP ? -MAINUI_MEDIA_SEEK_MS : MAINUI_MEDIA_SEEK_MS);
    }
}

/* Light text on a black box, at x from the left or, negative, the right. */
static void badge(SDL_Surface *screen, TTF_Font *font, const char *text, int x)
{
    int width = 0, height = font ? TTF_FontHeight(font) : 0;
    if (!font || TTF_SizeUTF8(font, text, &width, NULL) < 0) {
        return;
    }
    SDL_Color white = {255, 255, 255, 0};
    SDL_Rect back = {(Sint16)(x >= 0 ? x : screen->w + x - width - 12),
                     (Sint16)(screen->h - height - 14), (Uint16)(width + 12), (Uint16)(height + 6)};
    SDL_FillRect(screen, &back, SDL_MapRGB(screen->format, 0, 0, 0));
    mainui_label(screen, font, white, text, back.x + 6, back.y + 3);
}

/* The pause sign, two white bars on a black box as tall as a badge, at the
 * bottom left: drawn, so it needs no glyph from the theme's font. */
static void pause_sign(SDL_Surface *screen, TTF_Font *font)
{
    int height = (font ? TTF_FontHeight(font) : 18) + 6;
    int bar = height / 5 > 3 ? height / 5 : 3, tall = height - 10;
    SDL_Rect back = {8, (Sint16)(screen->h - height - 8), (Uint16)(bar * 3 + 16), (Uint16)height};
    SDL_FillRect(screen, &back, SDL_MapRGB(screen->format, 0, 0, 0));
    Uint32 white = SDL_MapRGB(screen->format, 255, 255, 255);
    for (int i = 0; i < 2; i++) {
        SDL_Rect rect = {(Sint16)(back.x + 8 + i * bar * 2), (Sint16)(back.y + 5), (Uint16)bar,
                         (Uint16)tall};
        SDL_FillRect(screen, &rect, white);
    }
}

static void centered(SDL_Surface *screen, TTF_Font *font, const char *text, int y)
{
    int width = 0;
    if (!font || !*text || TTF_SizeUTF8(font, text, &width, NULL) < 0) {
        return;
    }
    SDL_Color white = {255, 255, 255, 0};
    mainui_label(screen, font, white, text, (screen->w - width) / 2, y);
}

void mainui_media_draw(MainUIMediaViewer *viewer, MainUITheme *theme, SDL_Surface *screen,
                       bool snapshot)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
    if (!viewer->open) {
        return;
    }
    SDL_Surface *shown = viewer->image;
    const char *error = viewer->image_failed
                            ? viewer->items[viewer->index].kind == MAINUI_MEDIA_VIDEO
                                  ? "Cannot play this video"
                                  : "Cannot show this image"
                            : "";
    if (viewer->video) {
        /* A snapshot draws the first frame, at the instant it was shown. */
        if (snapshot) {
            mainui_video_wait_first(viewer->video, 5000);
        }
        shown = mainui_video_frame(viewer->video, snapshot ? viewer->shown_at : SDL_GetTicks());
        error = mainui_video_error(viewer->video);
    }
    if (*error) {
        TTF_Font *font = theme->menu_font ? theme->menu_font : theme->font;
        centered(screen, font, error, (screen->h - (font ? TTF_FontHeight(font) : 0)) / 2);
    }
    else if (shown) {
        mainui_blit(screen, shown, (screen->w - shown->w) / 2, (screen->h - shown->h) / 2);
    }
    if (viewer->count > 1) {
        char counter[32];
        snprintf(counter, sizeof counter, "%d/%d", viewer->index + 1, viewer->count);
        badge(screen, theme->hint_font, counter, -8);
    }
    if (viewer->video && !*error && mainui_video_paused(viewer->video)) {
        pause_sign(screen, theme->hint_font);
    }
}

int mainui_media_due(const MainUIMediaViewer *viewer, Uint32 now)
{
    return viewer->open && viewer->video ? mainui_video_due(viewer->video, now) : -1;
}

void mainui_media_sleep(MainUIMediaViewer *viewer, bool sleeping, Uint32 now)
{
    viewer->sleeping = sleeping;
    if (viewer->video) {
        mainui_video_hold(viewer->video, sleeping, now);
    }
}

void mainui_media_close(MainUIMediaViewer *viewer)
{
    bool sleeping = viewer->sleeping;
    release(viewer);
    free(viewer->items);
    *viewer = (MainUIMediaViewer){0};
    viewer->sleeping = sleeping;
}
