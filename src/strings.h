// Every text the player sees, with its key in the translation files and the English
// text. {0}, {1}, ... are filled in by the mod (numbers, key names). The translation
// files (lang/<code>.txt, installed as BuildingTools-Languages\<code>.txt) use the same
// keys; lang/en.txt must match this list (the unit tests check it).
//
// X(id, key, English)
#define BT_STRINGS(X) \
    /* numbers */ \
    X(S_DECIMAL, "num.decimal", ".") \
    /* messages */ \
    X(S_READY, "msg.ready", "Building Tools {0} ready") \
    X(S_READY_CREATIVE, "msg.ready_creative", "Building Tools {0} ready (creative building)") \
    X(S_UNSUPPORTED, "msg.unsupported", "Building Tools: game version not supported") \
    X(S_MOD_OFF, "msg.mod_off", "Building Tools OFF (vanilla game)  |  {0} turns it on") \
    X(S_MOD_ON, "msg.mod_on", "Building Tools ON  |  {0} turns it off") \
    X(S_MODE_AREA, "msg.mode_area", "Area mode") \
    X(S_MODE_PREFAB, "msg.mode_prefab", "Prefab mode (vanilla)") \
    X(S_REPLACE_ON, "msg.replace_on", "Replace ON") \
    X(S_REPLACE_OFF, "msg.replace_off", "Replace OFF") \
    X(S_STYLE_FULL, "msg.style_full", "Replace style: full shape") \
    X(S_STYLE_OVERLAP, "msg.style_overlap", "Replace style: overlap only") \
    X(S_STYLE_CONNECTED, "msg.style_connected", "Replace style: connected (fill bucket, Prefab mode; right click deletes)") \
    X(S_FILL_SOLID, "msg.fill_solid", "Area fill: solid") \
    X(S_FILL_HOLLOW, "msg.fill_hollow", "Area fill: hollow room (walls, floor, ceiling)") \
    X(S_FILL_WALLS, "msg.fill_walls", "Area fill: walls only") \
    X(S_FREE_ON, "msg.free_on", "Free placement ON (no surface snapping)") \
    X(S_FREE_OFF, "msg.free_off", "Free placement OFF") \
    X(S_FREE_DISTANCE, "msg.free_distance", "Free placement distance {0} m") \
    X(S_CORNERS_CLEARED, "msg.corners_cleared", "Corners cleared") \
    X(S_BUILD_CORNER, "msg.build_corner", "Build corner {0} of {1} set") \
    X(S_REPLACE_CORNER, "msg.replace_corner", "Replace corner {0} of {1} set") \
    X(S_REMOVE_CORNER, "msg.remove_corner", "Remove corner {0} of {1} set") \
    X(S_TOO_BIG, "msg.too_big", "Too big: {0} shapes (max {1})") \
    X(S_PLACED, "msg.placed", "Placed {0} shapes") \
    X(S_PLACED_PARTIAL, "msg.placed_partial", "Placed {0} of {1} shapes (not enough material)") \
    X(S_REMOVED, "msg.removed", "Removed {0} shapes") \
    X(S_UNDO_COVERS, "msg.undo_covers", "undo covers {0}") \
    X(S_NOT_ENOUGH, "msg.not_enough", "Not enough material for {0} shapes (needs {1} items)") \
    X(S_UNDO_NEEDS, "msg.undo_needs", "Undo needs the replaced material back in your inventory") \
    X(S_NOTHING_TO_REPLACE, "msg.nothing_to_replace", "Nothing to replace there") \
    X(S_REPLACE_READ_FAIL, "msg.replace_read_fail", "Replace: could not read the aimed block, try again") \
    X(S_CREPLACE_OWN, "msg.creplace_own", "Connected replace: that is already your material") \
    X(S_CREPLACE_NOTHING, "msg.creplace_nothing", "Connected replace: nothing to replace there") \
    X(S_CREPLACE_CAPPED, "msg.creplace_capped", "Connected replace: only the first {0} shapes (FillLimit)") \
    X(S_CDELETE_UNAVAILABLE, "msg.cdelete_unavailable", "Connected delete: not available in this game version") \
    X(S_CDELETE_NOTHING, "msg.cdelete_nothing", "Connected delete: nothing to remove there") \
    X(S_CDELETE_CAPPED, "msg.cdelete_capped", "Connected delete: only the first {0} shapes (FillLimit)") \
    X(S_LANGUAGE_SET, "msg.language_set", "Building Tools language: English") \
    /* info bar (bottom centre) */ \
    X(S_BAR_AREA, "bar.area", "Area {0}/{1}") \
    X(S_BAR_AREA_BUILD, "bar.area_build", "Area build {0}/{1}") \
    X(S_BAR_AREA_REPLACE, "bar.area_replace", "Area replace {0}/{1}") \
    X(S_BAR_AREA_REMOVE, "bar.area_remove", "Area remove {0}/{1}") \
    X(S_BAR_HOLLOW, "bar.hollow", "hollow") \
    X(S_BAR_WALLS, "bar.walls", "walls") \
    X(S_BAR_PREFAB, "bar.prefab", "Prefab") \
    X(S_BAR_PREFAB_REPLACE, "bar.prefab_replace", "Prefab replace") \
    X(S_BAR_FULL, "bar.full", "full shape") \
    X(S_BAR_OVERLAP, "bar.overlap", "overlap") \
    X(S_BAR_CONNECTED, "bar.connected", "connected") \
    X(S_BAR_FREE, "bar.free", "free {0} m") \
    X(S_BAR_CREATIVE, "bar.creative", "creative") \
    X(S_BAR_SIZE, "bar.size", "{0}x{1} h{2} = {3}") \
    X(S_BAR_TOO_BIG, "bar.too_big", "TOO BIG (max {0})") \
    X(S_BAR_KEY_AREA, "bar.key_area", "area") \
    X(S_BAR_KEY_REPLACE, "bar.key_replace", "replace") \
    X(S_BAR_KEY_CLEAR, "bar.key_clear", "clear") \
    X(S_BAR_KEY_STYLE, "bar.key_style", "style") \
    X(S_BAR_KEY_FREE, "bar.key_free", "free") \
    X(S_BAR_KEY_FILL, "bar.key_fill", "fill") \
    X(S_BAR_KEY_WHEEL, "bar.key_wheel", "+wheel dist") \
    X(S_BAR_KEY_OFF, "bar.key_off", "off") \
    /* rows in the game's building box (bottom right; the box is narrow) */ \
    X(S_HUD_AREA, "hud.area", "Area mode") \
    X(S_HUD_AREA_ON, "hud.area_on", "Area mode: on") \
    X(S_HUD_AREA_OFF, "hud.area_off", "Area mode: off") \
    X(S_HUD_FILL_SOLID, "hud.fill_solid", "Fill: solid") \
    X(S_HUD_FILL_HOLLOW, "hud.fill_hollow", "Fill: hollow") \
    X(S_HUD_FILL_WALLS, "hud.fill_walls", "Fill: walls") \
    X(S_HUD_CLEAR, "hud.clear", "Clear corners") \
    X(S_HUD_REPLACE, "hud.replace", "Replace") \
    X(S_HUD_REPLACE_ON, "hud.replace_on", "Replace: on") \
    X(S_HUD_REPLACE_OFF, "hud.replace_off", "Replace: off") \
    X(S_HUD_STYLE_FULL, "hud.style_full", "Style: full") \
    X(S_HUD_STYLE_OVERLAP, "hud.style_overlap", "Style: overlap") \
    X(S_HUD_STYLE_CONNECTED, "hud.style_connected", "Style: connected") \
    X(S_HUD_FREE, "hud.free", "Free placement") \
    X(S_HUD_FREE_ON, "hud.free_on", "Free placement: on") \
    X(S_HUD_FREE_OFF, "hud.free_off", "Free placement: off") \
    X(S_HUD_WHEEL, "hud.wheel", "+ wheel: {0} m") \
    X(S_HUD_MENU, "hud.menu", "Tools menu") \
    /* floating text under the crosshair */ \
    X(S_FLOAT_TOO_BIG, "float.too_big", "too big") \
    X(S_FLOAT_SIZE, "float.size", "{0} x {1}, h {2} = {3}") \
    /* key names */ \
    X(S_KEY_CTRL, "key.ctrl", "Ctrl") \
    X(S_KEY_SHIFT, "key.shift", "Shift") \
    X(S_KEY_ALT, "key.alt", "Alt") \
    X(S_KEY_NUMBER, "key.number", "key {0}") \
    /* menu */ \
    X(S_MENU_TITLE, "menu.title", "Building Tools") \
    X(S_MENU_KEYS, "menu.keys", "Keys") \
    X(S_MENU_OPTIONS, "menu.options", "Options") \
    X(S_MENU_FOOTER, "menu.footer", "Version {0}. Esc or the menu key closes the menu.") \
    X(S_MENU_CLOSE, "menu.close", "Close") \
    X(S_MENU_CANCEL, "menu.cancel", "Cancel") \
    X(S_MENU_PRESS_KEY, "menu.press_key", "Press a key...") \
    X(S_MENU_NO_KEY, "menu.no_key", "None") \
    X(S_MENU_KEYS_HELP, "menu.keys_help", "Click a key to change it. Saved to BuildingTools.ini.") \
    X(S_MENU_KEYS_WAITING, "menu.keys_waiting", "Press the new key, also with Shift, Ctrl or Alt. Esc cancels, Delete clears it.") \
    X(S_MENU_TRANSLATION_NOTE, "menu.translation_note", "") \
    X(S_ACT_MENU, "menu.key_menu", "Building Tools menu") \
    X(S_ACT_AREA, "menu.key_area", "Area mode on / off") \
    X(S_ACT_REPLACE, "menu.key_replace", "Replace on / off") \
    X(S_ACT_STYLE, "menu.key_style", "Replace style") \
    X(S_ACT_FILL, "menu.key_fill", "Area fill") \
    X(S_ACT_CLEAR, "menu.key_clear", "Clear area corners") \
    X(S_ACT_FREE, "menu.key_free", "Free placement on / off") \
    X(S_ACT_FARTHER, "menu.key_farther", "Free placement farther") \
    X(S_ACT_CLOSER, "menu.key_closer", "Free placement closer") \
    X(S_ACT_WHEEL, "menu.key_wheel", "Distance (hold + mouse wheel)") \
    X(S_ACT_OFF, "menu.key_off", "Whole mod off / on") \
    X(S_OPT_OFF, "option.off", "Off") \
    X(S_OPT_ON, "option.on", "On") \
    X(S_OPT_MODE, "option.mode", "Mode") \
    X(S_OPT_MODE_PREFAB, "option.mode_prefab", "Prefab (vanilla)") \
    X(S_OPT_MODE_AREA, "option.mode_area", "Area") \
    X(S_OPT_REPLACE, "option.replace", "Replace") \
    X(S_OPT_STYLE, "option.style", "Replace style") \
    X(S_OPT_STYLE_FULL, "option.style_full", "Full shape") \
    X(S_OPT_STYLE_OVERLAP, "option.style_overlap", "Overlap only") \
    X(S_OPT_STYLE_CONNECTED, "option.style_connected", "Connected (fill bucket)") \
    X(S_OPT_FILL, "option.fill", "Area fill") \
    X(S_OPT_FILL_SOLID, "option.fill_solid", "Solid") \
    X(S_OPT_FILL_HOLLOW, "option.fill_hollow", "Hollow room") \
    X(S_OPT_FILL_WALLS, "option.fill_walls", "Walls only") \
    X(S_OPT_FREE, "option.free", "Free placement") \
    X(S_OPT_LANGUAGE, "option.language", "Language") \
    X(S_OPT_LANGUAGE_GAME, "option.language_game", "Same as the game")

