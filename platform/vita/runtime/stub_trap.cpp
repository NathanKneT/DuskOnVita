// Target of every generated link stub (tools/gen_stubs.py). A stub is a symbol
// the Vita build does not implement yet; reaching one is a hard stop with its
// name in runtime.jsonl and the debug console, never a silent no-op, so missing
// behavior fails visibly instead of being approximated.
#include "telemetry.hpp"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

// Weak: the probes link the runtime without Borealis (then the address is null).
namespace borealis::log {
[[gnu::weak]] void flush(); // borealis/log.hpp; plain signature, no enum crosses
}

extern "C" [[noreturn]] void twv_stub_trap(const char* symbol) {
    sceClibPrintf("twilight-vita: unimplemented symbol reached: %s\n", symbol);
    // A stub can fire from a global constructor, before main() started the receipts.
    twv::vita::telemetryInit();
    twv::vita::telemetryText("stub_trap", symbol);
    twv::vita::telemetryFlush();
    // After the receipt: Dusklight's buffered log lines up to the stop point.
    if (&borealis::log::flush != nullptr) borealis::log::flush();
    sceKernelExitProcess(0);
    __builtin_trap();
}
