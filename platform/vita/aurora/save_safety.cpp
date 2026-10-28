// Save safety for the memory card: see save_safety.hpp and docs/ARCHITECTURE.md.
#include "aurora/save_safety.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <system_error>
#include <utility>

#if defined(__vita__)
#include "runtime/telemetry.hpp"
#endif

namespace twv::saves {
namespace {

#if defined(__vita__)
void defaultSink(const char* event, const char* text) { twv::vita::telemetryText(event, text); }
#else
void defaultSink(const char*, const char*) {}
#endif

ReceiptSink gSink = defaultSink;
int gImportMode = ImportAuto;
std::string gImportRoot = DusklightRoot;
std::string gDropFolder = DropFolder;

void receipt(const char* event, const std::string& text) {
    if (gSink != nullptr) gSink(event, text.c_str());
}

std::string join(const std::string& folder, const std::string& name) { return folder + "/" + name; }

// Anything at this path (a file, a folder, or something that cannot be inspected).
bool present(const std::string& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec) return ec != std::errc::no_such_file_or_directory;
    return std::filesystem::exists(status);
}

bool isFolder(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

bool makeFolder(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return isFolder(path);
}

bool writeFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const bool written = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool flushed = std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    return written && flushed && closed;
}

// Written, read back and compared: a copy that is not exactly right is removed again.
bool writeVerified(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint8_t> back;
    if (writeFile(path, bytes) && readFile(path, back) == Valid && back == bytes) return true;
    std::remove(path.c_str());
    return false;
}

// Never over an existing path, whatever the platform's rename does with one.
bool moveTo(const std::string& from, const std::string& to) {
    return !present(to) && std::rename(from.c_str(), to.c_str()) == 0;
}

std::string stem(const std::string& name) {
    return name.size() > 4 ? name.substr(0, name.size() - 4) : name;
}

std::string sizeText(std::size_t bytes) { return std::to_string(bytes) + " bytes"; }

bool sameGame(std::string_view diskName, const char* game, const char* maker) {
    return isGciFileName(diskName) && diskName.substr(3, 4) == game && diskName.substr(0, 2) == maker;
}

// broken-<local time>, or broken-<time>-<n> when that exists already. A clock that is plainly not set
// (Vita3K's reads 1970 at boot) gives broken-<n> instead.
std::string quarantineFolder(const std::string& folder) {
    char stamp[32] = "broken";
    const std::time_t now = std::time(nullptr);
    const std::tm* local = std::localtime(&now);
    if (local != nullptr && local->tm_year + 1900 >= 2020) std::strftime(stamp, sizeof stamp, "broken-%Y%m%d-%H%M%S", local);
    const bool dated = std::strcmp(stamp, "broken") != 0;
    std::string path = dated ? join(folder, stamp) : join(folder, "broken-1");
    for (int n = 2; present(path) && n < 100; ++n) path = join(folder, std::string(stamp) + "-" + std::to_string(n));
    return present(path) ? std::string() : path;
}

// Newest copy in backup/ that would open as the same save (index 1..3), or 0.
int newestValidBackup(const std::string& folder, const std::string& name, std::vector<std::uint8_t>& bytes) {
    for (int index = 1; index <= BackupsKept; ++index) {
        if (readFile(backupPath(folder, name, index), bytes) == Valid && inspect(bytes, name) == Valid) return index;
    }
    bytes.clear();
    return 0;
}

struct Broken {
    std::string name;
    int verdict;
};

// "<save>.gci.tmp-<n>" (Aurora's atomic write, renamed over the save when complete) or
// "<save>.gci.restore" (our own restore, renamed into place when complete): the save's name, else "".
std::string unfinishedTarget(std::string_view file) {
    std::size_t end = file.rfind(".gci.tmp-");
    if (end != std::string_view::npos) {
        const auto digits = file.substr(end + 9);
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return {};
    } else if (file.ends_with(".gci.restore")) {
        end = file.size() - 12;
    } else {
        return {};
    }
    const std::string name(file.substr(0, end + 4));
    return isGciFileName(name) ? name : std::string();
}

// (Its epoch is the file clock's, so times can be negative: compare them only with each other.)
std::filesystem::file_time_type modifiedTime(const std::string& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type::min() : time;
}

// Writes `bytes` next to the save as <name>.restore, checks it, then gives it the save's name (which
// must be free). Nothing changes if any step fails.
bool putInPlace(const std::string& folder, const std::string& name, const std::vector<std::uint8_t>& bytes) {
    const std::string target = join(folder, name), staged = target + ".restore";
    std::remove(staged.c_str());
    if (present(target) || !writeVerified(staged, bytes)) return false;
    if (moveTo(staged, target)) return true;
    std::remove(staged.c_str());
    return false;
}

// A save that is missing while the write that was replacing it is still complete on disk: on the
// Vita, rename() removes the old save before it renames the new one (newlib), so a process killed
// between the two leaves only "<save>.tmp-<n>". Put the newest valid such file back, but only if it
// is newer than the newest copy in backup/ (otherwise it is an old leftover and the save may have
// been removed on purpose: reported only). The leftovers stay where they are.
void recoverMissing(const std::string& folder, const std::string& name, const std::vector<std::string>& candidates,
                    BootReport& report) {
    std::vector<std::uint8_t> bytes, best;
    std::string bestFile;
    auto bestTime = std::filesystem::file_time_type::min();
    for (const auto& file : candidates) {
        const std::string path = join(folder, file);
        const auto time = modifiedTime(path);
        if (readFile(path, bytes) == Valid && inspect(bytes, name) == Valid && (bestFile.empty() || time > bestTime)) {
            best.swap(bytes);
            bestFile = file;
            bestTime = time;
        }
    }
    if (bestFile.empty()) {
        receipt("save_missing_kept", name + ": missing; " + std::to_string(candidates.size()) +
                " unfinished write file(s), none a complete save; nothing changed");
        return;
    }
    const std::string newest = backupPath(folder, name, 1);
    if (present(newest) && modifiedTime(newest) > bestTime) {
        receipt("save_missing_kept", name + ": missing; " + bestFile + " is older than " + BackupFolder + "/" +
                stem(name) + ".1.gci, so it is left as it is (copies in " + BackupFolder + "/)");
        return;
    }
    if (!putInPlace(folder, name, best)) {
        receipt("save_restore_failed", name + ": missing; could not put " + bestFile + " back; nothing was changed");
        return;
    }
    ++report.recovered;
    receipt("save_recovered", name + ": missing after an unfinished save; " + bestFile + " (" + sizeText(best.size()) +
            ", complete) copied back under its name");
}

// <name> stops the card from opening or holds no save. Put the newest valid copy in its place when
// there is one: the restored file is written next to it first (not a .gci, so the card never sees
// it), then the broken file moves into broken-<time>/ and the copy takes its name. No copy: the file
// stays where it is and the receipt says so.
void repair(const std::string& folder, const Broken& broken, BootReport& report) {
    const std::string target = join(folder, broken.name);
    std::vector<std::uint8_t> bytes;
    const int index = newestValidBackup(folder, broken.name, bytes);
    if (index == 0) {
        ++report.keptBroken;
        receipt("save_broken_kept", broken.name + ": " + verdictName(broken.verdict) +
                "; no valid copy in " + BackupFolder + "/, so the file is left as it is");
        return;
    }
    const std::string staged = target + ".restore";
    const std::string quarantine = quarantineFolder(folder);
    std::remove(staged.c_str());   // a copy an earlier failure left behind (backup/ still has it)
    if (quarantine.empty() || !writeVerified(staged, bytes)) {
        ++report.keptBroken;
        receipt("save_restore_failed", broken.name + ": could not stage " + backupPath(folder, broken.name, index) +
                "; nothing was changed");
        return;
    }
    const std::string kept = join(quarantine, broken.name);
    if (!makeFolder(quarantine) || !moveTo(target, kept)) {
        std::remove(staged.c_str());
        ++report.keptBroken;
        receipt("save_restore_failed", broken.name + ": could not move it into " + quarantine + "; nothing was changed");
        return;
    }
    if (!moveTo(staged, target)) {
        ++report.keptBroken;
        receipt("save_restore_failed", broken.name + ": moved to " + kept + ", but the copy stayed at " + staged);
        return;
    }
    ++report.restored;
    receipt("save_restored", broken.name + ": " + verdictName(broken.verdict) + "; moved to " + kept +
            " and replaced by " + BackupFolder + "/" + stem(broken.name) + "." + std::to_string(index) + ".gci (" +
            sizeText(bytes.size()) + ")");
}

// .gci files the player put in the import folder (any file name: PC Dusklight's card folder and
// Dolphin's GCI export use the same 64-byte header + blocks). Each is copied under the name its
// header gives, if it is a valid save of this exact game and maker (so the same region) and the card
// has no file of that name. Read only; the folder is left as it is.
void importDropped(const std::string& folder, const char* game, const char* maker, BootReport& report) {
    if (!isFolder(gDropFolder)) return;
    std::vector<std::string> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it{gDropFolder, ec}, end; !ec && it != end; it.increment(ec)) {
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::error_code typeError;
        if (ext == ".gci" && it->is_regular_file(typeError)) files.push_back(it->path().filename().string());
    }
    std::sort(files.begin(), files.end());
    std::vector<std::uint8_t> bytes;
    for (const auto& file : files) {
        const std::string source = join(gDropFolder, file);
        int verdict = readFile(source, bytes);
        std::string name;
        if (verdict == Valid) {
            if (bytes.size() < HeaderSize) {
                verdict = ShortHeader;
            } else {
                const auto* nameField = reinterpret_cast<const char*>(bytes.data() + 8);
                name = gciFileName({reinterpret_cast<const char*>(bytes.data()), 4},
                                   {reinterpret_cast<const char*>(bytes.data() + 4), 2},
                                   {nameField, static_cast<std::size_t>(std::find(nameField, nameField + 32, '\0') - nameField)});
                verdict = name.empty() ? OtherIdentity : inspect(bytes, name);
            }
        }
        if (verdict != Valid) {
            receipt("save_import_file", "skipped: " + source + " is " + verdictName(verdict));
        } else if (!sameGame(name, game, maker)) {
            receipt("save_import_file", "skipped: " + source + " is " + name + ", a save for another disc (this one is " +
                    std::string(maker) + "-" + game + ")");
        } else if (present(join(folder, name))) {
            receipt("save_import_file", "not needed: the card already has " + name + " (" + source + " left as it is)");
        } else if (!makeFolder(folder) || !putInPlace(folder, name, bytes)) {
            receipt("save_import_failed", "could not copy " + source + " into " + folder + "; nothing was changed");
        } else {
            ++report.imported;
            receipt("save_imported", source + " -> " + join(folder, name) + " (" + sizeText(bytes.size()) +
                    "; copied, the file in the import folder is unchanged)");
        }
    }
}

