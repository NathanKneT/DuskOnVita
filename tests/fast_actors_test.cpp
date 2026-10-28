// runtime/fast_actors.{hpp,cpp}: the --vita-fast-<name> options, per-switch mode selection and the
// counters the game patches report into.
#include "runtime/fast_actors.hpp"

#include <cassert>
#include <cstdio>
#include <map>
#include <string>

namespace twv::vita {
std::map<std::string, long long> events;
void telemetryEvent(const char* name, long long value) { events[name] = value; }
void telemetryText(const char*, const char*) {}
}

#include "runtime/fast_actors.cpp"

int main() {
    using namespace twv::vita;
    int choices[FastActorSwitchCount];
    for (int& c : choices) c = -1;
    assert(fastActorOption("--vita-fast-shadow-collect=on", choices) == 0);
    assert(fastActorOption("--vita-fast-obstaclex=on", choices) == 0);
    assert(fastActorOption("--stage", choices) == 0);
    assert(fastActorOption("--vita-fast-obstacle=verify", choices) == 1 && choices[FastObstacle] == 2);
    assert(fastActorOption("--vita-fast-obstacle=on", choices) == -1);
    for (const char* bad : {"--vita-fast-obstacle=ON", "--vita-fast-obstacle=", "--vita-fast-obstacle"}) {
        int fresh[FastActorSwitchCount];
        for (int& c : fresh) c = -1;
        assert(fastActorOption(bad, fresh) == -1);
    }
    for (int which = 0; which < FastActorSwitchCount; ++which) {
        int fresh[FastActorSwitchCount];
        for (int& c : fresh) c = -1;
        const std::string token = std::string("--vita-fast-") + FastActorSwitchNames[which] + "=on";
        assert(fastActorOption(token.c_str(), fresh) == 1 && fresh[which] == 1);
        for (int other = 0; other < FastActorSwitchCount; ++other) assert(other == which || fresh[other] == -1);
    }

    assert(twv_vita_fast_actor_mode(FastObstacle) == FastActorOff);
    assert(!twv_vita_fast_actor_select(FastActorSwitchCount, 1) && !twv_vita_fast_actor_select(-1, 1));
    assert(!twv_vita_fast_actor_select(FastObstacle, 3) && !twv_vita_fast_actor_select(FastObstacle, -1));
    assert(twv_vita_fast_actor_select(FastObstacle, FastActorVerify));
    assert(twv_vita_fast_actor_mode(FastObstacle) == FastActorVerify);
    assert(twv_vita_fast_actor_mode(FastActorSwitchCount) == 0 && twv_vita_fast_actor_mode(-1) == 0);

    // 8191 uses: no receipt yet; the 8192nd publishes the totals.
    for (unsigned i = 0; i < 8191; ++i) twv_vita_fast_actor_count(FastObstacle, int(i % 3), i % 2 ? 0 : -1);
    assert(events.empty());
    twv_vita_fast_actor_count(FastObstacle, 1, 0);
    FastActorStats s = fastActorStats(FastObstacle);
    assert(s.calls == 8192 && s.fast == 5461 && s.walks == 2730 && s.verified == 4096 && s.mismatches == 0);
    assert(events.at("fast_obstacle_calls") == 8192 && events.at("fast_obstacle_mismatches") == 0);
    // The first mismatches publish at once.
    events.clear();
    twv_vita_fast_actor_count(FastObstacle, 1, 1);
    assert(events.at("fast_obstacle_mismatches") == 1 && events.at("fast_obstacle_verified") == 4097);
    // Another switch keeps its own counts.
    twv_vita_fast_actor_count(FastBgList, 2, -1);
    assert(fastActorStats(FastBgList).calls == 1 && fastActorStats(FastBgList).walks == 1);
    for (unsigned i = 0; i < 5; ++i) twv_vita_fast_actor_count(FastObstacle, 1, 1);
    assert(fastActorStats(FastObstacle).mismatches == 6 && events.at("fast_obstacle_mismatches") == 4);
    twv_vita_fast_actor_count(FastActorSwitchCount, 1, 1);
    assert(fastActorStats(FastActorSwitchCount).calls == 0);
    // The receipt window publishes the totals of every switch that was used, none of the others.
    events.clear();
    twv_vita_fast_actor_publish();
    assert(events.at("fast_obstacle_calls") == 8198 && events.at("fast_obstacle_mismatches") == 6);
    assert(events.at("fast_bg-list_calls") == 1 && !events.count("fast_search_calls"));
    std::puts("fast_actors_test: options, per-switch modes and verify counters passed");
    return 0;
}
