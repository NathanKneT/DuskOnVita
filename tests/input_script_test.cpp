// Scripted pad input (runtime/input_script.hpp): parsing, step timing and rejection.
#include "runtime/input_script.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

// Step-by-step play: "live" and "shot" are one read each with no input; a command file is a script
// of its own, so it may hold them too.
static void live_steps() {
    static twv::vita::InputScript s;
    assert(s.parse("wait 2\nshot\nlive\n") && s.count == 3);
    assert(s.steps[1].kind == twv::vita::StepShot && s.steps[1].reads == 1 && s.steps[1].buttons == 0);
    assert(s.steps[2].kind == twv::vita::StepLive && s.steps[2].reads == 1);
    assert(s.next() && s.next() && s.next()->kind == twv::vita::StepShot && s.next()->kind == twv::vita::StepLive && !s.next());
    assert(s.parse("stick 0 100 30\nhold A 2\nshot\nwait 3\n") && s.count == 4);
    for (const char* bad : {"live 2\n", "shot now\n", "liv\n"}) assert(!s.parse(bad));
    // Buttons with the stick, and the three fixtures for trying a passage.
    assert(s.parse("press A 0 100 3\npress A+B -50 60 2\ngive 72\nequip 1 4\ntp -100 250 3000\n") && s.count == 5);
    assert(s.steps[0].buttons == twv::vita::ScriptButtonA && s.steps[0].y == 100 && s.steps[0].reads == 3);
    assert(s.steps[1].buttons == (twv::vita::ScriptButtonA | twv::vita::ScriptButtonB) && s.steps[1].x == -50);
    assert(s.steps[2].kind == twv::vita::StepGive && s.steps[2].room == 72 && s.steps[2].reads == 1);
    assert(s.steps[3].kind == twv::vita::StepEquip && s.steps[3].room == 1 && s.steps[3].point == 4);
    assert(s.steps[4].kind == twv::vita::StepTp && s.steps[4].room == -100 && s.steps[4].point == 250 && s.steps[4].layer == 3000);
    assert(s.parse("film 90\nstick 0 100 90\n") && s.steps[0].kind == twv::vita::StepFilm && s.steps[0].room == 90);
    assert(!s.parse("film 0\n") && !s.parse("film\n"));
    assert(s.parse("event 3332\n") && s.steps[0].kind == twv::vita::StepEvent && s.steps[0].room == 3332 && !s.parse("event 70000\n"));
    assert(s.parse("wear 49\n") && s.steps[0].kind == twv::vita::StepWear && s.steps[0].room == 49 && !s.parse("wear 300\n"));
    assert(s.parse("form 1\n") && s.steps[0].kind == twv::vita::StepForm && s.steps[0].room == 1 && !s.parse("form 2\n"));
    for (const char* bad : {"press A 0 100\n", "press Q 0 0 1\n", "give 256\n", "equip 2 0\n", "equip 0 24\n", "tp 1 2\n"})
        assert(!s.parse(bad));
    std::puts("input_script_test: live and shot steps, press, give, equip, tp");
}

int main() {
    live_steps();
    using twv::vita::InputScript;
    static InputScript s;
    assert(s.parse("# boot\r\nwait 3\n\nhold A+START 2\r\n  stick -128 127 1  \nhold DOWN 1\n"));
    assert(s.count==4);
    for(int i=0;i<3;++i){auto* step=s.next();assert(step && step->buttons==0 && step->x==0);}
    for(int i=0;i<2;++i){auto* step=s.next();assert(step && step->buttons==(twv::vita::ScriptButtonA|twv::vita::ScriptButtonStart));}
    auto* stick=s.next();assert(stick && stick->x==-128 && stick->y==127 && stick->buttons==0);
    auto* down=s.next();assert(down && down->buttons==twv::vita::ScriptButtonDown);
    assert(!s.next() && !s.next()); // ended: the real pad is read again
    for(const char* bad:{"","# only\n","wait\n","wait 0\n","wait x\n","hold Q 1\n","hold A+ 1\n","hold A\n",
                        "stick 1 2\n","stick 200 0 1\n","jump 1\n","wait 1 2\n","hold A 1 extra\n"})
        assert(!s.parse(bad) && s.count==0 && !s.next());
    // Stage tours: a warp lasts one read and carries the request; loop restarts at step 0.
    assert(s.parse("wait 2\nwarp F_SP103 0 23 -1\r\nstick 0 60 1\nwarp D_MN04A 50 1 15\nloop\n"));
    assert(s.count==5);
    for(int round=0;round<3;++round) {
        for(int i=0;i<2;++i){auto* step=s.next();assert(step && step->kind==twv::vita::StepPad && step->buttons==0);}
        auto* w=s.next();
        assert(w && w->kind==twv::vita::StepWarp && std::strcmp(w->stage,"F_SP103")==0 && w->room==0 &&
               w->point==23 && w->layer==-1 && w->buttons==0 && w->x==0 && w->y==0);
        auto* walk=s.next();assert(walk && walk->kind==twv::vita::StepPad && walk->y==60);
        auto* boss=s.next();
        assert(boss && boss->kind==twv::vita::StepWarp && std::strcmp(boss->stage,"D_MN04A")==0 && boss->room==50 &&
               boss->point==1 && boss->layer==15);
        assert(s.loops==unsigned(round));
    }
    assert(s.parse("warp R_SP01 4 0 0\n") && s.next()->kind==twv::vita::StepWarp && !s.next()); // ends without loop
    assert(s.parse("darkclear 2\nwait 1\n"));
    {auto* d=s.next();assert(d && d->kind==twv::vita::StepDarkClear && d->layer==2 && d->buttons==0);
     auto* w=s.next();assert(w && w->kind==twv::vita::StepPad);assert(!s.next());}
    for(const char* bad:{"darkclear 4\n","darkclear\n","darkclear -1\n","darkclear 1 1\n"})
        assert(!s.parse(bad) && s.count==0);
    for(const char* bad:{"loop\n","wait 1\nloop 2\n","warp F_SP103 0 23\n","warp F_SP1030 0 0 0\n",
                        "warp F-SP 0 0 0\n","warp F_SP103 128 0 0\n","warp F_SP103 0 0 16\n","warp F_SP103 0 0 -2\n",
                        "warp F_SP103 0 -5 0\n","warp F_SP103 0 0 0 1\n","wait 1 2 3 4 5\n"})
        assert(!s.parse(bad) && s.count==0 && !s.next());
    static InputScript many;
    static char text[(InputScript::MaxSteps+1)*7+1];unsigned used=0;
    for(unsigned i=0;i<InputScript::MaxSteps+1;++i)used+=unsigned(std::snprintf(text+used,sizeof text-used,"wait 1\n"));
    assert(!many.parse(text));
    std::printf("input_script_test: waits, held button sets, raw stick, warp, loop, end of script and malformed lines passed\n");
}