// The card holds no <maker>-<game>-gczelda2.gci: copy the Dusklight folder's file for the same region, if it is a
// valid save of this exact game. Read only; the target is never overwritten.
void importSave(const std::string& folder, const char* game, const char* maker, BootReport& report) {
    const std::string name = gciFileName(game, maker, DefaultSaveName);
    if (name.empty()) return;
    const std::string target = join(folder, name);
    if (present(target)) {
        receipt("save_import", "not needed: the card already has " + name);
        return;
    }
    const std::string region = regionFolder(game);
    const std::string sourceFolder = gImportRoot + "/" + region + "/Card A";
    const std::string source = join(sourceFolder, name);
    if (!present(source)) {
        receipt("save_import", "nothing to copy: no " + name + " in " + sourceFolder);
        // A save in Dusklight's folder for another region of the game cannot be loaded by this disc.
        static constexpr char Letters[] = {'E', 'P', 'J'};
        for (const char letter : Letters) {
            const std::string otherGame = std::string(game, 3) + letter;
            const std::string otherRegion = regionFolder(otherGame);
            if (otherGame == game || otherRegion == region) continue;
            const std::string other = gImportRoot + "/" + otherRegion + "/Card A/" + gciFileName(otherGame, maker, DefaultSaveName);
            if (present(other)) {
                receipt("save_import_other_region", other + " is a save for the " + otherRegion + " disc (" + otherGame +
                        "); this disc is " + region + " (" + game + "), so it was not copied");
            }
        }
        return;
    }
    std::vector<std::uint8_t> bytes;
    int verdict = readFile(source, bytes);
    if (verdict == Valid) verdict = inspect(bytes, name);
    if (verdict != Valid) {
        receipt("save_import", "skipped: " + source + " is " + verdictName(verdict));
        return;
    }
    if (!makeFolder(folder) || !putInPlace(folder, name, bytes)) {
        receipt("save_import_failed", "could not copy " + source + " into " + folder + "; nothing was changed");
        return;
    }
    ++report.imported;
    receipt("save_imported", source + " -> " + target + " (" + sizeText(bytes.size()) +
            "; copied, the source file is unchanged)");
}

