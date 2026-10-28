#pragma once
// Vita front touch panel -> Dusklight's menu pointer (platform/vita/touch.cpp). The finger
// tracking here is plain C++ so the host tests can run it (tests/touch_test.cpp).
#include <cstdint>
#include <optional>

namespace twv::vita::touch {

// The game's drawn area on the 960x544 screen, the same for both renderers: gfx_vgl/backend.cpp
// GameLeft/GameWidth/GameHeight (tools/host_tests.sh checks they still match).
inline constexpr float kScreenWidth = 960.0f;
inline constexpr float kScreenHeight = 544.0f;
inline constexpr float kGameLeft = 0.0f;
inline constexpr float kGameWidth = 960.0f;
inline constexpr float kGameHeight = 544.0f;

enum class Finger { Down, Motion, Up, Canceled }; // SDL_EVENT_FINGER_*
enum class Pointer { Move, Press, Release, Cancel }; // dusk::menu_pointer::Phase

struct PointerEvent {
  Pointer phase;
  float x; // pixels of the drawn area
  float y;
};

// One finger at a time, as upstream's touch menus (dusk/ui/touch_controls.cpp): a finger starts
// the pointer only while a menu reads it and only inside the drawn area; its moves and its release
// follow wherever it goes; other fingers are ignored. nx, ny: SDL's 0..1 front panel position.
class FrontTouch {
public:
  std::optional<PointerEvent> event(Finger finger, std::uint64_t id, float nx, float ny,
                                    bool menuActive) noexcept {
    const float x = nx * kScreenWidth - kGameLeft;
    const float y = ny * kScreenHeight;
    switch (finger) {
    case Finger::Down:
      if (mActive || !menuActive || x < 0.0f || x > kGameWidth || y < 0.0f || y > kGameHeight) {
        return std::nullopt;
      }
      mActive = true;
      mId = id;
      return PointerEvent{Pointer::Press, x, y};
    case Finger::Motion:
      if (!mActive || mId != id) {
        return std::nullopt;
      }
      return PointerEvent{Pointer::Move, x, y};
    case Finger::Up:
    case Finger::Canceled:
      if (!mActive || mId != id) {
        return std::nullopt;
      }
      mActive = false;
      return PointerEvent{finger == Finger::Up ? Pointer::Release : Pointer::Cancel, x, y};
    }
    return std::nullopt;
  }

  void reset() noexcept { mActive = false; }

private:
  bool mActive = false;
  std::uint64_t mId = 0;
};

} // namespace twv::vita::touch

// Defined in touch.cpp (game side, twv_dusk); called by aurora_vita.cpp for each front-panel SDL
// finger event. finger is a twv::vita::touch::Finger.
extern "C" void twv_vita_front_touch(int finger, std::uint64_t id, float nx, float ny) noexcept;
