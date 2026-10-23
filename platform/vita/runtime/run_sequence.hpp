#pragma once
// Console test sessions without file editing on the Vita: when
// ux0:data/twilight-vita/sequence.txt exists, each launch takes its next run
// (the index lives in sequence_state.txt) instead of args.txt. One run per
// line, "label: args" or just "args"; blank lines and '#' comments are skipped.
// After the last run, launches fall back to args.txt. SDK-free.
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace twv::vita {
constexpr const char* SequenceFile = "ux0:data/twilight-vita/sequence.txt";
constexpr const char* SequenceStateFile = "ux0:data/twilight-vita/sequence_state.txt";
constexpr std::size_t SequenceFileLimit = 4096;

struct SequenceRun {
    const char* label = "";
    const char* args = "";
    unsigned index = 0, count = 0;
};

// Splits `text` in place. True when run `index` exists (count is always set).
inline bool select_sequence_run(char* text, unsigned index, SequenceRun& out) noexcept {
    out = SequenceRun{};
    out.index = index;
    bool found = false;
    for (char* p = text; p && *p;) {
        char* line = p;
        p = std::strchr(p, '\n');
        if (p) *p++ = 0;
        std::size_t n = std::strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
        while (*line == ' ' || *line == '\t') ++line;
        if (!*line || *line == '#') continue;
        if (out.count++ != index) continue;
        found = true;
        out.args = line;
        // A label is a first word ending in ':' that is not an option.
        const std::size_t word = std::strcspn(line, " \t");
        if (word > 1 && line[0] != '-' && line[word - 1] == ':') {
            line[word - 1] = 0;
            out.label = line;
            char* rest = line + word;
            while (*rest == ' ' || *rest == '\t') ++rest;
            out.args = rest;
        }
    }
    return found;
}

// Decimal index from sequence_state.txt; missing or malformed means 0.
inline unsigned parse_sequence_state(const char* text) noexcept {
    unsigned value = 0;
    bool any = false;
    for (const char* p = text; p && *p >= '0' && *p <= '9'; ++p) {
        if (value > 100000) return 0;
        value = value * 10 + unsigned(*p - '0');
        any = true;
    }
    return any ? value : 0;
}

// --vita-exit-after=SECONDS (10..3600): the run ends itself that long after
// the player first appears. 0: not this option; 1: consumed; -1: invalid/repeated.
struct ExitAfterOption {
    unsigned seconds = 0;
    bool selected = false;
    int consume(const char* token) noexcept {
        if (!token) return -1;
        constexpr const char* name = "--vita-exit-after";
        constexpr std::size_t length = sizeof("--vita-exit-after") - 1;
        if (std::strncmp(token, name, length) != 0 || (token[length] && token[length] != '=')) return 0;
        if (token[length] != '=' || selected) return -1;
        unsigned value = 0;
        const char* p = token + length + 1;
        if (!*p) return -1;
        for (; *p; ++p) {
            if (*p < '0' || *p > '9' || value > 3600) return -1;
            value = value * 10 + unsigned(*p - '0');
        }
        if (value < 10 || value > 3600) return -1;
        seconds = value;
        selected = true;
        return 1;
    }
};
} // namespace twv::vita
