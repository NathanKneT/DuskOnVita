// Aurora's application glue (lib/aurora.cpp, pinned 3227d76c) for the Vita.
//
// Upstream's file is the WebGPU/Dawn device, surface and present path wrapped around
// the platform-neutral pieces. This keeps its order and its C API, and replaces only
// the GPU part with the selected Vita backend (null diagnostics or vitaGL/GXM):
//   initialize: config defaults, SDL window/events (lib/window.cpp, unchanged), the
//               main-thread options, the GX FIFO; no GPU device, no ImGui/RmlUi
//               renderer (those UI layers draw through WebGPU upstream);
//   update:     input on the first frame, GX viewport policy, window events (front
//               touch also to the menu pointer, platform/vita/touch.cpp);
//   begin/end:  the GX FIFO frame bracket/drain, followed by native presentation.
// Aurora's backend enum has no GXM entry, so the Vita reports kBackendGxm
// (BACKEND_NULL + 1): BACKEND_NULL would send Dusklight to its no-GPU ImGui loop
// instead of the game. Dusklight names backends it does not know "Auto".
#include "aurora_vita.hpp"
#include "InputVita.hpp"
#include "touch.hpp"

#include <aurora/aurora.h>
#include <aurora/time.hpp>

#include "gx/fifo.hpp"
#include "gx/gx.hpp"
#include "input.hpp"
#include "internal.hpp"
#include "system_info.hpp"
#include "thread.hpp"
#include "window.hpp"
#include "gfx_null/gfx_null.hpp"
#if TWV_VITA_GFX_VGL
#include "gfx_vgl/backend.hpp"
#include "gfx_vgl/profile_scope.hpp"
#include "frame_overlap.hpp"
#endif
#include "runtime/telemetry.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_sensor.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if TWV_VITA_SCENE_PROBE
extern "C" void twv_vita_scene_probe(unsigned frame);
#endif
// patches/dusklight/0019 (game clock): a frame-overlap settle's time inside the running tick.
extern "C" void dusk_vita_tick_pause_ns(long long ns) __attribute__((weak));
#if TWV_VITA_DECODE_PROBE
extern "C" void twv_decode_probe_frame_end() noexcept; // gfx_vgl/profile_hooks.cpp
#endif

