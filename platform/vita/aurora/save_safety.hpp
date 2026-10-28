#pragma once
// Save safety for the memory card (docs/ARCHITECTURE.md). Aurora's GCI-folder card calls the
// two hooks below (patches/aurora/0011):
//   before a change   rolling copies of the previous .gci in <card>/backup/<name>.<1..3>.gci
//   before the mount  a check of the saves in the card (a broken one is restored from its newest
//                     valid copy, a save lost between the two steps of a rename from the write
//                     that was replacing it, when that is unambiguous; otherwise only reported),
//                     then copies of saves into a card that has none (--vita-import-saves): .gci
//                     files the player put in ux0:data/twilight-vita/import/, then the save in Dusklight's folder.
// Nothing here can fail or change a card operation: every error ends in a receipt. Plain stdio and
// std::filesystem with error codes, no SDK types: the same code runs in the host tests. The change
// hook runs on the memory-card thread (32 KiB stack on the Vita), so file data lives on the heap.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace twv::saves {

constexpr std::size_t HeaderSize = 0x40;    // GCI header (CARD directory entry)
constexpr std::size_t BlockSize = 0x2000;   // card block
constexpr int BackupsKept = 3;              // <name>.1.gci (newest) .. <name>.3.gci
// Twilight Princess saves are 4 blocks (32,832 bytes); anything past this is left alone.
constexpr std::size_t MaxFileBytes = 8u << 20;
constexpr const char* BackupFolder = "backup";
constexpr const char* DefaultSaveName = "gczelda2";       // the game's own file name
constexpr const char* DusklightRoot = "ux0:data/TwilitRealm/Dusklight";
// Plain .gci files the player puts here (PC Dusklight's card folder, Dolphin's GCI export).
constexpr const char* DropFolder = "ux0:data/twilight-vita/import";

// What CardGciFolder::open makes of a file: Valid opens; OtherIdentity is skipped (its header names
// another file); ShortHeader, BadSize and Unreadable stop the whole card from opening; ZeroBody
// opens but holds no save (the game reports it as damaged).
enum Verdict : int { Valid, Missing, Unreadable, TooLarge, ShortHeader, OtherIdentity, BadSize, ZeroBody };
const char* verdictName(int verdict);

// Aurora's gci_filename / is_gci_filename rules: "<maker>-<game>-<file name>.gci".
std::string gciFileName(std::string_view game, std::string_view maker, std::string_view fileName);
bool isGciFileName(std::string_view name);
int inspect(const std::vector<std::uint8_t>& bytes, std::string_view diskName);
// The whole file (at most MaxFileBytes): Valid, or Missing / Unreadable / TooLarge.
int readFile(const std::string& path, std::vector<std::uint8_t>& out);
// Aurora's card region folder for a game code (dolphin/card.cpp get_card_region).
const char* regionFolder(std::string_view game);
std::string backupPath(const std::string& folder, const std::string& name, int index);

// Receipts as (event, text); the Vita build writes them to runtime.jsonl.
using ReceiptSink = void (*)(const char* event, const char* text);
void setReceiptSink(ReceiptSink sink);
enum ImportMode : int { ImportOff = 0, ImportAuto = 1 };
void setImportMode(int mode);
void setImportRoot(const std::string& root);   // tests; DusklightRoot otherwise
void setDropFolder(const std::string& folder); // tests; DropFolder otherwise

enum BackupResult : int { BackupTaken, BackupSkippedInvalid, BackupSkippedSame, BackupFailed };
int backupBeforeChange(const std::string& folder, const std::string& name, const char* reason);

struct BootReport {
    int checked = 0, broken = 0, restored = 0, keptBroken = 0, imported = 0;
    int unfinished = 0, recovered = 0;   // Aurora's .tmp-<n> write files; missing saves put back
};
BootReport prepareCard(const std::string& folder, const char* game, const char* maker, int slot);

} // namespace twv::saves

extern "C" void aurora_card_gci_before_change(const char* folder, const char* fileName, const char* reason);
extern "C" void aurora_card_gci_before_mount(const char* folder, const char* game, const char* maker, int slot);
// main_vita.cpp: --vita-import-saves=auto|off as 1|0 (returns 1). A strong reference, so it also
// pulls this file (and the hooks the card looks for) into the link.
extern "C" int twv_vita_save_import_select(int mode);
