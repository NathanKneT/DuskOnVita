// Host test for platform/vita/disc/gcm_nod.cpp against a synthetic GameCube image
// built in memory (no game data). Checks the nod C API behavior Aurora's DVD code
// and Borealis' disc inspection rely on: header and size, partition metadata blob
// sizes (as nod reads them), FST iteration order and skips, file reads and seeks,
// and the error paths (directories, out-of-range indices, container formats, Wii).
//
//   tools/host_tests.sh   (builds with the host compiler and runs)
#include <nod.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int gFailures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++gFailures;                                                                  \
    }                                                                               \
  } while (0)

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  b[at] = uint8_t(v >> 24), b[at + 1] = uint8_t(v >> 16), b[at + 2] = uint8_t(v >> 8), b[at + 3] = uint8_t(v);
}

struct Memory {
  std::vector<uint8_t> bytes;
  int closes = 0;
};
int64_t read_at(void* user, uint64_t offset, void* out, size_t len) {
  auto* m = static_cast<Memory*>(user);
  if (offset >= m->bytes.size()) return 0;
  const size_t n = std::min<size_t>(len, m->bytes.size() - offset);
  std::memcpy(out, m->bytes.data() + offset, n);
  return int64_t(n);
}
int64_t stream_len(void* user) { return int64_t(static_cast<Memory*>(user)->bytes.size()); }
void close_stream(void* user) { ++static_cast<Memory*>(user)->closes; }

// Root, "res/" (holding a.bin and "sub/" with c.bin), b.bin, empty "e/".
std::vector<uint8_t> make_image() {
  std::vector<uint8_t> img(0x8000, 0);
  std::memcpy(img.data(), "GZ2E01", 6);
  img[6] = 0;    // disc number
  img[7] = 0;    // version
  img[8] = 1;    // audio streaming
  const uint8_t gcn[4]{0xC2, 0x33, 0x9F, 0x3D};
  std::memcpy(img.data() + 0x1C, gcn, 4);
  std::memcpy(img.data() + 0x20, "Synthetic Test Disc", 19);
  // Apploader at 0x2440: size 0x10, trailer 0x8.
  put32(img, 0x2440 + 0x14, 0x10);
  put32(img, 0x2440 + 0x18, 0x8);
  // DOL at 0x3000: text0 at +0x100 size 0x40, data0 at +0x140 size 0x20 -> 0x160 bytes.
  put32(img, 0x3000 + 0x00, 0x100);
  put32(img, 0x3000 + 0x90, 0x40);
  put32(img, 0x3000 + 0x1C, 0x140);
  put32(img, 0x3000 + 0x90 + 7 * 4, 0x20);
  img[0x3000 + 0x15F] = 0xAB;
  // FST at 0x4000.
  const char strings[] = "\0res\0a.bin\0sub\0c.bin\0b.bin\0e";
  struct N { uint8_t kind; uint32_t name, off, len; };
  const N nodes[] = {
      {1, 0, 0, 7},           // 0 root, 7 nodes
      {1, 1, 0, 5},           // 1 res/ (children end at 5)
      {0, 5, 0x5000, 16},     // 2 res/a.bin
      {1, 11, 1, 5},          // 3 res/sub/
      {0, 15, 0x5100, 3},     // 4 res/sub/c.bin
      {0, 21, 0x5200, 0x1000},// 5 b.bin
      {1, 27, 0, 7},          // 6 e/ (empty)
  };
  size_t at = 0x4000;
  for (const auto& n : nodes) {
    img[at] = n.kind;
    img[at + 1] = uint8_t(n.name >> 16), img[at + 2] = uint8_t(n.name >> 8), img[at + 3] = uint8_t(n.name);
    put32(img, at + 4, n.off);
    put32(img, at + 8, n.len);
    at += 12;
  }
  std::memcpy(img.data() + at, strings, sizeof strings);
  const uint32_t fstSize = uint32_t(at + sizeof strings - 0x4000);
  put32(img, 0x420, 0x3000);  // dol offset
  put32(img, 0x424, 0x4000);  // fst offset
  put32(img, 0x428, fstSize); // fst size
  for (int i = 0; i < 16; ++i) img[0x5000 + i] = uint8_t(i);
  std::memcpy(img.data() + 0x5100, "abc", 3);
  for (int i = 0; i < 0x1000; ++i) img[0x5200 + i] = uint8_t(i * 7);
  return img;
}

struct Visit { uint32_t index; NodNodeKind kind; std::string name; uint32_t size; };
struct Walk { std::vector<Visit> seen; bool skipRes = false; };
uint32_t visit(uint32_t index, NodNodeKind kind, const char* name, uint32_t size, void* user) {
  auto* w = static_cast<Walk*>(user);
  w->seen.push_back({index, kind, name, size});
  if (w->skipRes && kind == NOD_NODE_KIND_DIRECTORY && std::string(name) == "res") return size;
  return index + 1;
}

NodHandle* open(Memory& m) {
  const NodDiscStream s{&m, read_at, stream_len, close_stream};
  const NodDiscOptions o{.partition_encryption = NOD_PARTITION_ENCRYPTION_ORIGINAL, .preloader_threads = 1};
  NodHandle* h = nullptr;
  return nod_disc_open_stream(&s, &o, &h) == NOD_RESULT_OK ? h : nullptr;
}
}  // namespace

