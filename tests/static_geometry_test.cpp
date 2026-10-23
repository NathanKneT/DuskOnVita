// T3: static-list certificates (gfx_vgl/static_geometry.hpp) and the geometry cache's use of them
// (gfx_vgl/geometry_cache.hpp): skip the byte comparison only for the same list ids, array origins
// and dynamic-array generation; verify mode still compares; anything else compares as before.
#include "gfx_vgl/static_geometry.hpp"
#include "runtime/renderer_options.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

using namespace twv::vgl;

namespace {
struct Dynamic {
    std::vector<const void*> named;
    uint32_t gen = 1;
    unsigned queries = 0;
    bool operator()(const void* array, uint32_t& answered) {
        ++queries;
        answered = gen;
        for (const void* n : named) if (n == array) return true;
        return false;
    }
};

void certify_cases() {
    std::vector<uint8_t> list(256), other(64), pos(96), nrm(96);
    StaticGeometryState s;
    Dynamic dyn;
    std::array<const void*, 12> arrays{};
    arrays[0] = pos.data();
    arrays[1] = nrm.data();
    const uint32_t mask = 0b11;
    // Off: nothing is certified.
    s.enter(list.data(), uint32_t(list.size()), 7);
    s.array_base(0); s.array_base(1);
    assert(!s.certify(list.data() + 32, 48, mask, arrays, dyn.gen, dyn).valid());
    s.mode = StaticGeometryMode::On;
    // Raw data outside the current list, or straddling its end.
    assert(!s.certify(other.data(), 48, mask, arrays, dyn.gen, dyn).valid());
    assert(!s.certify(list.data() + 240, 48, mask, arrays, dyn.gen, dyn).valid());
    assert(s.noList == 2);
    // Inside the list, both arrays loaded by list 7, none dynamic.
    auto cert = s.certify(list.data() + 32, 48, mask, arrays, dyn.gen, dyn);
    assert(cert.valid() && cert.list == 7 && cert.gen == 1 && cert.arrays[0] == 7 && cert.arrays[1] == 7);
    assert(cert.arrays[2] == 0 && dyn.queries == 2);
    // Same arrays at the same generation: the memo answers, no new query.
    assert(s.certify(list.data() + 64, 16, mask, arrays, dyn.gen, dyn).same(
        StaticCert{7, 1, cert.arrays}));
    assert(dyn.queries == 2);
    // An array base loaded outside a list breaks the certificate for packets that use it.
    s.enter(nullptr, 0, 0);
    s.array_base(1);
    s.enter(list.data(), uint32_t(list.size()), 7);
    assert(!s.certify(list.data() + 32, 48, mask, arrays, dyn.gen, dyn).valid() && s.noArrays == 1);
    assert(s.certify(list.data() + 32, 48, 0b01, arrays, dyn.gen, dyn).valid());  // unused attribute
    s.array_base(1);
    // An array named dynamic moves the generation on: queried again, then refused.
    dyn.named.push_back(nrm.data());
    dyn.gen = 2;
    assert(!s.certify(list.data() + 32, 48, mask, arrays, dyn.gen, dyn).valid());
    assert(s.dynamicArrays == 1 && dyn.queries == 4);
    // Another list id is another certificate.
    dyn.named.clear(); dyn.gen = 3;
    s.enter(list.data(), uint32_t(list.size()), 8);
    s.array_base(0); s.array_base(1);
    const auto eight = s.certify(list.data() + 32, 48, mask, arrays, dyn.gen, dyn);
    assert(eight.valid() && eight.list == 8 && eight.gen == 3 && !eight.same(cert));
}

void cache_cases() {
    GeometryCacheImpl<4> cache;
    std::vector<uint8_t> raw(64, 0x11), array(128, 0x22), vertices(256, 0x33);
    const SourceSpan spans[2] = {{raw.data(), raw.size()}, {array.data(), array.size()}};
    GeometryKey key{};
    key.pipeline = 1; key.vertexCount = 8;
    StaticCert cert;
    cert.list = 5; cert.gen = 9; cert.arrays[0] = 4;
    assert(!cache.lookup(1, key, cert).data);
    assert(cache.store(1, key, spans, 2, vertices.data(), vertices.size(), 1, {}, cert));
    assert(cache.stats.staticStores == 1);
    // Certified hit: no comparison at all.
    auto hit = cache.lookup(1, key, cert);
    assert(hit.data && hit.bytes == vertices.size() && std::memcmp(hit.data, vertices.data(), vertices.size()) == 0);
    assert(cache.stats.staticHits == 1 && cache.stats.validationSpans == 0);
    // The certificate is trusted: changed bytes under the same certificate still hit (the
    // invalidation discipline of patches/aurora/0006 is what guarantees they cannot change).
    array[5] ^= 1;
    assert(cache.lookup(1, key, cert).data && cache.stats.staticHits == 2);
    // Verify mode compares and catches it.
    assert(!cache.lookup(1, key, cert, true).data);
    assert(cache.stats.staticMismatches == 1 && cache.stats.invalidations == 1);
    array[5] ^= 1;
    cache.end_frame();  // past the invalidation cooldown below
    for (unsigned i = 0; i < 31; ++i) cache.end_frame();
    assert(!cache.lookup(1, key, cert).data);
    assert(cache.store(1, key, spans, 2, vertices.data(), vertices.size(), 1, {}, cert));
    // Verify mode, unchanged bytes: compared, counted as verified.
    const auto spansBefore = cache.stats.validationSpans;
    assert(cache.lookup(1, key, cert, true).data);
    assert(cache.stats.staticVerified == 1 && cache.stats.validationSpans == spansBefore + 2);
    // A newer generation compares; equal bytes re-certify at that generation.
    StaticCert later = cert;
    later.gen = 10;
    assert(cache.lookup(1, key, later).data && cache.stats.staticCertified == 1);
    assert(cache.stats.validationSpans == spansBefore + 4);
    assert(cache.lookup(1, key, later).data && cache.stats.staticHits == 3);
    assert(cache.stats.validationSpans == spansBefore + 4);
    // Outside a list (no certificate): compares, and drops the stored certificate.
    assert(cache.lookup(1, key, StaticCert{}).data);
    assert(cache.stats.validationSpans == spansBefore + 6);
    assert(cache.lookup(1, key, later).data && cache.stats.staticCertified == 2);
    // A different array origin is a different certificate: compare, then changed bytes miss.
    StaticCert moved = later;
    moved.arrays[0] = 6;
    raw[0] ^= 1;
    assert(!cache.lookup(1, key, moved).data && cache.stats.staticMismatches == 1);
    // Without certificates the cache behaves as before.
    GeometryCacheImpl<4> plain;
    raw[0] ^= 1;
    assert(plain.store(2, key, spans, 2, vertices.data(), vertices.size()));
    assert(plain.lookup(2, key).data && plain.stats.staticHits == 0 && plain.stats.validationSpans == 2);
}

void option_cases() {
    using twv::vita::RendererOptions;
    {
        RendererOptions o;
        assert(o.consume("--vita-static-geometry=verify") == 16 && o.staticGeometry == 2 && o.staticGeometrySelected);
        assert(o.consume("--vita-static-geometry=on") == -1);
    }
    for (const auto& [token, value] : {std::pair{"--vita-static-geometry=off", 0}, std::pair{"--vita-static-geometry=on", 1}}) {
        RendererOptions o;
        assert(o.consume(token) == 16 && o.staticGeometry == value);
    }
    for (const char* bad : {"--vita-static-geometry", "--vita-static-geometry=", "--vita-static-geometry=ON",
                            "--vita-static-geometry=1", "--vita-static-geometry=verify2"}) {
        RendererOptions o;
        assert(o.consume(bad) == -1 && !o.staticGeometrySelected);
    }
    RendererOptions o;
    assert(o.consume("--vita-static-geometry-extra=on") == 0);
}
}

int main() {
    certify_cases();
    cache_cases();
    option_cases();
    std::puts("static_geometry_test: certificates need the list, array origins and generation; certified hits skip only the comparison; verify mode catches changed bytes; option parsing passed");
}
