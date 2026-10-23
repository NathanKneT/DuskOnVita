#pragma once
#include <cstring>

namespace twv::vita {
enum class ClockMode { System, Legacy444 };

// Startup-only port option. Unrelated tokens remain the original game's CLI.
// Default: the standard application maximum (444/222/222/166 through scePower,
// no plugin). --vita-clocks=system keeps whatever the system set, as a control.
struct ClockOptions {
    ClockMode mode = ClockMode::Legacy444;
    bool selected = false;

    // 0: original CLI; 1: consumed; -1: invalid or repeated clock option.
    int consume(const char* token) noexcept {
        if (!token) return -1;
        constexpr const char* name = "--vita-clocks";
        constexpr auto length = sizeof("--vita-clocks") - 1;
        if (std::strncmp(token, name, length) != 0 ||
            (token[length] && token[length] != '=')) return 0;
        if (token[length] != '=' || selected) return -1;
        const char* value = token + length + 1;
        if (std::strcmp(value, "system") == 0) mode = ClockMode::System;
        else if (std::strcmp(value, "legacy444") == 0) mode = ClockMode::Legacy444;
        else return -1;
        selected = true;
        return 1;
    }
};

inline const char* clockModeName(ClockMode mode) noexcept {
    return mode == ClockMode::Legacy444 ? "legacy444" : "system";
}

// The pinned VitaSDK power.h getters and setters return signed int. Keep their
// values intact, including errors; a request is never substituted for a read.
struct ClockValues {
    int arm, bus, gpu, gpuXbar;
};
inline constexpr ClockValues Legacy444Requests{444, 222, 222, 166};

struct ClockNative {
    int (*getArm)();
    int (*getBus)();
    int (*getGpu)();
    int (*getGpuXbar)();
    int (*setArm)(int);
    int (*setBus)(int);
    int (*setGpu)(int);
    int (*setGpuXbar)(int);
};

struct ClockReceipt {
    ClockValues initial;
    ClockValues observed;
    bool settersAttempted = false;
    // Meaningful only when settersAttempted; system has no setter results.
    ClockValues setterResults{};
};

inline ClockValues readClocks(const ClockNative& native) {
    return {native.getArm(), native.getBus(), native.getGpu(), native.getGpuXbar()};
}

// Call once after every port-owned startup option is validated. All four
// initial reads precede any mutation. System never touches a setter callback;
// legacy attempts each historical request once, then reads every clock again.
// No retry, alternate frequency, or synthetic success is introduced on error.
inline ClockReceipt applyClockPolicy(ClockMode mode, const ClockNative& native) {
    ClockReceipt receipt{readClocks(native), {}, false, {}};
    if (mode == ClockMode::Legacy444) {
        receipt.settersAttempted = true;
        receipt.setterResults = {
            native.setArm(Legacy444Requests.arm),
            native.setBus(Legacy444Requests.bus),
            native.setGpu(Legacy444Requests.gpu),
            native.setGpuXbar(Legacy444Requests.gpuXbar),
        };
    }
    receipt.observed = readClocks(native);
    return receipt;
}
} // namespace twv::vita