int main() {
  Memory m{make_image()};
  NodHandle* disc = open(m);
  CHECK(disc != nullptr);
  NodDiscHeader header{};
  CHECK(nod_disc_header(disc, &header) == NOD_RESULT_OK);
  CHECK(std::memcmp(header.game_id, "GZ2E01", 6) == 0);
  CHECK(header.audio_streaming == 1);
  CHECK(std::string(header.game_title) == "Synthetic Test Disc");
  CHECK(nod_disc_size(disc) == m.bytes.size());

  // Raw disc reads (Borealis' full-image hash path) see the image bytes in order.
  uint8_t raw[8]{};
  CHECK(nod_read(disc, raw, sizeof raw) == 8 && std::memcmp(raw, "GZ2E01", 6) == 0);

  NodHandle* part = nullptr;
  CHECK(nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_UPDATE, nullptr, &part) == NOD_RESULT_ERR_NOT_FOUND);
  CHECK(nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, nullptr, &part) == NOD_RESULT_OK);
  NodPartitionMeta meta{};
  CHECK(nod_partition_meta(part, &meta) == NOD_RESULT_OK);
  CHECK(meta.raw_boot.size == 0x440);
  CHECK(meta.raw_bi2.size == 0x2000);
  CHECK(meta.raw_apploader.size == 0x20 + 0x10 + 0x8);
  CHECK(meta.raw_dol.size == 0x160 && meta.raw_dol.data[0x15F] == 0xAB);
  CHECK(meta.raw_fst.size > 7 * 12);
  CHECK(meta.raw_ticket.data == nullptr && meta.raw_tmd.size == 0);

  Walk all;
  nod_partition_iterate_fst(part, visit, &all);
  CHECK(all.seen.size() == 6);
  if (all.seen.size() == 6) {
    CHECK(all.seen[0].index == 1 && all.seen[0].kind == NOD_NODE_KIND_DIRECTORY && all.seen[0].name == "res" &&
          all.seen[0].size == 5);
    CHECK(all.seen[1].name == "a.bin" && all.seen[1].size == 16 && all.seen[1].kind == NOD_NODE_KIND_FILE);
    CHECK(all.seen[2].name == "sub" && all.seen[2].size == 5);
    CHECK(all.seen[3].name == "c.bin" && all.seen[3].size == 3);
    CHECK(all.seen[4].name == "b.bin" && all.seen[4].index == 5);
    CHECK(all.seen[5].name == "e" && all.seen[5].size == 7);
  }
  Walk skipped{.seen = {}, .skipRes = true};
  nod_partition_iterate_fst(part, visit, &skipped);
  CHECK(skipped.seen.size() == 3 && skipped.seen[1].name == "b.bin");

  NodHandle* file = nullptr;
  CHECK(nod_partition_open_file(part, 1, &file) == NOD_RESULT_ERR_FORMAT);  // directory
  CHECK(nod_partition_open_file(part, 99, &file) == NOD_RESULT_ERR_NOT_FOUND);
  CHECK(nod_error_message() != nullptr);
  CHECK(nod_partition_open_file(part, 2, &file) == NOD_RESULT_OK);
  uint8_t buf[32]{};
  CHECK(nod_read(file, buf, sizeof buf) == 16 && buf[15] == 15);  // clamped to the file
  CHECK(nod_read(file, buf, sizeof buf) == 0);                     // end of file
  CHECK(nod_seek(file, 4, 0) == 4 && nod_read(file, buf, 2) == 2 && buf[0] == 4 && buf[1] == 5);
  CHECK(nod_seek(file, -3, 2) == 13 && nod_read(file, buf, 8) == 3 && buf[0] == 13);
  CHECK(nod_seek(file, 1, 1) == 17 && nod_read(file, buf, 1) == 0);  // past the end reads nothing
  CHECK(nod_seek(file, -20, 1) == -1 && nod_seek(file, 0, 7) == -1);
  nod_free(file);
  CHECK(nod_partition_open_file(part, 5, &file) == NOD_RESULT_OK);
  std::vector<uint8_t> big(0x1000);
  CHECK(nod_read(file, big.data(), big.size()) == 0x1000 && big[0xFFF] == uint8_t(0xFFF * 7));
  // Freeing the disc and partition first keeps the stream alive for open files (nod's Arc).
  nod_free(part);
  nod_free(disc);
  CHECK(m.closes == 0);
  CHECK(nod_seek(file, 0, 0) == 0 && nod_read(file, big.data(), 4) == 4 && big[1] == 7);
  nod_free(file);
  CHECK(m.closes == 1);

  // Container formats and Wii discs.
  Memory ciso{make_image()};
  std::memcpy(ciso.bytes.data(), "CISO", 4);
  CHECK(open(ciso) == nullptr && std::string(nod_error_message()).find("CISO") != std::string::npos);
  CHECK(ciso.closes == 1);
  Memory wii{make_image()};
  std::memset(wii.bytes.data() + 0x1C, 0, 4);
  const uint8_t wiiMagic[4]{0x5D, 0x1C, 0x9E, 0xA3};
  std::memcpy(wii.bytes.data() + 0x18, wiiMagic, 4);
  NodHandle* wiiDisc = open(wii);
  CHECK(wiiDisc != nullptr);
  CHECK(nod_disc_open_partition_kind(wiiDisc, NOD_PARTITION_KIND_DATA, nullptr, &part) == NOD_RESULT_ERR_FORMAT);
  nod_free(wiiDisc);
  Memory junk{std::vector<uint8_t>(0x8000, 0)};
  CHECK(open(junk) == nullptr);

  if (gFailures == 0) {
    std::printf("gcm_nod_test: all checks passed\n");
  }
  return gFailures == 0 ? 0 : 1;
}
