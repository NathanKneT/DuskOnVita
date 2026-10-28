// The real card (Aurora lib/card + dolphin/card.cpp with patches/aurora/0005 and 0011) on a real
// folder, driven through the CARD API the game uses, with and without the port's hooks
// (platform/vita/aurora/save_safety.cpp linked or not: -DTWV_TEST_HOOKS=1/0).
//   card_save_safety_test <mode> <work folder> [<transcript>]
// Each mode is one process (CARDInit runs once). "saves" writes a transcript of what the game read
// back, so the two builds can be compared byte for byte.
#include <aurora/aurora.h>
#include <aurora/card.h>
#include <dolphin/card.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if TWV_TEST_HOOKS
#include "aurora/save_safety.hpp"
#endif

namespace aurora {   // what lib/aurora.cpp and lib/logging.cpp would provide
AuroraConfig g_config{};
char g_gameName[4] = {'G', 'Z', '2', 'P'};
void log_internal(AuroraLogLevel, const char*, const char*, unsigned int) noexcept {}
} // namespace aurora

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

#define CHECK(c)                                                                                       \
    do {                                                                                               \
        if (!(c)) {                                                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                          \
            std::exit(1);                                                                              \
        }                                                                                              \
    } while (0)

namespace {
constexpr std::size_t Header = 0x40, Block = 0x2000, Body = 4 * Block;
const char* const Name = "01-GZ2P-gczelda2.gci";
int gBackups = 0, gSame = 0, gImports = 0, gRestores = 0;
void sink(const char* event, const char* text) {
    std::printf("  receipt %s: %s\n", event, text);
    gBackups += std::strcmp(event, "save_backup") == 0;
    gSame += std::strcmp(event, "save_backup_same") == 0;
    gImports += std::strcmp(event, "save_imported") == 0;
    gRestores += std::strcmp(event, "save_restored") == 0;
}

Bytes gci(unsigned seed, bool zero = false) {
    Bytes b(Header + Body, 0);
    std::memcpy(b.data(), "GZ2P01", 6);
    std::strcpy(reinterpret_cast<char*>(b.data() + 8), "gczelda2");
    b[0x28] = 0x2c; b[0x2b] = 0x10;                       // a fixed modified time
    b[0x2c] = b[0x2d] = b[0x2e] = b[0x2f] = 0xff;          // no icon
    b[0x34] = 0x04;                                        // public
    b[0x37] = 5;                                           // first block
    b[0x39] = 4;                                           // 4 blocks
    b[0x3c] = b[0x3d] = b[0x3e] = b[0x3f] = 0xff;          // no comment
    if (!zero)
        for (std::size_t i = Header; i < b.size(); ++i) b[i] = static_cast<std::uint8_t>((i * 5 + seed * 11) % 253 + 1);
    return b;
}
void put(const fs::path& path, const Bytes& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
Bytes get(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path gWork, gCard;
void init() {
    static std::string user = gWork.string();
    aurora::g_config.userPath = user.c_str();
    aurora::g_config.logLevel = LOG_FATAL;
    CARDInit("GZ2P", "01");
}

// What the game reads back through the card: every file's status and data.
Bytes readAll() {
    Bytes out;
    for (s32 fileNo = 0; fileNo < 127; ++fileNo) {
        CARDStat stat{};
        if (CARDGetStatus(0, fileNo, &stat) != CARD_RESULT_READY) continue;
        CARDFileInfo info{};
        CHECK(CARDOpen(0, stat.fileName, &info) == CARD_RESULT_READY);
        Bytes data(stat.length);
        CHECK(CARDRead(&info, data.data(), static_cast<s32>(data.size()), 0) == CARD_RESULT_READY);
        CHECK(CARDClose(&info) == CARD_RESULT_READY);
        out.insert(out.end(), stat.fileName, stat.fileName + 32);
        const auto* raw = reinterpret_cast<const std::uint8_t*>(&stat);
        out.insert(out.end(), raw + 0x20, raw + sizeof stat);
        out.insert(out.end(), data.begin(), data.end());
    }
    return out;
}

// mDoMemCdRWm_Store: open, header sectors at 0, the data sector at 0x4000, read back, the same at
// 0x6000, read back, status, close.
void gameSave(unsigned seed) {
    CARDFileInfo info{};
    CHECK(CARDOpen(0, "gczelda2", &info) == CARD_RESULT_READY);
    Bytes sectors(0x4000), data(0x2000), check(0x2000);
    for (std::size_t i = 0; i < sectors.size(); ++i) sectors[i] = static_cast<std::uint8_t>(i * 3 + seed);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 7 + seed * 5 + 1);
    CHECK(CARDWrite(&info, sectors.data(), 0x4000, 0) == CARD_RESULT_READY);
    CHECK(CARDWrite(&info, data.data(), 0x2000, 0x4000) == CARD_RESULT_READY);
    CHECK(CARDRead(&info, check.data(), 0x2000, 0x4000) == CARD_RESULT_READY && check == data);
    CHECK(CARDWrite(&info, data.data(), 0x2000, 0x6000) == CARD_RESULT_READY);
    CHECK(CARDRead(&info, check.data(), 0x2000, 0x6000) == CARD_RESULT_READY && check == data);
    CARDStat stat{};
    CHECK(CARDGetStatus(0, info.fileNo, &stat) == CARD_RESULT_READY);
    stat.bannerFormat = 2; stat.iconAddr = 0; stat.iconFormat = 2; stat.iconSpeed = 3; stat.commentAddr = 0x2000;
    CHECK(CARDSetStatus(0, info.fileNo, &stat) == CARD_RESULT_READY);
    CHECK(CARDClose(&info) == CARD_RESULT_READY);
}

fs::path backup(int i) { return gCard / "backup" / ("01-GZ2P-gczelda2." + std::to_string(i) + ".gci"); }

// Four saves over a valid save, a remount, a rename and a delete. With the hooks: one backup per
// save (not per write), rotating; the game reads exactly what it reads without them.
void modeSaves(const char* transcriptPath) {
    put(gCard / Name, gci(1));
    init();
    CHECK(CARDCheck(0) == CARD_RESULT_READY);
    Bytes transcript = readAll();
    Bytes previous = get(gCard / Name);
    for (unsigned seed = 1; seed <= 4; ++seed) {
        gameSave(seed);
        const Bytes now = readAll();
        transcript.insert(transcript.end(), now.begin(), now.end());
#if TWV_TEST_HOOKS
        CHECK(gBackups == static_cast<int>(seed));
        CHECK(get(backup(1)) == previous);
#endif
        previous = get(gCard / Name);
    }
#if TWV_TEST_HOOKS
    CHECK(fs::exists(backup(3)) && !fs::exists(backup(4)));
#else
    CHECK(!fs::exists(gCard / "backup"));
#endif
    // The scanner after a remount: backup/ is not a card file, the save is the only one.
    CHECK(aurora_card_remount(0) && CARDCheck(0) == CARD_RESULT_READY);
    s32 files = 0;
    for (s32 fileNo = 0; fileNo < 127; ++fileNo) {
        CARDStat stat{};
        files += CARDGetStatus(0, fileNo, &stat) == CARD_RESULT_READY;
    }
    CHECK(files == 1);
    const Bytes afterRemount = readAll();
    transcript.insert(transcript.end(), afterRemount.begin(), afterRemount.end());
    const Bytes beforeRename = get(gCard / Name);
    CHECK(CARDRename(0, "gczelda2", "gczelda3") == CARD_RESULT_READY);
    CHECK(CARDRename(0, "gczelda3", "gczelda2") == CARD_RESULT_READY);
    const Bytes beforeDelete = get(gCard / Name);
    CHECK(CARDDelete(0, "gczelda2") == CARD_RESULT_READY);
    CHECK(!fs::exists(gCard / Name) && get(gCard / "_deleted" / Name) == beforeDelete);
#if TWV_TEST_HOOKS
    // Rename there and back: one copy of each name. The delete finds the newest copy identical.
    CHECK(gBackups == 6 && gSame == 1);
    CHECK(beforeDelete == beforeRename && get(backup(1)) == beforeDelete);
    CHECK(get(gCard / "backup" / "01-GZ2P-gczelda3.1.gci").size() == Header + Body);
#endif
    const Bytes afterDelete = readAll();
    transcript.insert(transcript.end(), afterDelete.begin(), afterDelete.end());
    put(transcriptPath, transcript);
}

// Boot with a card that has no save and a save in Dusklight's folder for the same disc.
void modeImport(bool hooksExpected) {
    const Bytes theirs = gci(9);
    const fs::path source = gWork / "TwilitRealm/Dusklight/EUR/Card A" / Name;
    put(source, theirs);
#if TWV_TEST_HOOKS
    twv::saves::setImportRoot((gWork / "TwilitRealm/Dusklight").string());
#endif
    init();
    CHECK(CARDCheck(0) == CARD_RESULT_READY);
    CARDFileInfo info{};
    const s32 opened = CARDOpen(0, "gczelda2", &info);
    if (!hooksExpected) {
        CHECK(opened == CARD_RESULT_NOFILE && !fs::exists(gCard / Name));
    } else {
        CHECK(opened == CARD_RESULT_READY && gImports == 1);
        Bytes body(Body);
        CHECK(CARDRead(&info, body.data(), static_cast<s32>(Body), 0) == CARD_RESULT_READY);
        CHECK(std::equal(body.begin(), body.end(), theirs.begin() + Header));
        CHECK(CARDClose(&info) == CARD_RESULT_READY);
    }
    CHECK(get(source) == theirs);
}

// Boot after a kill between the two steps of the Vita's rename (remove the save, rename the write
// file): only "<save>.tmp-<n>" is left, with a stray incomplete write file next to it.
void modeUnfinished() {
    const Bytes written = gci(12);
    put(gCard / (std::string(Name) + ".tmp-77"), written);
    put(gCard / (std::string(Name) + ".tmp-78"), Bytes(written.begin(), written.begin() + 4096));
    init();
    CHECK(CARDCheck(0) == CARD_RESULT_READY);
    CARDFileInfo info{};
    const s32 opened = CARDOpen(0, "gczelda2", &info);
    if (!TWV_TEST_HOOKS) {
        CHECK(opened == CARD_RESULT_NOFILE && !fs::exists(gCard / Name));   // the game would offer a new file
        return;
    }
    CHECK(opened == CARD_RESULT_READY && get(gCard / Name) == written);
    Bytes body(Body);
    CHECK(CARDRead(&info, body.data(), static_cast<s32>(Body), 0) == CARD_RESULT_READY);
    CHECK(std::equal(body.begin(), body.end(), written.begin() + Header));
}

// Boot with a plain .gci in the import folder (a PC Dusklight or Dolphin export, any file name).
void modeDrop() {
    const Bytes exported = gci(13);
    const fs::path dropped = gWork / "import" / "Zelda TP (Dolphin export).gci";
    put(dropped, exported);
#if TWV_TEST_HOOKS
    twv::saves::setDropFolder((gWork / "import").string());
#endif
    init();
    CARDFileInfo info{};
    const s32 opened = CARDOpen(0, "gczelda2", &info);
    CHECK(get(dropped) == exported);
    if (!TWV_TEST_HOOKS) {
        CHECK(opened == CARD_RESULT_NOFILE);
        return;
    }
    CHECK(opened == CARD_RESULT_READY && gImports == 1);
    Bytes body(Body);
    CHECK(CARDRead(&info, body.data(), static_cast<s32>(Body), 0) == CARD_RESULT_READY);
    CHECK(std::equal(body.begin(), body.end(), exported.begin() + Header));
}

// Boot with a broken save (all zero after the header, or only the header) and a valid copy.
void modeRecover(bool headerOnly, bool withBackup) {
    Bytes broken = gci(1, true);
    if (headerOnly) broken.resize(Header);
    put(gCard / Name, broken);
    if (withBackup) put(backup(1), gci(6));
    init();
    const bool restored = TWV_TEST_HOOKS && withBackup;
    if (restored) {
        CHECK(CARDCheck(0) == CARD_RESULT_READY && gRestores == 1);
        CARDFileInfo info{};
        CHECK(CARDOpen(0, "gczelda2", &info) == CARD_RESULT_READY);
        Bytes body(Body);
        CHECK(CARDRead(&info, body.data(), static_cast<s32>(Body), 0) == CARD_RESULT_READY);
        CHECK(std::equal(body.begin(), body.end(), gci(6).begin() + Header));
        bool kept = false;
        for (const auto& e : fs::directory_iterator(gCard))
            kept |= e.path().filename().string().rfind("broken-", 0) == 0 && get(e.path() / Name) == broken;
        CHECK(kept);
    } else {
        // As without the hooks: the file stays, and the card behaves as it always did with it.
        CHECK(get(gCard / Name) == broken);
        CHECK(headerOnly ? CARDCheck(0) != CARD_RESULT_READY : CARDCheck(0) == CARD_RESULT_READY);
    }
}
} // namespace

int main(int argc, char** argv) {
    CHECK(argc >= 3);
    const std::string mode = argv[1];
    gWork = argv[2];
    fs::remove_all(gWork);
    gCard = gWork / "EUR" / "Card A";
    fs::create_directories(gCard);
#if TWV_TEST_HOOKS
    twv::saves::setReceiptSink(sink);
#else
    (void)sink;
#endif
    if (mode == "saves") modeSaves(argc > 3 ? argv[3] : "transcript.bin");
    else if (mode == "import") modeImport(TWV_TEST_HOOKS);
    else if (mode == "import-off") {
#if TWV_TEST_HOOKS
        twv::saves::setImportMode(twv::saves::ImportOff);
#endif
        modeImport(false);
    }
    else if (mode == "unfinished") modeUnfinished();
    else if (mode == "drop") modeDrop();
    else if (mode == "recover-zero") modeRecover(false, true);
    else if (mode == "recover-header") modeRecover(true, true);
    else if (mode == "keep-zero") modeRecover(false, false);
    else if (mode == "keep-header") modeRecover(true, false);
    else CHECK(!"unknown mode");
    std::printf("card_save_safety_test %s (hooks %d): ok\n", mode.c_str(), TWV_TEST_HOOKS);
    return 0;
}
