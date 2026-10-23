// borealis::crash on the Vita. Upstream's handler (src/crash.cpp) installs POSIX
// signal handlers and walks the stack with dladdr/backtrace, none of which the Vita
// userland provides. A crashing Vita process is stopped by the system, which writes
// a core dump (ux0:data/*.psp2dmp); the receipts in runtime.jsonl mark the last
// phase reached. So install() records that no in-process handler exists, once,
// instead of pretending to have one.
#include "borealis/crash.hpp"
#include "borealis/log.hpp"

namespace borealis::crash {
namespace {
constexpr Log Log{"borealis::crash"};
}

void install() {
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log.info("No in-process crash handler on the Vita; the system core dump (ux0:data/*.psp2dmp) "
                 "and runtime.jsonl receipts cover crashes");
    }
}

}  // namespace borealis::crash
