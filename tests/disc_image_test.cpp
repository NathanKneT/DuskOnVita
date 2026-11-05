// Host test for platform/vita/disc/disc_image.hpp (ISO, CISO, GCZ in place) and disc_locate.cpp (the
// start-up search, the per-file probe and the read test), with synthetic discs only (no game data):
// a GameCube header with a chosen game ID and a few MiB of patterned "file" bytes, wrapped the way
// nod and Dolphin write CISO and GCZ.
//
//   tools/host_tests.sh   (builds with the host compiler and miniz, then runs)
#include "disc/disc_image.hpp"
#include "disc/disc_report.h"

#include <miniz.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using twv::disc::Format;

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

void put_be32(std::vector<uint8_t>& b, std::size_t at, uint32_t v) {
    b[at] = uint8_t(v >> 24); b[at + 1] = uint8_t(v >> 16); b[at + 2] = uint8_t(v >> 8); b[at + 3] = uint8_t(v);
}
void put_le32(std::vector<uint8_t>& b, std::size_t at, uint32_t v) {
    b[at] = uint8_t(v); b[at + 1] = uint8_t(v >> 8); b[at + 2] = uint8_t(v >> 16); b[at + 3] = uint8_t(v >> 24);
}
void put_le64(std::vector<uint8_t>& b, std::size_t at, uint64_t v) {
    put_le32(b, at, uint32_t(v)); put_le32(b, at + 4, uint32_t(v >> 32));
}

// A GameCube disc of `bytes` (a multiple of 32 KiB): header, patterned data, and one run of zeros
// (0x40000..0x80000) that a CISO leaves out.
std::vector<uint8_t> make_disc(const char* id, std::size_t bytes = 4u << 20, uint8_t revision = 0) {
    std::vector<uint8_t> d(bytes, 0);
    std::memcpy(d.data(), id, 6);
    d[7] = revision;
    put_be32(d, 0x1C, twv::disc::GameCubeMagic);
    std::memcpy(d.data() + 0x20, "Synthetic Disc", 14);
    uint32_t x = 0x12345678u;
    for (std::size_t i = 0x2440; i < bytes; ++i) {
        if (i >= 0x40000 && i < 0x80000) continue;
        x = x * 1664525u + 1013904223u;
        d[i] = uint8_t(x >> 24) & (i % 7 ? 0xFF : 0x0F); // compressible, not trivially
    }
    return d;
}

std::vector<uint8_t> make_ciso(const std::vector<uint8_t>& disc, uint32_t block) {
    std::vector<uint8_t> out(twv::disc::CisoHeaderBytes, 0);
    std::memcpy(out.data(), "CISO", 4);
    put_le32(out, 4, block);
    for (std::size_t b = 0; b * block < disc.size(); ++b) {
        const auto begin = disc.begin() + std::ptrdiff_t(b * block);
        const auto end = disc.begin() + std::ptrdiff_t(std::min(disc.size(), (b + 1) * block));
        if (std::all_of(begin, end, [](uint8_t v) { return v == 0; })) continue; // absent
        out[8 + b] = 1;
        out.insert(out.end(), begin, end);
    }
    return out;
}

