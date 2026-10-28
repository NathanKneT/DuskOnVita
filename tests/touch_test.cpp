// Front touch -> menu pointer finger tracking (platform/vita/touch.hpp).
//   touch_test <path to platform/vita/gfx_vgl/backend.cpp>
#include "touch.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#define CHECK(c)                                                                     \
  do {                                                                               \
    if (!(c)) {                                                                      \
      std::fprintf(stderr, "touch_test: line %d: %s\n", __LINE__, #c);               \
      std::abort();                                                                  \
    }                                                                                \
  } while (0)

using namespace twv::vita::touch;

namespace {
bool near(float a, float b) { return a - b < 0.01f && b - a < 0.01f; }

void expect(const std::optional<PointerEvent>& e, Pointer phase, float x, float y) {
  CHECK(e.has_value());
  CHECK(e->phase == phase);
  CHECK(near(e->x, x) && near(e->y, y));
}

// The drawn area must stay the renderers' game viewport.
void drawn_area_matches_backend(const char* path) {
  std::ifstream in(path);
  CHECK(in.good());
  std::stringstream text;
  text << in.rdbuf();
  const std::string s = text.str();
  const auto at = s.find("constexpr int GameLeft=");
  CHECK(at != std::string::npos);
  int left = 0, width = 0, height = 0;
  const int read = std::sscanf(s.c_str() + at, "constexpr int GameLeft=%d, GameWidth=%d, GameHeight=%d;",
                               &left, &width, &height);
  CHECK(read == 3);
  CHECK(float(left) == kGameLeft && float(width) == kGameWidth && float(height) == kGameHeight);
  CHECK(kGameLeft * 2 + kGameWidth == kScreenWidth && kGameHeight == kScreenHeight);
}
} // namespace

int main(int argc, char** argv) {
  CHECK(argc == 2);
  drawn_area_matches_backend(argv[1]);

  FrontTouch t;
  // Screen centre -> centre of the drawn area; a tap is press then release.
  expect(t.event(Finger::Down, 7, 0.5f, 0.5f, true), Pointer::Press, 480.0f, 272.0f);
  expect(t.event(Finger::Motion, 7, 0.5f, 0.25f, true), Pointer::Move, 480.0f, 136.0f);
  expect(t.event(Finger::Up, 7, 0.5f, 0.25f, true), Pointer::Release, 480.0f, 136.0f);
  CHECK(!t.event(Finger::Motion, 7, 0.5f, 0.5f, true));

  // No menu reading the pointer: the finger is not tracked at all.
  CHECK(!t.event(Finger::Down, 1, 0.5f, 0.5f, false));
  CHECK(!t.event(Finger::Motion, 1, 0.6f, 0.5f, true));
  CHECK(!t.event(Finger::Up, 1, 0.6f, 0.5f, true));

  // The game fills the screen: presses right at both edges count.
  expect(t.event(Finger::Down, 3, (kGameLeft + 0.5f) / kScreenWidth, 0.0f, true), Pointer::Press, 0.5f, 0.0f);
  // Its moves follow it anywhere on the panel.
  expect(t.event(Finger::Motion, 3, 0.0f, 1.0f, true), Pointer::Move, 0.0f, 544.0f);
  // A second finger is ignored while the first is down.
  CHECK(!t.event(Finger::Down, 4, 0.5f, 0.5f, true));
  CHECK(!t.event(Finger::Motion, 4, 0.5f, 0.5f, true));
  CHECK(!t.event(Finger::Up, 4, 0.5f, 0.5f, true));
  expect(t.event(Finger::Canceled, 3, 0.0f, 1.0f, true), Pointer::Cancel, 0.0f, 544.0f);
  expect(t.event(Finger::Down, 4, (kGameLeft + kGameWidth - 0.5f) / kScreenWidth, 1.0f, true),
         Pointer::Press, 959.5f, 544.0f);

  // Menu pointer turned off in the options: tracking is dropped.
  t.reset();
  CHECK(!t.event(Finger::Up, 4, 0.5f, 0.5f, true));
  expect(t.event(Finger::Down, 5, 0.5f, 0.5f, true), Pointer::Press, 480.0f, 272.0f);

  std::puts("touch_test: ok");
  return 0;
}
