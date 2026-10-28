#pragma once
// Start-up checks that can fail before the game draws anything, and the plain on-screen message for
// each (ui/startup_canvas.hpp draws it, ui/startup_screen.cpp shows it). SDK-free, host-tested
// (tests/startup_check_test.cpp).
//
// The disc check reads what gcm_nod.cpp would read first: the 0x440-byte disc header and the FST. It
// accepts the GameCube discs Dusklight accepts (src/dusk/iso_validate.cpp: GZ2E01, GZ2J01, GZ2P01);
// the Wii ones are refused because the Vita disc reader has no Wii partition support. Container
// formats are named as gcm_nod.cpp names them. A plain image is complete when it holds the header
// blocks, the DOL, the FST and every file the FST lists (a trimmed image passes; a cut one does not).
#include "ui/startup_canvas.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace twv::vita {

constexpr std::size_t DiscHeaderBytes = 0x440;
constexpr uint32_t GameCubeMagic = 0xC2339F3Du, WiiMagic = 0x5D1C9EA3u;
constexpr uint64_t GameCubeDiscBytes = 1459978240ull;
constexpr uint32_t FstLimit = 4u << 20; // Twilight Princess: 64 KiB; anything near this is not a TP disc
constexpr const char* DiscFolder = "ux0:data/twilight-vita/";
constexpr const char* ShaderCompilerPaths[] = {"ur0:data/libshacccg.suprx", "ur0:data/external/libshacccg.suprx"};

enum class DiscProblem : uint8_t {
    None, Missing, NotAFile, Unreadable, Container, NKit, Wii, NotGameCube, WrongGame, Truncated
};
constexpr const char* DiscProblemNames[] = {"ok", "missing", "not_a_file", "unreadable", "container",
                                            "nkit", "wii", "not_gamecube", "wrong_game", "truncated"};

struct DiscCheck {
    DiscProblem problem = DiscProblem::None;
    char gameId[7] = {};
    const char* format = nullptr; // container name
    uint64_t size = 0, needed = 0;
    bool tested = false; // GZ2P01: the only disc run on the Vita so far
    uint32_t fstOffset = 0, fstSize = 0;
};

inline uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

inline const char* disc_container_name(const uint8_t* p) {
    if (std::memcmp(p, "CISO", 4) == 0) return "CISO";
    if (std::memcmp(p, "WBFS", 4) == 0) return "WBFS";
    if (std::memcmp(p, "RVZ\x01", 4) == 0) return "RVZ";
    if (std::memcmp(p, "WIA\x01", 4) == 0) return "WIA";
    if (std::memcmp(p, "EGGS", 4) == 0) return "NFS";
    if (read_be32(p) == 0xB10BC001u) return "GCZ";
    if (read_be32(p) == 0xAE0F38A2u) return "TGC";
    return nullptr;
}

// Step 1: the header. `n` bytes were read from the start of a file of `fileSize` bytes. With
// problem None, fstOffset/fstSize say which bytes step 2 needs.
inline DiscCheck check_disc_header(const uint8_t* head, std::size_t n, uint64_t fileSize) {
    DiscCheck out;
    out.size = fileSize;
    if (n < 0x20 || fileSize < 0x20) {
        out.problem = DiscProblem::Unreadable;
        return out;
    }
    if ((out.format = disc_container_name(head))) {
        out.problem = DiscProblem::Container;
        return out;
    }
    for (int i = 0; i < 6; ++i) out.gameId[i] = char(head[i] >= 0x20 && head[i] < 0x7f ? head[i] : '?');
    if (read_be32(head + 0x18) == WiiMagic) {
        out.problem = DiscProblem::Wii;
        return out;
    }
    if (read_be32(head + 0x1C) != GameCubeMagic) {
        out.problem = DiscProblem::NotGameCube;
        return out;
    }
    // NKit keeps the disc header and writes its own mark at 0x200 (GameCube images are shrunk).
    if (n >= 0x204 && std::memcmp(head + 0x200, "NKIT", 4) == 0) {
        out.problem = DiscProblem::NKit;
        return out;
    }
    if (std::memcmp(head, "GZ2", 3) != 0 || (head[3] != 'E' && head[3] != 'P' && head[3] != 'J') ||
        head[4] != '0' || head[5] != '1') {
        out.problem = DiscProblem::WrongGame;
        return out;
    }
    out.tested = head[3] == 'P';
    if (n < DiscHeaderBytes || fileSize < DiscHeaderBytes) {
        out.problem = DiscProblem::Truncated;
        out.needed = DiscHeaderBytes;
        return out;
    }
    const uint32_t dol = read_be32(head + 0x420);
    out.fstOffset = read_be32(head + 0x424);
    out.fstSize = read_be32(head + 0x428);
    uint64_t needed = 0x2440 + 0x20; // boot.bin, bi2.bin, the apploader header
    if (uint64_t(dol) + 0x100 > needed) needed = uint64_t(dol) + 0x100;
    if (uint64_t(out.fstOffset) + out.fstSize > needed) needed = uint64_t(out.fstOffset) + out.fstSize;
    out.needed = needed;
    if (out.fstSize < 12 || out.fstSize > FstLimit) {
        out.problem = DiscProblem::NotGameCube;
        return out;
    }
    if (fileSize < needed) out.problem = DiscProblem::Truncated;
    return out;
}

