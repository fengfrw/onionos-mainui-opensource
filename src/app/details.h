/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_DETAILS_H
#define MAINUI_DETAILS_H
#include "catalog/gamelist.h"
#include "ui/media_viewer.h"
#include "ui/preview.h"

/* Owns copied text and metadata; borrows the list preview and source models.
 * UI-thread only. Close before changing/freeing a source model or theme. */
typedef struct {
    bool open, favorite;
    int scroll_line, lines_per_page, line_count, ordinal, total;
    char title[4096], system[4096], rom[MAINUI_PATH_MAX];
    MainUIMetadata metadata;
    MainUIPreview *preview;
    /* The shown game, for its cover: borrowed like the list models. */
    MainUICatalog *catalog;
    const MainUILibrary *library;
    int index;
    /* For the media gallery: the ROM on this SD and its console's ROM folder
     * ("" when the console is not known). */
    char host_rom[MAINUI_PATH_MAX], rom_root[MAINUI_PATH_MAX];
    MainUIMediaViewer media;
} MainUIDetails;

bool mainui_details_open(MainUIDetails *details, MainUICatalog *catalog,
                         const MainUILibrary *library, const MainUILibrary *favorites, int row,
                         MainUIPreview *preview);
/* Up/Down wraps over games only and synchronizes the source list viewport.
 * B/Left closes. L2/R2 moves one description page. */
void mainui_details_key(MainUIDetails *details, MainUICatalog *catalog,
                        const MainUILibrary *library, const MainUILibrary *favorites,
                        MainUIViewport *view, int rows, SDLKey key);
/* Before drawing: advance the cover request without waiting (wait_ms 0), so a
 * slow decode never holds up input; it is drawn when it is done. Snapshots
 * pass MAINUI_PREVIEW_WAIT_FOREVER. */
void mainui_details_progress(MainUIDetails *details, Uint32 wait_ms);
/* The Favorite markers were reloaded: update the shown star. */
void mainui_details_markers(MainUIDetails *details, const MainUILibrary *favorites);
void mainui_details_draw(MainUIDetails *details, MainUITheme *theme, SDL_Surface *screen);
/* Right opens the game's media gallery when it has any (see media_viewer.h);
 * else it does nothing. */
void mainui_details_media(MainUIDetails *details, const char *sd, SDL_Surface *screen);
/* Also closes the gallery, stopping any video. */
void mainui_details_close(MainUIDetails *details);
#endif
