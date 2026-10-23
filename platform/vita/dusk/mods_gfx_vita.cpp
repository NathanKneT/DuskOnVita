// dusk::mods::gfx_run_stage without the mods gfx service (patches/dusklight/0004).
// Upstream (src/dusk/mods/svc/gfx.cpp) runs the stage hooks mods registered through
// that service and returns at once when there are none. The service is not built on
// the Vita, so no hook can exist and the stage has nothing to run.
#include "dusk/gfx.hpp"

namespace dusk::mods {
void gfx_run_stage(GfxStage stage, const view_class* gameView, const view_port_class* gameViewport) {
    (void)stage;
    (void)gameView;
    (void)gameViewport;
}
}  // namespace dusk::mods