// Step 2: the FST (fstSize bytes from fstOffset). Raises `needed` to the end of the last file it
// lists; a malformed FST is NotGameCube, an image shorter than its files Truncated.
inline void check_disc_files(DiscCheck& d, const uint8_t* fst, std::size_t n) {
    if (d.problem != DiscProblem::None) return;
    if (n < 12 || fst[0] != 1) {
        d.problem = DiscProblem::NotGameCube;
        return;
    }
    const uint32_t entries = read_be32(fst + 8);
    if (!entries || uint64_t(entries) * 12 > n) {
        d.problem = DiscProblem::NotGameCube;
        return;
    }
    for (uint32_t i = 1; i < entries; ++i) {
        const uint8_t* e = fst + std::size_t(i) * 12;
        if (e[0] != 0) continue; // directory
        const uint64_t end = uint64_t(read_be32(e + 4)) + read_be32(e + 8);
        if (end > d.needed) d.needed = end;
    }
    if (d.size < d.needed) d.problem = DiscProblem::Truncated;
}

// --- messages ---
enum class StartupProblem : uint8_t {
    None, Disc, ShaderCompiler, Memory, Arguments, Graphics
};
constexpr const char* StartupProblemNames[] = {"none", "disc", "shader_compiler", "memory", "arguments", "graphics"};

// Owns the text a Message points into.
struct MessageText {
    ui::Message message;
    char kind[32] = {};
    char buffers[4][192] = {};
};

inline void mib_text(char* out, std::size_t size, uint64_t bytes) {
    std::snprintf(out, size, "%llu MB", static_cast<unsigned long long>((bytes + (1u << 19)) >> 20));
}

constexpr const char* SupportedDiscs =
    "Supported: Twilight Princess for GameCube, Europe (GZ2P01, tested on the Vita), USA (GZ2E01) or "
    "Japan (GZ2J01) (both supported by Dusklight, not yet tested on the Vita).";

// `similar`: other disc-like files found in the folder (e.g. "Zelda.iso"), at most SimilarLimit
// characters, or null.
constexpr std::size_t SimilarLimit = 80;
inline void disc_message(MessageText& t, const DiscCheck& d, const char* path, const char* similar) {
    auto& m = t.message;
    m = {};
    std::snprintf(t.kind, sizeof t.kind, "disc_%s", DiscProblemNames[int(d.problem)]);
    char size[32], needed[32];
    mib_text(size, sizeof size, d.size);
    mib_text(needed, sizeof needed, d.needed);
    switch (d.problem) {
    case DiscProblem::None:
        break;
    case DiscProblem::Missing:
    case DiscProblem::NotAFile:
        m.title = "Game disc image not found";
        m.items[0] = {ui::Style::Text, "Copy your own dump of the GameCube disc, a plain .iso file, to:"};
        m.items[1] = {ui::Style::Path, path};
        if (similar) {
            std::snprintf(t.buffers[0], sizeof t.buffers[0], "Found there: %s. If it is the disc, rename it "
                          "game.iso.", similar);
            m.items[2] = {ui::Style::Text, t.buffers[0]};
        }
        m.items[3] = {ui::Style::Note, SupportedDiscs};
        break;
    case DiscProblem::Unreadable:
        m.title = "Cannot read the disc image";
        m.items[0] = {ui::Style::Text, "The file is empty or could not be read. Copy it to the memory card again:"};
        m.items[1] = {ui::Style::Path, path};
        break;
    case DiscProblem::Container:
        m.title = "Disc image format not supported";
        std::snprintf(t.buffers[0], sizeof t.buffers[0],
                      "This file is a compressed %s image. The Vita port reads plain, full .iso images only. "
                      "Convert it to ISO on a PC (for example in Dolphin: Convert File, format ISO).", d.format);
        m.items[0] = {ui::Style::Text, t.buffers[0]};
        m.items[1] = {ui::Style::Path, path};
        break;
    case DiscProblem::NKit:
        m.title = "NKit disc image not supported";
        m.items[0] = {ui::Style::Text, "This file is an NKit image (shrunk). Restore it to a full .iso on a PC with "
                                       "NKit's Recover to ISO, then copy it again."};
        m.items[1] = {ui::Style::Path, path};
        break;
    case DiscProblem::Wii:
        m.title = "Wii disc image not supported";
        std::snprintf(t.buffers[0], sizeof t.buffers[0],
                      "This is a Wii disc (%s). The Vita port needs the GameCube disc.", d.gameId);
        m.items[0] = {ui::Style::Text, t.buffers[0]};
        m.items[1] = {ui::Style::Path, path};
        m.items[2] = {ui::Style::Note, SupportedDiscs};
        break;
    case DiscProblem::NotGameCube:
        m.title = "Not a GameCube disc image";
        m.items[0] = {ui::Style::Text, "This file is not a plain GameCube disc image (it may be damaged, or a "
                                       "different file renamed to game.iso)."};
        m.items[1] = {ui::Style::Path, path};
        m.items[2] = {ui::Style::Note, SupportedDiscs};
        break;
    case DiscProblem::WrongGame:
        m.title = "Wrong game";
        std::snprintf(t.buffers[0], sizeof t.buffers[0], "This disc is %s, not Twilight Princess.", d.gameId);
        m.items[0] = {ui::Style::Text, t.buffers[0]};
        m.items[1] = {ui::Style::Path, path};
        m.items[2] = {ui::Style::Note, SupportedDiscs};
        break;
    case DiscProblem::Truncated:
        m.title = "Disc image is incomplete";
        std::snprintf(t.buffers[0], sizeof t.buffers[0],
                      "The file has %s but the disc needs at least %s (a full image is 1392 MB). The copy or "
                      "the dump stopped early: copy it again.", size, needed);
        m.items[0] = {ui::Style::Text, t.buffers[0]};
        m.items[1] = {ui::Style::Path, path};
        break;
    }
}

