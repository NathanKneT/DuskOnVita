// Aurora copy_stream before/after patches/aurora/0005 (4 KiB chunks on the Vita):
// the real functions (tools/copy_stream_test_fixture.py) against recorded SDL
// stream boundaries. Same output, success and stream status in 225 cases.
// Build with -DTARGET_VITA=1 so the candidate is the Vita variant.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

struct SDL_IOStream;
enum SDL_IOStatus { SDL_IO_STATUS_READY, SDL_IO_STATUS_EOF, SDL_IO_STATUS_ERROR };
constexpr size_t Unlimited = std::numeric_limits<size_t>::max();
struct SDL_IOStream {
    std::vector<uint8_t> bytes;
    size_t cursor = 0, chunk = Unlimited, failAfter = Unlimited;
    SDL_IOStatus status = SDL_IO_STATUS_READY;
};
size_t SDL_ReadIO(SDL_IOStream* s, void* to, size_t requested) {
    assert(s && to && requested);
    if (s->cursor >= s->failAfter) { s->status = SDL_IO_STATUS_ERROR; return 0; }
    if (s->cursor == s->bytes.size()) { s->status = SDL_IO_STATUS_EOF; return 0; }
    const auto n = std::min({requested, s->chunk, s->bytes.size() - s->cursor, s->failAfter - s->cursor});
    assert(n);
    std::memcpy(to, s->bytes.data() + s->cursor, n);
    s->cursor += n;
    return n;
}
size_t SDL_WriteIO(SDL_IOStream* s, const void* from, size_t requested) {
    assert(s && from && requested);
    if (s->bytes.size() >= s->failAfter) { s->status = SDL_IO_STATUS_ERROR; return 0; }
    const auto n = std::min({requested, s->chunk, s->failAfter - s->bytes.size()});
    assert(n);
    const auto* p = static_cast<const uint8_t*>(from);
    s->bytes.insert(s->bytes.end(), p, p + n);
    return n;
}
SDL_IOStatus SDL_GetIOStatus(SDL_IOStream* s) { return s->status; }
bool SDL_InvalidParamError(const char*) { return false; }

namespace original {
#include "original.inc"
}
namespace candidate {
#include "candidate.inc"
}

namespace {
using Copy = bool (*)(SDL_IOStream*, SDL_IOStream*) noexcept;
struct Result { bool success; std::vector<uint8_t> output; SDL_IOStatus sourceStatus, destStatus; };
Result copy(Copy fn, const std::vector<uint8_t>& bytes, size_t readChunk, size_t writeChunk,
            size_t readFailure, size_t writeFailure) {
    SDL_IOStream from{bytes, 0, readChunk, readFailure, SDL_IO_STATUS_READY};
    SDL_IOStream to{{}, 0, writeChunk, writeFailure, SDL_IO_STATUS_READY};
    const bool success = fn(&from, &to);
    return {success, std::move(to.bytes), from.status, to.status};
}
}

int main() {
    const std::array<size_t, 9> sizes{{0, 1, 4095, 4096, 4097, 32768, 65536, 65537, 200000}};
    const std::array<size_t, 3> reads{{Unlimited, 37, 4096}}, writes{{Unlimited, 13, 4096}};
    unsigned cases = 0;
    for (const auto size : sizes) {
        std::vector<uint8_t> bytes(size);
        for (size_t i = 0; i < size; ++i) bytes[i] = uint8_t(i * 37 + i / 17);
        for (const auto readChunk : reads)
            for (const auto writeChunk : writes)
                for (unsigned failure = 0; failure < 3; ++failure) {
                    if (failure && size == 0) continue;
                    const size_t cutoff = size ? size / 2 : 0;
                    const size_t readFailure = failure == 1 ? cutoff : Unlimited;
                    const size_t writeFailure = failure == 2 ? cutoff : Unlimited;
                    const auto a = copy(original::copy_stream, bytes, readChunk, writeChunk, readFailure, writeFailure);
                    const auto b = copy(candidate::copy_stream, bytes, readChunk, writeChunk, readFailure, writeFailure);
                    assert(a.success == b.success && a.output == b.output);
                    assert(a.sourceStatus == b.sourceStatus && a.destStatus == b.destStatus);
                    assert(b.success == (failure == 0));
                    if (!failure) assert(b.output == bytes);
                    else assert(b.output == std::vector<uint8_t>(bytes.begin(), bytes.begin() + cutoff));
                    ++cases;
                }
    }
    assert(cases == 225);
    std::puts("copy_stream_test: patch 0005 (4 KiB Vita chunks) matches the original in 225 stream cases");
}
