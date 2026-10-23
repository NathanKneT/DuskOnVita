// The nod C API (nod v2.0.0-alpha.12, nod-ffi/include/nod.h) for plain GameCube
// disc images, replacing the Rust crate on the Vita (no Rust target in the SDK image).
//
// Only the functions Aurora (lib/dolphin/dvd) and Borealis (src/disc.cpp) call are
// provided; anything else stays unresolved and becomes a named trap stub. Behavior
// follows nod-ffi/src/lib.rs and nod/src/disc/gcn.rs at that tag:
//   * a disc handle reads the raw image; disc size is the stream length (ISO);
//   * the GameCube data partition is the whole image; opening it loads boot.bin,
//     bi2.bin, the apploader, fst.bin and main.dol eagerly, as nod does;
//   * a file handle reads [offset, offset + length) of its FST node;
//   * nod_partition_iterate_fst visits nodes from index 1 and continues at the
//     index the callback returns.
// Differences, all visible as errors rather than approximations:
//   * container formats (CISO, GCZ, NFS, RVZ, WBFS, WIA, TGC) fail with
//     NOD_RESULT_ERR_FORMAT and a message naming the format;
//   * Wii discs open (so the header can be inspected) but their encrypted
//     partitions do not;
//   * FST names are decoded like Shift_JIS for ASCII and half-width katakana; a
//     double-byte character becomes U+FFFD (GameCube Twilight Princess names are ASCII);
//   * NodDiscOptions::preloader_threads is ignored (reads are synchronous).
// The stream callbacks are serialized per image: Aurora's DVD worker and the main
// thread may read different files at once, and the SDL stream behind them is not
// thread-safe.
#include <nod.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kBootSize = 0x440;     // disc header + debug block + boot block (boot.bin)
constexpr std::uint64_t kBb2Offset = 0x420;    // boot block (BB2)
constexpr std::uint64_t kBi2Size = 0x2000;     // bi2.bin
constexpr std::size_t kApploaderHeader = 0x20; // date[16], entry, size, trailer size, pad
constexpr std::size_t kDolHeader = 0x100;
constexpr std::size_t kFstNode = 12;
constexpr std::uint32_t kHandleMagic = 0x4e4f4448; // 'NODH'

thread_local std::string tLastError;
thread_local bool tHasError = false;

void clear_error() { tHasError = false; }
NodResult fail(NodResult result, std::string message) {
  tLastError = std::move(message);
  tHasError = true;
  return result;
}