std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }

} // namespace

const char* verdictName(int verdict) {
    switch (verdict) {
    case Valid: return "valid";
    case Missing: return "missing";
    case Unreadable: return "unreadable";
    case TooLarge: return "too large to check";
    case ShortHeader: return "shorter than its header";
    case OtherIdentity: return "a file whose header names another save";
    case BadSize: return "the wrong size for its block count";
    case ZeroBody: return "all zero after its header";
    default: return "unknown";
    }
}

std::string gciFileName(std::string_view game, std::string_view maker, std::string_view fileName) {
    if (game.size() != 4 || maker.size() != 2 || fileName.size() > 32 || game.find('\0') != std::string_view::npos ||
        maker.find('\0') != std::string_view::npos || fileName.find('\0') != std::string_view::npos) {
        return {};
    }
    std::string result = std::string(maker) + "-" + std::string(game) + "-" + std::string(fileName);
    for (char& c : result) {
        if (static_cast<unsigned char>(c) < 32 || std::strchr("<>:\"/\\|?*", c)) c = '_';
    }
    return result + ".gci";
}

bool isGciFileName(std::string_view name) {
    if (name.size() < 13 || name.size() > 12 + 32 || name[2] != '-' || name[7] != '-' || !name.ends_with(".gci")) {
        return false;
    }
    return name == gciFileName(name.substr(3, 4), name.substr(0, 2), name.substr(8, name.size() - 12));
}

