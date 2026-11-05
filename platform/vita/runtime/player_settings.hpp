#pragma once
// Player settings the Vita takes from the console, resolved before the game starts (main_vita.cpp):
// the game language and the confirm button. SDK-free and host-tested (tests/player_settings_test.cpp).
//
// Language. The original reads the language from the console (the GameCube IPL setting,
// OSGetLanguage); Dusklight keeps it in game.language (src/dusk/settings.h GameLanguage, the
// OS_LANGUAGE_* values) and opens Msguk/Msgde/Msgfr/Msgsp/Msgit on a PAL disc, Msgus on a USA disc
// and Msgjp on a Japanese one (src/dusk/language.cpp). What each GameCube disc carries:
//   GZ2P01 (Europe)  English, German, French, Spanish, Italian
//   GZ2E01 (USA)     English only
//   GZ2J01 (Japan)   Japanese only
// Portuguese is on no disc (only fan patches exist).
// Choice, first that applies: --vita-language= in args.txt, then "game.language" in config.json
// (the player's own Dusklight setting), then the Vita's system language; then fitted to the disc:
// a language the disc lacks becomes the disc's first language (English, or Japanese on GZ2J01).
// Vita system languages without PAL text map to English, except Portuguese (Portugal and Brazil),
// which maps to Spanish: written Spanish is far easier for a Portuguese reader than English (shared
// vocabulary and grammar), and Portuguese-speaking players of other ports already play in Spanish
// through a fan modification. English stays one setting away.
//
// Confirm button. The Vita has a system "Enter button" (cross, or circle as on Japanese consoles).
// The GameCube confirms with A and cancels with B; A on cross and B on circle is the default
// mapping (InputVita.cpp). Setting "system" (default) follows the console; "cross" or "circle"
// choose. With circle, A is on circle and B on cross, and the PlayStation prompts follow
// (ui/ps_glyphs.cpp: an A prompt shows the circle). --vita-confirm= in args.txt, then
// "vita.confirmButton" in config.json (Dusklight keeps keys it does not know), then the console.
// Only these two face buttons change; touch, gyro and every other button are untouched.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace twv::vita {

