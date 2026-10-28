// platform/vita/aurora/save_safety.cpp on real folders: what a .gci looks like to the card, the
// rolling backups, the boot check (restore only from a valid copy of the same save) and the
// Save import (same game and region only, copy never move, never over an existing file).
#include "aurora/save_safety.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using namespace twv::saves;

#define CHECK(c)                                                                                       \
    do {                                                                                               \
        if (!(c)) {                                                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                          \
            dumpReceipts();                                                                            \
            std::exit(1);                                                                              \
        }                                                                                              \
    } while (0)

namespace {
std::vector<std::pair<std::string, std::string>> gReceipts;
void dumpReceipts() {
    for (const auto& r : gReceipts) std::fprintf(stderr, "  receipt %s: %s\n", r.first.c_str(), r.second.c_str());
}
void sink(const char* event, const char* text) { gReceipts.emplace_back(event, text); }
int count(const char* event) {
    int n = 0;
    for (const auto& r : gReceipts) n += r.first == event;
    return n;
}
bool said(const char* event, const char* fragment) {
    for (const auto& r : gReceipts)
        if (r.first == event && r.second.find(fragment) != std::string::npos) return true;
    return false;
}

// A GCI as the card writes it: big-endian block count at 0x38, a body that is never all zero.
Bytes gci(const char* game, const char* maker, const char* name, unsigned blocks, unsigned seed) {
    Bytes b(HeaderSize + blocks * BlockSize, 0);
    std::memcpy(b.data(), game, 4);
    std::memcpy(b.data() + 4, maker, 2);
    std::strncpy(reinterpret_cast<char*>(b.data() + 8), name, 32);
    b[0x38] = static_cast<std::uint8_t>(blocks >> 8);
    b[0x39] = static_cast<std::uint8_t>(blocks);
    for (std::size_t i = HeaderSize; i < b.size(); ++i) b[i] = static_cast<std::uint8_t>((i * 7 + seed * 13) % 251 + 1);
    return b;
}
Bytes save(unsigned seed) { return gci("GZ2P", "01", "gczelda2", 4, seed); }
Bytes prefix(const Bytes& bytes, std::size_t n) { return Bytes(bytes.begin(), bytes.begin() + n); }
Bytes zeroBody() {
    Bytes b = save(1);
    std::fill(b.begin() + HeaderSize, b.end(), 0);
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
std::vector<std::string> names(const fs::path& folder) {
    std::vector<std::string> out;
    if (fs::exists(folder))
        for (const auto& e : fs::directory_iterator(folder)) out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

fs::path gRoot;
fs::path fresh(const char* name) {
    const fs::path path = gRoot / name;
    fs::remove_all(path);
    fs::create_directories(path);
    gReceipts.clear();
    return path;
}

const std::string Name = "01-GZ2P-gczelda2.gci";

void testNamesAndInspect() {
    CHECK(gciFileName("GZ2P", "01", "gczelda2") == Name);
    CHECK(gciFileName("GZ2P", "01", "a/b:c") == "01-GZ2P-a_b_c.gci");
    CHECK(gciFileName("GZ2", "01", "x").empty());
    CHECK(isGciFileName(Name));
    CHECK(!isGciFileName("backup") && !isGciFileName(Name + ".import") && !isGciFileName(Name + ".restore"));
    CHECK(!isGciFileName("gczelda2.gci"));
    CHECK(inspect(save(1), Name) == Valid);
    CHECK(inspect(zeroBody(), Name) == ZeroBody);
    CHECK(inspect(Bytes(10, 1), Name) == ShortHeader);
    const Bytes header = prefix(save(1), HeaderSize);
    CHECK(inspect(header, Name) == BadSize);               // the 23 Oct crash left the header only
    Bytes noBlocks = save(1);
    noBlocks[0x38] = noBlocks[0x39] = 0;
    CHECK(inspect(noBlocks, Name) == BadSize);
    Bytes longer = save(1);
    longer.push_back(1);
    CHECK(inspect(longer, Name) == BadSize);
    CHECK(inspect(save(1), "01-GZ2E-gczelda2.gci") == OtherIdentity);
    CHECK(inspect(gci("GZ2E", "01", "gczelda2", 4, 1), Name) == OtherIdentity);
    // A backup copied by hand next to the saves still names gczelda2 in its header: the card skips it.
    CHECK(isGciFileName("01-GZ2P-gczelda2.1.gci") && inspect(save(1), "01-GZ2P-gczelda2.1.gci") == OtherIdentity);
    CHECK(std::string(regionFolder("GZ2P")) == "EUR" && std::string(regionFolder("GZ2E")) == "USA" &&
          std::string(regionFolder("GZ2J")) == "JAP");
    std::printf("names and inspect: ok\n");
}

void testBackups() {
    const fs::path card = fresh("backups/EUR/Card A");
    const std::string folder = card.string();
    const fs::path file = card / Name;
    auto copy = [&](int i) { return get(backupPath(folder, Name, i)); };
    for (unsigned version = 1; version <= 4; ++version) {
        put(file, save(version));
        CHECK(backupBeforeChange(folder, Name, "write") == BackupTaken);
        CHECK(get(file) == save(version));                   // the card's file is only read
    }
    CHECK(copy(1) == save(4) && copy(2) == save(3) && copy(3) == save(2));   // version 1 rotated out
    CHECK(!fs::exists(backupPath(folder, Name, 4)));
    CHECK(names(card / BackupFolder) == (std::vector<std::string>{"01-GZ2P-gczelda2.1.gci", "01-GZ2P-gczelda2.2.gci",
                                                                  "01-GZ2P-gczelda2.3.gci"}));
    CHECK(names(card) == (std::vector<std::string>{Name, "backup"}));
    CHECK(count("save_backup") == 4 && said("save_backup", "3 kept"));
    // Unchanged since the newest copy: nothing rotates.
    CHECK(backupBeforeChange(folder, Name, "status") == BackupSkippedSame);
    CHECK(copy(1) == save(4) && copy(3) == save(2));
    // Only a version that would open as a save is kept.
    put(file, zeroBody());
    CHECK(backupBeforeChange(folder, Name, "write") == BackupSkippedInvalid);
    put(file, prefix(save(5), 100));
    CHECK(backupBeforeChange(folder, Name, "write") == BackupSkippedInvalid);
    fs::remove(file);
    CHECK(backupBeforeChange(folder, Name, "delete") == BackupSkippedInvalid);
    CHECK(copy(1) == save(4) && copy(2) == save(3) && copy(3) == save(2));
    CHECK(said("save_backup_skipped", "all zero") && said("save_backup_skipped", "missing"));

    // Fail safe: no backup folder can be made. The save is untouched and nothing throws.
    const fs::path blocked = fresh("blocked");
    put(blocked / Name, save(7));
    put(blocked / BackupFolder, Bytes{1, 2, 3});             // a file where the folder would go
    CHECK(backupBeforeChange(blocked.string(), Name, "write") == BackupFailed);
    CHECK(get(blocked / Name) == save(7) && get(blocked / BackupFolder) == (Bytes{1, 2, 3}));
    CHECK(said("save_backup_failed", "older copies kept"));
    // Fail safe: a read-only backup folder (not testable as root). The kept copies stay as they were.
    if (geteuid() != 0) {
        const fs::path ro = fresh("readonly");
        put(ro / Name, save(1));
        CHECK(backupBeforeChange(ro.string(), Name, "write") == BackupTaken);
        put(ro / Name, save(2));
        fs::permissions(ro / BackupFolder, fs::perms::owner_read | fs::perms::owner_exec);
        CHECK(backupBeforeChange(ro.string(), Name, "write") == BackupFailed);
        fs::permissions(ro / BackupFolder, fs::perms::owner_all);
        CHECK(get(backupPath(ro.string(), Name, 1)) == save(1) && get(ro / Name) == save(2));
        CHECK(names(ro / BackupFolder) == (std::vector<std::string>{"01-GZ2P-gczelda2.1.gci"}));
    }
    std::printf("backups: ok\n");
}

void testRecovery() {
    // The 23 Oct case: header and size right, body all zero, a valid copy in backup/.
    {
        const fs::path card = fresh("recover-zero");
        put(card / Name, zeroBody());
        put(backupPath(card.string(), Name, 1), save(3));
        put(backupPath(card.string(), Name, 2), save(2));
        const auto report = prepareCard(card.string(), "GZ2P", "01", 0);
        CHECK(report.checked == 1 && report.broken == 1 && report.restored == 1 && report.keptBroken == 0);
        CHECK(get(card / Name) == save(3));
        CHECK(get(backupPath(card.string(), Name, 1)) == save(3));   // the copy is copied, not moved
        std::string quarantine;
        for (const auto& n : names(card))
            if (n.rfind("broken-", 0) == 0) quarantine = n;
        CHECK(!quarantine.empty() && get(card / quarantine / Name) == zeroBody());
        CHECK(names(card).size() == 3);   // the save, backup/, broken-<time>/: no staging file left
        CHECK(said("save_restored", "all zero") && said("save_restored", "01-GZ2P-gczelda2.1.gci"));
    }
    // Header only (the card would refuse to open at all); copy 1 is not a save, copy 2 is.
    {
        const fs::path card = fresh("recover-short");
        put(card / Name, prefix(save(1), HeaderSize));
        put(backupPath(card.string(), Name, 1), zeroBody());
        put(backupPath(card.string(), Name, 2), save(2));
        const auto report = prepareCard(card.string(), "GZ2P", "01", 0);
        CHECK(report.restored == 1 && get(card / Name) == save(2));
    }
    // No valid copy: reported, nothing moves.
    {
        const fs::path card = fresh("recover-none");
        put(card / Name, zeroBody());
        put(backupPath(card.string(), Name, 1), Bytes(5, 1));
        const auto report = prepareCard(card.string(), "GZ2P", "01", 0);
        CHECK(report.broken == 1 && report.restored == 0 && report.keptBroken == 1);
        CHECK(get(card / Name) == zeroBody() && names(card) == (std::vector<std::string>{Name, "backup"}));
        CHECK(said("save_broken_kept", "no valid copy"));
    }
    // Another game's broken file, a hand-copied backup at the top, a valid save: none is touched.
    {
        const fs::path card = fresh("recover-others");
        const std::string other = "8P-GZLE-zelda.gci";
        Bytes otherZero = gci("GZLE", "8P", "zelda", 2, 1);
        std::fill(otherZero.begin() + HeaderSize, otherZero.end(), 0);
        put(card / other, otherZero);
        put(backupPath(card.string(), other, 1), gci("GZLE", "8P", "zelda", 2, 9));
        put(card / "01-GZ2P-gczelda2.1.gci", save(4));
        put(card / Name, save(5));
        put(card / "notes.txt", Bytes{1});
        const auto report = prepareCard(card.string(), "GZ2P", "01", 0);
        CHECK(report.checked == 3 && report.broken == 1 && report.restored == 0 && report.keptBroken == 1);
        CHECK(get(card / other) == otherZero && get(card / Name) == save(5) && get(card / "01-GZ2P-gczelda2.1.gci") == save(4));
        CHECK(said("save_broken_kept", "another game") && said("save_check", "the card skips it"));
    }
    std::printf("recovery: ok\n");
}

void testImport() {
    const fs::path gxm = gRoot / "TwilitRealm/Dusklight";
    setImportRoot(gxm.string());
    auto source = [&](const char* region, const std::string& name) { return gxm / region / "Card A" / name; };
    // Empty card (no folder yet): the same game's save is copied, the source stays as it was.
    {
        const fs::path card = fresh("import/EUR/Card A");
        fs::remove_all(card);
        fs::remove_all(gxm);
        put(source("EUR", Name), save(8));
        const auto before = fs::last_write_time(source("EUR", Name));
        const auto report = prepareCard(card.string(), "GZ2P", "01", 0);
        CHECK(report.imported == 1 && get(card / Name) == save(8));
        CHECK(get(source("EUR", Name)) == save(8) && fs::last_write_time(source("EUR", Name)) == before);
        CHECK(names(card) == (std::vector<std::string>{Name}) && names(gxm / "EUR/Card A") == (std::vector<std::string>{Name}));
        CHECK(said("save_imported", "copied, the source file is unchanged"));
        // Next boot: the card has a save now; nothing is copied again.
        gReceipts.clear();
        CHECK(prepareCard(card.string(), "GZ2P", "01", 0).imported == 0 && said("save_import", "already has"));
    }
    // The card has a save of its own: never overwritten.
    {
        const fs::path card = fresh("import-existing");
        put(card / Name, save(1));
        CHECK(prepareCard(card.string(), "GZ2P", "01", 0).imported == 0 && get(card / Name) == save(1));
    }
    // A broken save of its own (no copy to restore): still never overwritten.
    {
        const fs::path card = fresh("import-broken");
        put(card / Name, zeroBody());
        const auto report = prepareCard(card.string(), "GZ2P", "01", 0);
        CHECK(report.keptBroken == 1 && report.imported == 0 && get(card / Name) == zeroBody());
    }
    // Invalid sources are not copied.
    for (const Bytes& bad : {zeroBody(), prefix(save(1), 200), gci("GZ2E", "01", "gczelda2", 4, 3)}) {
        const fs::path card = fresh("import-bad");
        put(source("EUR", Name), bad);
        CHECK(prepareCard(card.string(), "GZ2P", "01", 0).imported == 0 && names(card).empty());
        CHECK(said("save_import", "skipped") && get(source("EUR", Name)) == bad);
    }
    // Another region's save (most such saves are for the USA disc): reported, not copied.
    {
        const fs::path card = fresh("import-region");
        fs::remove_all(gxm);
        put(source("USA", "01-GZ2E-gczelda2.gci"), gci("GZ2E", "01", "gczelda2", 4, 5));
        CHECK(prepareCard(card.string(), "GZ2P", "01", 0).imported == 0 && names(card).empty());
        CHECK(said("save_import_other_region", "USA disc (GZ2E); this disc is EUR (GZ2P)"));
    }
    // Off, and slot B: nothing is read or copied.
    {
        put(source("EUR", Name), save(8));
        const fs::path card = fresh("import-off");
        setImportMode(ImportOff);
        CHECK(prepareCard(card.string(), "GZ2P", "01", 0).imported == 0 && names(card).empty());
        CHECK(said("save_import", "off"));
        setImportMode(ImportAuto);
        CHECK(prepareCard(card.string(), "GZ2P", "01", 1).imported == 0 && names(card).empty());
        CHECK(prepareCard(card.string(), "GZ2P", "01", 0).imported == 1);
    }
    std::printf("import: ok\n");
}

// A kill between newlib's remove(save) and rename(tmp, save) leaves only "<save>.tmp-<n>".
void testUnfinished() {
    using namespace std::chrono_literals;
    const auto now = fs::file_time_type::clock::now();
    auto aged = [&](const fs::path& p, const Bytes& b, std::chrono::seconds age) { put(p, b); fs::last_write_time(p, now - age); };
    setImportRoot((gRoot / "no-dusklight-saves").string());   // only the card's own files here
    {   // The newest complete write file goes back; an incomplete one and the leftovers stay.
        const fs::path card = fresh("unfinished");
        aged(backupPath(card.string(), Name, 1), save(4), 60s);
        aged(card / (Name + ".tmp-111"), save(5), 30s);
        aged(card / (Name + ".tmp-222"), prefix(save(6), 1000), 10s);   // killed while copying
        const auto report = prepareCard(card.string(), "GZ2P", "01", 0);
        CHECK(report.unfinished == 2 && report.recovered == 1 && get(card / Name) == save(5));
        CHECK(fs::exists(card / (Name + ".tmp-111")) && fs::exists(card / (Name + ".tmp-222")));
        CHECK(said("save_recovered", ".tmp-111"));
    }
    {   // Older than the newest backup: a leftover, and the save may have been removed on purpose.
        const fs::path card = fresh("unfinished-old");
        aged(backupPath(card.string(), Name, 1), save(4), 10s);
        aged(card / (Name + ".tmp-111"), save(5), 60s);
        const auto report = prepareCard(card.string(), "GZ2P", "01", 0);
        CHECK(report.recovered == 0 && !fs::exists(card / Name) && said("save_missing_kept", "older than"));
    }
    {   // Our own restore, stopped before its last rename.
        const fs::path card = fresh("unfinished-restore");
        put(card / (Name + ".restore"), save(3));
        CHECK(prepareCard(card.string(), "GZ2P", "01", 0).recovered == 1 && get(card / Name) == save(3));
    }
    {   // The save is there (killed before the rename): nothing to do; another game's file: not ours.
        const fs::path card = fresh("unfinished-present");
        put(card / Name, save(1));
        put(card / (Name + ".tmp-9"), save(2));
        put(card / "01-GZ2E-gczelda2.gci.tmp-9", gci("GZ2E", "01", "gczelda2", 4, 2));
        put(card / (Name + ".tmp-x"), save(2));                          // not Aurora's pattern
        const auto report = prepareCard(card.string(), "GZ2P", "01", 0);
        CHECK(report.unfinished == 2 && report.recovered == 0 && get(card / Name) == save(1));
        CHECK(!fs::exists(card / "01-GZ2E-gczelda2.gci"));
    }
    std::printf("unfinished saves: ok\n");
}

// .gci files the player drops in ux0:data/twilight-vita/import/ (any name; identity from the header).
void testDropFolder() {
    const fs::path drop = gRoot / "drop";
    const fs::path gxm = gRoot / "TwilitRealm/Dusklight";
    setDropFolder(drop.string());
    setImportRoot(gxm.string());
    fs::remove_all(drop);
    fs::remove_all(gxm);
    Bytes zero3 = gci("GZ2P", "01", "gczelda3", 4, 1);
    std::fill(zero3.begin() + HeaderSize, zero3.end(), 0);
    put(drop / "My TP save.gci", save(6));
    put(drop / "second.GCI", gci("GZ2P", "01", "gczelda4", 4, 7));
    put(drop / "usa.gci", gci("GZ2E", "01", "gczelda2", 4, 5));
    put(drop / "zero.gci", zero3);
    put(drop / "notes.txt", Bytes{1, 2});
    put(gxm / "EUR/Card A" / Name, save(8));
    {
        const fs::path card = fresh("drop-card/EUR/Card A");
        fs::remove_all(card);
        const auto report = prepareCard(card.string(), "GZ2P", "01", 0);
        CHECK(report.imported == 2 && get(card / Name) == save(6));             // the dropped file wins
        CHECK(get(card / "01-GZ2P-gczelda4.gci") == gci("GZ2P", "01", "gczelda4", 4, 7));
        CHECK(!fs::exists(card / "01-GZ2P-gczelda3.gci") && !fs::exists(card / "01-GZ2E-gczelda2.gci"));
        CHECK(said("save_import_file", "a save for another disc") && said("save_import_file", "all zero"));
        CHECK(said("save_import", "already has"));                                 // the folder's not needed
        CHECK(names(drop).size() == 5 && get(drop / "My TP save.gci") == save(6));  // the folder is unchanged
        gReceipts.clear();
        CHECK(prepareCard(card.string(), "GZ2P", "01", 0).imported == 0 && count("save_imported") == 0);
    }
    {
        const fs::path card = fresh("drop-off");
        setImportMode(ImportOff);
        CHECK(prepareCard(card.string(), "GZ2P", "01", 0).imported == 0 && names(card).empty());
        setImportMode(ImportAuto);
    }
    setDropFolder((gRoot / "no-drop-folder").string());
    std::printf("import folder: ok\n");
}
} // namespace

int main(int argc, char** argv) {
    gRoot = fs::path(argc > 1 ? argv[1] : "save-safety-work");
    fs::remove_all(gRoot);
    fs::create_directories(gRoot);
    setReceiptSink(sink);
    setDropFolder((gRoot / "no-drop-folder").string());
    testNamesAndInspect();
    testBackups();
    testRecovery();
    testImport();
    testUnfinished();
    testDropFolder();
    fs::remove_all(gRoot);
    std::printf("save_safety_test: all passed\n");
    return 0;
}
