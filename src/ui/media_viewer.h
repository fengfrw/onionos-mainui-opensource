/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_MEDIA_VIEWER_H
#define MAINUI_MEDIA_VIEWER_H
#include "platform/video.h"
#include "ui/theme.h"
#include <SDL.h>
#include <stdbool.h>

/* Game Info's fullscreen gallery: a game's front and back covers, 3D box,
 * screenshots and video from the media folder of its console's ROM folder,
 * matched by the ROM file's name without its extension:
 *
 *   media/box2dfront/<name>.png
 *   media/box2dback/<name>.png
 *   media/box3d/<name>.png
 *   media/screenshot/<name>.png, then <name>-1.png, <name>-2.png, ...
 *   media/videos/<name>.mp4
 *
 * The folders are named as Skraper names them by default, so its media can
 * be copied as it comes.
 *
 * Imgs, the list's and Game Info's cover, is not part of it. Separate from
 * the list's cover cache: one image or video at a time, read when shown and
 * freed when another is shown or the gallery closes. UI thread only. */

enum {
    MAINUI_MEDIA_IMAGE,
    MAINUI_MEDIA_VIDEO
};

/* At most 20 screenshots: <name>.png and <name>-1.png on. */
#define MAINUI_MEDIA_SCREENSHOTS 20
#define MAINUI_MEDIA_MAX (MAINUI_MEDIA_SCREENSHOTS + 4)

typedef struct {
    int kind;
    char path[4096];
} MainUIMediaItem;

typedef struct {
    bool open, image_failed;
    int count, index;
    MainUIMediaItem *items; /* owned while open */
    SDL_Surface *image;     /* the shown image, fitted to the screen */
    MainUIVideo *video;     /* the shown video while it plays */
    Uint32 shown_at;
    char library_dir[4096]; /* Onion's FFmpeg: <sd>/.tmp_update/lib/parasyte */
} MainUIMediaViewer;

/* The game's media in gallery order, by stat() alone: nothing is read. A
 * missing screenshot number from -2 on ends the numbered ones. Returns the
 * count. */
int mainui_media_find(const char *rom_root, const char *rom, MainUIMediaItem *items, int max);
/* Open on the first item. False, with nothing open, when the game has none.
 * sd is the SD card's root, for Onion's FFmpeg in .tmp_update/lib/parasyte. */
bool mainui_media_open(MainUIMediaViewer *viewer, const char *rom_root, const char *rom,
                       const char *sd, SDL_Surface *screen);
/* Right shows the next item, up to the last; Left the previous one, and
 * from the first it closes, as B does. Every key is taken
 * while the gallery is open. */
void mainui_media_key(MainUIMediaViewer *viewer, SDLKey key, SDL_Surface *screen);
/* snapshot: wait for a video's first frame, to draw a given state. */
void mainui_media_draw(MainUIMediaViewer *viewer, MainUITheme *theme, SDL_Surface *screen,
                       bool snapshot);
/* Milliseconds until the playing video's next frame is due, or -1. */
int mainui_media_due(const MainUIMediaViewer *viewer, Uint32 now);
/* Stop any video and free everything. Safe when closed. */
void mainui_media_close(MainUIMediaViewer *viewer);
#endif
