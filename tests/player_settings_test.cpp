// Host test for platform/vita/runtime/player_settings.hpp: the config.json scan, the language
// mapping (console language, player choice, what each disc carries) and the confirm button,
// including the pad bits the Vita buttons press and the input script's CROSS/CIRCLE.
#include "runtime/input_script.hpp"
#include "runtime/player_settings.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace twv::vita;

int main() {
    // --- config.json: top-level keys only, strings skipped correctly ---
    const char* json = "{\n    \"backend.isoPath\": \"ux0:data/dusklight/Zelda.ciso\",\n"
                       "    \"note\": \"\\\"game.language\\\": 4 }\",\n"
                       "    \"nested\": {\"game.language\": 2, \"x\": [1, {\"vita.confirmButton\": \"cross\"}]},\n"
                       "    \"game.language\" : 3,\n    \"vita.confirmButton\": \"circle\"\n}\n";
    char text[64];
    long v = -1;
    assert(json_string(json_top_level_value(json, "backend.isoPath"), text, sizeof text));
    assert(std::string(text) == "ux0:data/dusklight/Zelda.ciso");
    assert(json_int(json_top_level_value(json, "game.language"), v) && v == 3);
    assert(json_string(json_top_level_value(json, "vita.confirmButton"), text, sizeof text) &&
           std::string(text) == "circle");
    assert(!json_top_level_value(json, "x") && !json_top_level_value(json, "missing"));
    assert(!json_top_level_value("", "game.language") && !json_top_level_value(nullptr, "a"));
    assert(!json_int("1.5", v) && !json_int("\"3\"", v) && !json_int("x", v) && json_int("-2,", v) && v == -2);
    assert(!json_string("\"a\\\"b\"", text, sizeof text) && !json_string("\"abc\"", text, 3) && !json_string("3", text, 8));
    assert(!json_string("\"unterminated", text, sizeof text));

    // --- language ---
    assert(parse_language("es") == LangSpanish && parse_language("italian") == LangItalian);
    assert(parse_language("system") == LangSystem && parse_language("pt") == LangNone && parse_language(nullptr) == LangNone);
    assert(config_language(6) == LangJapanese && config_language(5) == LangNone && config_language(-1) == LangNone);
    // Console languages: the five PAL ones, Japanese, Portuguese (both) to Spanish, the rest English.
    const int expect[20] = {LangJapanese, LangEnglish, LangFrench, LangSpanish, LangGerman, LangItalian, LangEnglish,
                            LangSpanish,  LangEnglish, LangEnglish, LangEnglish, LangEnglish, LangEnglish, LangEnglish,
                            LangEnglish,  LangEnglish, LangEnglish, LangSpanish, LangEnglish, LangEnglish};
    for (int sys = 0; sys < 20; ++sys) assert(game_language_for_system(sys) == expect[sys]);
    assert(game_language_for_system(-1) == LangEnglish && game_language_for_system(99) == LangEnglish);
    assert(std::string(system_language_name(SysPortugueseBr)) == "portuguese_br" &&
           std::string(system_language_name(42)) == "unknown");
    // What each disc carries.
    for (int lang : {LangEnglish, LangGerman, LangFrench, LangSpanish, LangItalian}) assert(disc_has_language('P', lang));
    assert(!disc_has_language('P', LangJapanese) && disc_has_language('E', LangEnglish) && !disc_has_language('E', LangSpanish));
    assert(disc_has_language('J', LangJapanese) && !disc_has_language('J', LangEnglish));
    // Choice order: args, config.json, console, English; then fitted to the disc.
    LanguageChoice c = choose_language('P', LangNone, LangNone, SysPortugueseBr);
    assert(c.language == LangSpanish && !c.fitted && std::string(c.source) == "system");
    c = choose_language('P', LangNone, LangGerman, SysFrench);
    assert(c.language == LangGerman && std::string(c.source) == "config");
    c = choose_language('P', LangSystem, LangGerman, SysFrench);
    assert(c.language == LangFrench && std::string(c.source) == "system");
    c = choose_language('P', LangItalian, LangGerman, SysFrench);
    assert(c.language == LangItalian && std::string(c.source) == "args");
    c = choose_language('P', LangNone, LangNone, -1);
    assert(c.language == LangEnglish && std::string(c.source) == "default");
    c = choose_language('P', LangNone, LangNone, SysJapanese); // no Japanese on the PAL disc
    assert(c.language == LangEnglish && c.fitted && c.requested == LangJapanese);
    c = choose_language('E', LangNone, LangNone, SysSpanish);  // USA disc: English only
    assert(c.language == LangEnglish && c.fitted && c.requested == LangSpanish);
    c = choose_language('E', LangGerman, LangNone, -1);
    assert(c.language == LangEnglish && c.fitted && std::string(c.source) == "args");
    c = choose_language('J', LangNone, LangNone, SysEnglishUs); // Japanese disc: Japanese only
    assert(c.language == LangJapanese && c.fitted);
    c = choose_language('J', LangNone, LangNone, SysJapanese);
    assert(c.language == LangJapanese && !c.fitted);
    assert(std::string(disc_languages_text('P')) == "english german french spanish italian");

    // --- confirm button ---
    Confirm s = Confirm::Cross;
    assert(parse_confirm("system", s) && s == Confirm::System && parse_confirm("circle", s) && s == Confirm::Circle);
    assert(!parse_confirm("Circle", s) && !parse_confirm("o", s) && !parse_confirm(nullptr, s) && s == Confirm::Circle);
    assert(!confirm_on_circle(Confirm::System, SysEnterCross) && confirm_on_circle(Confirm::System, SysEnterCircle));
    assert(!confirm_on_circle(Confirm::System, -1)); // unread: cross, as before
    assert(confirm_on_circle(Confirm::Circle, SysEnterCross) && !confirm_on_circle(Confirm::Cross, SysEnterCircle));
    assert(face_buttons(false).cross == PadA && face_buttons(false).circle == PadB);
    assert(face_buttons(true).cross == PadB && face_buttons(true).circle == PadA);

    // Input script: CROSS and CIRCLE press what the mapping says; GameCube names are unchanged.
    InputScript script;
    assert(script.parse("hold CROSS 1\nhold CIRCLE+START 1\nhold A 1\n"));
    const InputStep* a = script.next();
    const InputStep* b = script.next();
    const InputStep* g = script.next();
    assert(a && b && g);
    assert(resolve_vita_buttons(a->buttons, false) == ScriptButtonA && resolve_vita_buttons(a->buttons, true) == ScriptButtonB);
    assert(resolve_vita_buttons(b->buttons, false) == (ScriptButtonB | ScriptButtonStart));
    assert(resolve_vita_buttons(b->buttons, true) == (ScriptButtonA | ScriptButtonStart));
    assert(resolve_vita_buttons(g->buttons, true) == ScriptButtonA);
    assert(!script.parse("hold CROSSX 1\n") && !script.parse("hold cross 1\n"));

    std::puts("player_settings_test: config.json keys, console-to-game languages, per-disc languages and fallbacks, "
              "choice order, confirm button and pad bits, script CROSS/CIRCLE passed");
    return 0;
}
