// Vita front touch for Dusklight's menu pointer: file select, save, item wheel, collection,
// options and dialogs, as upstream's touch menus. Upstream feeds the pointer from its RmlUi touch
// layer, which the Vita does not have; aurora_vita.cpp passes the front panel's SDL finger events
// here instead, and patches/dusklight/0017 maps the drawn area onto the game's 2D coordinates.
// The rear panel is not a pointer.
#include "touch.hpp"
#include "runtime/telemetry.hpp"

#include "dusk/menu_pointer.h"

#include <cstdio>

namespace {
twv::vita::touch::FrontTouch g_front;
}

extern "C" void twv_vita_front_touch(int finger, std::uint64_t id, float nx, float ny) noexcept {
  namespace mp = dusk::menu_pointer;
  using twv::vita::touch::Pointer;
  if (!mp::enabled()) {
    g_front.reset();
    return;
  }
  const auto e = g_front.event(static_cast<twv::vita::touch::Finger>(finger), id, nx, ny, mp::active());
  if (!e) {
    return;
  }
  mp::Phase phase = mp::Phase::Move;
  switch (e->phase) {
  case Pointer::Move: phase = mp::Phase::Move; break;
  case Pointer::Press: phase = mp::Phase::Press; break;
  case Pointer::Release: phase = mp::Phase::Release; break;
  case Pointer::Cancel: phase = mp::Phase::Cancel; break;
  }
  mp::set_pointer_surface(twv::vita::touch::kGameWidth, twv::vita::touch::kGameHeight);
  mp::handle_fallthrough_pointer(e->x, e->y, phase, true);
  // Receipt per tap: where it landed and whether the menu pointer counted it as a click.
  if (e->phase != Pointer::Move) {
    char text[64];
    std::snprintf(text, sizeof text, "%s x=%d y=%d clicked=%d",
                  e->phase == Pointer::Press ? "press" : e->phase == Pointer::Release ? "release" : "cancel",
                  static_cast<int>(e->x), static_cast<int>(e->y), mp::state().clicked ? 1 : 0);
    twv::vita::telemetryText("menu_touch", text);
  }
}
