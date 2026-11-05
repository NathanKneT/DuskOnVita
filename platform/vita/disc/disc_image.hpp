#pragma once
// Disc image containers the Vita reads in place, under gcm_nod.cpp's nod C API and the start-up
// disc search (disc_locate.cpp). SDK-free and host-tested (tests/disc_image_test.cpp).
//
// Formats, as nod v2.0.0-alpha.12 reads them (nod/src/io/{iso,ciso,gcz}.rs):
//   ISO/GCM  the plain image; logical bytes = file bytes.
//   CISO     "CISO", block size (u32 LE), a 0x7FF8-byte map of 0/1 per block; present blocks follow
//            the 0x8000-byte header in order, absent blocks read as zeros. No compression: a read
//            costs one map lookup. (nod also regenerates NKit junk data for absent blocks; a game
//            never reads those blocks, so they stay zeros here.)
//   GCZ      Dolphin's compressed image: header (LE), per-block offsets (bit 63 = stored) and
//            Adler-32 checksums, then zlib-deflated blocks. One block is inflated per miss and kept
//            (the last block read), so a file read in small pieces inflates each block once.
// Everything else Dolphin or nod writes (RVZ, WIA, WBFS, NFS, TGC) and PSP-style compressed CSO are
// detected and named, so the start-up message can say what the file is; they are not read.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace twv::disc {

enum class Format : uint8_t { Unknown, Iso, Ciso, Gcz, Rvz, Wia, Wbfs, Nfs, Tgc, Cso };
constexpr const char* FormatNames[] = {"unknown", "ISO", "CISO", "GCZ", "RVZ", "WIA", "WBFS", "NFS", "TGC", "CSO"};
inline const char* format_name(Format f) { return FormatNames[int(f)]; }
inline bool format_supported(Format f) { return f == Format::Iso || f == Format::Ciso || f == Format::Gcz; }

constexpr uint64_t GameCubeDiscBytes = 1459978240ull; // nod MINI_DVD_SIZE
constexpr uint32_t GameCubeMagic = 0xC2339F3Du, WiiMagic = 0x5D1C9EA3u;
constexpr std::size_t CisoHeaderBytes = 0x8000, CisoMapBytes = CisoHeaderBytes - 8;
constexpr std::size_t GczHeaderBytes = 32;
constexpr uint32_t GczBlockLimit = 4u << 20; // larger blocks are refused (memory)
constexpr uint32_t CisoBlockLimit = 64u << 20;

inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint32_t le32(const uint8_t* p) {
    return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
}
inline uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }

// The container from the first `n` bytes of a file. A plain image is recognised by its GameCube or
// Wii magic (0x18/0x1C), so ISO needs n >= 0x20; containers need their magic (and CSO 0x18 bytes).
inline Format detect_format(const uint8_t* head, std::size_t n) {
    if (n >= 4) {
        if (std::memcmp(head, "CISO", 4) == 0) {
            // PSP-style CSO shares the magic: header size (0x18), then a 64-bit image size and a
            // 0x800 block size. A GameCube/Wii CISO has a block size and then map bytes of 0 or 1.
            if (n >= 0x18 && le32(head + 4) == 0x18 && le32(head + 16) == 0x800) return Format::Cso;
            return Format::Ciso;
        }
        if (le32(head) == 0xB10BC001u) return Format::Gcz; // bytes 01 C0 0B B1 (nod GCZ_MAGIC)
        if (std::memcmp(head, "RVZ\x01", 4) == 0) return Format::Rvz;
        if (std::memcmp(head, "WIA\x01", 4) == 0) return Format::Wia;
        if (std::memcmp(head, "WBFS", 4) == 0) return Format::Wbfs;
        if (std::memcmp(head, "EGGS", 4) == 0) return Format::Nfs;
        if (be32(head) == 0xAE0F38A2u) return Format::Tgc;
    }
    if (n >= 0x20 && (be32(head + 0x1C) == GameCubeMagic || be32(head + 0x18) == WiiMagic)) return Format::Iso;
    return Format::Unknown;
}

// (Not named adler32: miniz.h defines zlib's names as macros.)
inline uint32_t adler32_of(const uint8_t* p, std::size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        const std::size_t chunk = n < 5552 ? n : 5552; // largest run without a 32-bit overflow
        for (std::size_t i = 0; i < chunk; ++i) {
            a += p[i];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
        p += chunk;
        n -= chunk;
    }
    return (b << 16) | a;
}

