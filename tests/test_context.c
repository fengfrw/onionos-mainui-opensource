/* SPDX-License-Identifier: GPL-3.0-only */
#include "menus/context.h"
#include "menus/favorite_context.h"
#include "menus/game_menu.h"
#include <limits.h>
#include <stdlib.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void favorite_popups(void)
{
    MainUILibrary *library = calloc(1, sizeof *library);
    assert(library);
    library->current = -1;
    library->visible_count = 1;
    library->visible[0] = 0;
    MainUIContext context;
    mainui_favorite_context(&context, library, 0, false);
    const MainUIContextAction game[] = {CONTEXT_FAVORITE_MOVE, CONTEXT_FAVORITE_CREATE,
                                        CONTEXT_FAVORITE_SORT, CONTEXT_REMOVE_FAVORITE};
    const char *labels[] = {"Move selected", "Create folder", "Sort A-Z", "Remove Favorite"};
    assert(context.visible_count == 4);
    for (int i = 0; i < 4; i++) {
        assert(context.entries[i].action == game[i]);
        assert(!strcmp(mainui_context_label(&context.entries[i]), labels[i]));
    }
    library->visible[0] = -1;
    mainui_favorite_context(&context, library, 0, false);
    assert(context.visible_count == 4);
    assert(context.entries[2].action == CONTEXT_FAVORITE_RENAME);
    assert(context.entries[3].action == CONTEXT_FAVORITE_DELETE);
    library->visible[0] = INT_MIN;
    mainui_favorite_context(&context, library, 0, false);
    assert(context.visible_count == 1 && context.entries[0].action == CONTEXT_FAVORITE_CREATE);
    library->visible_count = 0;
    mainui_favorite_context(&context, library, -1, false);
    assert(context.visible_count == 1);
    mainui_favorite_context(&context, library, -1, true);
    assert(context.visible_count == 2 && context.entries[0].action == CONTEXT_FAVORITE_PASTE);
    library->current = 2;
    library->folders[2].parent = 1;
    library->folders[1].parent = 0;
    library->folders[0].parent = -1;
    mainui_favorite_context(&context, library, -1, false);
    assert(context.visible_count == 0);
    library->visible_count = 1;
    library->visible[0] = 0;
    mainui_favorite_context(&context, library, 0, false);
    assert(context.visible_count == 3);
    assert(context.entries[1].action == CONTEXT_FAVORITE_SORT);
    assert(context.entries[2].action == CONTEXT_REMOVE_FAVORITE);
    free(library);
}

static void actions_are(const MainUIContext *context, const MainUIContextAction *want, int count)
{
    assert(context->count == count && context->visible_count == count);
    for (int i = 0; i < count; i++) {
        assert(context->entries[context->visible[i]].action == want[i]);
        assert(context->entries[i].enabled);
    }
}