namespace aurora {
AuroraConfig g_config;
uint32_t g_sdlCustomEventsStart;
char g_gameName[4];

namespace {
constexpr Module Log{"aurora"};

constexpr std::array PreferredBackendOrder{twv::vita::kBackendGxm};

bool g_initialFrame = false;
// T2 (patches/aurora/0007): process the FIFO on its own worker instead of inline.
bool g_gxThread = false;
AuroraSampler g_resampler = SAMPLER_BILINEAR;

#if TWV_VITA_GFX_VGL
// --vita-frame-overlap=verify: the frame end waits for the whole frame (so it renders as with the
// overlap off), the worker records a hash of every piece of game memory the frame's commands read
// in place (patches/aurora/0009: indexed matrix rows, array bases, static lists), and the settle,
// after the game tick, hashes them again. A change is game memory the tick wrote while, with the
// overlap on, the worker could still have been reading it for the last frame.
struct OverlapRead {
  const void* data;
  uint32_t size, hash, kind, order;
};
bool g_verify = false;
std::vector<OverlapRead> g_reads;
struct VerifyStats {
  uint64_t frames = 0, reads = 0, large = 0, withinFrame = 0;
  std::array<uint64_t, 3> changed{}, checked{};
  unsigned logged = 0;
} g_verifyStats;
constexpr uint32_t kVerifyMaxBytes = 256 * 1024; // larger declared array sizes are counted, not hashed
uint32_t verify_hash(const void* data, uint32_t size) noexcept {
  uint32_t h = 2166136261u;
  for (const auto* p = static_cast<const uint8_t*>(data), *e = p + size; p != e; ++p) {
    h = (h ^ *p) * 16777619u;
  }
  return h;
}
uint64_t g_overlap_frames() noexcept;
std::vector<OverlapRead> g_frameReads; // the ended frame's reads, one per (address, size)
// Game thread, at the frame end, the worker done with the whole frame: one entry per (address,
// size), hashed now. A change since the worker's read happened within the frame (the painter wrote
// after a draw read it), with or without the overlap; it is counted apart.
void verify_snapshot() noexcept {
  std::sort(g_reads.begin(), g_reads.end(), [](const OverlapRead& a, const OverlapRead& b) {
    return a.data != b.data ? a.data < b.data : a.size != b.size ? a.size < b.size : a.order < b.order;
  });
  g_frameReads.clear();
  for (size_t i = 0; i < g_reads.size(); ++i) {
    auto r = g_reads[i];
    if (i + 1 < g_reads.size() && g_reads[i + 1].data == r.data && g_reads[i + 1].size == r.size) {
      continue; // the last read of each is the one the frame's draws used
    }
    const uint32_t now = verify_hash(r.data, r.size);
    if (now != r.hash) {
      ++g_verifyStats.withinFrame;
    }
    r.hash = now;
    g_frameReads.push_back(r);
  }
  g_reads.clear();
}
// Game thread, at the settle (after the tick the overlap runs concurrently): what the tick changed.
void verify_check() noexcept {
  auto& s = g_verifyStats;
  ++s.frames;
  for (const auto& r : g_frameReads) {
    ++s.reads;
    ++s.checked[r.kind];
    if (verify_hash(r.data, r.size) != r.hash) {
      ++s.changed[r.kind];
      if (s.logged < 24) {
        ++s.logged;
        char text[96];
        std::snprintf(text, sizeof text, "frame=%llu kind=%u addr=%p size=%u",
                      static_cast<unsigned long long>(g_overlap_frames()), r.kind, r.data, r.size);
        twv::vita::telemetryText("frame_overlap_verify_change", text);
      }
    }
  }
  g_frameReads.clear();
}

// --vita-frame-overlap (frame_overlap.hpp): end frames without waiting for the worker.
void finish_frame() noexcept;
bool begin_now() noexcept;
struct OverlapOps {
  uint64_t publish_frame() noexcept { return gx::fifo::publish_frame(); }
  void wait_draw_done(uint64_t end) noexcept {
    gx::fifo::wait_processed(g_verify ? end : gx::fifo::draw_done_mark());
    if (g_verify) {
      verify_snapshot();
    }
  }
  void close_frame() noexcept { gx::fifo::close_frame(); }
  bool settle_frame(uint64_t end) noexcept {
    const uint64_t before = gx::fifo::wait_us();
    gx::fifo::settle_frame(end);
    if (g_verify) {
      verify_check();
    }
    return gx::fifo::wait_us() == before;
  }
  void finish_frame() noexcept { aurora::finish_frame(); }
  bool begin_now() noexcept { return aurora::begin_now(); }
};
twv::vita::FrameOverlap<OverlapOps> g_overlap;
uint64_t g_overlap_frames() noexcept { return g_overlap.stats.frames; }
// A settle inside a game tick (the game's GXDrawDone, a second tick) is not the tick's own cost:
// patches/dusklight/0019's game clock leaves it out of the cost that limits ticks per loop, as the
// frame-end drain it replaces was outside the ticks.
void settle_in_tick(twv::vita::SettleSite site) noexcept {
  if (!g_overlap.pending) {
    return;
  }
  const uint64_t start = twv_vgl_time_us();
  g_overlap.settle(site);
  if (dusk_vita_tick_pause_ns) {
    dusk_vita_tick_pause_ns(static_cast<long long>(twv_vgl_time_us() - start) * 1000);
  }
}
bool g_viewportPolicySet = false;
// Game-thread time blocked on the worker, added to the profile once per presented frame.
uint64_t g_waitReported = 0;
#endif

AuroraInfo initialize(int argc, char* argv[], const AuroraConfig& config) noexcept {
  (void)argc;
  (void)argv;
  g_config = config;
  // Desktop defaults (256 MiB MEM1 + 24 MiB ARAM) exceed the handheld
  // process's measured budget. This changes allocation capacity, not game state.
  g_config.mem1Size = 64 * 1024 * 1024;
  g_config.mem2Size = 16 * 1024 * 1024;
  twv::vita::telemetryEvent("mem1_reserve_kib", g_config.mem1Size / 1024);
  twv::vita::telemetryEvent("aram_reserve_kib", g_config.mem2Size / 1024);
#if TWV_VITA_GFX_VGL
  Log.info("Aurora initializing (Vita, vitaGL/Cg on GXM)");
#else
  Log.info("Aurora initializing (Vita, null graphics diagnostic; no game pixels)");
#endif
  log_system_information();
  if (g_config.appName == nullptr) {
    g_config.appName = "Aurora";
  } else {
    g_config.appName = strdup(g_config.appName);
  }
  if (g_config.userPath == nullptr) {
    g_config.userPath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.userPath = strdup(g_config.userPath);
  }
  if (g_config.cachePath == nullptr) {
    g_config.cachePath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.cachePath = strdup(g_config.cachePath);
  }
  if (g_config.resourcesPath == nullptr) {
    g_config.resourcesPath = SDL_GetBasePath();
  } else {
    g_config.resourcesPath = strdup(g_config.resourcesPath);
  }
  if (g_config.msaa == 0) {
    g_config.msaa = 1;
  }
  if (g_config.maxTextureAnisotropy == 0) {
    g_config.maxTextureAnisotropy = 16;
  }
  AURORA_ASSERT(window::initialize(), "Error initializing window");

  g_sdlCustomEventsStart = SDL_RegisterEvents(2);
  AURORA_ASSERT(g_sdlCustomEventsStart, "Failed to allocate user events: {}", SDL_GetError());
  AURORA_ASSERT(window::initialize_event_watch(), "Error initializing SDL event watch");

  // The GPU belongs to the Vita backend, not SDL: the window is created with an external
  // graphics context (no SDL renderer, no GL), as upstream does for every non-null backend.
  const AuroraBackend selectedBackend = twv::vita::kBackendGxm;
  AURORA_ASSERT(window::create_window(selectedBackend), "Error creating window: {}", SDL_GetError());

  window::show_window();
  thread::set_current({
      .name = "Main thread",
      .affinity = thread::Affinity::SharedCache,
  });

#if TWV_VITA_GFX_VGL
  gx::fifo::set_processing_mode(g_gxThread ? gx::fifo::ProcessingMode::Thread : gx::fifo::ProcessingMode::Inline);
  twv::vita::telemetryEvent("gx_thread_mode", g_gxThread);
#endif
  gx::fifo::init();
  twv::vita::configureInputDefaults();
#if TWV_VITA_GFX_VGL
  twv::gfx_vgl::init();
#endif
  const auto size = window::get_window_size();
  Log.info("Using framebuffer size {}x{} scale {}", size.fb_width, size.fb_height, size.scale);
  if (g_config.imGuiInitCallback != nullptr) {
    Log.info("Vita shell has no desktop ImGui renderer; original J2D game UI retained");
  }

  g_initialFrame = true;
  g_config.desiredBackend = selectedBackend;
  twv::vita::telemetryBuffered(true);
  return {
      .backend = selectedBackend,
      .userPath = g_config.userPath,
      .cachePath = g_config.cachePath,
      .window = window::get_sdl_window(),
      .windowSize = size,
  };
}

void shutdown() noexcept {
#if TWV_VITA_GFX_VGL
  g_overlap.settle(twv::vita::SettleSite::Other);
#endif
  gx::fifo::shutdown();
#if TWV_VITA_GFX_VGL
  gx::shutdown();
  twv::gfx_vgl::shutdown();
#endif
  input::shutdown();
  window::shutdown();
}

// Which motion sensors SDL's Vita driver offers; Dusklight's gyro aim reads them through Aurora's
// lib/device.cpp. Listing them does not open them.
void report_sensors() noexcept {
  int count = 0;
  SDL_SensorID* sensors = SDL_GetSensors(&count);
  int gyro = 0;
  int accel = 0;
  for (int i = 0; sensors != nullptr && i < count; ++i) {
    const SDL_SensorType type = SDL_GetSensorTypeForID(sensors[i]);
    gyro += type == SDL_SENSOR_GYRO;
    accel += type == SDL_SENSOR_ACCEL;
  }
  SDL_free(sensors);
  Log.info("SDL sensors: {} (gyroscope {}, accelerometer {})", count, gyro, accel);
  twv::vita::telemetryEvent("sdl_sensors", count);
  twv::vita::telemetryEvent("sdl_sensor_gyro", gyro);
  twv::vita::telemetryEvent("sdl_sensor_accel", accel);
}

// The front panel's fingers also go to Dusklight's menu pointer (platform/vita/touch.cpp); the
// game still receives every event. The rear panel is not a pointer.
void forward_front_touch(const AuroraEvent* events) noexcept {
  using twv::vita::touch::Finger;
  constexpr SDL_TouchID FrontPanel = 1; // SDL's Vita driver: 1 front, 2 rear
  for (const AuroraEvent* e = events; e->type != AURORA_NONE; ++e) {
    if (e->type != AURORA_SDL_EVENT) {
      continue;
    }
    Finger finger;
    switch (e->sdl.type) {
    case SDL_EVENT_FINGER_DOWN: finger = Finger::Down; break;
    case SDL_EVENT_FINGER_MOTION: finger = Finger::Motion; break;
    case SDL_EVENT_FINGER_UP: finger = Finger::Up; break;
    case SDL_EVENT_FINGER_CANCELED: finger = Finger::Canceled; break;
    default: continue;
    }
    const SDL_TouchFingerEvent& t = e->sdl.tfinger;
    if (t.touchID == FrontPanel) {
      twv_vita_front_touch(static_cast<int>(finger), t.fingerID, t.x, t.y);
    }
  }
}

const AuroraEvent* update() noexcept {
#if TWV_VITA_GFX_VGL
  twv::vgl::ProfileScope timer(TWV_PROFILE_EVENTS);
#endif
  if (g_initialFrame) {
    g_initialFrame = false;
    input::initialize();
    report_sensors();
  }
#if TWV_VITA_GFX_VGL
  // A new viewport policy changes state the worker reads: the pending frame finishes first.
  if (g_viewportPolicySet) {
    g_viewportPolicySet = false;
    g_overlap.settle(twv::vita::SettleSite::Other);
  }
#endif
  gx::update();
  const AuroraEvent* events = window::poll_events();
  forward_front_touch(events);
  return events;
}

#if TWV_VITA_GFX_VGL
bool begin_now() noexcept {
  gx::fifo::begin_frame();
  return twv::gfx_vgl::begin_frame();
}

// What end_frame does once the worker has processed the whole frame: the draw cache, the present
// and the backend's per-frame receipts (their counters belong to the worker until then).
void finish_frame() noexcept {
  gx::fifo::end_frame();
  const uint64_t waited = gx::fifo::wait_us();
  twv_vgl_profile_add(TWV_PROFILE_GX_WAIT, waited - g_waitReported);
  g_waitReported = waited;
  twv::gfx_vgl::end_frame();
#if TWV_VITA_DECODE_PROBE
  twv_decode_probe_frame_end();
#endif
  if (g_overlap.enabled &&
      g_overlap.stats.frames % (twv::vita::telemetryIsQuiet() ? twv::vita::QuietReceiptFrames : 60) == 0) {
    const auto& s = g_overlap.stats;
    twv::vita::telemetryEvent("frame_overlap_frames", static_cast<long long>(s.frames));
    twv::vita::telemetryEvent("frame_overlap_settle_idle", static_cast<long long>(s.idle));
    for (unsigned site = 1; site < twv::vita::SettleSites; ++site) {
      const std::string key = std::string("frame_overlap_settle_") + twv::vita::settle_site_name(site);
      twv::vita::telemetryEvent(key.c_str(), static_cast<long long>(s.sites[site]));
    }
    twv::vita::telemetryEvent("frame_overlap_gx_wait_us_total", static_cast<long long>(waited));
    if (g_verify) {
      const auto& v = g_verifyStats;
      const char* kinds[] = {"matrix_rows", "arrays", "static_lists"};
      twv::vita::telemetryEvent("frame_overlap_verify_frames", static_cast<long long>(v.frames));
      twv::vita::telemetryEvent("frame_overlap_verify_reads", static_cast<long long>(v.reads));
      twv::vita::telemetryEvent("frame_overlap_verify_large_arrays", static_cast<long long>(v.large));
      twv::vita::telemetryEvent("frame_overlap_verify_changed_within_frame", static_cast<long long>(v.withinFrame));
      for (unsigned k = 0; k < 3; ++k) {
        twv::vita::telemetryEvent((std::string("frame_overlap_verify_checked_") + kinds[k]).c_str(),
                                  static_cast<long long>(v.checked[k]));
        twv::vita::telemetryEvent((std::string("frame_overlap_verify_changed_") + kinds[k]).c_str(),
                                  static_cast<long long>(v.changed[k]));
      }
    }
  }
}
#endif

bool begin_frame() noexcept {
#if TWV_VITA_GFX_VGL
  if (g_overlap.enabled) {
    return g_overlap.begin_frame(window::is_presentable() && !window::is_paused());
  }
#endif
  if (!window::is_presentable() || window::is_paused()) {
    return false;
  }
  gx::fifo::begin_frame();
#if TWV_VITA_GFX_VGL
  return twv::gfx_vgl::begin_frame();
#endif
  return true;
}

void end_frame() noexcept {
#if TWV_VITA_GFX_VGL
  if (g_overlap.enabled) {
    // The worker goes on with this frame while the next game tick runs (frame_overlap.hpp).
    g_overlap.end_frame();
  } else {
    gx::fifo::drain();
    finish_frame();
  }
#else
  gx::fifo::drain();
  gx::fifo::end_frame();
  twv::gfx_null::end_frame();
#endif
#if TWV_VITA_SCENE_PROBE
  static unsigned frame = 0;
  twv_vita_scene_probe(++frame);
#endif
}
} // namespace
} // namespace aurora

