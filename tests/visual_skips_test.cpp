// runtime/visual_skips.hpp: the --vita-skip-hidden-draw, --vita-skip-offscreen-anim and
// --vita-state-digest options.
#include "runtime/visual_skips.hpp"

#include <cassert>
#include <cstdio>

int main() {
    using namespace twv::vita;
    int choices[VisualSkipCount] = {-1, -1, -1};
    int digest = -1;
    assert(visualSkipOption("--stage", choices, digest) == 0);
    assert(visualSkipOption("--vita-fast-search=on", choices, digest) == 0);
    assert(visualSkipOption("--vita-skip-hidden-drawx=on", choices, digest) == 0);
    assert(visualSkipOption("--vita-skip-hidden-draw=on", choices, digest) == 1 && choices[SkipHiddenDraw] == 1);
    assert(visualSkipOption("--vita-skip-hidden-draw=off", choices, digest) == -1);
    assert(visualSkipOption("--vita-skip-offscreen-anim=verify", choices, digest) == -1);
    assert(visualSkipOption("--vita-skip-offscreen-anim=off", choices, digest) == 1 && choices[SkipOffscreenAnim] == 0);
    assert(visualSkipOption("--vita-state-digest=on", choices, digest) == 1 && digest == 1);
    assert(visualSkipOption("--vita-state-digest=on", choices, digest) == -1);
    assert(visualSkipOption("--vita-rest-creatures=on", choices, digest) == 1 && choices[RestCreatures] == 1);
    int crowd = -1;
    assert(crowdOption("--vita-crowd=half", crowd) == 1 && crowd == CrowdHalf);
    assert(crowdOption("--vita-crowd=low", crowd) == -1);
    int crowd2 = -1;
    assert(crowdOption("--vita-crowd=none", crowd2) == -1 && crowdOption("--vita-crowdx", crowd2) == 0);
    int fresh[VisualSkipCount] = {-1, -1, -1};
    int freshDigest = -1;
    assert(visualSkipOption("--vita-skip-hidden-draw", fresh, freshDigest) == 0);
    assert(visualSkipOption("--vita-skip-hidden-draw=", fresh, freshDigest) == -1 && fresh[SkipHiddenDraw] == -1);
    std::printf("visual_skips_test: options passed\n");
    return 0;
}
