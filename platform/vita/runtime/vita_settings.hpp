#pragma once
// Port-only player settings, changed on the settings page (ui/settings_page.hpp): render size,
// performance mode, FPS counter and the gesture that opens the page. Game settings (prompts, gyro,
// camera, language, confirm button) stay in Dusklight's config.json (dusk/settings_bridge.cpp).
//
// File: ux0:data/twilight-vita/vita_settings.json, a flat JSON object. Only the values the player
// chose are written, as Dusklight writes only its Value layer: a built-in default that changes later
// still reaches a player who never chose. Written atomically: a temporary file, then a rename (if the
// rename cannot replace the old file, it is removed first; a reader that finds only the temporary
// file reads that).
//
// Precedence (main_vita.cpp, at start): an option typed in args.txt wins; then this file; then the
// built-in default. A sequence.txt run (bench and test lines) ignores the file entirely, so a
// player's choice never changes a measurement.
//   renderScale  50 | 67 | 75 | 100   the play profile's render size (--vita-gxm-scale), default 75
//   performance  "normal" | "battery" normal: 444 MHz (--vita-clocks=legacy444, the default);
//                                     battery: the console's own clocks (--vita-clocks=system)
//   showFps      true | false         a frames-per-second counter (GXM renderer), default false
//   openGesture  "rear" | "front" | "both"   default rear
// SDK-free; host-tested (tests/settings_page_test.cpp, tests/clock_entry_test.cpp).
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace twv::vita {

constexpr const char* VitaSettingsFile = "ux0:data/twilight-vita/vita_settings.json";
constexpr const char* VitaSettingsTemp = "ux0:data/twilight-vita/vita_settings.json.tmp";
constexpr std::size_t VitaSettingsLimit = 4096;

enum class OpenGesture : uint8_t { Rear, Front, Both };
constexpr const char* OpenGestureNames[] = {"rear", "front", "both"};

struct VitaSettings {
    int renderScale = 75;      // 50, 67, 75 or 100
    bool batterySaver = false; // false: 444 MHz (legacy444); true: the console's own clocks (system)
    bool showFps = false;
    // Both by default: the rear hold, or three fingers on the screen if the rear pad gets in the way.
    OpenGesture gesture = OpenGesture::Both;
    // Chosen by the player (written to the file); the others follow the built-in defaults.
    bool renderScaleSet = false, performanceSet = false, showFpsSet = false, gestureSet = false;

    bool any_set() const { return renderScaleSet || performanceSet || showFpsSet || gestureSet; }
};

inline bool valid_render_scale(long v) { return v == 50 || v == 67 || v == 75 || v == 100; }

namespace detail {
inline const char* skip_space(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}
// The value text after "key": at the top level of the object (no nesting is expected), or nullptr.
inline const char* json_value(const char* text, const char* key) {
    const std::size_t keyLen = std::strlen(key);
    int depth = 0;
    for (const char* p = text; *p; ++p) {
        if (*p == '{' || *p == '[') { ++depth; continue; }
        if (*p == '}' || *p == ']') { --depth; continue; }
        if (*p != '"') continue;
        const char* start = p + 1;
        const char* q = start;
        while (*q && *q != '"') q += (*q == '\\' && q[1]) ? 2 : 1;
        if (!*q) return nullptr;
        const char* after = skip_space(q + 1);
        if (*after == ':' && depth == 1 && std::size_t(q - start) == keyLen && std::strncmp(start, key, keyLen) == 0)
            return skip_space(after + 1);
        p = q;
    }
    return nullptr;
}
inline bool json_long(const char* v, long& out) {
    if (!v) return false;
    const bool negative = *v == '-';
    const char* p = negative ? v + 1 : v;
    if (*p < '0' || *p > '9') return false;
    long n = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
        if (n > 1000000L) return false;
        n = n * 10 + (*p - '0');
    }
    if (*p == '.' || *p == 'e' || *p == 'E') return false;
    out = negative ? -n : n;
    return true;
}
inline bool json_bool(const char* v, bool& out) {
    if (!v) return false;
    if (std::strncmp(v, "true", 4) == 0) { out = true; return true; }
    if (std::strncmp(v, "false", 5) == 0) { out = false; return true; }
    return false;
}
// A short string without escapes.
inline bool json_word(const char* v, char* out, std::size_t size) {
    if (!v || *v != '"') return false;
    std::size_t n = 0;
    for (const char* p = v + 1; *p; ++p) {
        if (*p == '"') { out[n] = 0; return true; }
        if (*p == '\\' || n + 1 >= size) return false;
        out[n++] = *p;
    }
    return false;
}
} // namespace detail

