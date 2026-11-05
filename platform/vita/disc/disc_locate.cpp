// The start-up disc search and the per-file probe (disc/disc_report.h). Reads only the header of
// each candidate (through disc_image.hpp, so CISO and GCZ show their logical disc header), and the
// read-speed test. Host-tested (tests/disc_image_test.cpp) with synthetic images in a temp folder.
#include "disc/disc_report.h"
#include "disc/disc_image.hpp"

#include <miniz.h>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using twv::disc::Format;

constexpr const char* VitaFolders[] = {"ux0:data/twilight-vita", "ux0:data/dusklight"};
constexpr const char* VitaDefaultImage = "game.iso";
// Read as discs, then named only (so a message can say what the file is).
constexpr const char* Extensions[] = {".iso", ".gcm", ".ciso", ".gcz", ".rvz", ".wia", ".wbfs", ".nfs", ".tgc", ".cso"};

bool inflate_block(const uint8_t* in, std::size_t inLen, uint8_t* out, std::size_t outLen) {
    mz_ulong written = mz_ulong(outLen);
    return mz_uncompress(out, &written, in, mz_ulong(inLen)) == MZ_OK && written == outLen;
}

int64_t file_read_at(void* user, uint64_t offset, void* out, std::size_t len) {
    FILE* file = static_cast<FILE*>(user);
    if (offset > 0x7FFFFFFFull || std::fseek(file, long(offset), SEEK_SET) != 0) return -1;
    const std::size_t n = std::fread(out, 1, len, file);
    return std::ferror(file) ? -1 : int64_t(n);
}

template <std::size_t N> void copy(char (&dst)[N], const char* src) { std::snprintf(dst, N, "%s", src ? src : ""); }

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
bool ends_with(const std::string& name, const char* ext) {
    const std::size_t n = std::strlen(ext);
    if (name.size() <= n) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (lower(name[name.size() - n + i]) != ext[i]) return false;
    return true;
}
bool less_nocase(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](char x, char y) { return lower(x) < lower(y); });
}

// Disc image files in `folder` (by extension), in case-insensitive name order.
std::vector<std::string> images_in(const char* folder) {
    std::vector<std::string> names;
    if (DIR* dir = opendir(folder)) {
        while (const dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.empty() || name[0] == '.') continue;
            for (const char* ext : Extensions)
                if (ends_with(name, ext)) {
                    names.push_back(name);
                    break;
                }
        }
        closedir(dir);
    }
    std::sort(names.begin(), names.end(), less_nocase);
    return names;
}

bool is_tp_gamecube(const char* id) {
    return std::strncmp(id, "GZ2", 3) == 0 && (id[3] == 'E' || id[3] == 'P' || id[3] == 'J') && id[4] == '0' &&
           id[5] == '1';
}

int probe(const char* path, TwvDiscReport& r) {
    const std::string keepSearched = r.searched;
    const int keepCandidates = r.candidates;
    r = TwvDiscReport{};
    copy(r.searched, keepSearched.c_str());
    r.candidates = keepCandidates;
    copy(r.path, path);
    struct stat info{};
    if (stat(path, &info) != 0) return r.status = TWV_DISC_NO_IMAGE;
    if (!S_ISREG(info.st_mode)) {
        copy(r.detail, "not a file");
        return r.status = TWV_DISC_UNREADABLE;
    }
    r.file_bytes = (unsigned long long)info.st_size;
    // Too short for any disc magic: usually a copy that failed (an empty file).
    if (info.st_size < 0x20) {
        copy(r.detail, info.st_size ? "too short to be a disc image" : "the file is empty");
        return r.status = TWV_DISC_UNREADABLE;
    }
    FILE* file = std::fopen(path, "rb");
    if (!file) {
        copy(r.detail, "cannot be opened");
        return r.status = TWV_DISC_UNREADABLE;
    }
    twv::disc::Image image;
    std::string error;
    const bool opened = image.open({file, file_read_at, uint64_t(info.st_size)}, inflate_block, error);
    copy(r.format, twv::disc::format_name(image.format()));
    r.block_bytes = image.block_size();
    if (!opened) {
        std::fclose(file);
        copy(r.detail, error.c_str());
        if (image.io_failure()) return r.status = TWV_DISC_UNREADABLE;
        if (image.format() == Format::Unknown) return r.status = TWV_DISC_NOT_GAMECUBE;
        return r.status = TWV_DISC_UNSUPPORTED_FORMAT;
    }
    r.disc_bytes = image.size();
    uint8_t head[0x440] = {};
    const int64_t n = image.read(0, head, sizeof head);
    std::fclose(file);
    if (n != int64_t(sizeof head)) {
        copy(r.detail, n < 0 ? image.last_error().c_str() : "the disc header is cut short");
        return r.status = n < 0 ? TWV_DISC_UNSUPPORTED_FORMAT : TWV_DISC_UNREADABLE;
    }
    for (int i = 0; i < 6; ++i) r.game_id[i] = char(head[i] >= 0x20 && head[i] < 0x7f ? head[i] : '?');
    r.revision = head[7];
    const bool gamecube = twv::disc::be32(head + 0x1C) == twv::disc::GameCubeMagic;
    const bool wii = twv::disc::be32(head + 0x18) == twv::disc::WiiMagic;
    if (!gamecube && !wii) {
        r.game_id[0] = 0;
        copy(r.detail, "no GameCube disc magic");
        return r.status = TWV_DISC_NOT_GAMECUBE;
    }
    if (wii || !is_tp_gamecube(r.game_id)) {
        copy(r.detail, wii ? "a Wii disc; the Vita reads the GameCube disc (GZ2E01, GZ2P01, GZ2J01)"
                           : "not Twilight Princess for GameCube (GZ2E01, GZ2P01, GZ2J01)");
        return r.status = TWV_DISC_WRONG_DISC;
    }
    // Dusklight's catalog (src/dusk/iso_validate.cpp, borealis find_accepted_disc) takes disc 0,
    // revision 0 of each; anything else would stop in its prelaunch check.
    if (head[6] != 0 || head[7] != 0) {
        std::snprintf(r.detail, sizeof r.detail, "%s disc %d revision %d; Dusklight accepts disc 0 revision 0",
                      r.game_id, head[6], head[7]);
        return r.status = TWV_DISC_WRONG_DISC;
    }
    // NKit shrinks the image and moves its files (mark at 0x200); gcm_nod does not undo that.
    if (std::memcmp(head + 0x200, "NKIT", 4) == 0) {
        copy(r.format, "NKit");
        copy(r.detail, "NKit images are not supported; restore it to ISO (NKit: Recover to ISO)");
        return r.status = TWV_DISC_UNSUPPORTED_FORMAT;
    }
    r.region = r.game_id[3];
    return r.status = TWV_DISC_OK;
}