// Where the container's bytes come from: the nod stream on the Vita, memory or a FILE in tests.
struct Source {
    void* user = nullptr;
    int64_t (*read_at)(void* user, uint64_t offset, void* out, std::size_t len) = nullptr; // -1 error, 0 end
    uint64_t size = 0;
};
// zlib-wrapped deflate (RFC 1950) of `inLen` bytes into exactly `outLen` bytes; false otherwise.
using InflateFn = bool (*)(const uint8_t* in, std::size_t inLen, uint8_t* out, std::size_t outLen);

// The logical disc behind a container. Not thread-safe: gcm_nod.cpp serialises reads per image.
class Image {
public:
    // Reads the container's header and tables. `inflate` is needed for GCZ only.
    bool open(Source source, InflateFn inflater, std::string& error) {
        src_ = source;
        inflate_ = inflater;
        uint8_t head[0x40] = {};
        const int64_t n = raw_read(0, head, sizeof head);
        ioFailure_ = n < 0;
        if (n < 0) return fail(error, "Failed to read the disc image");
        format_ = detect_format(head, std::size_t(n));
        switch (format_) {
        case Format::Iso: size_ = src_.size; return true;
        case Format::Ciso: return open_ciso(error);
        case Format::Gcz: return open_gcz(head, std::size_t(n), error);
        case Format::Unknown: return fail(error, "Unknown disc format (no GameCube or Wii magic)");
        default:
            return fail(error, std::string(format_name(format_)) +
                                   " disc images are not supported by the Vita build; use ISO, CISO or GCZ");
        }
    }
    Format format() const { return format_; }
    uint64_t size() const { return size_; }        // logical disc bytes
    uint32_t block_size() const { return block_; } // 0 for ISO
    uint64_t blocks_inflated() const { return inflated_; }
    const std::string& last_error() const { return readError_; }
    // open() failed because the source could not be read (not because of its contents).
    bool io_failure() const { return ioFailure_; }

    // Logical read: up to `len` bytes at `offset` (fewer at the end of the disc), -1 on error.
    int64_t read(uint64_t offset, void* out, std::size_t len) {
        if (offset >= size_ || len == 0) return 0;
        if (len > size_ - offset) len = std::size_t(size_ - offset);
        if (format_ == Format::Iso) return raw_read(offset, out, len);
        auto* dst = static_cast<uint8_t*>(out);
        std::size_t done = 0;
        while (done < len) {
            const uint64_t at = offset + done;
            const uint64_t block = at / block_;
            const std::size_t within = std::size_t(at % block_);
            const std::size_t n = std::size_t(std::min<uint64_t>(len - done, block_ - within));
            if (!(format_ == Format::Ciso ? read_ciso(block, within, dst + done, n)
                                          : read_gcz(block, within, dst + done, n)))
                return -1;
            done += n;
        }
        return int64_t(done);
    }

private:
    static bool fail(std::string& error, std::string text) {
        error = std::move(text);
        return false;
    }
    // Fills `len` bytes unless the source ends first; bytes read, or -1.
    int64_t raw_read(uint64_t offset, void* out, std::size_t len) {
        auto* dst = static_cast<uint8_t*>(out);
        std::size_t total = 0;
        while (total < len) {
            const int64_t n = src_.read_at(src_.user, offset + total, dst + total, len - total);
            if (n < 0) return -1;
            if (n == 0) break;
            total += std::size_t(n);
        }
        return int64_t(total);
    }
    bool raw_exact(uint64_t offset, void* out, std::size_t len) { return raw_read(offset, out, len) == int64_t(len); }

    bool open_ciso(std::string& error) {
        std::vector<uint8_t> header(CisoHeaderBytes);
        if (!raw_exact(0, header.data(), header.size())) return fail(error, "Reading CISO header");
        block_ = le32(header.data() + 4);
        if (block_ == 0 || block_ > CisoBlockLimit || block_ % 0x800 != 0)
            return fail(error, "CISO block size " + std::to_string(block_) + " is not supported");
        cisoMap_.assign(CisoMapBytes, UINT32_MAX);
        uint32_t present = 0, last = 0;
        for (std::size_t i = 0; i < CisoMapBytes; ++i) {
            const uint8_t flag = header[8 + i];
            if (flag > 1) return fail(error, "Invalid CISO block map");
            if (flag) {
                cisoMap_[i] = present++;
                last = uint32_t(i);
            }
        }
        if (present == 0) return fail(error, "CISO image has no blocks");
        const uint64_t needed = CisoHeaderBytes + uint64_t(present) * block_;
        if (needed > src_.size)
            return fail(error, "CISO file size mismatch: expected at least " + std::to_string(needed) + " bytes, got " +
                                   std::to_string(src_.size));
        // CISO does not store the disc size: a GameCube disc, or the last present block if larger.
        const uint64_t extent = (uint64_t(last) + 1) * block_;
        size_ = extent > GameCubeDiscBytes ? extent : GameCubeDiscBytes;
        return true;
    }
    bool read_ciso(uint64_t block, std::size_t within, uint8_t* out, std::size_t n) {
        const uint32_t phys = block < cisoMap_.size() ? cisoMap_[std::size_t(block)] : UINT32_MAX;
        if (phys == UINT32_MAX) {
            std::memset(out, 0, n);
            return true;
        }
        if (!raw_exact(CisoHeaderBytes + uint64_t(phys) * block_ + within, out, n)) {
            readError_ = "CISO block " + std::to_string(block) + " is cut short";
            return false;
        }
        return true;
    }