int inspect(const std::vector<std::uint8_t>& bytes, std::string_view diskName) {
    if (bytes.size() < HeaderSize) return ShortHeader;
    const auto* header = bytes.data();
    const auto* nameField = reinterpret_cast<const char*>(header + 8);
    const std::size_t nameLength = std::find(nameField, nameField + 32, '\0') - nameField;
    const std::string identity = gciFileName({reinterpret_cast<const char*>(header), 4},
                                             {reinterpret_cast<const char*>(header + 4), 2}, {nameField, nameLength});
    if (identity != diskName) return OtherIdentity;
    const std::size_t blocks = be16(header + 0x38);
    if (blocks == 0 || bytes.size() != HeaderSize + blocks * BlockSize) return BadSize;
    const bool zero = std::all_of(bytes.begin() + HeaderSize, bytes.end(), [](std::uint8_t b) { return b == 0; });
    return zero ? ZeroBody : Valid;
}

int readFile(const std::string& path, std::vector<std::uint8_t>& out) {
    out.clear();
    if (!present(path)) return Missing;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return Unreadable;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return Unreadable;
    if (size > MaxFileBytes) return TooLarge;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return Unreadable;
    out.resize(static_cast<std::size_t>(size));
    const bool read = out.empty() || std::fread(out.data(), 1, out.size(), file) == out.size();
    const bool atEnd = std::fgetc(file) == EOF;
    std::fclose(file);
    if (!read || !atEnd) {
        out.clear();
        return Unreadable;
    }
    return Valid;
}

