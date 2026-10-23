// Aurora's UI entry points when the Vita build has no desktop RmlUi/ImGui context.
// Shared with the 3DS port (platform/3ds/aurora/ui_absent.cpp there).
// This does not replace or suppress the original game's J2D HUD, menus or dialogue.
// Upstream already supports running without RmlUi: dusk::ui::initialize(), handle_event() and the
// mods UI service all check aurora::rmlui::is_initialized() and take their no-UI path when it is
// false, and Aurora's own frame loop (lib/aurora.cpp) does the same. So these report the truth,
// "no RmlUi context", instead of trapping, and the game reaches its own logic. Calls that only
// configure a UI (scale, input type, texture providers) have no UI to apply to. Any other RmlUi or
// ImGui symbol stays a named trap stub (link_report.txt), so real UI use still fails visibly.
// SDK-free; built with the game's int-sized enums.
#include <aurora/rmlui.hpp>

#include "imgui.hpp"
#include "rmlui.hpp"

namespace aurora::rmlui {
namespace {
float gUiScale = 1.f;
}

Rml::Context* get_context() noexcept { return nullptr; }
bool is_initialized() noexcept { return false; }
void set_input_type(InputType) noexcept {}
void set_ui_scale(float scale) noexcept { gUiScale = scale; }
float get_ui_scale() noexcept { return gUiScale; }
void set_glass_light_dir(float, float) noexcept {}
void register_texture_provider(std::string, TextureProvider) {}
void unregister_texture_provider(std::string_view) noexcept {}
void handle_event(SDL_Event&) noexcept {}
}  // namespace aurora::rmlui

namespace aurora::imgui {
// No ImGui context exists, so there is nothing to feed events to.
void process_event(const SDL_Event&) noexcept {}
}  // namespace aurora::imgui