    bool open_gcz(const uint8_t* head, std::size_t n, std::string& error) {
        if (n < GczHeaderBytes) return fail(error, "Reading GCZ header");
        gczCompressed_ = le64(head + 8);
        size_ = le64(head + 16);
        block_ = le32(head + 24);
        const uint32_t count = le32(head + 28);
        if (!inflate_) return fail(error, "GCZ needs an inflater");
        if (block_ == 0 || block_ > GczBlockLimit)
            return fail(error, "GCZ block size " + std::to_string(block_) + " is not supported");
        if (count == 0 || uint64_t(count) * block_ < size_ || (uint64_t(count) - 1) * block_ >= size_)
            return fail(error, "GCZ block count does not match the disc size");
        gczData_ = GczHeaderBytes + uint64_t(count) * 12;
        if (gczData_ + gczCompressed_ > src_.size) return fail(error, "GCZ file is cut short");
        std::vector<uint8_t> tables(std::size_t(count) * 12);
        if (!raw_exact(GczHeaderBytes, tables.data(), tables.size())) return fail(error, "Reading GCZ block map");
        gczMap_.resize(count);
        gczHash_.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            gczMap_[i] = le64(tables.data() + std::size_t(i) * 8);
            gczHash_[i] = le32(tables.data() + std::size_t(count) * 8 + std::size_t(i) * 4);
        }
        cache_.assign(block_, 0);
        packed_.assign(block_, 0);
        cached_ = UINT64_MAX;
        return true;
    }
    bool read_gcz(uint64_t block, std::size_t within, uint8_t* out, std::size_t n) {
        if (block != cached_ && !load_gcz(block)) return false;
        std::memcpy(out, cache_.data() + within, n);
        return true;
    }
    bool load_gcz(uint64_t block) {
        constexpr uint64_t Stored = 1ull << 63;
        cached_ = UINT64_MAX;
        const uint64_t entry = gczMap_[std::size_t(block)];
        const uint64_t start = entry & ~Stored;
        const uint64_t end = (block + 1 < gczMap_.size() ? gczMap_[std::size_t(block + 1)] : gczCompressed_) & ~Stored;
        if (end < start || end - start > block_ || ((entry & Stored) && end - start != block_)) {
            readError_ = "GCZ block " + std::to_string(block) + " has an invalid size";
            return false;
        }
        const std::size_t packed = std::size_t(end - start);
        if (!raw_exact(gczData_ + start, packed_.data(), packed)) {
            readError_ = "GCZ block " + std::to_string(block) + " is cut short";
            return false;
        }
        if (adler32_of(packed_.data(), packed) != gczHash_[std::size_t(block)]) {
            readError_ = "GCZ block " + std::to_string(block) + " checksum mismatch";
            return false;
        }
        if (entry & Stored) {
            std::memcpy(cache_.data(), packed_.data(), block_);
        } else if (!inflate_(packed_.data(), packed, cache_.data(), block_)) {
            // Every block inflates to a whole block, as nod requires (a GameCube disc is a whole
            // number of 32 KiB blocks).
            readError_ = "GCZ block " + std::to_string(block) + " does not inflate";
            return false;
        }
        cached_ = block;
        ++inflated_;
        return true;
    }

    Source src_{};
    InflateFn inflate_ = nullptr;
    Format format_ = Format::Unknown;
    uint64_t size_ = 0;
    uint32_t block_ = 0;
    std::vector<uint32_t> cisoMap_;
    std::vector<uint64_t> gczMap_;
    std::vector<uint32_t> gczHash_;
    uint64_t gczData_ = 0, gczCompressed_ = 0;
    std::vector<uint8_t> packed_, cache_;
    uint64_t cached_ = UINT64_MAX, inflated_ = 0;
    std::string readError_;
    bool ioFailure_ = false;
};

} // namespace twv::disc