void note(TwvDiscReport& r, const char* path, int status) {
    const std::size_t used = std::strlen(r.searched);
    if (used + 4 < sizeof r.searched)
        std::snprintf(r.searched + used, sizeof r.searched - used, "%s%s=%s", used ? ";" : "", path,
                      twv_disc_status_name(status));
}

} // namespace

int twv::disc::find_in(const char* configured, const char* defaultImage, const char* const* folders, int folderCount,
                       TwvDiscReport* out) {
    std::vector<std::string> order;
    const auto add = [&](const std::string& path) {
        if (!path.empty() && std::find(order.begin(), order.end(), path) == order.end()) order.push_back(path);
    };
    if (configured) add(configured);
    if (folderCount > 0 && defaultImage) add(std::string(folders[0]) + "/" + defaultImage);
    for (int i = 0; i < folderCount; ++i)
        for (const std::string& name : images_in(folders[i])) add(std::string(folders[i]) + "/" + name);

    TwvDiscReport current{}, firstProblem{};
    bool problem = false;
    for (const std::string& path : order) {
        const int status = probe(path.c_str(), current);
        note(current, path.c_str(), status);
        if (status != TWV_DISC_NO_IMAGE) ++current.candidates;
        if (status == TWV_DISC_OK) {
            *out = current;
            return TWV_DISC_OK;
        }
        if (status != TWV_DISC_NO_IMAGE && !problem) {
            firstProblem = current;
            problem = true;
        }
    }
    if (problem) {
        copy(firstProblem.searched, current.searched);
        firstProblem.candidates = current.candidates;
        *out = firstProblem;
    } else {
        *out = TwvDiscReport{};
        out->status = TWV_DISC_NO_IMAGE;
        copy(out->searched, current.searched);
        if (folderCount > 0 && defaultImage)
            std::snprintf(out->path, sizeof out->path, "%s/%s", folders[0], defaultImage);
    }
    return out->status;
}

extern "C" {

int twv_disc_probe(const char* path, TwvDiscReport* out) {
    *out = TwvDiscReport{};
    probe(path ? path : "", *out);
    note(*out, path ? path : "", out->status);
    return out->status;
}

int twv_disc_find(const char* configured, TwvDiscReport* out) {
    return twv::disc::find_in(configured, VitaDefaultImage, VitaFolders, 2, out);
}

long long twv_disc_read(const char* path, unsigned long long offset, void* out, unsigned len) {
    struct stat info{};
    if (!path || stat(path, &info) != 0) return -1;
    FILE* file = std::fopen(path, "rb");
    if (!file) return -1;
    twv::disc::Image image;
    std::string error;
    long long n = -1;
    if (image.open({file, file_read_at, uint64_t(info.st_size)}, inflate_block, error)) n = image.read(offset, out, len);
    std::fclose(file);
    return n;
}

const char* twv_disc_status_name(int status) {
    static const char* const names[] = {"ok", "no_image", "unreadable", "unsupported_format", "not_gamecube",
                                        "wrong_disc"};
    return status >= 0 && status <= TWV_DISC_WRONG_DISC ? names[status] : "?";
}

const char* twv_disc_card_folder(char region) {
    switch (region) {
    case 'P': return "EUR";
    case 'J': return "JAP";
    default: return "USA";
    }
}

int twv_disc_read_test(const char* path, unsigned mib, unsigned chunk_bytes, TwvDiscReadTest* out) {
    *out = TwvDiscReadTest{};
    struct stat info{};
    if (!path || stat(path, &info) != 0 || chunk_bytes == 0) return 0;
    FILE* file = std::fopen(path, "rb");
    if (!file) return 0;
    twv::disc::Image image;
    std::string error;
    int ok = image.open({file, file_read_at, uint64_t(info.st_size)}, inflate_block, error) ? 1 : 0;
    std::vector<uint8_t> buffer(chunk_bytes);
    const uint64_t want = std::min<uint64_t>(uint64_t(mib) << 20, image.size());
    const auto start = std::chrono::steady_clock::now();
    uint64_t done = 0;
    while (ok && done < want) {
        const int64_t n = image.read(done, buffer.data(), std::size_t(std::min<uint64_t>(chunk_bytes, want - done)));
        if (n <= 0) ok = 0;
        else done += uint64_t(n);
    }
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
    std::fclose(file);
    out->bytes = done;
    out->micros = (unsigned long long)micros.count();
    out->blocks_inflated = image.blocks_inflated();
    return ok;
}

} // extern "C"
