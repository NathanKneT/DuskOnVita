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
#endif
#include "runtime/telemetry.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_sensor.h>
#include <SDL3/SDL_stdinc.h>

#include <array>
#include <cstring>

#if TWV_VITA_SCENE_PROBE
extern "C" void twv_vita_scene_probe(unsigned frame);
#endif
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
  gx::update();
  const AuroraEvent* events = window::poll_events();
  forward_front_touch(events);
  return events;
}

bool begin_frame() noexcept {
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
  gx::fifo::drain();
  gx::fifo::end_frame();
#if TWV_VITA_GFX_VGL
  twv::gfx_vgl::end_frame();
#else
  twv::gfx_null::end_frame();
#endif
#if TWV_VITA_DECODE_PROBE
  twv_decode_probe_frame_end();
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