// T2: start-up only (main_vita.cpp), before aurora_initialize.
extern "C" int twv_vita_gx_thread_select(int enabled) {
#if TWV_VITA_GFX_VGL
  aurora::g_gxThread = enabled != 0;
  return 1;
#else
  return enabled == 0;
#endif
}

// --vita-frame-overlap: start-up only (main_vita.cpp), after the GX thread choice and before
// aurora_initialize. Needs the GX worker (with Inline processing there is nothing to overlap).
// 0 off, 1 on, 2 verify (frame ends wait for the whole frame; game memory read in place is checked).
extern "C" int twv_vita_frame_overlap_select(int mode) {
#if TWV_VITA_GFX_VGL
  if (mode < 0 || mode > 2 || (mode != 0 && !aurora::g_gxThread)) {
    return 0;
  }
  aurora::g_overlap.enabled = mode != 0;
  aurora::g_verify = mode == 2;
  if (aurora::g_verify) {
    aurora::g_reads.reserve(1 << 14);
  }
  twv::vita::telemetryEvent("frame_overlap_enabled", mode);
  return 1;
#else
  return mode == 0;
#endif
}

#if TWV_VITA_GFX_VGL
// patches/dusklight/0019 (m_Do_main.cpp): before a second game tick (1), the presentation draw (2)
// or the one-tick-per-frame path (3).
extern "C" void twv_vita_frame_overlap_settle(int site) {
  using twv::vita::SettleSite;
  aurora::settle_in_tick(site == 1 ? SettleSite::SecondTick : site == 2 ? SettleSite::Presentation :
                         site == 3 ? SettleSite::FrameTick : SettleSite::Other);
}
// patches/aurora/0009: the first thing fifo::drain() does (GXDrawDone, AuroraGXSync).
extern "C" void twv_gx_frame_overlap_settle() noexcept {
  aurora::settle_in_tick(twv::vita::SettleSite::Drain);
}
// patches/aurora/0009, GX worker: game memory a command reads in place (verify mode only).
extern "C" void twv_gx_overlap_read(unsigned kind, const void* data, uint32_t size) noexcept {
  if (!aurora::g_verify || data == nullptr || size == 0 || kind > 2) {
    return;
  }
  if (size > aurora::kVerifyMaxBytes) {
    ++aurora::g_verifyStats.large;
    return;
  }
  static uint32_t order = 0;
  aurora::g_reads.push_back({data, size, aurora::verify_hash(data, size), kind, order++});
}
// patches/aurora/0009: AuroraSetViewportPolicy; the next update() settles first.
extern "C" void twv_gx_viewport_policy_set() noexcept {
  aurora::g_viewportPolicySet = aurora::g_overlap.enabled;
}
#endif