// --- config.json (Dusklight's nlohmann dump): a top-level key's value ---
// The value text after `"key":` at the top level of a JSON object, or nullptr. Strings with
// escapes are skipped correctly; nested objects and arrays are not searched.
inline const char* json_top_level_value(const char* text, const char* key) {
    if (!text || !key) return nullptr;
    const std::size_t keyLen = std::strlen(key);
    int depth = 0;
    for (const char* p = text; *p; ++p) {
        const char c = *p;
        if (c == '{' || c == '[') { ++depth; continue; }
        if (c == '}' || c == ']') { --depth; continue; }
        if (c != '"') continue;
        const char* start = p + 1;
        const char* q = start;
        while (*q && *q != '"') q += (*q == '\\' && q[1]) ? 2 : 1;
        if (!*q) return nullptr;
        const char* after = q + 1;
        while (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n') ++after;
        if (*after == ':') {
            if (depth == 1 && std::size_t(q - start) == keyLen && std::strncmp(start, key, keyLen) == 0) {
                ++after;
                while (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n') ++after;
                return after;
            }
        }
        p = q; // continue after the closing quote
    }
    return nullptr;
}
// An integer value (optionally negative); false for anything else.
inline bool json_int(const char* value, long& out) {
    if (!value) return false;
    const char* p = value;
    const bool negative = *p == '-';
    if (negative) ++p;
    if (*p < '0' || *p > '9') return false;
    long v = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
        if (v > 100000000L) return false;
        v = v * 10 + (*p - '0');
    }
    if (*p == '.' || *p == 'e' || *p == 'E') return false;
    out = negative ? -v : v;
    return true;
}
// A string value without escapes into `out` (size bytes); false otherwise.
inline bool json_string(const char* value, char* out, std::size_t size) {
    if (!value || *value != '"' || size == 0) return false;
    std::size_t n = 0;
    for (const char* p = value + 1; *p; ++p) {
        if (*p == '"') {
            out[n] = 0;
            return true;
        }
        if (*p == '\\' || n + 1 >= size) return false;
        out[n++] = *p;
    }
    return false;
}

// --- language ---
// Dusklight's GameLanguage (OS_LANGUAGE_* values; Japanese is 6).
enum : int { LangEnglish = 0, LangGerman = 1, LangFrench = 2, LangSpanish = 3, LangItalian = 4, LangJapanese = 6 };
constexpr int LangNone = -1;   // no choice from this source
constexpr int LangSystem = -2; // --vita-language=system: the console decides even over config.json

inline const char* language_name(int lang) {
    switch (lang) {
    case LangEnglish: return "english";
    case LangGerman: return "german";
    case LangFrench: return "french";
    case LangSpanish: return "spanish";
    case LangItalian: return "italian";
    case LangJapanese: return "japanese";
    default: return "?";
    }
}
// "english"/"en", "german"/"de", "french"/"fr", "spanish"/"es", "italian"/"it", "japanese"/"ja",
// "system" (LangSystem); LangNone for anything else.
inline int parse_language(const char* s) {
    if (!s) return LangNone;
    struct Name { const char* full; const char* code; int lang; };
    static constexpr Name names[] = {{"english", "en", LangEnglish}, {"german", "de", LangGerman},
                                     {"french", "fr", LangFrench},   {"spanish", "es", LangSpanish},
                                     {"italian", "it", LangItalian}, {"japanese", "ja", LangJapanese}};
    if (std::strcmp(s, "system") == 0) return LangSystem;
    for (const Name& n : names)
        if (std::strcmp(s, n.full) == 0 || std::strcmp(s, n.code) == 0) return n.lang;
    return LangNone;
}
// A game.language value from config.json (Dusklight writes the number): valid values only.
inline int config_language(long v) {
    return (v >= LangEnglish && v <= LangItalian) || v == LangJapanese ? int(v) : LangNone;
}

// SCE_SYSTEM_PARAM_LANG_* (psp2/system_param.h).
enum : int {
    SysJapanese = 0, SysEnglishUs = 1, SysFrench = 2, SysSpanish = 3, SysGerman = 4, SysItalian = 5, SysDutch = 6,
    SysPortuguesePt = 7, SysRussian = 8, SysKorean = 9, SysChineseT = 10, SysChineseS = 11, SysFinnish = 12,
    SysSwedish = 13, SysDanish = 14, SysNorwegian = 15, SysPolish = 16, SysPortugueseBr = 17, SysEnglishGb = 18,
    SysTurkish = 19
};
inline const char* system_language_name(int sys) {
    static constexpr const char* names[] = {"japanese", "english_us", "french", "spanish", "german", "italian",
                                            "dutch", "portuguese_pt", "russian", "korean", "chinese_t", "chinese_s",
                                            "finnish", "swedish", "danish", "norwegian", "polish", "portuguese_br",
                                            "english_gb", "turkish"};
    return sys >= 0 && sys < int(sizeof names / sizeof names[0]) ? names[sys] : "unknown";
}
// The game language for a Vita system language (before fitting it to the disc).
inline int game_language_for_system(int sys) {
    switch (sys) {
    case SysJapanese: return LangJapanese;
    case SysFrench: return LangFrench;
    case SysSpanish: return LangSpanish;
    case SysGerman: return LangGerman;
    case SysItalian: return LangItalian;
    case SysPortuguesePt:
    case SysPortugueseBr: return LangSpanish; // no Portuguese on any disc: see the header comment
    default: return LangEnglish;
    }
}

// Region letter of the game ID (GZ2E01 -> 'E'): the languages that disc carries.
inline bool disc_has_language(char region, int lang) {
    switch (region) {
    case 'P': return lang >= LangEnglish && lang <= LangItalian;
    case 'J': return lang == LangJapanese;
    default: return lang == LangEnglish;
    }
}
inline int disc_first_language(char region) { return region == 'J' ? LangJapanese : LangEnglish; }
inline const char* disc_languages_text(char region) {
    switch (region) {
    case 'P': return "english german french spanish italian";
    case 'J': return "japanese";
    default: return "english";
    }
}

struct LanguageChoice {
    int language = LangEnglish; // what the game gets
    int requested = LangEnglish; // before fitting to the disc
    const char* source = "default"; // "args", "config", "system", "default"
    bool fitted = false;            // requested is not on this disc
};
// arg: parse_language() of --vita-language (LangNone if absent); config: config_language() of
// config.json's game.language (LangNone if absent); system: SCE_SYSTEM_PARAM_LANG_* or -1.
inline LanguageChoice choose_language(char region, int arg, int config, int system) {
    LanguageChoice c;
    if (arg >= 0) {
        c.requested = arg;
        c.source = "args";
    } else if (arg != LangSystem && config >= 0) {
        c.requested = config;
        c.source = "config";
    } else if (system >= 0) {
        c.requested = game_language_for_system(system);
        c.source = "system";
    } else {
        c.requested = LangEnglish;
        c.source = "default";
    }
    c.fitted = !disc_has_language(region, c.requested);
    c.language = c.fitted ? disc_first_language(region) : c.requested;
    return c;
}

// --- confirm button ---
enum class Confirm : uint8_t { System, Cross, Circle };
constexpr const char* ConfirmNames[] = {"system", "cross", "circle"};
inline bool parse_confirm(const char* s, Confirm& out) {
    for (int i = 0; i < 3; ++i)
        if (s && std::strcmp(s, ConfirmNames[i]) == 0) {
            out = Confirm(i);
            return true;
        }
    return false;
}
// SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE = 0, _CROSS = 1; -1 when it could not be read (cross).
constexpr int SysEnterCircle = 0, SysEnterCross = 1;
inline bool confirm_on_circle(Confirm setting, int systemEnter) {
    if (setting == Confirm::Circle) return true;
    if (setting == Confirm::Cross) return false;
    return systemEnter == SysEnterCircle;
}

// GameCube PAD bits (dolphin/pad.h) for the two face buttons that swap.
constexpr uint32_t PadA = 0x100, PadB = 0x200;
struct FaceButtons {
    uint32_t cross, circle; // the GameCube button each Vita button presses
};
inline FaceButtons face_buttons(bool circleConfirms) {
    return circleConfirms ? FaceButtons{PadB, PadA} : FaceButtons{PadA, PadB};
}

} // namespace twv::vita

// Process-wide confirm choice (runtime/player_settings.cpp), set by main_vita.cpp before the game
// starts and read by the pad mapping, the input script and the prompts. Plain C: the readers live
// in both enum domains.
extern "C" void twv_vita_set_confirm_circle(int circle);
extern "C" int twv_vita_confirm_circle(void);