enum Str {
#define BT_STR_ID(id, key, en) id,
    BT_STRINGS(BT_STR_ID)
#undef BT_STR_ID
    S_COUNT
};

// The languages of the game's language menu: file code (BuildingTools-Languages\<code>.txt),
// name in the language itself, the game's LanguageId names (user_settings.json) and the
// Steam language names that select it.
struct LangInfo { const char* code; const char* name; const char* game[3]; const char* steam[3]; };
static const LangInfo kLanguages[] = {
    { "en",    "English",            { "En_Us", "En_Gb" },           { "english" } },
    { "de",    "Deutsch",            { "De_De" },                    { "german" } },
    { "fr",    "Français",           { "Fr_Fr", "Fr_Ca" },           { "french" } },
    { "it",    "Italiano",           { "It_It" },                    { "italian" } },
    { "ja",    "日本語 (ja)",        { "Ja_Jp" },                    { "japanese" } },
    { "ko",    "한국어 (ko)",        { "Ko_Kr" },                    { "koreana" } },
    { "pl",    "Polski",             { "Pl_Pl" },                    { "polish" } },
    { "pt-BR", "Português (Brasil)", { "Pt_Br", "Pt_Pt" },           { "brazilian", "portuguese" } },
    { "ru",    "Русский",            { "Ru_Ru" },                    { "russian" } },
    { "es",    "Español",            { "Es_Es", "Es_Mx", "Es_419" }, { "spanish", "latam" } },
    { "th",    "ไทย (th)",           { "Th_Th" },                    { "thai" } },
    { "tr",    "Türkçe",             { "Tr_Tr" },                    { "turkish" } },
    { "uk",    "Українська",         { "Uk_Ua" },                    { "ukrainian" } },
    { "zh-CN", "简体中文 (zh-CN)",   { "Zh_Cn" },                    { "schinese" } },
    { "zh-TW", "繁體中文 (zh-TW)",   { "Zh_Tw" },                    { "tchinese" } },
};
static const int kLanguageCount = sizeof kLanguages / sizeof kLanguages[0];
