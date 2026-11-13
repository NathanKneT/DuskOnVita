// Breadcrumbs for the two ways the process ends without a trap stub or a fatal log
// line: std::terminate (an uncaught exception, or one leaving a noexcept function)
// and abort() (newlib assert, libstdc++ internals, any direct call). Both write a
// receipt and flush before the process stops. abort() is wrapped at link time
// (-Wl,--wrap=abort, VitaApp.cmake) so calls from prebuilt libraries are covered too.
// The caller address maps to a function with arm-vita-eabi-addr2line on the ELF.
#include "telemetry.hpp"

#include <psp2/kernel/clib.h>

#include <cstdio>
#include <exception>
#include <typeinfo>

extern "C" [[noreturn]] void __real_abort(void);

// Weak: the probes link the runtime without Borealis (then the address is null).
namespace borealis::log {
[[gnu::weak]] void flush(); // borealis/log.hpp; plain signature, no enum crosses
}

extern "C" [[noreturn]] void __wrap_abort(void) {
    char text[64];
    std::snprintf(text, sizeof text, "caller=0x%08lx",
                  static_cast<unsigned long>(reinterpret_cast<unsigned long>(__builtin_return_address(0))));
    sceClibPrintf("twilight-vita: abort() %s\n", text);
    twv::vita::telemetryInit();
    twv::vita::telemetryText("abort", text);
    twv::vita::telemetrySync();
    if (&borealis::log::flush != nullptr) borealis::log::flush(); // after the receipt: the log may be what failed
    __real_abort();
}

// The reason the game gives when it stops itself (Log.fatal; patches/dusklight/0074 hands the text
// over just before the abort above). The log file may never reach the card with that line in it.
extern "C" void twv_vita_fatal_text(const char* text, unsigned size) {
    char line[480];
    std::snprintf(line, sizeof line, "%.*s", static_cast<int>(size < 470 ? size : 470), text ? text : "");
    sceClibPrintf("twilight-vita: fatal: %s\n", line);
    twv::vita::telemetryInit();
    twv::vita::telemetryText("fatal", line);
    twv::vita::telemetrySync();
}

// The game's error lines (patches/dusklight/0074): the first ones of a run are kept as receipts.
// A stop is preceded by a few of them; a run that logs errors all the time must not fill the file.
extern "C" void twv_vita_error_text(const char* text, unsigned size) {
    static unsigned kept = 0;
    if (kept >= 96 || !text) return;
    ++kept;
    char line[480];
    std::snprintf(line, sizeof line, "%.*s", static_cast<int>(size < 470 ? size : 470), text);
    twv::vita::telemetryText("game_error", line);
}

namespace twv::vita {
namespace {
[[noreturn]] void onTerminate() {
    char text[224] = "no active exception";
    if (const std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            std::snprintf(text, sizeof text, "%s: %s", typeid(e).name(), e.what());
        } catch (...) {
            std::snprintf(text, sizeof text, "non-std exception");
        }
    }
    sceClibPrintf("twilight-vita: std::terminate: %s\n", text);
    telemetryInit();
    telemetryText("terminate", text);
    telemetrySync();
    if (&borealis::log::flush != nullptr) borealis::log::flush();
    __real_abort();
}
}  // namespace

void installFaultReceipts() { std::set_terminate(onTerminate); }

}  // namespace twv::vita
