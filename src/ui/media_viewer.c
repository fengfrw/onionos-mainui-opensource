/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui/media_viewer.h"
#include "localization/language.h"
#include "platform/audio.h"
#include "ui/artwork.h"
#include "ui/drawing.h"
#include "ui/panels.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

/* The ROM file's name without its extension, not the game's title. */
static bool media_name(const char *rom_root, const char *rom, char name[1024])
{
    if (!rom_root || !*rom_root || !rom) {
        return false;
    }
    const char *file = strrchr(rom, '/');
    file = file ? file + 1 : rom;
    snprintf(name, 1024, "%s", file);
    char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        *dot = 0;
    }
    return *name && strlen(file) < 1024;
}

/* Prefer the original lowercase .mp4 path without reading the directory.
 * Otherwise inspect media/videos once, accepting only the exact ROM basename
 * and a case-insensitive extension. Keep the actual directory-entry filename
 * for files found this way, including on case-sensitive host filesystems. */
static bool find_video(const char *rom_root, const char *name, char path[4096])
{
    int n = snprintf(path, 4096, "%s/media/videos/%s.mp4", rom_root, name);
    if (n >= 0 && n < 4096 && exists(path)) {
        return true;
    }
    path[0] = 0;

    char directory[4096];
    n = snprintf(directory, sizeof directory, "%s/media/videos", rom_root);
    if (n < 0 || (size_t)n >= sizeof directory) {
        return false;
    }
    DIR *dir = opendir(directory);
    if (!dir) {
        return false;
    }
    /* One video per game, in a stable format order regardless of readdir(). */
    static const char *const formats[] = {"mp4", "mkv", "avi", "mov", "webm", "m4v"};
    const size_t name_len = strlen(name);
    int best = (int)(sizeof formats / sizeof *formats);
    char best_name[256] = "";
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *filename = entry->d_name;
        size_t length = strlen(filename);
        if (length <= name_len + 1 || length >= sizeof best_name ||
            strncmp(filename, name, name_len) || filename[name_len] != '.') {
            continue;
        }
        const char *extension = filename + name_len + 1;
        for (int i = 0; i < (int)(sizeof formats / sizeof *formats); i++) {
            if (strcasecmp(extension, formats[i]) ||
                (i > best || (i == best && strcmp(filename, best_name) >= 0))) {
                continue;
            }
            n = snprintf(path, 4096, "%s/%s", directory, filename);
            if (n >= 0 && n < 4096 && exists(path)) {
                best = i;
                memcpy(best_name, filename, length + 1);
            }
            break;
        }
    }
    closedir(dir);
    if (best == (int)(sizeof formats / sizeof *formats)) {
        path[0] = 0;
        return false;
    }
    n = snprintf(path, 4096, "%s/%s", directory, best_name);
    return n >= 0 && n < 4096;
}

bool mainui_media_video(const char *rom_root, const char *rom, char path[4096])
{
    char name[1024];
    return path && media_name(rom_root, rom, name) && find_video(rom_root, name, path);
}