std::uint32_t be32(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

const char* container_name(const std::uint8_t* p) {
  if (std::memcmp(p, "CISO", 4) == 0) return "CISO";
  if (std::memcmp(p, "WBFS", 4) == 0) return "WBFS";
  if (std::memcmp(p, "RVZ\x01", 4) == 0) return "RVZ";
  if (std::memcmp(p, "WIA\x01", 4) == 0) return "WIA";
  if (std::memcmp(p, "EGGS", 4) == 0) return "NFS";
  if (be32(p) == 0xB10BC001u) return "GCZ";
  if (be32(p) == 0xAE0F38A2u) return "TGC";
  return nullptr;
}

struct Image {
  NodDiscStream stream{};
  std::mutex lock;
  std::uint64_t size = 0;
  std::array<std::uint8_t, kBootSize> boot{};
  bool wii = false;
  // Partition metadata (GameCube data partition), loaded on first partition open.
  bool metaLoaded = false;
  std::vector<std::uint8_t> bi2, apploader, fst, dol;

  ~Image() {
    if (stream.close != nullptr) {
      stream.close(stream.user_data);
    }
  }

  // Fills `len` bytes at `offset` unless the stream ends first. Returns bytes read, or -1.
  std::int64_t read_at(std::uint64_t offset, void* out, std::size_t len) {
    std::lock_guard guard{lock};
    auto* dst = static_cast<std::uint8_t*>(out);
    std::size_t total = 0;
    while (total < len) {
      const std::int64_t n = stream.read_at(stream.user_data, offset + total, dst + total, len - total);
      if (n < 0) {
        return -1;
      }
      if (n == 0) {
        break;
      }
      total += static_cast<std::size_t>(n);
    }
    return static_cast<std::int64_t>(total);
  }

  bool read_exact(std::uint64_t offset, void* out, std::size_t len) {
    return read_at(offset, out, len) == static_cast<std::int64_t>(len);
  }

  NodResult load_meta() {
    if (metaLoaded) {
      return NOD_RESULT_OK;
    }
    const std::uint8_t* bb2 = boot.data() + kBb2Offset;
    const std::uint64_t dolOffset = be32(bb2 + 0), fstOffset = be32(bb2 + 4), fstSize = be32(bb2 + 8);

    bi2.resize(kBi2Size);
    if (!read_exact(kBootSize, bi2.data(), bi2.size())) {
      return fail(NOD_RESULT_ERR_IO, "Reading bi2.bin");
    }
    std::array<std::uint8_t, kApploaderHeader> appHeader{};
    if (!read_exact(kBootSize + kBi2Size, appHeader.data(), appHeader.size())) {
      return fail(NOD_RESULT_ERR_IO, "Reading apploader header");
    }
    const std::size_t appSize = kApploaderHeader + be32(appHeader.data() + 0x14) + be32(appHeader.data() + 0x18);
    apploader.assign(appSize, 0);
    if (!read_exact(kBootSize + kBi2Size, apploader.data(), apploader.size())) {
      return fail(NOD_RESULT_ERR_IO, "Reading apploader");
    }
    fst.assign(static_cast<std::size_t>(fstSize), 0);
    if (fstSize < kFstNode || !read_exact(fstOffset, fst.data(), fst.size())) {
      return fail(NOD_RESULT_ERR_IO, "Reading partition FST (offset " + std::to_string(fstOffset) + ", size " +
                                         std::to_string(fstSize) + ")");
    }
    std::array<std::uint8_t, kDolHeader> dolHeader{};
    if (!read_exact(dolOffset, dolHeader.data(), dolHeader.size())) {
      return fail(NOD_RESULT_ERR_IO, "Reading DOL header");
    }
    // DOL size: furthest end of the 7 text and 11 data sections (offsets at 0x00, sizes at 0x90).
    std::uint32_t dolSize = kDolHeader;
    for (int i = 0; i < 18; ++i) {
      dolSize = std::max(dolSize, be32(dolHeader.data() + i * 4) + be32(dolHeader.data() + 0x90 + i * 4));
    }
    dol.assign(dolSize, 0);
    if (!read_exact(dolOffset, dol.data(), dol.size())) {
      return fail(NOD_RESULT_ERR_IO, "Reading DOL");
    }
    metaLoaded = true;
    return NOD_RESULT_OK;
  }

  // FST view (nod::disc::fst::Fst::new): the root node's length is the node count.
  std::size_t fst_nodes() const {
    if (fst.size() < kFstNode) return 0;
    const std::uint64_t count = be32(fst.data() + 8);
    return count * kFstNode <= fst.size() ? static_cast<std::size_t>(count) : 0;
  }
  const std::uint8_t* node(std::size_t index) const { return fst.data() + index * kFstNode; }
};

enum class Kind { Disc, Partition, File };

}  // namespace

struct NodHandle {
  std::uint32_t magic = kHandleMagic;
  Kind kind;
  std::shared_ptr<Image> image;
  std::uint64_t base = 0, length = 0, pos = 0;
};

