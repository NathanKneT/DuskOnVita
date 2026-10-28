#pragma once
// Test-only scripted pad input (--vita-input-script=FILE, main_vita.cpp): drives a boot without a
// person at the pad (Vita3K boot-to-play checks). Each step lasts a number of pad reads (one per
// game tick); while the script runs it replaces controller 0, and once it ends the real pad is
// read again untouched. SDK-free and host-tested (tests/input_script_test.cpp).
//   wait N                 no input for N reads
//   hold A+START N         buttons held for N reads (A B X Y Z L R START UP DOWN LEFT RIGHT)
//   stick X Y N            main stick, raw -128..127, for N reads
//   # comment, blank lines
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace twv::vita {
// GameCube PAD bits (dolphin/pad.h; input_script.cpp checks them against the SDK values).
constexpr uint32_t ScriptButtonLeft=0x1,ScriptButtonRight=0x2,ScriptButtonDown=0x4,ScriptButtonUp=0x8,
    ScriptTriggerZ=0x10,ScriptTriggerR=0x20,ScriptTriggerL=0x40,ScriptButtonA=0x100,ScriptButtonB=0x200,
    ScriptButtonX=0x400,ScriptButtonY=0x800,ScriptButtonStart=0x1000;

struct InputStep {uint32_t reads=0,buttons=0;int x=0,y=0;};

struct InputScript {
    static constexpr unsigned MaxSteps=512;
    InputStep steps[MaxSteps];
    unsigned count=0,index=0,used=0;

    static uint32_t button(const char* name,std::size_t n) noexcept {
        struct Name {const char* text;uint32_t bit;};
        static constexpr Name names[]={{"A",ScriptButtonA},{"B",ScriptButtonB},{"X",ScriptButtonX},{"Y",ScriptButtonY},
            {"Z",ScriptTriggerZ},{"L",ScriptTriggerL},{"R",ScriptTriggerR},{"START",ScriptButtonStart},
            {"UP",ScriptButtonUp},{"DOWN",ScriptButtonDown},{"LEFT",ScriptButtonLeft},{"RIGHT",ScriptButtonRight}};
        for(const auto& b:names)if(std::strlen(b.text)==n && std::strncmp(b.text,name,n)==0)return b.bit;
        return 0;
    }
    // False, and no steps, on any malformed line or more than MaxSteps steps.
    bool parse(const char* text) noexcept {
        count=index=used=0;
        if(!text)return false;
        while(*text) {
            const char* end=text;while(*end && *end!='\n')++end;
            if(!line(text,end)){count=0;return false;}
            text=*end?end+1:end;
        }
        return count>0;
    }
    // The step for this pad read, or nullptr once the script has ended.
    const InputStep* next() noexcept {
        while(index<count && used>=steps[index].reads){++index;used=0;}
        if(index>=count)return nullptr;
        ++used;return &steps[index];
    }
private:
    static const char* skip(const char* p,const char* end) noexcept {while(p<end && (*p==' '||*p=='\t'||*p=='\r'))++p;return p;}
    static const char* word(const char* p,const char* end) noexcept {while(p<end && *p!=' '&&*p!='\t'&&*p!='\r')++p;return p;}
    static bool integer(const char* a,const char* b,long lo,long hi,long& out) noexcept {
        if(a==b || b-a>11)return false;
        char buffer[12];std::memcpy(buffer,a,std::size_t(b-a));buffer[b-a]=0;
        char* stop=nullptr;out=std::strtol(buffer,&stop,10);
        return stop==buffer+(b-a) && out>=lo && out<=hi;
    }
    bool line(const char* p,const char* end) noexcept {
        p=skip(p,end);
        if(p==end || *p=='#')return true;
        const char* w=word(p,end);
        const std::size_t n=std::size_t(w-p);
        const char* fields[3]{};const char* ends[3]{};unsigned k=0;
        for(const char* q=skip(w,end);q<end && k<3;q=skip(ends[k++],end)){fields[k]=q;ends[k]=word(q,end);}
        if(skip(k?ends[k-1]:w,end)!=end || count>=MaxSteps)return false;
        InputStep s;long v=0;
        if(n==4 && std::strncmp(p,"wait",4)==0) {
            if(k!=1 || !integer(fields[0],ends[0],1,100000,v))return false;
            s.reads=uint32_t(v);
        } else if(n==4 && std::strncmp(p,"hold",4)==0) {
            if(k!=2 || !integer(fields[1],ends[1],1,100000,v))return false;
            s.reads=uint32_t(v);
            for(const char* a=fields[0];a<ends[0];) {
                const char* plus=a;while(plus<ends[0] && *plus!='+')++plus;
                const uint32_t bit=button(a,std::size_t(plus-a));
                if(!bit || plus+1==ends[0])return false; // unknown name, or "A+" with nothing after
                s.buttons|=bit;a=plus<ends[0]?plus+1:plus;
            }
        } else if(n==5 && std::strncmp(p,"stick",5)==0) {
            long x=0,y=0;
            if(k!=3 || !integer(fields[0],ends[0],-128,127,x) || !integer(fields[1],ends[1],-128,127,y) ||
               !integer(fields[2],ends[2],1,100000,v))return false;
            s.x=int(x);s.y=int(y);s.reads=uint32_t(v);
        } else return false;
        steps[count++]=s;
        return true;
    }
};
}