const char* regionFolder(std::string_view game) {
    if (game.size() != 4) return "USA";
    switch (game[3]) {
    case 'P': return "EUR";
    case 'J': return "JAP";
    default: return "USA";
    }
}

std::string backupPath(const std::string& folder, const std::string& name, int index) {
    return folder + "/" + BackupFolder + "/" + stem(name) + "." + std::to_string(index) + ".gci";
}

void setReceiptSink(ReceiptSink sink) { gSink = sink; }
void setImportMode(int mode) { gImportMode = mode == ImportOff ? ImportOff : ImportAuto; }
void setImportRoot(const std::string& root) { gImportRoot = root; }
void setDropFolder(const std::string& folder) { gDropFolder = folder; }

// Keep the version on disk before the card changes it: backup/<stem>.1.gci is the newest, .3 the
// oldest. Only a version that would open as a save is kept, and never twice in a row. The new copy
// is written and checked before anything rotates, so a failure costs nothing that was kept.
int backupBeforeChange(const std::string& folder, const std::string& name, const char* reason) {
    const std::string path = join(folder, name);
    std::vector<std::uint8_t> bytes;
    int verdict = readFile(path, bytes);
    if (verdict == Valid) verdict = inspect(bytes, name);
    if (verdict != Valid) {
        receipt("save_backup_skipped", name + " before " + reason + ": " + verdictName(verdict));
        return BackupSkippedInvalid;
    }
    const std::string backups = join(folder, BackupFolder);
    std::vector<std::uint8_t> newest;
    if (readFile(backupPath(folder, name, 1), newest) == Valid && newest == bytes) {
        receipt("save_backup_same", name + " before " + reason + ": unchanged since " + BackupFolder + "/" + stem(name) + ".1.gci");
        return BackupSkippedSame;
    }
    newest.clear();
    newest.shrink_to_fit();
    const std::string staged = join(backups, stem(name) + ".new");
    std::remove(staged.c_str());   // a copy an earlier failure left behind
    if (!makeFolder(backups) || !writeVerified(staged, bytes)) {
        receipt("save_backup_failed", name + " before " + reason + ": could not write " + staged + "; older copies kept");
        return BackupFailed;
    }
    // Make room: drop the oldest, then shift 2 -> 3 and 1 -> 2.
    const std::string oldest = backupPath(folder, name, BackupsKept);
    bool rotated = !present(oldest) || std::remove(oldest.c_str()) == 0;
    for (int index = BackupsKept - 1; rotated && index >= 1; --index) {
        const std::string from = backupPath(folder, name, index);
        if (present(from)) rotated = moveTo(from, backupPath(folder, name, index + 1));
    }
    const std::string first = backupPath(folder, name, 1);
    if (!rotated || !moveTo(staged, first)) {
        std::remove(staged.c_str());
        receipt("save_backup_failed", name + " before " + reason + ": could not rotate the copies in " + backups);
        return BackupFailed;
    }
    int kept = 0;
    for (int index = 1; index <= BackupsKept; ++index) kept += present(backupPath(folder, name, index)) ? 1 : 0;
    receipt("save_backup", name + " before " + reason + ": " + BackupFolder + "/" + stem(name) + ".1.gci (" +
            sizeText(bytes.size()) + "), " + std::to_string(kept) + " kept");
    return BackupTaken;
}