namespace {

template <typename H>
H* checked(H* handle) {
  if (handle == nullptr) {
    fail(NOD_RESULT_ERR_INVALID_HANDLE, "null handle");
    return nullptr;
  }
  if (handle->magic != kHandleMagic) {
    fail(NOD_RESULT_ERR_INVALID_HANDLE, "use after free");
    return nullptr;
  }
  return handle;
}

// Shift_JIS for the single-byte range (ASCII and half-width katakana); double-byte
// characters are replaced, as encoding_rs does for undecodable input.
std::string decode_name(const char* name, std::size_t max) {
  std::string out;
  for (std::size_t i = 0; i < max && name[i] != '\0'; ++i) {
    const auto c = static_cast<std::uint8_t>(name[i]);
    std::uint32_t cp;
    if (c < 0x80) {
      cp = c;
    } else if (c >= 0xA1 && c <= 0xDF) {
      cp = 0xFF61 + (c - 0xA1);
    } else {
      cp = 0xFFFD;
      if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && i + 1 < max && name[i + 1] != '\0') {
        ++i;
      }
    }
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

NodHandle* new_handle(Kind kind, std::shared_ptr<Image> image, std::uint64_t base, std::uint64_t length) {
  auto* handle = new (std::nothrow) NodHandle{};
  if (handle != nullptr) {
    handle->kind = kind;
    handle->image = std::move(image);
    handle->base = base;
    handle->length = length;
  }
  return handle;
}

}  // namespace

extern "C" {

const char* nod_error_message(void) { return tHasError ? tLastError.c_str() : nullptr; }

NodResult nod_disc_open_stream(const NodDiscStream* stream, const NodDiscOptions* options, NodHandle** out) {
  (void)options; // preloader_threads: reads are synchronous here
  clear_error();
  if (stream == nullptr || out == nullptr || stream->read_at == nullptr || stream->stream_len == nullptr ||
      stream->close == nullptr) {
    return fail(NOD_RESULT_ERR_INVALID_HANDLE, "null pointer argument");
  }
  auto image = std::make_shared<Image>();
  image->stream = *stream; // owned from here on: closed when the last handle goes away
  const std::int64_t length = stream->stream_len(stream->user_data);
  if (length < 0) {
    return fail(NOD_RESULT_ERR_IO, "Failed to get the disc stream length");
  }
  image->size = static_cast<std::uint64_t>(length);
  if (image->size < kBootSize || !image->read_exact(0, image->boot.data(), image->boot.size())) {
    return fail(NOD_RESULT_ERR_IO, "Failed to read the disc header");
  }
  if (const char* container = container_name(image->boot.data())) {
    return fail(NOD_RESULT_ERR_FORMAT,
                std::string(container) + " disc images are not supported by the Vita build; use a plain ISO/GCM");
  }
  static constexpr std::uint8_t kGcn[4]{0xC2, 0x33, 0x9F, 0x3D}, kWii[4]{0x5D, 0x1C, 0x9E, 0xA3};
  image->wii = std::memcmp(image->boot.data() + 0x18, kWii, 4) == 0;
  if (!image->wii && std::memcmp(image->boot.data() + 0x1C, kGcn, 4) != 0) {
    return fail(NOD_RESULT_ERR_FORMAT, "Unknown disc format (no GameCube or Wii magic)");
  }
  NodHandle* handle = new_handle(Kind::Disc, std::move(image), 0, static_cast<std::uint64_t>(length));
  if (handle == nullptr) {
    return fail(NOD_RESULT_ERR_OTHER, "out of memory");
  }
  *out = handle;
  return NOD_RESULT_OK;
}

NodResult nod_disc_open_partition_kind(NodHandle* disc, uint32_t kind, const NodPartitionOptions* options,
                                       NodHandle** out) {
  (void)options; // validate_hashes: Wii only
  clear_error();
  if (out == nullptr) {
    return fail(NOD_RESULT_ERR_INVALID_HANDLE, "null pointer argument");
  }
  if (checked(disc) == nullptr) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  if (disc->kind != Kind::Disc) {
    return fail(NOD_RESULT_ERR_INVALID_HANDLE, "handle is not a disc");
  }
  if (disc->image->wii) {
    return fail(NOD_RESULT_ERR_FORMAT, "Wii partitions are not supported by the Vita build");
  }
  if (kind != NOD_PARTITION_KIND_DATA) {
    return fail(NOD_RESULT_ERR_NOT_FOUND, "GameCube discs only have a data partition");
  }
  if (const NodResult result = disc->image->load_meta(); result != NOD_RESULT_OK) {
    return result;
  }
  NodHandle* handle = new_handle(Kind::Partition, disc->image, 0, disc->image->size);
  if (handle == nullptr) {
    return fail(NOD_RESULT_ERR_OTHER, "out of memory");
  }
  *out = handle;
  return NOD_RESULT_OK;
}

NodResult nod_partition_open_file(NodHandle* partition, uint32_t fst_index, NodHandle** out) {
  clear_error();
  if (out == nullptr) {
    return fail(NOD_RESULT_ERR_INVALID_HANDLE, "null pointer argument");
  }
  if (checked(partition) == nullptr) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  if (partition->kind != Kind::Partition) {
    return fail(NOD_RESULT_ERR_INVALID_HANDLE, "handle is not a partition");
  }
  const Image& image = *partition->image;
  const std::size_t count = image.fst_nodes();
  if (count == 0) {
    return fail(NOD_RESULT_ERR_FORMAT, "FST string table out of bounds");
  }
  if (fst_index >= count) {
    return fail(NOD_RESULT_ERR_NOT_FOUND,
                "FST index " + std::to_string(fst_index) + " out of range (total: " + std::to_string(count) + ")");
  }
  const std::uint8_t* node = image.node(fst_index);
  if (node[0] != 0) {
    return fail(NOD_RESULT_ERR_FORMAT, "node is not a file");
  }
  NodHandle* handle = new_handle(Kind::File, partition->image, be32(node + 4), be32(node + 8));
  if (handle == nullptr) {
    return fail(NOD_RESULT_ERR_OTHER, "out of memory");
  }
  *out = handle;
  return NOD_RESULT_OK;
}

void nod_free(NodHandle* handle) {
  if (handle != nullptr && handle->magic == kHandleMagic) {
    handle->magic = 0;
    delete handle;
  }
}

int64_t nod_read(NodHandle* handle, uint8_t* buf, size_t len) {
  clear_error();
  if (buf == nullptr) {
    fail(NOD_RESULT_ERR_INVALID_HANDLE, "null pointer argument");
    return -1;
  }
  if (checked(handle) == nullptr) {
    return -1;
  }
  if (handle->pos >= handle->length || len == 0) {
    return 0;
  }
  const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(len, handle->length - handle->pos));
  const std::int64_t n = handle->image->read_at(handle->base + handle->pos, buf, want);
  if (n < 0) {
    fail(NOD_RESULT_ERR_IO, "Failed to read from the disc stream");
    return -1;
  }
  handle->pos += static_cast<std::uint64_t>(n);
  return n;
}

int64_t nod_seek(NodHandle* handle, int64_t offset, int32_t whence) {
  clear_error();
  if (checked(handle) == nullptr) {
    return -1;
  }
  std::int64_t origin;
  switch (whence) {
  case 0: origin = 0; break;
  case 1: origin = static_cast<std::int64_t>(handle->pos); break;
  case 2: origin = static_cast<std::int64_t>(handle->length); break;
  default:
    fail(NOD_RESULT_ERR_OTHER, "invalid whence value: " + std::to_string(whence));
    return -1;
  }
  const std::int64_t target = origin + offset;
  if (target < 0) {
    fail(NOD_RESULT_ERR_IO, "invalid seek to a negative or overflowing position");
    return -1;
  }
  handle->pos = static_cast<std::uint64_t>(target);
  return target;
}

NodResult nod_disc_header(const NodHandle* disc, NodDiscHeader* out) {
  clear_error();
  if (out == nullptr) {
    return fail(NOD_RESULT_ERR_INVALID_HANDLE, "null pointer argument");
  }
  if (checked(disc) == nullptr) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  if (disc->kind != Kind::Disc) {
    return fail(NOD_RESULT_ERR_INVALID_HANDLE, "handle is not a disc");
  }
  static_assert(sizeof(NodDiscHeader) == 0x400, "nod DiscHeader is the first 0x400 bytes of the image");
  std::memcpy(out, disc->image->boot.data(), sizeof(NodDiscHeader));
  return NOD_RESULT_OK;
}

uint64_t nod_disc_size(const NodHandle* disc) {
  if (checked(disc) == nullptr || disc->kind != Kind::Disc) {
    return 0;
  }
  return disc->image->size;
}

NodResult nod_partition_meta(const NodHandle* partition, NodPartitionMeta* out) {
  clear_error();
  if (out == nullptr) {
    return fail(NOD_RESULT_ERR_INVALID_HANDLE, "null pointer argument");
  }
  if (checked(partition) == nullptr) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  if (partition->kind != Kind::Partition) {
    return fail(NOD_RESULT_ERR_INVALID_HANDLE, "handle is not a partition");
  }
  const Image& image = *partition->image;
  *out = NodPartitionMeta{};
  out->raw_boot = {image.boot.data(), image.boot.size()};
  out->raw_bi2 = {image.bi2.data(), image.bi2.size()};
  out->raw_apploader = {image.apploader.data(), image.apploader.size()};
  out->raw_dol = {image.dol.data(), image.dol.size()};
  out->raw_fst = {image.fst.data(), image.fst.size()};
  return NOD_RESULT_OK;
}

void nod_partition_iterate_fst(const NodHandle* partition, NodFstCallback callback, void* user_data) {
  clear_error();
  if (checked(partition) == nullptr) {
    return;
  }
  if (partition->kind != Kind::Partition) {
    fail(NOD_RESULT_ERR_INVALID_HANDLE, "handle is not a partition");
    return;
  }
  const Image& image = *partition->image;
  const std::size_t count = image.fst_nodes();
  if (count == 0) {
    fail(NOD_RESULT_ERR_FORMAT, "FST string table out of bounds");
    return;
  }
  const auto* strings = reinterpret_cast<const char*>(image.fst.data() + count * kFstNode);
  const std::size_t stringsSize = image.fst.size() - count * kFstNode;
  std::size_t index = 1; // skip the root node
  while (index < count) {
    const std::uint8_t* node = image.node(index);
    const std::uint32_t nameOffset = (std::uint32_t(node[1]) << 16) | (std::uint32_t(node[2]) << 8) | node[3];
    const std::string name =
        nameOffset < stringsSize ? decode_name(strings + nameOffset, stringsSize - nameOffset) : "<invalid>";
    const NodNodeKind kind = node[0] == 1 ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE;
    index = callback(static_cast<uint32_t>(index), kind, name.c_str(), be32(node + 8), user_data);
  }
}

}  // extern "C"
