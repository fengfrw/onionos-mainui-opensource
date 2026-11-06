/* SPDX-License-Identifier: GPL-3.0-only */
#include "menus/game_menu.h"
#include "platform/files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *id;
    MainUIContextAction action, alternate;
} GameMenuItem;

/* Each section's IDs in their default order. The alternate is the same row
 * in another state: Move here during a move, Remove for a Favorite. */
static const GameMenuItem roms[] = {{"launch", CONTEXT_START, CONTEXT_START},
                                    {"favorite", CONTEXT_ADD_FAVORITE, CONTEXT_REMOVE_FAVORITE},
                                    {"delete", CONTEXT_DELETE_ROM, CONTEXT_DELETE_ROM},
                                    {"search", CONTEXT_SEARCH, CONTEXT_SEARCH},
                                    {"refresh", CONTEXT_REFRESH_SYSTEM, CONTEXT_REFRESH_SYSTEM}};
static const GameMenuItem favorites[] = {
    {"move", CONTEXT_FAVORITE_MOVE, CONTEXT_FAVORITE_PASTE},
    {"create_folder", CONTEXT_FAVORITE_CREATE, CONTEXT_FAVORITE_CREATE},
    {"sort", CONTEXT_FAVORITE_SORT, CONTEXT_FAVORITE_SORT},
    {"remove_favorite", CONTEXT_REMOVE_FAVORITE, CONTEXT_REMOVE_FAVORITE},
    {"rename_folder", CONTEXT_FAVORITE_RENAME, CONTEXT_FAVORITE_RENAME},
    {"delete_folder", CONTEXT_FAVORITE_DELETE, CONTEXT_FAVORITE_DELETE}};
static const GameMenuItem recents[] = {
    {"launch", CONTEXT_START, CONTEXT_START},
    {"remove_recent", CONTEXT_REMOVE_RECENT, CONTEXT_REMOVE_RECENT},
    {"clear_recents", CONTEXT_CLEAR_RECENT, CONTEXT_CLEAR_RECENT}};
static const GameMenuItem search_results[] = {
    {"launch", CONTEXT_START, CONTEXT_START},
    {"favorite", CONTEXT_ADD_FAVORITE, CONTEXT_REMOVE_FAVORITE}};
static const GameMenuItem consoles[] = {
    {"refresh_all", CONTEXT_REFRESH, CONTEXT_REFRESH},
    {"refresh", CONTEXT_REFRESH_SYSTEM, CONTEXT_REFRESH_SYSTEM}};

static const struct {
    const char *key;
    const GameMenuItem *items;
    int count;
} sections[MAINUI_GAME_MENU_ALL] = {
    {"roms", roms, (int)(sizeof roms / sizeof *roms)},
    {"favorites", favorites, (int)(sizeof favorites / sizeof *favorites)},
    {"recents", recents, (int)(sizeof recents / sizeof *recents)},
    {"search_results", search_results, (int)(sizeof search_results / sizeof *search_results)},
    {"menu", consoles, (int)(sizeof consoles / sizeof *consoles)}};

void mainui_game_menu_defaults(MainUIGameMenu *menu)
{
    *menu = (MainUIGameMenu){0};
}

bool mainui_game_menu_parse(MainUIGameMenu *menu, const char *text)
{
    mainui_game_menu_defaults(menu);
    if (!text) {
        return true;
    }
    cJSON *root = mainui_menu_json(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    for (int s = 0; s < MAINUI_GAME_MENU_SECTIONS; s++) {
        mainui_game_menu_parse_list(menu, s,
                                    cJSON_GetObjectItemCaseSensitive(root, sections[s].key));
    }
    cJSON_Delete(root);
    return true;
}

void mainui_game_menu_parse_list(MainUIGameMenu *menu, MainUIGameMenuSection s, const cJSON *list)
{
    if (!cJSON_IsArray(list) || (int)s < 0 || s >= MAINUI_GAME_MENU_ALL) {
        return;
    }
    menu->configured[s] = true;
    menu->count[s] = 0;
    const cJSON *id;
    cJSON_ArrayForEach(id, list)
    {
        if (!cJSON_IsString(id)) {
            continue;
        }
        for (int i = 0; i < sections[s].count; i++) {
            if (strcmp(id->valuestring, sections[s].items[i].id)) {
                continue;
            }
            bool seen = false;
            for (int k = 0; k < menu->count[s]; k++) {
                seen |= menu->order[s][k] == i;
            }
            if (!seen && menu->count[s] < MAINUI_GAME_MENU_ITEMS) {
                menu->order[s][menu->count[s]++] = i;
            }
            break;
        }
    }
}

void mainui_game_menu_load(MainUIGameMenu *menu, const char *directory)
{
    char path[4096];
    int n = directory ? snprintf(path, sizeof path, "%s/game-menu.json", directory) : -1;
    char *text = n > 0 && n < (int)sizeof path ? mainui_read_text(path, 128 * 1024) : NULL;
    if (!mainui_game_menu_parse(menu, text)) {
        fprintf(stderr, "[menu] %s is not a JSON object; the game menus are the default ones\n",
                path);
    }
    free(text);
}

void mainui_game_menu_apply(MainUIContext *context, const MainUIGameMenu *menu,
                            MainUIGameMenuSection section)
{
    if (!menu || (int)section < 0 || section >= MAINUI_GAME_MENU_ALL ||
        !menu->configured[section]) {
        return;
    }
    MainUIContextAction actions[CONTEXT_COUNT];
    int count = 0;
    for (int k = 0; k < menu->count[section]; k++) {
        const GameMenuItem *item = &sections[section].items[menu->order[section][k]];
        for (int v = 0; v < context->visible_count && count < CONTEXT_COUNT; v++) {
            MainUIContextAction action = context->entries[context->visible[v]].action;
            if (action == item->action || action == item->alternate) {
                actions[count++] = action;
                break;
            }
        }
    }
    mainui_context_rows(context, actions, count);
}