int mainui_media_find(const char *rom_root, const char *rom, MainUIMediaItem *items, int max)
{
    char name[1024];
    if (!items || max <= 0 || !media_name(rom_root, rom, name)) {
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
    /* The list preview and the gallery share the same video choice. */
    if (count < max && find_video(rom_root, name, items[count].path)) {
        items[count++].kind = MAINUI_MEDIA_VIDEO;
    }
    return count;
}

enum {
    MEDIA_GALLERY,
    MEDIA_MENU,
    MEDIA_MAPS,
    MEDIA_TEXT
};

enum {
    MENU_INFO,
    MENU_ARTWORK,
    MENU_MAPS,
    MENU_TEXT,
    MENU_PDF
};

/* The menu's rows: every view the game has, Game Info always, but never the
 * one the menu was opened from: Game Info from Game Info, Gallery from the
 * gallery, and so on. */
static bool menu_has(const MainUIMediaViewer *viewer, int type)
{
    static const int from[] = {MEDIA_RETURN_DETAILS, MEDIA_RETURN_GALLERY, MEDIA_RETURN_MAPS,
                               MEDIA_RETURN_TEXT, -1};
    if (from[type] == viewer->menu_return) {
        return false;
    }
    return type == MENU_INFO      ? true
           : type == MENU_ARTWORK ? viewer->count > 0
           : type == MENU_MAPS    ? viewer->map_count > 0
           : type == MENU_TEXT    ? *viewer->manual_path != 0
                                  : *viewer->pdf_path != 0;
}

static int menu_items(const MainUIMediaViewer *viewer)
{
    int count = 0;
    for (int type = MENU_INFO; type <= MENU_PDF; type++) {
        count += menu_has(viewer, type);
    }
    return count;
}

/* Visible menu slot -> media category; there are no disabled/empty rows. */
static int menu_type(const MainUIMediaViewer *viewer, int slot)
{
    for (int type = MENU_INFO; type <= MENU_PDF; type++) {
        if (menu_has(viewer, type) && slot-- == 0) {
            return type;
        }
    }
    return MENU_INFO;
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
    viewer->video_pending = false;
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

/* The video shown starts: with sound when the theme's mixer can take it,
 * in place of the theme's music, which goes on when the video is left. */
static void start_video(MainUIMediaViewer *viewer, SDL_Surface *screen)
{
    const MainUIMediaItem *item = &viewer->items[viewer->index];
    viewer->video_pending = false;
    viewer->shown_at = SDL_GetTicks();
    int rate = mainui_audio_rate();
    viewer->video = mainui_video_open(item->path, *viewer->library_dir ? viewer->library_dir : NULL,
                                      screen, screen->w, screen->h, rate);
    viewer->image_failed = !viewer->video;
    viewer->hooked = viewer->video && rate && mainui_audio_hook(mainui_video_sound, viewer->video);
    if (viewer->sleeping) {
        mainui_video_hold(viewer->video, true, SDL_GetTicks());
    }
}

/* Switch map/zoom only on input, never in the render loop. Decoding uses the
 * existing artwork dimension/size limits (2000 pixels, 16 MiB). */
static void show_map(MainUIMediaViewer *viewer, SDL_Surface *screen)
{
    release(viewer);
    const char *path = viewer->maps[viewer->map_index].path;
    if (viewer->map_zoom) {
        SDL_Surface *loaded = mainui_artwork_load(path);
        viewer->image = loaded ? mainui_artwork_software(loaded) : NULL;
        if (loaded) {
            SDL_FreeSurface(loaded);
        }
        if (viewer->image) {
            mainui_artwork_blend(viewer->image);
        }
    }
    else {
        viewer->image = fitted(path, screen);
    }
    viewer->image_failed = !viewer->image;
    viewer->map_pan_x = viewer->map_pan_y = 0;
    if (viewer->image_failed) {
        fprintf(stderr, "[media] cannot show map %.300s\n", path);
    }
}

static void show(MainUIMediaViewer *viewer, SDL_Surface *screen)
{
    (void)screen;
    release(viewer);
    const MainUIMediaItem *item = &viewer->items[viewer->index];
    viewer->shown_at = SDL_GetTicks();
    if (item->kind == MAINUI_MEDIA_VIDEO) {
        /* Started by mainui_media_draw() once it has stayed shown. */
        viewer->video_pending = true;
        viewer->video_at = viewer->shown_at + MAINUI_MEDIA_VIDEO_DELAY_MS;
    }
    else {
        viewer->image = fitted(item->path, screen);
        viewer->image_failed = !viewer->image;
        if (viewer->image_failed) {
            fprintf(stderr, "[media] cannot show %.300s\n", item->path);
        }
    }
}

static bool open_media(MainUIMediaViewer *viewer, const char *rom_root, const char *rom,
                       const char *sd, SDL_Surface *screen, bool menu)
{
    mainui_media_close(viewer);
    if (sd) {
        snprintf(viewer->library_dir, sizeof viewer->library_dir, "%s/.tmp_update/lib/parasyte",
                 sd);
    }
    MainUIMediaItem *items = calloc(MAINUI_MEDIA_MAX, sizeof *items);
    /* Ordinary games without a maps directory never allocate the map array. */
    char map_directory[4096];
    struct stat info;
    int n =
        rom_root ? snprintf(map_directory, sizeof map_directory, "%s/media/maps", rom_root) : -1;
    bool has_maps = n >= 0 && (size_t)n < sizeof map_directory && !stat(map_directory, &info) &&
                    S_ISDIR(info.st_mode);
    MainUIMap *maps = has_maps ? calloc(MAINUI_MEDIA_MAPS_MAX, sizeof *maps) : NULL;
    int count = items ? mainui_media_find(rom_root, rom, items, MAINUI_MEDIA_MAX) : 0;
    int map_count = maps ? mainui_maps_find(rom_root, rom, maps, MAINUI_MEDIA_MAPS_MAX) : 0;
    bool has_text = mainui_text_find(rom_root, rom, viewer->manual_path);
    bool has_pdf = mainui_manual_find(rom_root, rom, ".pdf", viewer->pdf_path);
    /* Right opens the gallery only; Select opens the menu. */
    if ((!count && !map_count && !has_text && !has_pdf) || (!menu && !count)) {
        free(maps);
        free(items);
        return false;
    }
    if (!count) {
        free(items);
        items = NULL;
    }
    if (!map_count) {
        free(maps);
        maps = NULL;
    }
    viewer->maps = maps;
    viewer->map_count = map_count;
    viewer->items = items;
    viewer->count = count;
    viewer->index = 0;
    viewer->open = true;
    viewer->mode = menu ? MEDIA_MENU : MEDIA_GALLERY;
    viewer->menu_return = MEDIA_RETURN_DETAILS;
    viewer->menu_selection = 0;
    if (viewer->mode == MEDIA_GALLERY) {
        show(viewer, screen);
    }
    return true;
}

bool mainui_media_open(MainUIMediaViewer *viewer, const char *rom_root, const char *rom,
                       const char *sd, SDL_Surface *screen)
{
    return open_media(viewer, rom_root, rom, sd, screen, false);
}

bool mainui_media_open_menu(MainUIMediaViewer *viewer, const char *rom_root, const char *rom,
                            const char *sd, SDL_Surface *screen)
{
    return open_media(viewer, rom_root, rom, sd, screen, true);
}

bool mainui_media_over_details(const MainUIMediaViewer *viewer)
{
    return viewer->open && viewer->mode == MEDIA_MENU &&
           viewer->menu_return == MEDIA_RETURN_DETAILS;
}

/* Open the menu over the view shown, which it leaves out: Select there. */
static void menu_over(MainUIMediaViewer *viewer, int back)
{
    viewer->mode = MEDIA_MENU;
    viewer->menu_selection = 0;
    viewer->menu_return = back;
}

/* Back where the menu was opened from: the gallery as it was, a paused video
 * going on and one not started yet waiting its delay again; the map or the
 * walkthrough at the same place; or Game Info. */
static void menu_back(MainUIMediaViewer *viewer, SDL_Surface *screen)
{
    int back = viewer->menu_return;
    if (back == MEDIA_RETURN_GALLERY && viewer->count) {
        viewer->mode = MEDIA_GALLERY;
        if (viewer->video) {
            if (viewer->menu_paused) {
                mainui_video_pause(viewer->video, false, SDL_GetTicks());
            }
        }
        else if (viewer->video_pending) {
            viewer->video_at = SDL_GetTicks() + MAINUI_MEDIA_VIDEO_DELAY_MS;
        }
        else if (!viewer->image) {
            show(viewer, screen);
        }
        viewer->menu_paused = false;
    }
    else if (back == MEDIA_RETURN_MAPS && viewer->map_count) {
        viewer->mode = MEDIA_MAPS;
        if (!viewer->image && !viewer->image_failed) {
            show_map(viewer, screen);
        }
    }
    else if (back == MEDIA_RETURN_TEXT) {
        viewer->mode = MEDIA_TEXT;
    }
    else {
        mainui_media_close(viewer);
    }
}

/* Another view chosen in the menu: the picture, map or video shown under it
 * goes, not only pauses; going back shows it again from its start. */
static void leave_view(MainUIMediaViewer *viewer)
{
    release(viewer);
    viewer->menu_paused = false;
}

void mainui_media_back_from_pdf(MainUIMediaViewer *viewer, SDL_Surface *screen)
{
    if (viewer && viewer->open && viewer->mode == MEDIA_MENU) {
        menu_back(viewer, screen);
    }
}

static void close_text(MainUIMediaViewer *viewer)
{
    if (viewer->text) {
        mainui_text_close(viewer->text);
        free(viewer->text);
    }
    viewer->text = NULL;
    viewer->text_scroll = 0;
}

void mainui_media_key(MainUIMediaViewer *viewer, SDLKey key, SDL_Surface *screen)
{
    if (!viewer->open) {
        return;
    }
    if (viewer->mode == MEDIA_MENU) {
        /* B and Select go back where the menu was opened from. */
        if (key == SDLK_ESCAPE || key == SDLK_RCTRL) {
            menu_back(viewer, screen);
        }
        else if (key == SDLK_UP || key == SDLK_DOWN) {
            int n = menu_items(viewer);
            viewer->menu_selection = (viewer->menu_selection + (key == SDLK_UP ? n - 1 : 1)) % n;
        }
        else if (key == SDLK_RETURN) {
            int option = menu_type(viewer, viewer->menu_selection);
            if (option == MENU_INFO) {
                mainui_media_close(viewer);
            }
            else if (option == MENU_ARTWORK) {
                leave_view(viewer);
                close_text(viewer);
                viewer->menu_return = MEDIA_RETURN_GALLERY;
                menu_back(viewer, screen);
            }
            else if (option == MENU_MAPS) {
                leave_view(viewer);
                close_text(viewer);
                viewer->map_index = 0;
                viewer->map_zoom = false;
                viewer->menu_return = MEDIA_RETURN_MAPS;
                menu_back(viewer, screen);
            }
            else if (option == MENU_PDF) {
                /* Green takes the screen and the sound: nothing may play. The
                 * menu stays, over what it was opened from, for afterwards. */
                leave_view(viewer);
                viewer->pdf_requested = true;
            }
            else {
                leave_view(viewer);
                close_text(viewer);
                viewer->text = calloc(1, sizeof *viewer->text);
                if (viewer->text && !mainui_text_open(viewer->text, viewer->manual_path)) {
                    free(viewer->text);
                    viewer->text = NULL;
                }
                viewer->mode = MEDIA_TEXT;
            }
        }
        return;
    }
    /* B in the gallery, a map or the walkthrough goes back to Game Info. */
    if (key == SDLK_ESCAPE) {
        mainui_media_close(viewer);
        return;
    }
    if (viewer->mode == MEDIA_TEXT) {
        viewer->search_missing = false;
        if (key == SDLK_RCTRL) {
            menu_over(viewer, MEDIA_RETURN_TEXT);
        }
        /* A searches, through the application's keyboard; Start finds the
         * next match. */
        else if (key == SDLK_RETURN && viewer->text) {
            viewer->search_requested = true;
        }
        else if (key == SDLK_F2 && viewer->text && *viewer->search) {
            mainui_media_search(viewer, viewer->search, true);
        }
        else if (key == SDLK_UP) {
            viewer->text_scroll--;
        }
        else if (key == SDLK_DOWN) {
            viewer->text_scroll++;
        }
        else if (key == SDLK_PAGEUP) {
            viewer->text_scroll -= 15;
        }
        else if (key == SDLK_PAGEDOWN) {
            viewer->text_scroll += 15;
        }
        else if (key == SDLK_HOME) {
            viewer->text_scroll -= 75;
        }
        else if (key == SDLK_END) {
            viewer->text_scroll += 75;
        }
        if (viewer->text_scroll > 10000) {
            viewer->text_scroll = 10000;
        }
        if (viewer->text_scroll < -10000) {
            viewer->text_scroll = -10000;
        }
        return;
    }
    if (viewer->mode == MEDIA_MAPS) {
        if (key == SDLK_RCTRL) {
            menu_over(viewer, MEDIA_RETURN_MAPS);
        }
        else if (key == SDLK_PAGEUP || key == SDLK_PAGEDOWN) {
            viewer->map_index =
                (viewer->map_index + viewer->map_count + (key == SDLK_PAGEUP ? -1 : 1)) %
                viewer->map_count;
            show_map(viewer, screen);
        }
        /* A toggles fit and native size; L2 fits, R2 shows native size. */
        else if (key == SDLK_RETURN || (key == SDLK_HOME && viewer->map_zoom) ||
                 (key == SDLK_END && !viewer->map_zoom)) {
            viewer->map_zoom = !viewer->map_zoom;
            show_map(viewer, screen);
        }
        else if (viewer->map_zoom && viewer->image) {
            const int step = 48;
            if (key == SDLK_LEFT) {
                viewer->map_pan_x += step;
            }
            if (key == SDLK_RIGHT) {
                viewer->map_pan_x -= step;
            }
            if (key == SDLK_UP) {
                viewer->map_pan_y += step;
            }
            if (key == SDLK_DOWN) {
                viewer->map_pan_y -= step;
            }
        }
        return;
    }
    if (key == SDLK_RCTRL) {
        /* A playing video pauses under the menu and goes on after it. */
        viewer->menu_paused = viewer->video && !mainui_video_paused(viewer->video);
        if (viewer->menu_paused) {
            mainui_video_pause(viewer->video, true, SDL_GetTicks());
        }
        menu_over(viewer, MEDIA_RETURN_GALLERY);
    }
    /* Left from the first item goes back to Game Info, as B does. */
    else if (key == SDLK_LEFT && viewer->index == 0) {
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

static void draw_text(MainUIMediaViewer *viewer, MainUITheme *theme, SDL_Surface *screen);
static void draw_map(MainUIMediaViewer *viewer, MainUITheme *theme, SDL_Surface *screen);

static void draw_menu(MainUIMediaViewer *viewer, MainUITheme *theme, SDL_Surface *screen)
{
    /* The stock context popup is also used by Games/Expert. Keep its themed
     * background and selection, but suppress footer instructions here. */
    static const char *const fallbacks[] = {"Game Info", "Gallery", "Game Maps",
                                            "Walkthrough (TXT)", "Manual (PDF)"};
    /* Kept off the stack, and only drawn from, on the UI thread. */
    static MainUIContext menu;
    memset(&menu, 0, sizeof menu);
    menu.visible_count = menu_items(viewer);
    menu.selected = viewer->menu_selection;
    for (int i = 0; i < menu.visible_count; i++) {
        int type = menu_type(viewer, i);
        menu.visible[i] = i;
        /* These are presentation-only rows; the viewer handles selection. */
        menu.entries[i].action = CONTEXT_CUSTOM1;
        snprintf(menu.entries[i].label, sizeof menu.entries[i].label, "%s",
                 mainui_translate(411 + type, fallbacks[type]));
    }
    /* Over the view it was opened from, as it stands: Game Info is drawn
     * by the caller, a map or the walkthrough here, and the gallery's
     * picture or its video's paused frame. */
    if (viewer->menu_return == MEDIA_RETURN_MAPS) {
        draw_map(viewer, theme, screen);
    }
    else if (viewer->menu_return == MEDIA_RETURN_TEXT) {
        draw_text(viewer, theme, screen);
    }
    else if (viewer->menu_return == MEDIA_RETURN_GALLERY) {
        SDL_Surface *shown =
            viewer->video ? mainui_video_frame(viewer->video, SDL_GetTicks()) : viewer->image;
        if (shown) {
            mainui_blit(screen, shown, (screen->w - shown->w) / 2, (screen->h - shown->h) / 2);
        }
    }
    /* Game Info keeps its footer clear, as under other popups; the views
     * without one are darkened to the bottom. */
    if (viewer->menu_return == MEDIA_RETURN_DETAILS) {
        mainui_dim_popup_background(screen, theme);
    }
    else {
        mainui_dim_popup_screen(screen, theme);
    }
    mainui_draw_media_context(screen, theme, &menu);
}

static int text_measure(const char *text, void *context)
{
    int width = 0;
    return TTF_SizeUTF8((TTF_Font *)context, text, &width, NULL) == 0 ? width : 100000;
}

/* Where a search's text is in the walkthrough, from the byte at from: ASCII
 * letters in any case, other bytes as they are. */
static const char *find_text(const char *data, size_t bytes, size_t from, const char *term)
{
    size_t length = strlen(term);
    for (size_t i = from; length && i + length <= bytes; i++) {
        size_t k = 0;
        while (k < length &&
               tolower((unsigned char)data[i + k]) == tolower((unsigned char)term[k])) {
            k++;
        }
        if (k == length) {
            return data + i;
        }
    }
    return NULL;
}

void mainui_media_search(MainUIMediaViewer *viewer, const char *term, bool next)
{
    MainUITextReader *reader = viewer->text;
    if (term != viewer->search) {
        snprintf(viewer->search, sizeof viewer->search, "%s", term ? term : "");
    }
    viewer->search_missing = viewer->search_pending = false;
    if (!*viewer->search) {
        viewer->search_found = false;
    }
    if (!reader || !reader->data || !*viewer->search) {
        return;
    }
    /* Input coalescing may have skipped the redraw that normally applies
     * queued scrolling. Search from the requested line, not the old top. */
    if (viewer->text_scroll && reader->count > 0) {
        long wanted = (long)reader->top + viewer->text_scroll;
        if (wanted < 0) {
            wanted = 0;
        }
        if (wanted >= reader->count) {
            wanted = reader->count - 1;
        }
        reader->top = (int)wanted;
        viewer->text_scroll = 0;
    }
    /* From the page shown, or after the match shown; then round from the
     * start. */
    size_t from = next && viewer->search_found  ? viewer->search_at + 1
                  : reader->top < reader->count ? reader->lines[reader->top].start
                                                : 0;
    const char *hit = find_text(reader->data, reader->bytes, from, viewer->search);
    if (!hit) {
        hit = find_text(reader->data, reader->bytes, 0, viewer->search);
    }
    if (!hit) {
        viewer->search_missing = true;
        viewer->search_missing_at = SDL_GetTicks();
        return;
    }
    viewer->search_at = (size_t)(hit - reader->data);
    viewer->search_found = viewer->search_pending = true;
}

/* The line holding byte at: laid out as far as it, within the reader's
 * limits. -1 when it is past them. */
static int line_of(MainUITextReader *reader, size_t at, TTF_Font *font, int width)
{
    while (!reader->complete && (!reader->count || reader->lines[reader->count - 1].start <= at)) {
        int count = reader->count;
        mainui_text_layout(reader, width, text_measure, font, count + 256);
        if (reader->count == count) {
            break;
        }
    }
    int line = -1;
    for (int i = 0; i < reader->count && reader->lines[i].start <= at; i++) {
        line = i;
    }
    return line >= 0 &&
                   (line + 1 < reader->count || at < reader->lines[line].end || reader->complete)
               ? line
               : -1;
}

/* Highlight the visible part of each source-buffer match, including a
 * phrase whose words straddle two separately wrapped visual lines. */
static void mark_matches(SDL_Surface *screen, TTF_Font *font, const MainUITextReader *reader,
                         int index, const char *term, int x, int y, int height)
{
    MainUITextLine spans[64];
    int matches = mainui_text_match_segments(reader, index, term, spans, 64);
    if (matches <= 0) {
        return;
    }
    MainUITextLine visual = reader->lines[index];
    char part[MAINUI_TEXT_LINE_BYTES];
    for (int i = 0; i < matches; i++) {
        size_t before_bytes = spans[i].start - visual.start;
        size_t mark_bytes = spans[i].end - spans[i].start;
        if (before_bytes >= sizeof part || mark_bytes >= sizeof part) {
            continue;
        }
        memcpy(part, reader->data + visual.start, before_bytes);
        part[before_bytes] = 0;
        int before = 0, wide = 0;
        TTF_SizeUTF8(font, part, &before, NULL);
        memcpy(part, reader->data + spans[i].start, mark_bytes);
        part[mark_bytes] = 0;
        TTF_SizeUTF8(font, part, &wide, NULL);
        SDL_Rect mark = {(Sint16)(x + before - 1), (Sint16)y, (Uint16)(wide + 2), (Uint16)height};
        SDL_FillRect(screen, &mark, SDL_MapRGB(screen->format, 150, 110, 20));
    }
}

static void draw_text(MainUIMediaViewer *viewer, MainUITheme *theme, SDL_Surface *screen)
{
    TTF_Font *font = theme->detail_font ? theme->detail_font : theme->font;
    if (!font) {
        return;
    }
    if (!viewer->text) {
        centered(screen, font, "Cannot read walkthrough", screen->h / 2);
        return;
    }
    MainUITextReader *reader = viewer->text;
    int height = TTF_FontHeight(font);
    if (height <= 0) {
        return;
    }
    /* No title: the text from the top, down to the line number's badge. */
    TTF_Font *hint = theme->hint_font ? theme->hint_font : font;
    int line_step = height + 3, top = 8, width = screen->w - 40;
    int rows = (screen->h - TTF_FontHeight(hint) - 14 - 4 - top) / line_step;
    if (rows < 1) {
        rows = 1;
    }
    /* Build only as many visual rows as needed, then reuse their offsets. */
    int wanted_top = reader->top + viewer->text_scroll;
    if (viewer->search_pending) {
        viewer->search_pending = false;
        int line = line_of(reader, viewer->search_at, font, width);
        if (line >= 0) {
            wanted_top = line;
        }
        else {
            viewer->search_missing = true;
            viewer->search_missing_at = SDL_GetTicks();
        }
    }
    if (wanted_top < 0) {
        wanted_top = 0;
    }
    if (wanted_top > MAINUI_TEXT_MAX_LINES - rows) {
        wanted_top = MAINUI_TEXT_MAX_LINES - rows;
    }
    if (wanted_top < 0) {
        wanted_top = 0;
    }
    mainui_text_layout(reader, width, text_measure, font, wanted_top + rows);
    if (reader->complete && wanted_top > reader->count - rows) {
        wanted_top = reader->count > rows ? reader->count - rows : 0;
    }
    reader->top = wanted_top;
    viewer->text_scroll = 0;

    SDL_Color white = {230, 230, 230, 0};
    for (int i = 0; i < rows && reader->top + i < reader->count; i++) {
        char line[MAINUI_TEXT_LINE_BYTES];
        mainui_text_line(reader, reader->top + i, line);
        if (*line) {
            if (viewer->search_found) {
                mark_matches(screen, font, reader, reader->top + i, viewer->search, 16,
                             top + i * line_step, height);
            }
            mainui_label(screen, font, white, line, 16, top + i * line_step);
        }
    }
    /* The line number, or for a moment that a search found nothing. */
    if (viewer->search_missing && SDL_GetTicks() - viewer->search_missing_at >= 2000) {
        viewer->search_missing = false;
    }
    char position[32];
    snprintf(position, sizeof position, "%d", reader->top + (reader->count > 0));
    badge(screen, theme->hint_font, viewer->search_missing ? "Not found" : position, 8);
}

static void draw_map(MainUIMediaViewer *viewer, MainUITheme *theme, SDL_Surface *screen)
{
    SDL_Surface *image = viewer->image;
    if (!image) {
        TTF_Font *font = theme->menu_font ? theme->menu_font : theme->font;
        centered(screen, font, "Cannot show this map", screen->h / 2);
    }
    else {
        int x = (screen->w - image->w) / 2, y = (screen->h - image->h) / 2;
        if (viewer->map_zoom) {
            if (image->w > screen->w) {
                int min = screen->w - image->w;
                x = (screen->w - image->w) / 2 + viewer->map_pan_x;
                if (x < min) {
                    x = min;
                }
                if (x > 0) {
                    x = 0;
                }
                viewer->map_pan_x = x - (screen->w - image->w) / 2;
            }
            if (image->h > screen->h) {
                int min = screen->h - image->h;
                y = (screen->h - image->h) / 2 + viewer->map_pan_y;
                if (y < min) {
                    y = min;
                }
                if (y > 0) {
                    y = 0;
                }
                viewer->map_pan_y = y - (screen->h - image->h) / 2;
            }
        }
        /* SDL clips the source image to the destination screen. */
        mainui_blit(screen, image, x, y);
    }
    /* No title: only which map it is, fixed while the image pans. */
    char counter[32];
    snprintf(counter, sizeof counter, "%d/%d", viewer->map_index + 1, viewer->map_count);
    badge(screen, theme->hint_font, counter, -8);
}

void mainui_media_draw(MainUIMediaViewer *viewer, MainUITheme *theme, SDL_Surface *screen,
                       bool snapshot)
{
    if (!mainui_media_over_details(viewer)) {
        SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
    }
    if (!viewer->open) {
        return;
    }
    if (viewer->mode == MEDIA_MENU) {
        draw_menu(viewer, theme, screen);
        return;
    }
    if (viewer->mode == MEDIA_MAPS) {
        draw_map(viewer, theme, screen);
        return;
    }
    if (viewer->mode == MEDIA_TEXT) {
        draw_text(viewer, theme, screen);
        return;
    }
    if (viewer->video_pending && (snapshot || (Sint32)(SDL_GetTicks() - viewer->video_at) >= 0)) {
        start_video(viewer, screen);
    }
    SDL_Surface *shown = viewer->image;
    const char *error = viewer->image_failed
                            ? viewer->items[viewer->index].kind == MAINUI_MEDIA_VIDEO
                                  ? "Cannot play this video"
                                  : "Cannot show this image"
                            : "";
    if (viewer->video) {
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
    bool paused = viewer->video && !*error && mainui_video_paused(viewer->video);
    if (paused) {
        pause_sign(screen, theme->hint_font);
    }
}

int mainui_media_due(const MainUIMediaViewer *viewer, Uint32 now)
{
    if (viewer->open && viewer->mode == MEDIA_TEXT && viewer->search_missing) {
        Sint32 until = (Sint32)(viewer->search_missing_at + 2000 - now);
        return until > 0 ? (int)until : 0;
    }
    if (viewer->open && viewer->mode == MEDIA_GALLERY && viewer->video_pending) {
        Sint32 until = (Sint32)(viewer->video_at - now);
        return until > 0 ? (int)until : 0;
    }
    return viewer->open && viewer->mode == MEDIA_GALLERY && viewer->video
               ? mainui_video_due(viewer->video, now)
               : -1;
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
    free(viewer->maps);
    if (viewer->text) {
        mainui_text_close(viewer->text);
        free(viewer->text);
    }
    *viewer = (MainUIMediaViewer){0};
    viewer->sleeping = sleeping;
}
