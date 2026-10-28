// Scripted pad input (runtime/input_script.hpp): parsing, step timing and rejection.
#include "runtime/input_script.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

int main() {
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