static void game_menus(void)
{
    const MainUIContextAction rom[] = {CONTEXT_START, CONTEXT_ADD_FAVORITE, CONTEXT_DELETE_ROM,
                                       CONTEXT_SEARCH, CONTEXT_REFRESH_SYSTEM};
    MainUIGameMenu menu;
    MainUIContext context, built;
    /* No file, a malformed one, or one without the section: menus as built. */
    const char *unchanged[] = {NULL,
                               "{",
                               "[\"roms\"]",
                               "\"roms\"",
                               "{}",
                               "{\"roms\":\"launch\"}",
                               "{\"roms\":{\"launch\":true}}",
                               "{\"Roms\":[]}"};
    for (size_t i = 0; i < sizeof unchanged / sizeof *unchanged; i++) {
        bool ok = mainui_game_menu_parse(&menu, unchanged[i]);
        assert(ok == (i == 0 || i >= 4));
        mainui_context_rows(&built, rom, 5);
        context = built;
        mainui_game_menu_apply(&context, &menu, MAINUI_GAME_MENU_ROMS);
        assert(!memcmp(&context, &built, sizeof context));
    }
    /* Reordered and hidden; unknown, repeated and wrong-type IDs skipped. */
    assert(mainui_game_menu_parse(
        &menu, "// comment\n{\"roms\": [\"refresh\", \"Launch\", \"bogus\", 3, \"launch\", "
               "\"favorite\", \"refresh\", \"launch\"], \"recents\": []}"));
    mainui_context_rows(&context, rom, 5);
    mainui_game_menu_apply(&context, &menu, MAINUI_GAME_MENU_ROMS);
    const MainUIContextAction reordered[] = {CONTEXT_REFRESH_SYSTEM, CONTEXT_START,
                                             CONTEXT_ADD_FAVORITE};
    actions_are(&context, reordered, 3);
    /* The selection still decides: a folder offers only Search and Refresh,
     * and a console that cannot be refreshed no Refresh. */
    const MainUIContextAction folder[] = {CONTEXT_SEARCH, CONTEXT_REFRESH_SYSTEM};
    mainui_context_rows(&context, folder, 2);
    mainui_game_menu_apply(&context, &menu, MAINUI_GAME_MENU_ROMS);
    actions_are(&context, folder + 1, 1);
    mainui_context_rows(&context, rom, 4);
    mainui_game_menu_apply(&context, &menu, MAINUI_GAME_MENU_ROMS);
    actions_are(&context, reordered + 1, 2);
    /* "favorite" stands for Remove too. */
    const MainUIContextAction removing[] = {CONTEXT_START, CONTEXT_REMOVE_FAVORITE};
    mainui_context_rows(&context, removing, 2);
    mainui_game_menu_apply(&context, &menu, MAINUI_GAME_MENU_ROMS);
    const MainUIContextAction removing_order[] = {CONTEXT_START, CONTEXT_REMOVE_FAVORITE};
    actions_are(&context, removing_order, 2);
    /* An empty array: no menu. Other sections keep theirs. */
    const MainUIContextAction recent[] = {CONTEXT_START, CONTEXT_REMOVE_RECENT,
                                          CONTEXT_CLEAR_RECENT};
    mainui_context_rows(&context, recent, 3);
    mainui_game_menu_apply(&context, &menu, MAINUI_GAME_MENU_RECENTS);
    assert(context.count == 0 && context.visible_count == 0);
    mainui_context_rows(&context, recent, 3);
    mainui_game_menu_apply(&context, &menu, MAINUI_GAME_MENU_SEARCH_RESULTS);
    actions_are(&context, recent, 3);
    /* An ID from another section does nothing here. */
    assert(mainui_game_menu_parse(&menu, "{\"search_results\":[\"delete\",\"favorite\"]}"));
    mainui_context_rows(&context, rom, 2);
    mainui_game_menu_apply(&context, &menu, MAINUI_GAME_MENU_SEARCH_RESULTS);
    actions_are(&context, rom + 1, 1);
    /* Favorites: the default order gives today's game and folder menus, and
     * "move" is Move here during a move. */
    MainUILibrary *library = calloc(1, sizeof *library);
    assert(library);
    library->current = -1;
    library->visible_count = 1;
    library->visible[0] = 0;
    assert(mainui_game_menu_parse(&menu, "{\"favorites\":[\"move\",\"create_folder\",\"sort\","
                                         "\"remove_favorite\",\"rename_folder\","
                                         "\"delete_folder\"]}"));
    for (int row = -1; row <= 0; row++) {
        for (int moving = 0; moving <= 1; moving++) {
            library->visible[0] = row;
            mainui_favorite_context(&built, library, 0, moving);
            context = built;
            mainui_game_menu_apply(&context, &menu, MAINUI_GAME_MENU_FAVORITES);
            assert(!memcmp(&context, &built, sizeof context));
        }
    }
    assert(mainui_game_menu_parse(&menu, "{\"favorites\":[\"remove_favorite\",\"delete_folder\","
                                         "\"move\"]}"));
    library->visible[0] = 0;
    mainui_favorite_context(&context, library, 0, true);
    mainui_game_menu_apply(&context, &menu, MAINUI_GAME_MENU_FAVORITES);
    const MainUIContextAction pasting[] = {CONTEXT_REMOVE_FAVORITE, CONTEXT_FAVORITE_PASTE};
    actions_are(&context, pasting, 2);
    library->visible[0] = -1;
    mainui_favorite_context(&context, library, 0, false);
    mainui_game_menu_apply(&context, &menu, MAINUI_GAME_MENU_FAVORITES);
    const MainUIContextAction folder_menu[] = {CONTEXT_FAVORITE_DELETE, CONTEXT_FAVORITE_MOVE};
    actions_are(&context, folder_menu, 2);
    free(library);
    /* Every ID once, in reverse: all of them still fit. */
    assert(mainui_game_menu_parse(&menu, "{\"favorites\":[\"delete_folder\",\"rename_folder\","
                                         "\"remove_favorite\",\"sort\",\"create_folder\","
                                         "\"move\",\"move\"]}"));
    assert(menu.count[MAINUI_GAME_MENU_FAVORITES] == 6);
    mainui_game_menu_defaults(&menu);
    mainui_context_rows(&context, rom, 5);
    mainui_game_menu_apply(&context, &menu, MAINUI_GAME_MENU_ROMS);
    actions_are(&context, rom, 5);
    mainui_game_menu_apply(&context, NULL, MAINUI_GAME_MENU_ROMS);
    actions_are(&context, rom, 5);
}

