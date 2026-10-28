// runtime/painter_profile_stats.hpp: section names, sampling, keyed tables, top-N choice, receipt text.
#include "runtime/painter_profile_stats.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace twv::vita::paintprof;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

int main() {
    CHECK(std::strcmp(SectionNames[SecCopy2D], "copy2d") == 0);
    CHECK(std::strcmp(SectionNames[SecPaNormal], "pa_normal") == 0);
    CHECK(std::strcmp(SectionNames[Sec2DXlu], "2d_xlu") == 0);
    CHECK(std::strcmp(SectionNames[SecPa2DmenuFore], "pa_2dmenu_fore") == 0);
    CHECK(std::strcmp(SectionNames[SecOther], "other") == 0);

    unsigned sampled_frames = 0;
    for (unsigned f = 1; f <= 80; ++f) {
        sampled_frames += sampled(f);
        CHECK(!sampled(f) || (f % 8 != 1 && f % 8 != 5));  // never an actor or a J3D frame
    }
    CHECK(sampled_frames == 20 && sampled(3) && sampled(7) && !sampled(4));

    static KeyTable<8> t;
    Entry& a = t.at(33060);
    a.us += 50; a.calls += 1; a.items += 40;
    CHECK(&t.at(33060) == &a);
    t.at(16).us += 70; t.at(16).calls += 2;
    t.at(17).us += 70; t.at(17).calls += 1;   // tie with 16: lower slot first
    t.at(900).calls += 1;                     // 0 us
    t.at(901);                                // seen, never drawn: not listed
    for (uintptr_t k = 2000; k < 2010; ++k) t.at(k).calls += 1;  // fills the table, the rest overflow
    CHECK(t.overflow.calls > 0);
    unsigned top[3];
    CHECK(t.top(top, 3) == 3);
    CHECK(t.slots[top[0]].us == 70 && t.slots[top[1]].us == 70 && t.slots[top[2]].key == 33060);
    CHECK(top[0] < top[1]);
    uint64_t us; uint32_t calls, items;
    t.total(us, calls, items);
    CHECK(us == 190 && calls == 15 && items == 40);
    t.clear();
    CHECK(t.top(top, 3) == 0 && !t.overflow.calls);

    char text[200];
    part_text(text, sizeof text, SectionNames[SecXlu], 1234, 8);
    CHECK(std::strcmp(text, "n=xlu;us=1234;c=8") == 0);
    Entry e; e.us = 900; e.calls = 3; e.items = 120;
    jpa_text(text, sizeof text, 33060, e);
    CHECK(std::strcmp(text, "id=33060;us=900;c=3;p=120") == 0);
    class_text(text, sizeof text, "13dMeter2Draw_c", e);
    CHECK(std::strcmp(text, "n=13dMeter2Draw_c;us=900;c=3") == 0);
    class_text(text, sizeof text, "a;b=c", e);
    CHECK(std::strcmp(text, "n=a_b_c;us=900;c=3") == 0);
    class_text(text, sizeof text, nullptr, e);
    CHECK(std::strcmp(text, "n=?;us=900;c=3") == 0);
    std::puts("painter_profile_stats_test: ok");
    return 0;
}