// Dolphin's GCZ: header, offsets (bit 63 = stored), Adler-32 of each stored payload, payloads.
std::vector<uint8_t> make_gcz(const std::vector<uint8_t>& disc, uint32_t block, std::size_t storedBlock) {
    const uint32_t count = uint32_t((disc.size() + block - 1) / block);
    std::vector<uint8_t> payload;
    std::vector<uint64_t> offsets;
    std::vector<uint32_t> hashes;
    for (uint32_t b = 0; b < count; ++b) {
        const uint8_t* src = disc.data() + std::size_t(b) * block;
        std::vector<uint8_t> packed(mz_compressBound(block));
        mz_ulong n = mz_ulong(packed.size());
        bool stored = b == storedBlock;
        if (!stored) {
            CHECK(mz_compress2(packed.data(), &n, src, block, 6) == MZ_OK);
            stored = n >= block;
        }
        if (stored) {
            packed.assign(src, src + block);
            n = block;
        }
        packed.resize(n);
        offsets.push_back(payload.size() | (stored ? 1ull << 63 : 0));
        hashes.push_back(twv::disc::adler32_of(packed.data(), packed.size()));
        payload.insert(payload.end(), packed.begin(), packed.end());
    }
    std::vector<uint8_t> out(32 + std::size_t(count) * 12, 0);
    put_le32(out, 0, 0xB10BC001u);
    put_le32(out, 4, 0);
    put_le64(out, 8, payload.size());
    put_le64(out, 16, disc.size());
    put_le32(out, 24, block);
    put_le32(out, 28, count);
    for (uint32_t b = 0; b < count; ++b) {
        put_le64(out, 32 + std::size_t(b) * 8, offsets[b]);
        put_le32(out, 32 + std::size_t(count) * 8 + std::size_t(b) * 4, hashes[b]);
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

struct Memory {
    const std::vector<uint8_t>* bytes;
};
int64_t memory_read(void* user, uint64_t offset, void* out, std::size_t len) {
    const auto& b = *static_cast<Memory*>(user)->bytes;
    if (offset >= b.size()) return 0;
    const std::size_t n = std::min<std::size_t>(len, std::size_t(b.size() - offset));
    std::memcpy(out, b.data() + offset, n);
    return int64_t(n);
}
bool inflate_block(const uint8_t* in, std::size_t inLen, uint8_t* out, std::size_t outLen) {
    mz_ulong written = mz_ulong(outLen);
    return mz_uncompress(out, &written, in, mz_ulong(inLen)) == MZ_OK && written == outLen;
}

// Reads at offsets that start and end inside, across and between blocks; all must match the disc.
// Past the synthetic disc's end, a CISO (sized as a whole GameCube disc) reads zeros.
void check_reads(twv::disc::Image& image, const std::vector<uint8_t>& disc, const char* what) {
    const uint64_t spots[][2] = {{0, 0x440},        {0x7FF0, 0x20},     {0x3FFF0, 0x40020}, {0x41000, 0x100},
                                 {0x8000, 0x30000},  {disc.size() - 16, 64}, {0x123456, 0x9000}, {0, disc.size()}};
    std::vector<uint8_t> got, expected;
    for (const auto& s : spots) {
        const uint64_t want = std::min<uint64_t>(s[1], image.size() - s[0]);
        expected.assign(std::size_t(want), 0);
        std::memcpy(expected.data(), disc.data() + s[0], std::size_t(std::min<uint64_t>(want, disc.size() - s[0])));
        got.assign(std::size_t(s[1]), 0xEE);
        const int64_t n = image.read(s[0], got.data(), std::size_t(s[1]));
        CHECK(n == int64_t(want));
        const bool same = n == int64_t(want) && std::memcmp(got.data(), expected.data(), std::size_t(want)) == 0;
        if (!same) std::fprintf(stderr, "%s: read at 0x%llx differs\n", what, (unsigned long long)s[0]);
        CHECK(same);
    }
}

void write_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}
std::vector<uint8_t> magic(const char* m, std::size_t n = 0x8000) {
    std::vector<uint8_t> b(n, 0);
    std::memcpy(b.data(), m, std::strlen(m));
    return b;
}
} // namespace