int mainui_suite_context(void)
{
    favorite_popups();
    game_menus();
    MainUIContext c;
    mainui_context_parse(&c, NULL, false, false, false);
    assert(c.visible_count == 1 && c.entries[0].action == CONTEXT_REFRESH && c.hotkey);
    mainui_context_parse(&c, "{}", true, true, true);
    assert(c.visible_count == 3 && c.entries[2].action == CONTEXT_TWEAKS && c.hotkey);
    /* Root hotkey: omitted or literal true enables; any other present value disables. */
    mainui_context_parse(&c, "{\"hotkey\":true}", false, false, false);
    assert(c.hotkey);
    static const char *const disabled[] = {"{\"hotkey\":false}",    "{\"hotkey\":null}",
                                           "{\"hotkey\":0}",        "{\"hotkey\":1}",
                                           "{\"hotkey\":\"true\"}", "{\"hotkey\":\"false\"}"};
    for (size_t i = 0; i < sizeof disabled / sizeof *disabled; ++i) {
        mainui_context_parse(&c, disabled[i], false, false, false);
        assert(!c.hotkey);
    }
    mainui_context_parse(&c, "{\"context\":[]}", true, true, true);
    assert(c.count == 0);
    mainui_context_parse(&c,
                         "{\"context\":{\"settings\":false,\"favs\":true,\"settings\":true,"
                         "\"recent\":1},\"hotkey\":false}",
                         false, false, false);
    assert(!c.hotkey && c.count == 3 && c.visible_count == 2);
    assert(c.entries[0].action == CONTEXT_SETTINGS && c.entries[0].enabled);
    assert(c.entries[1].action == CONTEXT_FAVORITES && !c.entries[2].enabled);
    mainui_context_reveal(&c);
    assert(c.visible_count == 3);
    mainui_context_parse(
        &c,
        "{\"context\":[\"custom1\",\"custom2\",\"games\",\"games\"],\"custom\":{\"custom1\":{"
        "\"label\":\"Literal %s\",\"launch\":\"/mnt/SDCARD/tool.sh\",\"type\":3.5}}}",
        false, false, false);
    assert(c.count == 2 && c.entries[0].type == 3);
    assert(!strcmp(mainui_context_label(&c.entries[0]), "Literal %s"));
    assert(mainui_context_section(c.entries[1].action) == MAINUI_MENU_GAMES);
    /* Search and Tweaks named in main-menu.json need their app installed:
     * without it they are left out, shown or hidden, and reveal cannot
     * expose them; reinstalled, the same configuration shows them again. */
    const char *apps = "{\"context\":{\"tweaks\":true,\"search\":false,\"games\":true}}";
    mainui_context_parse(&c, apps, false, false, false);
    assert(c.count == 1 && c.entries[0].action == CONTEXT_GAMES);
    mainui_context_reveal(&c);
    assert(c.count == 1 && c.visible_count == 1);
    mainui_context_parse(&c, "{\"context\":[\"search\",\"tweaks\"]}", false, false, false);
    assert(c.count == 0 && c.visible_count == 0);
    mainui_context_parse(&c, apps, true, true, true);
    assert(c.count == 3 && c.visible_count == 2 && c.entries[0].action == CONTEXT_TWEAKS);
    assert(c.entries[1].action == CONTEXT_SEARCH && !c.entries[1].enabled);
    mainui_context_reveal(&c);
    assert(c.visible_count == 3);
    /* Themes needs App/ThemeSwitcher in the same way. */
    const char *themes = "{\"context\":{\"themes\":false,\"games\":true}}";
    mainui_context_parse(&c, themes, true, true, false);
    assert(c.count == 1 && c.entries[0].action == CONTEXT_GAMES);
    mainui_context_reveal(&c);
    assert(c.count == 1 && c.visible_count == 1);
    mainui_context_parse(&c, themes, false, false, true);
    assert(c.count == 2 && c.visible_count == 1 && c.entries[0].action == CONTEXT_THEMES);
    mainui_context_reveal(&c);
    assert(c.visible_count == 2);
    puts("Context defaults, order, aliases, reveal, missing apps and bounded custom action "
         "checks passed");
    return 0;
}
