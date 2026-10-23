#include "runtime/clock_policy.hpp"
#include <array>
#include <cassert>
#include <climits>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using namespace twv::vita;
std::vector<std::string> calls;
std::array<unsigned, 4> reads{};
constexpr std::array<int, 4> initial{321, -17, 111, INT_MIN};
constexpr std::array<int, 4> observed{-71, 202, -73, 144};
constexpr std::array<int, 4> setterResults{-101, INT_MIN, 2, -104};
const char* names[]{"arm", "bus", "gpu", "gpu_xbar"};

int get(unsigned axis) {
    calls.emplace_back(std::string("get_") + names[axis]);
    assert(reads[axis] < 2);
    return reads[axis]++ == 0 ? initial[axis] : observed[axis];
}
int set(unsigned axis, int value) {
    calls.emplace_back(std::string("set_") + names[axis] + "=" + std::to_string(value));
    return setterResults[axis];
}
int getArm() { return get(0); }
int getBus() { return get(1); }
int getGpu() { return get(2); }
int getXbar() { return get(3); }
int setArm(int value) { return set(0, value); }
int setBus(int value) { return set(1, value); }
int setGpu(int value) { return set(2, value); }
int setXbar(int value) { return set(3, value); }

void checkValues(const ClockValues& actual, const std::array<int, 4>& expected) {
    assert(actual.arm == expected[0] && actual.bus == expected[1]);
    assert(actual.gpu == expected[2] && actual.gpuXbar == expected[3]);
}
}

int main() {
    using namespace twv::vita;
    {
        ClockOptions defaults;
        assert(defaults.mode == ClockMode::Legacy444 && !defaults.selected);
    }
    for (const bool explicitSystem : {true}) {
        ClockOptions options;
        if (explicitSystem) assert(options.consume("--vita-clocks=system") == 1);
        assert(options.mode == ClockMode::System && options.selected == explicitSystem);
        // Null setters make any accidental system mutation fail under
        // the sanitizers instead of returning a convenient fake success.
        const ClockNative native{getArm, getBus, getGpu, getXbar, nullptr, nullptr, nullptr, nullptr};
        calls.clear(); reads = {};
        const auto receipt = applyClockPolicy(options.mode, native);
        assert(!receipt.settersAttempted);
        checkValues(receipt.initial, initial); checkValues(receipt.observed, observed);
        assert(calls == (std::vector<std::string>{"get_arm", "get_bus", "get_gpu", "get_gpu_xbar",
                                                 "get_arm", "get_bus", "get_gpu", "get_gpu_xbar"}));
    }
    ClockOptions legacy;
    assert(legacy.consume("--vita-clocks=legacy444") == 1);
    assert(legacy.mode == ClockMode::Legacy444 && legacy.selected);
    const ClockNative native{getArm, getBus, getGpu, getXbar, setArm, setBus, setGpu, setXbar};
    calls.clear(); reads = {};
    const auto receipt = applyClockPolicy(legacy.mode, native);
    assert(receipt.settersAttempted);
    checkValues(receipt.initial, initial); checkValues(receipt.observed, observed);
    checkValues(receipt.setterResults, setterResults);
    assert(calls == (std::vector<std::string>{"get_arm", "get_bus", "get_gpu", "get_gpu_xbar",
        "set_arm=444", "set_bus=222", "set_gpu=222", "set_gpu_xbar=166",
        "get_arm", "get_bus", "get_gpu", "get_gpu_xbar"}));
    assert(legacy.consume("--vita-clocks=legacy444") == -1);
    assert(legacy.consume("--vita-clocks=system") == -1);
    assert(legacy.mode == ClockMode::Legacy444);
    for (const char* token : {"--vita-clocks", "--vita-clocks=", "--vita-clocks=444",
                             "--vita-clocks=SYSTEM", "--vita-clocks=legacy444=system"}) {
        ClockOptions options;
        assert(options.consume(token) == -1);
        assert(!options.selected && options.mode == ClockMode::Legacy444);
    }
    ClockOptions options;
    assert(options.consume(nullptr) == -1);
    for (const char* token : {"", "--stage", "system", "--vita-clocks-extra=system"})
        assert(options.consume(token) == 0 && !options.selected);
    assert(std::string(clockModeName(ClockMode::System)) == "system");
    assert(std::string(clockModeName(ClockMode::Legacy444)) == "legacy444");
    std::puts("clock_policy_test: default 444 requests; system has no setters; legacy order, signed errors, real observations and strict mode selection passed");
}
