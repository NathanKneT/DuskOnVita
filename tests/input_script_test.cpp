// Scripted pad input (runtime/input_script.hpp): parsing, step timing and rejection.
#include "runtime/input_script.hpp"
#include <cassert>
#include <cstdio>
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
    static InputScript many;
    char text[600*7+1];unsigned used=0;
    for(unsigned i=0;i<InputScript::MaxSteps+1;++i)used+=unsigned(std::snprintf(text+used,sizeof text-used,"wait 1\n"));
    assert(!many.parse(text));
    std::printf("input_script_test: waits, held button sets, raw stick, end of script and malformed lines passed\n");
}
