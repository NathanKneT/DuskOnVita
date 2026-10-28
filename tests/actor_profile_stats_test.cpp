// runtime/actor_profile_stats.hpp: type slots, distance bins, top-N choice, receipt text, sampling.
#include "runtime/actor_profile_stats.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace twv::vita::actprof;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

int main() {
    CHECK(type_slot(0) == 0 && type_slot(254) == 254 && type_slot(1022) == 1022);
    CHECK(type_slot(-1) == TypeSlots - 1 && type_slot(1023) == TypeSlots - 1 && type_slot(30000) == TypeSlots - 1);

    CHECK(distance_bin(0.0f) == 0 && distance_bin(999.9f) == 0 && distance_bin(1000.0f) == 1);
    CHECK(distance_bin(2999.0f) == 1 && distance_bin(3000.0f) == 2 && distance_bin(5999.0f) == 2);
    CHECK(distance_bin(6000.0f) == 3 && distance_bin(1.0e9f) == 3);

    static TypeStats types[TypeSlots];
    unsigned top[3];
    CHECK(top_types(types, TypeSlots, top, 3) == 0);
    types[5].execCalls = 1; types[5].execUs = 10;
    types[9].drew = 1; types[9].drawUs = 50;
    types[2].execCalls = 2; types[2].execUs = 30; types[2].drawUs = 20;  // 50: tie with 9, lower slot first
    types[700].culledFrustum = 3;                                          // seen, 0 us
    types[40].skipped = 1; types[40].execUs = 40;
    CHECK(top_types(types, TypeSlots, top, 3) == 3);
    CHECK(top[0] == 2 && top[1] == 9 && top[2] == 40);
    unsigned all[8];
    CHECK(top_types(types, TypeSlots, all, 8) == 5);
    CHECK(all[3] == 5 && all[4] == 700);

    char text[240];
    TypeStats t; t.execUs = 1200; t.drawUs = 340; t.execCalls = 8; t.drew = 6; t.culledFrustum = 1;
    t.culledFar = 1; t.actor = true;
    type_text(text, sizeof text, "Npc_ne", 254, t);
    CHECK(std::strcmp(text, "n=Npc_ne;p=254;a=1;ex=1200;dw=340;ce=8;cd=6;cf=1;cr=1;cs=0") == 0);
    PartStats s; s.us[PartCalc] = 900; s.calls[PartCalc] = 30; s.us[PartView] = 100; s.calls[PartView] = 30;
    part_text(text, sizeof text, WhereNames[WhereDrawNonfinal], s);
    CHECK(std::strcmp(text, "w=draw_nonfinal;us=900,0,0,0,100;n=30,0,0,0,30") == 0);
    PartStats sum; sum.add(s); sum.add(s);
    CHECK(sum.us[PartCalc] == 1800 && sum.calls[PartView] == 60);

    unsigned actors = 0, j3d = 0;
    for (unsigned f = 1; f <= 80; ++f) {
        actors += frame_mode(f) == ModeActors;
        j3d += frame_mode(f) == ModeJ3D;
    }
    CHECK(actors == 10 && j3d == 10 && frame_mode(1) == ModeActors && frame_mode(5) == ModeJ3D);
    std::puts("actor_profile_stats_test: ok");
    return 0;
}
