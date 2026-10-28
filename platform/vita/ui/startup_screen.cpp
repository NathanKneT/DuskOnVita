// SDK domain: start-up screens on the display (startup_screen.hpp). The CPU draws; sceDisplay shows.
#include "ui/startup_screen.hpp"

#include "runtime/telemetry.hpp"

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <cstdio>

namespace twv::vita {
namespace {

constexpr uint32_t ScreenBytes = ui::ScreenWidth * ui::ScreenHeight * 4;
constexpr uint64_t RedrawUs = 100000, GraceUs = 1500000;

int show(void* base) {
    SceDisplayFrameBuf fb{};
    fb.size = sizeof fb;
    fb.base = base;
    fb.pitch = ui::ScreenWidth;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = ui::ScreenWidth;
    fb.height = ui::ScreenHeight;
    return sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

// A display buffer of its own for error screens: CDRAM, or physically contiguous main memory when
// CDRAM is what ran out. The process ends after the screen, so it is never freed.
void* error_buffer() {
    const struct {SceKernelMemBlockType type; uint32_t align;} kinds[] = {
        {SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256u * 1024u},
        {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 1024u * 1024u}};
    for (const auto& kind : kinds) {
        const uint32_t size = (ScreenBytes + kind.align - 1) & ~(kind.align - 1);
        const SceUID block = sceKernelAllocMemBlock("twv_startup_screen", kind.type, size, nullptr);
        if (block < 0) {
            telemetryEvent("startup_screen_alloc_result", block);
            continue;
        }
        void* base = nullptr;
        if (sceKernelGetMemBlockBase(block, &base) >= 0 && base) return base;
        sceKernelFreeMemBlock(block);
    }
    return nullptr;
}

uint32_t buttons() {
    SceCtrlData pad{};
    return sceCtrlPeekBufferPositive(0, &pad, 1) > 0 ? pad.buttons : 0;
}

struct ProgressScreen {
    ui::Canvas canvas;
    bool begun = false, shown = false;
    uint32_t draws = 0;
    uint64_t drawUs = 0, lastDraw = 0;
} progress;

void timed_progress_draw(const ui::Progress& p, bool full) {
    const uint64_t start = processTimeUs();
    ui::draw_progress(progress.canvas, p, full);
    const uint64_t end = processTimeUs();
    progress.drawUs += end - start;
    progress.lastDraw = end;
    ++progress.draws;
}

ui::Progress compiling_view(uint32_t done, uint32_t total) {
    ui::Progress p;
    p.label = "Preparing graphics (first launch only)...";
    p.done = done;
    p.total = total;
    p.bar = true;
    p.note = "This takes about a minute once. Later launches skip it.";
    return p;
}

struct Noted {
    StartupProblem problem = StartupProblem::None;
    char detail[96] = {};
    uint64_t have = 0, need = 0;
} noted;

}  // namespace

void startupErrorExit(const MessageText& text, int code) {
    telemetryText("startup_screen", text.kind);
    telemetryText("startup_screen_title", text.message.title);
    void* base = error_buffer();
    if (!base) {
        telemetryText("startup_screen_unavailable", "no display buffer; exiting without a screen");
        telemetrySync();
        sceKernelExitProcess(code);
        for (;;) sceKernelDelayThread(1000000);
    }
    const ui::Canvas canvas{static_cast<uint32_t*>(base), ui::ScreenWidth, ui::ScreenHeight, ui::ScreenWidth};
    const ui::Layout layout = ui::draw_message(&canvas, text.message);
    telemetryEvent("startup_screen_fits", layout.fits);
    telemetryEvent("startup_screen_display", show(base));
    telemetrySync();
    const uint64_t shownAt = processTimeUs();
    // A button held since launch does not count, nor one pressed in the first 1.5 s.
    while (buttons() || processTimeUs() - shownAt < GraceUs) sceKernelDelayThread(16000);
    uint32_t pressed = 0;
    while (!(pressed = buttons())) sceKernelDelayThread(16000);
    telemetryEvent("startup_screen_buttons", pressed);
    telemetrySync();
    sceKernelExitProcess(code);
    for (;;) sceKernelDelayThread(1000000);
}

void startupProgressBegin(void* buffer, uint32_t total) {
    if (!buffer || progress.begun) return;
    progress.canvas = {static_cast<uint32_t*>(buffer), ui::ScreenWidth, ui::ScreenHeight, ui::ScreenWidth};
    progress.begun = true;
    ui::Progress p;
    p.total = total;
    timed_progress_draw(p, true);
    telemetryEvent("startup_progress_begin", total);
    telemetryEvent("startup_progress_display", show(buffer));
}

void startupProgressUpdate(uint32_t done, uint32_t total, bool compiling) {
    if (!progress.begun) return;
    if (!progress.shown) {
        if (!compiling) return;
        progress.shown = true;
        timed_progress_draw(compiling_view(done, total), true);
        telemetryEvent("startup_progress_shown", done);
        return;
    }
    if (done < total && processTimeUs() - progress.lastDraw < RedrawUs) return;
    timed_progress_draw(compiling_view(done, total), false);
}

void startupProgressEnd(uint32_t done, uint32_t total) {
    if (!progress.begun) return;
    if (progress.shown) {
        ui::Progress p = compiling_view(done, total);
        p.label = "Starting the game...";
        timed_progress_draw(p, false);
    }
    telemetryEvent("startup_progress_draws", progress.draws);
    telemetryEvent("startup_progress_draw_us", static_cast<long long>(progress.drawUs));
}

void startupNoteProblem(StartupProblem problem, const char* detail, uint64_t have, uint64_t need) {
    if (noted.problem != StartupProblem::None) return; // the first cause, not its consequences
    noted.problem = problem;
    std::snprintf(noted.detail, sizeof noted.detail, "%s", detail ? detail : "");
    noted.have = have;
    noted.need = need;
}

void startupGraphicsFailureExit(int code) {
    static MessageText text;
    switch (noted.problem) {
    case StartupProblem::ShaderCompiler: shader_compiler_message(text); break;
    case StartupProblem::Memory: memory_message(text, noted.detail, noted.have, noted.need); break;
    default: graphics_message(text, noted.detail); break;
    }
    startupErrorExit(text, code);
}

}  // namespace twv::vita