inline void shader_compiler_message(MessageText& t) {
    auto& m = t.message;
    m = {};
    std::snprintf(t.kind, sizeof t.kind, "shader_compiler_missing");
    m.title = "Shader compiler not found";
    m.items[0] = {ui::Style::Text, "The graphics need libshacccg.suprx, the shader compiler from your own Vita's "
                                   "system files. Extract it with the ShaRKBR33D app, then start again. "
                                   "Looked for:"};
    m.items[1] = {ui::Style::Path, ShaderCompilerPaths[0]};
    m.items[2] = {ui::Style::Path, ShaderCompilerPaths[1]};
}

// what: the memory that is short ("CDRAM", "main memory", or a block name); have/need in bytes, or 0.
inline void memory_message(MessageText& t, const char* what, uint64_t have, uint64_t need) {
    auto& m = t.message;
    m = {};
    std::snprintf(t.kind, sizeof t.kind, "memory");
    m.title = "Not enough free memory";
    if (need) {
        char a[32], b[32];
        mib_text(a, sizeof a, have);
        mib_text(b, sizeof b, need);
        std::snprintf(t.buffers[0], sizeof t.buffers[0], "The graphics need %s of %s; %s is free.", b, what, a);
    } else {
        std::snprintf(t.buffers[0], sizeof t.buffers[0], "Could not reserve memory for the graphics (%s).", what);
    }
    m.items[0] = {ui::Style::Text, t.buffers[0]};
    m.items[1] = {ui::Style::Text, "Turn off plugins that load into games (ux0:tai/config.txt or "
                                   "ur0:tai/config.txt), restart the Vita, then start again."};
}

// source: "args.txt", "sequence.txt" or null (the built-in play profile); token: the option, or null.
inline void arguments_message(MessageText& t, const char* source, const char* problem, const char* token) {
    auto& m = t.message;
    m = {};
    std::snprintf(t.kind, sizeof t.kind, "args_error");
    m.title = source ? (std::strcmp(source, "sequence.txt") == 0 ? "Problem in sequence.txt" : "Problem in args.txt")
                     : "Start-up options rejected";
    std::snprintf(t.buffers[0], sizeof t.buffers[0], "%s.", problem);
    m.items[0] = {ui::Style::Text, t.buffers[0]};
    if (token) {
        std::snprintf(t.buffers[1], sizeof t.buffers[1], "Option: %s", token);
        m.items[1] = {ui::Style::Text, t.buffers[1]};
    }
    if (source) {
        std::snprintf(t.buffers[2], sizeof t.buffers[2], "%s%s", DiscFolder, source);
        m.items[2] = {ui::Style::Text, "Fix or delete this file, then start again:"};
        m.items[3] = {ui::Style::Path, t.buffers[2]};
    } else {
        m.items[2] = {ui::Style::Text, "This build cannot run its own default options. Please report it."};
    }
}

inline void graphics_message(MessageText& t, const char* detail) {
    auto& m = t.message;
    m = {};
    std::snprintf(t.kind, sizeof t.kind, "graphics_init");
    m.title = "The graphics could not start";
    std::snprintf(t.buffers[0], sizeof t.buffers[0], "%s.", detail && *detail ? detail : "GPU set-up failed");
    m.items[0] = {ui::Style::Text, t.buffers[0]};
    m.items[1] = {ui::Style::Text, "Restart the Vita and try again. If it happens again, please report it with "
                                   "this file:"};
    m.items[2] = {ui::Style::Path, "ux0:data/twilight-vita/runtime.jsonl"};
}

} // namespace twv::vita