// C API bindings (same set as lib/aurora.cpp)
AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config) {
  return aurora::initialize(argc, argv, *config);
}
void aurora_shutdown() { aurora::shutdown(); }
const AuroraEvent* aurora_update() { return aurora::update(); }
bool aurora_begin_frame() { return aurora::begin_frame(); }
void aurora_end_frame() { aurora::end_frame(); }
AuroraBackend aurora_get_backend() { return aurora::g_config.desiredBackend; }
const AuroraBackend* aurora_get_available_backends(size_t* count) {
  if (count != nullptr) {
    *count = aurora::PreferredBackendOrder.size();
  }
  return aurora::PreferredBackendOrder.data();
}
void aurora_set_log_level(AuroraLogLevel level) { aurora::g_config.logLevel = level; }
void aurora_set_pause_on_focus_lost(bool value) { aurora::g_config.pauseOnFocusLost = value; }
void aurora_set_background_input(bool value) {
  aurora::g_config.allowJoystickBackgroundEvents = value;
  aurora::window::set_background_input(value);
}
// The resampler filters the present blit; the null gfx presents nothing, so the
// choice is only kept for the GXM backend.
void aurora_set_resampler(AuroraSampler sampler) { aurora::g_resampler = sampler; }
void aurora_set_timescale(float scale) { aurora::time::set_scale(scale); }
float aurora_get_timescale() { return aurora::time::scale(); }