// Reads the file's text. Unknown keys are ignored; a known key with a bad value is ignored and
// counted in `problems` (the receipt says so); the built-in default stays for it.
inline VitaSettings parse_vita_settings(const char* text, int* problems = nullptr) {
    using namespace detail;
    VitaSettings s;
    int bad = 0;
    if (!text) text = "";
    const char* p = skip_space(text);
    if (*p && *p != '{') ++bad;
    else if (*p) {
        long n = 0;
        bool b = false;
        char word[16];
        if (const char* v = json_value(p, "renderScale")) {
            if (json_long(v, n) && valid_render_scale(n)) { s.renderScale = int(n); s.renderScaleSet = true; }
            else ++bad;
        }
        if (const char* v = json_value(p, "performance")) {
            if (json_word(v, word, sizeof word) && (std::strcmp(word, "normal") == 0 || std::strcmp(word, "battery") == 0)) {
                s.batterySaver = std::strcmp(word, "battery") == 0;
                s.performanceSet = true;
            } else ++bad;
        }
        if (const char* v = json_value(p, "showFps")) {
            if (json_bool(v, b)) { s.showFps = b; s.showFpsSet = true; }
            else ++bad;
        }
        if (const char* v = json_value(p, "openGesture")) {
            bool ok = json_word(v, word, sizeof word);
            int g = -1;
            for (int i = 0; ok && i < 3; ++i)
                if (std::strcmp(word, OpenGestureNames[i]) == 0) g = i;
            if (g >= 0) { s.gesture = OpenGesture(g); s.gestureSet = true; }
            else ++bad;
        }
    }
    if (problems) *problems = bad;
    return s;
}

// The file's text for the chosen values only ("{}" when none). Returns the length, or 0 if `size`
// is too small.
inline std::size_t format_vita_settings(const VitaSettings& s, char* out, std::size_t size) {
    std::size_t used = 0;
    bool first = true;
    const auto put = [&](const char* text) {
        const std::size_t n = std::strlen(text);
        if (used + n + 1 > size) { used = size; return; }
        std::memcpy(out + used, text, n);
        used += n;
        out[used] = 0;
    };
    const auto key = [&](const char* name) {
        put(first ? "{\n    \"" : ",\n    \"");
        put(name);
        put("\": ");
        first = false;
    };
    if (size == 0) return 0;
    out[0] = 0;
    char number[16];
    if (s.renderScaleSet) {
        key("renderScale");
        std::snprintf(number, sizeof number, "%d", s.renderScale);
        put(number);
    }
    if (s.performanceSet) { key("performance"); put(s.batterySaver ? "\"battery\"" : "\"normal\""); }
    if (s.showFpsSet) { key("showFps"); put(s.showFps ? "true" : "false"); }
    if (s.gestureSet) {
        key("openGesture");
        put("\"");
        put(OpenGestureNames[int(s.gesture)]);
        put("\"");
    }
    put(first ? "{}\n" : "\n}\n");
    return used >= size ? 0 : used;
}

// Reads `path` (or, when it is missing, `temp`: a write interrupted between removing the old file
// and renaming the new one). Returns 1 read, 0 no file, -1 unreadable or too large.
inline int read_vita_settings(const char* path, const char* temp, VitaSettings& out, int* problems = nullptr) {
    if (problems) *problems = 0;
    FILE* file = std::fopen(path, "rb");
    if (!file && temp) file = std::fopen(temp, "rb");
    if (!file) return 0;
    char text[VitaSettingsLimit + 1];
    const std::size_t n = std::fread(text, 1, VitaSettingsLimit, file);
    const bool tooLong = n == VitaSettingsLimit && std::fgetc(file) != EOF;
    std::fclose(file);
    if (tooLong) return -1;
    text[n] = 0;
    out = parse_vita_settings(text, problems);
    return 1;
}

// Writes the chosen values: `temp`, then renamed over `path`. Where a rename cannot replace an
// existing file, the old file is removed and the rename tried once more. True when `path` holds
// the new text.
inline bool write_vita_settings(const VitaSettings& s, const char* path, const char* temp) {
    char text[VitaSettingsLimit];
    const std::size_t n = format_vita_settings(s, text, sizeof text);
    if (!n) return false;
    FILE* file = std::fopen(temp, "wb");
    if (!file) return false;
    bool ok = std::fwrite(text, 1, n, file) == n;
    ok = std::fflush(file) == 0 && ok;
    ok = std::fclose(file) == 0 && ok;
    if (!ok) {
        std::remove(temp);
        return false;
    }
    if (std::rename(temp, path) == 0) return true;
    std::remove(path);
    if (std::rename(temp, path) == 0) return true;
    std::remove(temp);
    return false;
}

} // namespace twv::vita