int main() {
    const std::vector<uint8_t> pal = make_disc("GZ2P01");

    // --- detection: magics as nod reads them (GCZ is 01 C0 0B B1 on disc) ---
    {
        uint8_t gcz[4] = {0x01, 0xC0, 0x0B, 0xB1}, gczBigEndian[4] = {0xB1, 0x0B, 0xC0, 0x01};
        CHECK(twv::disc::detect_format(gcz, 4) == Format::Gcz);
        CHECK(twv::disc::detect_format(gczBigEndian, 4) == Format::Unknown);
        CHECK(twv::disc::detect_format(pal.data(), 0x20) == Format::Iso);
        CHECK(twv::disc::detect_format(pal.data(), 0x1F) == Format::Unknown);
        const auto ciso = make_ciso(pal, 0x8000);
        CHECK(twv::disc::detect_format(ciso.data(), 0x20) == Format::Ciso);
        std::vector<uint8_t> cso = magic("CISO", 0x20);
        cso[4] = 0x18; put_le64(cso, 8, 0x57058000ull); cso[16] = 0x00; cso[17] = 0x08; // PSP-style CSO
        CHECK(twv::disc::detect_format(cso.data(), cso.size()) == Format::Cso);
        CHECK(twv::disc::detect_format(magic("RVZ\x01").data(), 4) == Format::Rvz);
        CHECK(twv::disc::detect_format(magic("WIA\x01").data(), 4) == Format::Wia);
        CHECK(twv::disc::detect_format(magic("WBFS").data(), 4) == Format::Wbfs);
        CHECK(twv::disc::detect_format(magic("EGGS").data(), 4) == Format::Nfs);
        uint8_t tgc[4] = {0xAE, 0x0F, 0x38, 0xA2};
        CHECK(twv::disc::detect_format(tgc, 4) == Format::Tgc);
        CHECK(twv::disc::adler32_of(reinterpret_cast<const uint8_t*>("Wikipedia"), 9) == 0x11E60398u);
        CHECK(twv::disc::adler32_of(pal.data(), pal.size()) == uint32_t(mz_adler32(MZ_ADLER32_INIT, pal.data(), pal.size())));
    }

    // --- the logical disc behind each container ---
    {
        Memory m{&pal};
        twv::disc::Image iso;
        std::string error;
        CHECK(iso.open({&m, memory_read, pal.size()}, nullptr, error) && iso.format() == Format::Iso);
        CHECK(iso.size() == pal.size() && iso.block_size() == 0);
        check_reads(iso, pal, "iso");

        for (uint32_t block : {0x8000u, 0x20000u}) {
            const auto bytes = make_ciso(pal, block);
            CHECK(bytes.size() < twv::disc::CisoHeaderBytes + pal.size()); // the zero run is left out
            Memory c{&bytes};
            twv::disc::Image ciso;
            CHECK(ciso.open({&c, memory_read, bytes.size()}, nullptr, error) && ciso.format() == Format::Ciso);
            CHECK(ciso.block_size() == block && ciso.size() == twv::disc::GameCubeDiscBytes);
            check_reads(ciso, pal, "ciso");
            uint8_t tail[16];
            CHECK(ciso.read(twv::disc::GameCubeDiscBytes - 16, tail, 16) == 16 && tail[0] == 0 && tail[15] == 0);
            CHECK(ciso.read(twv::disc::GameCubeDiscBytes, tail, 16) == 0);
            // Cut short: the map promises more blocks than the file holds.
            std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 1);
            Memory k{&cut};
            twv::disc::Image bad;
            CHECK(!bad.open({&k, memory_read, cut.size()}, nullptr, error) && error.find("CISO") != std::string::npos);
        }
        {
            auto bytes = make_ciso(pal, 0x8000);
            bytes[8 + 3] = 2; // a map byte that is neither 0 nor 1
            Memory c{&bytes};
            twv::disc::Image bad;
            CHECK(!bad.open({&c, memory_read, bytes.size()}, nullptr, error) && !bad.io_failure());
            bytes[8 + 3] = 1;
            put_le32(bytes, 4, 0x8001); // not a whole number of sectors
            CHECK(!bad.open({&c, memory_read, bytes.size()}, nullptr, error));
        }

        const auto gcz = make_gcz(pal, 0x8000, 5);
        CHECK(gcz.size() < pal.size());
        Memory g{&gcz};
        twv::disc::Image image;
        CHECK(!image.open({&g, memory_read, gcz.size()}, nullptr, error)); // needs an inflater
        CHECK(image.open({&g, memory_read, gcz.size()}, inflate_block, error) && image.format() == Format::Gcz);
        CHECK(image.size() == pal.size() && image.block_size() == 0x8000);
        check_reads(image, pal, "gcz");
        // The last block is kept: reading inside it again inflates nothing.
        uint8_t small[64];
        const uint64_t before = image.blocks_inflated();
        CHECK(image.read(0x123400, small, 32) == 32 && image.read(0x123420, small, 32) == 32);
        CHECK(image.blocks_inflated() == before + 1);
        // A damaged payload is an error, never wrong bytes.
        auto damaged = gcz;
        damaged[damaged.size() - 100] ^= 0x5A;
        Memory d{&damaged};
        twv::disc::Image bad;
        CHECK(bad.open({&d, memory_read, damaged.size()}, inflate_block, error));
        std::vector<uint8_t> tail(0x8000);
        CHECK(bad.read(pal.size() - 0x8000, tail.data(), tail.size()) == -1);
        CHECK(bad.last_error().find("checksum") != std::string::npos);
        std::vector<uint8_t> cut(gcz.begin(), gcz.end() - 1);
        Memory k{&cut};
        CHECK(!bad.open({&k, memory_read, cut.size()}, inflate_block, error));
        // Unsupported containers are named.
        const auto rvz = magic("RVZ\x01");
        Memory r{&rvz};
        CHECK(!bad.open({&r, memory_read, rvz.size()}, inflate_block, error) && error.find("RVZ") == 0);
    }

    // --- the search and the probe ---
    auto pattern = (fs::temp_directory_path() / "twv-disc-XXXXXX").string();
    const fs::path root = mkdtemp(pattern.data());
    const std::string ours = (root / "twilight-vita").string(), gxm = (root / "dusklight").string();
    fs::create_directories(ours);
    fs::create_directories(gxm);
    const char* folders[] = {ours.c_str(), gxm.c_str()};
    TwvDiscReport r{};
    const auto find = [&](const char* configured = nullptr) {
        return twv::disc::find_in(configured, "game.iso", folders, 2, &r);
    };
    const auto reset = [&] {
        for (const auto& dir : {ours, gxm})
            for (const auto& e : fs::directory_iterator(dir)) fs::remove(e.path());
    };

    // Nothing anywhere: "no image", about our game.iso.
    CHECK(find() == TWV_DISC_NO_IMAGE && std::string(r.path) == ours + "/game.iso" && r.candidates == 0);
    CHECK(std::string(r.searched) == ours + "/game.iso=no_image");

    // ux0:data/dusklight/: a USA CISO is found and read in place.
    const std::vector<uint8_t> usa = make_disc("GZ2E01");
    write_file(fs::path(gxm) / "Zelda.ciso", make_ciso(usa, 0x8000));
    CHECK(find() == TWV_DISC_OK && std::string(r.path) == gxm + "/Zelda.ciso");
    CHECK(std::string(r.format) == "CISO" && std::string(r.game_id) == "GZ2E01" && r.region == 'E');
    CHECK(r.block_bytes == 0x8000 && r.disc_bytes == twv::disc::GameCubeDiscBytes);
    CHECK(std::string(twv_disc_card_folder(r.region)) == "USA");

    // Ours first; a bad game.iso does not hide a good image elsewhere, and is listed.
    write_file(fs::path(ours) / "game.iso", magic("RVZ\x01"));
    CHECK(find() == TWV_DISC_OK && std::string(r.path) == gxm + "/Zelda.ciso" && r.candidates == 2);
    CHECK(std::string(r.searched) == ours + "/game.iso=unsupported_format;" + gxm + "/Zelda.ciso=ok");
    // Only bad images: the first problem, named.
    fs::remove(fs::path(gxm) / "Zelda.ciso");
    CHECK(find() == TWV_DISC_UNSUPPORTED_FORMAT && std::string(r.format) == "RVZ");
    CHECK(std::string(r.path) == ours + "/game.iso" && std::string(r.detail).find("RVZ") == 0);

    // Any file name in our folder; the region comes from the header, not the name.
    reset();
    write_file(fs::path(ours) / "USA.iso", pal);
    CHECK(find() == TWV_DISC_OK && std::string(r.path) == ours + "/USA.iso" && r.region == 'P');
    CHECK(std::string(twv_disc_card_folder(r.region)) == "EUR" && std::string(r.format) == "ISO");
    // Dusklight accepts revision 0 only.
    reset();
    write_file(fs::path(ours) / "tp.iso", make_disc("GZ2E01", 0x10000, 1));
    CHECK(find() == TWV_DISC_WRONG_DISC && r.revision == 1 && std::string(r.detail).find("revision 1") != std::string::npos);
    // A GCZ of the Japanese disc (case-insensitive extension).
    reset();
    write_file(fs::path(ours) / "tp.GCZ", make_gcz(make_disc("GZ2J01", 1u << 20), 0x4000, 0));
    CHECK(find() == TWV_DISC_OK && r.region == 'J' && r.revision == 0 && std::string(r.format) == "GCZ");
    CHECK(std::string(twv_disc_card_folder(r.region)) == "JAP");
    // Read test through the container.
    TwvDiscReadTest t{};
    CHECK(twv_disc_read_test(r.path, 1, 32 << 10, &t) == 1 && t.bytes == (1u << 20) && t.blocks_inflated == 64);
    // Logical bytes through the container (the start-up check of the chosen image's file table).
    {
        const std::vector<uint8_t> plain = make_disc("GZ2J01", 1u << 20);
        std::vector<uint8_t> got(0x440);
        CHECK(twv_disc_read(r.path, 0, got.data(), unsigned(got.size())) == 0x440 &&
              std::equal(got.begin(), got.end(), plain.begin()));
        CHECK(twv_disc_read(r.path, 0x9F000, got.data(), 0x440) == 0x440 &&
              std::equal(got.begin(), got.end(), plain.begin() + 0x9F000));
        CHECK(twv_disc_read(r.path, (1u << 20) - 0x10, got.data(), 0x440) == 0x10); // the disc's end
        CHECK(twv_disc_read((fs::path(ours) / "absent.iso").string().c_str(), 0, got.data(), 16) == -1);
    }

    // The configured image comes first, wherever it is.
    write_file(root / "elsewhere" / "mine.iso", usa);
    const std::string mine = (root / "elsewhere" / "mine.iso").string();
    CHECK(find(mine.c_str()) == TWV_DISC_OK && std::string(r.path) == mine && r.region == 'E');
    // A configured path that is gone falls back to the search.
    CHECK(find((root / "gone.iso").string().c_str()) == TWV_DISC_OK && r.region == 'J');

    // Wrong discs: another GameCube game, the Wii version, a file that is no disc.
    reset();
    write_file(fs::path(ours) / "game.iso", make_disc("GALE01", 0x10000));
    CHECK(find() == TWV_DISC_WRONG_DISC && std::string(r.game_id) == "GALE01");
    auto wii = make_disc("RZDE01", 0x10000);
    put_be32(wii, 0x1C, 0);
    put_be32(wii, 0x18, twv::disc::WiiMagic);
    write_file(fs::path(ours) / "game.iso", wii);
    CHECK(find() == TWV_DISC_WRONG_DISC && std::string(r.game_id) == "RZDE01" &&
          std::string(r.detail).find("Wii disc") != std::string::npos);
    write_file(fs::path(ours) / "game.iso", std::vector<uint8_t>(0x1000, 0x41));
    CHECK(find() == TWV_DISC_NOT_GAMECUBE && r.game_id[0] == 0);
    write_file(fs::path(ours) / "game.iso", std::vector<uint8_t>(0x100, 0));
    CHECK(find() == TWV_DISC_NOT_GAMECUBE);
    auto nkit = make_disc("GZ2E01", 0x10000);
    std::memcpy(nkit.data() + 0x200, "NKIT", 4);
    write_file(fs::path(ours) / "game.iso", nkit);
    CHECK(find() == TWV_DISC_UNSUPPORTED_FORMAT && std::string(r.format) == "NKit");
    // A header cut short; an empty file (a failed copy) and one too short for any magic.
    write_file(fs::path(ours) / "game.iso", std::vector<uint8_t>(pal.begin(), pal.begin() + 0x300));
    CHECK(find() == TWV_DISC_UNREADABLE);
    write_file(fs::path(ours) / "game.iso", std::vector<uint8_t>());
    CHECK(find() == TWV_DISC_UNREADABLE && std::string(r.detail) == "the file is empty");
    write_file(fs::path(ours) / "game.iso", std::vector<uint8_t>(0x1F, 0));
    CHECK(find() == TWV_DISC_UNREADABLE && std::string(r.detail) == "too short to be a disc image");
    // A folder named like an image is not a file.
    reset();
    fs::create_directories(fs::path(ours) / "game.iso");
    CHECK(find() == TWV_DISC_UNREADABLE && std::string(r.detail) == "not a file");
    fs::remove(fs::path(ours) / "game.iso");
    // Other files are not candidates.
    write_file(fs::path(ours) / "notes.txt", pal);
    CHECK(find() == TWV_DISC_NO_IMAGE);

    CHECK(twv_disc_probe((root / "elsewhere" / "mine.iso").string().c_str(), &r) == TWV_DISC_OK && r.region == 'E');
    CHECK(std::string(twv_disc_status_name(TWV_DISC_WRONG_DISC)) == "wrong_disc");
    fs::remove_all(root);

    if (failures) return 1;
    std::puts("disc_image_test: ISO/CISO/GCZ logical reads, damaged and unsupported containers, the search order "
              "(configured, game.iso, our folder, ux0:data/dusklight/), region from the header and the read test passed");
    return 0;
}
