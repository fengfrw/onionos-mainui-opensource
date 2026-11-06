/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAINUI_GAME_MENU_H
#define MAINUI_GAME_MENU_H
#include "menus/context.h"

/* The game-list context menus .tmp_update/config/game-menu.json can reorder
 * and hide. Apps, the console grids and the main menu are not among them. */
typedef enum {
    MAINUI_GAME_MENU_ROMS,
    MAINUI_GAME_MENU_FAVORITES,
    MAINUI_GAME_MENU_RECENTS,
    MAINUI_GAME_MENU_SEARCH_RESULTS,
    MAINUI_GAME_MENU_SECTIONS
} MainUIGameMenuSection;

#define MAINUI_GAME_MENU_ITEMS 6

/* Per section, the IDs to show in order, as indices into that section's own
 * list of IDs. A section not configured keeps the menu exactly as built. */
typedef struct {
    bool configured[MAINUI_GAME_MENU_SECTIONS];
    int count[MAINUI_GAME_MENU_SECTIONS];
    int order[MAINUI_GAME_MENU_SECTIONS][MAINUI_GAME_MENU_ITEMS];
} MainUIGameMenu;

/* Nothing configured: every menu as built. */
void mainui_game_menu_defaults(MainUIGameMenu *menu);
/* Parse the file's text. NULL is a missing file and returns true; text that
 * is not a JSON object returns false, and both leave the defaults. A section
 * that is missing or not an array keeps its default; unknown and repeated
 * IDs are skipped; an empty array means no menu. */
bool mainui_game_menu_parse(MainUIGameMenu *menu, const char *text);
/* Read directory/game-menu.json once; a malformed file logs one line. Never
 * writes the file. */
void mainui_game_menu_load(MainUIGameMenu *menu, const char *directory);
/* Keep and order the entries a builder offered for the selection, by the
 * section's IDs. An ID whose entry the builder did not offer adds nothing:
 * the selection still decides what can show. Move stands for Move here
 * during a move, favorite for Add or Remove. */
void mainui_game_menu_apply(MainUIContext *context, const MainUIGameMenu *menu,
                            MainUIGameMenuSection section);
#endif