namespace {
using Unfinished = std::vector<std::pair<std::string, std::vector<std::string>>>;   // save name -> write files

// The same files CardGciFolder::open looks at (regular .gci files with a canonical name in the folder
// itself; backup/, broken-*/ and _deleted/ are folders it never reads), plus unfinished write files.
[[gnu::noinline]] void scanCard(const std::string& folder, BootReport& report, std::vector<Broken>& broken,
                                Unfinished& unfinished) {
    std::error_code ec;
    std::filesystem::directory_iterator it{folder, ec}, end;
    std::vector<std::uint8_t> bytes;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        std::error_code typeError;
        if (!it->is_regular_file(typeError)) continue;
        if (const std::string target = unfinishedTarget(name); !target.empty()) {
            ++report.unfinished;
            auto found = std::find_if(unfinished.begin(), unfinished.end(), [&](const auto& u) { return u.first == target; });
            if (found == unfinished.end()) found = unfinished.insert(unfinished.end(), {target, {}});
            found->second.push_back(name);
            continue;
        }
        if (it->path().extension() != ".gci" || !isGciFileName(name)) continue;
        ++report.checked;
        int verdict = readFile(join(folder, name), bytes);
        if (verdict == Valid) verdict = inspect(bytes, name);
        if (verdict == Valid || verdict == TooLarge) continue;
        if (verdict == OtherIdentity) {
            receipt("save_check", name + ": " + verdictName(verdict) + "; the card skips it");
            continue;
        }
        ++report.broken;
        broken.push_back({name, verdict});
    }
    if (ec) receipt("save_check", "could not list " + folder + ": " + ec.message());
}
} // namespace

BootReport prepareCard(const std::string& folder, const char* game, const char* maker, int slot) {
    BootReport report;
    if (game == nullptr || maker == nullptr || std::strlen(game) != 4 || std::strlen(maker) != 2) return report;
    std::vector<Broken> broken;
    Unfinished unfinished;
    const bool exists = isFolder(folder);
    if (exists) scanCard(folder, report, broken, unfinished);
    for (const auto& file : broken) {
        if (sameGame(file.name, game, maker)) {
            repair(folder, file, report);
        } else {
            ++report.keptBroken;
            receipt("save_broken_kept", file.name + ": " + verdictName(file.verdict) + "; another game's file, left as it is");
        }
    }
    for (const auto& [name, files] : unfinished) {
        if (!present(join(folder, name)) && sameGame(name, game, maker)) recoverMissing(folder, name, files, report);
    }
    if (exists) {
        receipt("save_check", folder + ": " + std::to_string(report.checked) + " save file(s), " +
                std::to_string(report.broken) + " broken, " + std::to_string(report.restored) + " restored, " +
                std::to_string(report.recovered) + " put back after an unfinished save, " +
                std::to_string(report.unfinished) + " unfinished write file(s) left in place");
    } else if (slot == 0) {
        receipt("save_check", folder + ": no card folder yet");
    }
    if (slot != 0) return report;
    if (gImportMode == ImportOff) {
        receipt("save_import", "off (--vita-import-saves=off)");
        return report;
    }
    importDropped(folder, game, maker, report);
    importSave(folder, game, maker, report);
    return report;
}

} // namespace twv::saves

extern "C" void aurora_card_gci_before_change(const char* folder, const char* fileName, const char* reason) {
    if (folder == nullptr || fileName == nullptr) return;
#if defined(__cpp_exceptions)
    try {
#endif
        twv::saves::backupBeforeChange(folder, fileName, reason != nullptr ? reason : "change");
#if defined(__cpp_exceptions)
    } catch (const std::exception& error) {
        twv::saves::receipt("save_backup_failed", std::string(fileName) + ": " + error.what() + "; older copies kept");
    }
#endif
}

extern "C" void aurora_card_gci_before_mount(const char* folder, const char* game, const char* maker, int slot) {
    if (folder == nullptr) return;
#if defined(__cpp_exceptions)
    try {
#endif
        twv::saves::prepareCard(folder, game, maker, slot);
#if defined(__cpp_exceptions)
    } catch (const std::exception& error) {
        twv::saves::receipt("save_check", std::string(folder) + ": stopped, " + error.what());
    }
#endif
}

extern "C" int twv_vita_save_import_select(int mode) {
    twv::saves::setImportMode(mode);
    return 1;
}
