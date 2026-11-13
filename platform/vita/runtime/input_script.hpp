#pragma once
// Test-only scripted pad input (--vita-input-script=FILE, main_vita.cpp): drives a boot without a
// person at the pad (Vita3K boot-to-play checks). Each step lasts a number of pad reads (one per
// game tick); while the script runs it replaces controller 0, and once it ends the real pad is
// read again untouched. SDK-free and host-tested (tests/input_script_test.cpp).
//   wait N                 no input for N reads
//   hold A+START N         buttons held for N reads (A B X Y Z L R START UP DOWN LEFT RIGHT)
//   stick X Y N            main stick, raw -128..127, for N reads
//   warp STAGE ROOM POINT LAYER
//                          one read, no input: asks the game for that stage the way a loading zone
//                          does (dComIfGp_setNextStage), e.g. "warp F_SP103 0 23 -1" (stage tours)
//   darkclear N            one read, no input: marks twilight N (0 Faron, 1 Eldin, 2 Lanayru, 3) cleared
//                          in the running save (dComIfGs_onDarkClearLV), so the next stage loads in
//                          the light world; a test fixture for tours, never in a player's script
//   loop                   back to the first step; the run then ends only by --vita-exit-after
//   page open              (one pad read, no input) asks for the settings page, as its gesture does
//   page gesture           (one pad read, no input) puts two fingers on the rear pad, held still: the
//                          page's own gesture recogniser opens it 1 s later (pad reads wait meanwhile)
//   page KEY               one key on the open settings page, read by the page, not by a pad read:
//                          UP DOWN LEFT RIGHT CONFIRM BACK DEFAULT START, L R (previous and next
//                          section), or SHOT (a PNG of the page)
//   page wait N            the open page shows N passes with no key (time for a window capture)
//   press A+B X Y N        buttons and the main stick together for N reads (a roll, a jump attack)
//   give ITEM              (one pad read, no input) the game's own item-get for item number ITEM
//                          (execItemGet, d_item.cpp): a test fixture to try a passage without the
//                          hours before it, never in a player's script
//   equip SLOT PLACE       (one pad read, no input) puts inventory place PLACE (0..23) on button SLOT
//                          (0 or 1), as the item wheel does (dComIfGs_setSelectItemIndex)
//   tp X Y Z               (one pad read, no input) sets Link down at that point of the room
//   shot                   (one pad read, no input) the next presented frame is written as a PNG
//                          (captures/live-NNNN.png), whatever --vita-capture says
//   live                   (one pad read, no input) step-by-step play, for a person or a tool driving
//                          the emulator: from here on, each time the steps run out the game stops
//                          inside the pad read, writes where it is (receipt live_ready) and waits for
//                          the next command file, live/0001.txt, live/0002.txt, ... in the data folder;
//                          a file holds steps in this same syntax, or the single word "end"
//   # comment, blank lines
// While the settings page is open the game reads no pad, so "page KEY" steps cost no pad read and
// the pad steps after them run exactly as without the page.
// CROSS and CIRCLE name the Vita buttons: they press A or B as the confirm setting maps them
// (runtime/player_settings.hpp), so a script can drive menus with the X/O swap on or off.
#include "player_settings.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace twv::vita {
// GameCube PAD bits (dolphin/pad.h; input_script.cpp checks them against the SDK values).
constexpr uint32_t ScriptButtonLeft=0x1,ScriptButtonRight=0x2,ScriptButtonDown=0x4,ScriptButtonUp=0x8,
    ScriptTriggerZ=0x10,ScriptTriggerR=0x20,ScriptTriggerL=0x40,ScriptButtonA=0x100,ScriptButtonB=0x200,
    ScriptButtonX=0x400,ScriptButtonY=0x800,ScriptButtonStart=0x1000;
// Vita face buttons, resolved when the step is read (resolve_vita_buttons).
constexpr uint32_t ScriptVitaCross=0x10000,ScriptVitaCircle=0x20000;
inline uint32_t resolve_vita_buttons(uint32_t bits,bool circleConfirms) noexcept {
    const FaceButtons face=face_buttons(circleConfirms);
    uint32_t out=bits&0xFFFFu;
    if(bits&ScriptVitaCross)out|=face.cross;
    if(bits&ScriptVitaCircle)out|=face.circle;
    return out;
}

// Settings page keys (ui/settings_page.hpp Key bits, ui/settings_screen.hpp TwvPageShot; the host
// test checks they match), and the open request.
constexpr uint32_t ScriptPageUp=1u<<0,ScriptPageDown=1u<<1,ScriptPageLeft=1u<<2,ScriptPageRight=1u<<3,
    ScriptPageConfirm=1u<<4,ScriptPageBack=1u<<5,ScriptPageDefault=1u<<6,ScriptPageStart=1u<<7,
    ScriptPageSectionPrev=1u<<8,ScriptPageSectionNext=1u<<9,
    ScriptPageShot=1u<<16,ScriptPageWait=1u<<17,ScriptPageGesture=1u<<30,ScriptPageOpen=1u<<31;
// Steps a pad read consumes ("page open", "page gesture"); the other page steps are the page's.
constexpr bool script_pad_page_step(uint32_t page) noexcept {return page==ScriptPageOpen || page==ScriptPageGesture;}

constexpr uint8_t StepPad=0,StepWarp=1,StepLoop=2,StepDarkClear=3,StepLive=4,StepShot=5,StepGive=6,StepEquip=7,StepTp=8;
struct InputStep {uint32_t reads=0,buttons=0;int x=0,y=0;uint8_t kind=StepPad;char stage[8]{};int room=0,point=0,layer=0;
                  uint32_t page=0;};

struct InputScript {
    static constexpr unsigned MaxSteps=2048; // stage tours: about six steps per place
    InputStep steps[MaxSteps];
    unsigned count=0,index=0,used=0,loops=0;

    static uint32_t button(const char* name,std::size_t n) noexcept {
        struct Name {const char* text;uint32_t bit;};
        static constexpr Name names[]={{"A",ScriptButtonA},{"B",ScriptButtonB},{"X",ScriptButtonX},{"Y",ScriptButtonY},
            {"Z",ScriptTriggerZ},{"L",ScriptTriggerL},{"R",ScriptTriggerR},{"START",ScriptButtonStart},
            {"UP",ScriptButtonUp},{"DOWN",ScriptButtonDown},{"LEFT",ScriptButtonLeft},{"RIGHT",ScriptButtonRight},
            {"CROSS",ScriptVitaCross},{"CIRCLE",ScriptVitaCircle}};
        for(const auto& b:names)if(std::strlen(b.text)==n && std::strncmp(b.text,name,n)==0)return b.bit;
        return 0;
    }
    // False, and no steps, on any malformed line or more than MaxSteps steps.
    bool parse(const char* text) noexcept {
        count=index=used=loops=0;
        if(!text)return false;
        while(*text) {
            const char* end=text;while(*end && *end!='\n')++end;
            if(!line(text,end)){count=0;return false;}
            text=*end?end+1:end;
        }
        return count>0;
    }
    // The step for this pad read, or nullptr once the script has ended. "loop" is never returned: it
    // restarts at step 0, which line() guarantees lasts at least one read.
    const InputStep* next() noexcept {
        advance();
        if(index>=count)return nullptr;
        ++used;return &steps[index];
    }
    // The next step is the settings page's (a key or a page wait): a pad read leaves it for the page.
    bool page_pending() noexcept {
        advance();
        return index<count && steps[index].page && !script_pad_page_step(steps[index].page);
    }
    // The settings page's next key, consumed, when the next step is a "page KEY" step; else 0.
    // A "page wait N" step answers ScriptPageWait N times (one per page pass).
    uint32_t page_key() noexcept {
        advance();
        if(index>=count || !steps[index].page || script_pad_page_step(steps[index].page))return 0;
        if(steps[index].page==ScriptPageWait){++used;return ScriptPageWait;}
        used=steps[index].reads;
        return steps[index].page;
    }
private:
    // Past finished steps; "loop" restarts at step 0.
    void advance() noexcept {
        while(index<count && (steps[index].kind==StepLoop || used>=steps[index].reads)) {
            if(steps[index].kind==StepLoop){index=0;used=0;++loops;continue;}
            ++index;used=0;
        }
    }
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
        const char* fields[4]{};const char* ends[4]{};unsigned k=0;
        for(const char* q=skip(w,end);q<end && k<4;q=skip(ends[k++],end)){fields[k]=q;ends[k]=word(q,end);}
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
        } else if(n==5 && std::strncmp(p,"press",5)==0) {
            long x=0,y=0;
            if(k!=4 || !integer(fields[1],ends[1],-128,127,x) || !integer(fields[2],ends[2],-128,127,y) ||
               !integer(fields[3],ends[3],1,100000,v))return false;
            s.x=int(x);s.y=int(y);s.reads=uint32_t(v);
            for(const char* a=fields[0];a<ends[0];) {
                const char* plus=a;while(plus<ends[0] && *plus!='+')++plus;
                const uint32_t bit=button(a,std::size_t(plus-a));
                if(!bit || plus+1==ends[0])return false;
                s.buttons|=bit;a=plus<ends[0]?plus+1:plus;
            }
        } else if(n==4 && std::strncmp(p,"give",4)==0) {
            if(k!=1 || !integer(fields[0],ends[0],0,255,v))return false;
            s.kind=StepGive;s.reads=1;s.room=int(v);
        } else if(n==5 && std::strncmp(p,"equip",5)==0) {
            long place=0;
            if(k!=2 || !integer(fields[0],ends[0],0,1,v) || !integer(fields[1],ends[1],0,23,place))return false;
            s.kind=StepEquip;s.reads=1;s.room=int(v);s.point=int(place);
        } else if(n==2 && std::strncmp(p,"tp",2)==0) {
            long x=0,y=0,z=0;
            if(k!=3 || !integer(fields[0],ends[0],-1000000,1000000,x) || !integer(fields[1],ends[1],-1000000,1000000,y) ||
               !integer(fields[2],ends[2],-1000000,1000000,z))return false;
            s.kind=StepTp;s.reads=1;s.room=int(x);s.point=int(y);s.layer=int(z);
        } else if(n==4 && (std::strncmp(p,"live",4)==0 || std::strncmp(p,"shot",4)==0)) {
            if(k!=0)return false;
            s.kind=*p=='l'?StepLive:StepShot;s.reads=1;
        } else if(n==4 && std::strncmp(p,"warp",4)==0) {
            // Stage names are at most 7 characters (dStage_nextStage_c); room and layer are s8, point s16.
            long room=0,point=0,layer=0;
            const std::size_t length=k==4?std::size_t(ends[0]-fields[0]):0;
            if(k!=4 || length==0 || length>7 || !integer(fields[1],ends[1],0,127,room) ||
               !integer(fields[2],ends[2],-4,32767,point) || !integer(fields[3],ends[3],-1,15,layer))return false;
            for(std::size_t i=0;i<length;++i) {
                const char c=fields[0][i];
                if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'))return false;
                s.stage[i]=c;
            }
            s.kind=StepWarp;s.reads=1;s.room=int(room);s.point=int(point);s.layer=int(layer);
        } else if(n==9 && std::strncmp(p,"darkclear",9)==0) {
            if(k!=1 || !integer(fields[0],ends[0],0,3,v))return false;
            s.kind=StepDarkClear;s.reads=1;s.layer=int(v);
        } else if(n==4 && std::strncmp(p,"loop",4)==0) {
            if(k!=0 || count==0)return false; // a first-step loop would never read the pad
            s.kind=StepLoop;
        } else if(n==4 && std::strncmp(p,"page",4)==0) {
            struct Name {const char* text;uint32_t bit;};
            static constexpr Name keys[]={{"open",ScriptPageOpen},{"gesture",ScriptPageGesture},{"UP",ScriptPageUp},{"DOWN",ScriptPageDown},
                {"LEFT",ScriptPageLeft},{"RIGHT",ScriptPageRight},{"CONFIRM",ScriptPageConfirm},{"BACK",ScriptPageBack},
                {"DEFAULT",ScriptPageDefault},{"START",ScriptPageStart},{"SHOT",ScriptPageShot},
                {"L",ScriptPageSectionPrev},{"R",ScriptPageSectionNext}};
            if(k==2 && ends[0]-fields[0]==4 && std::strncmp(fields[0],"wait",4)==0) {
                if(!integer(fields[1],ends[1],1,100000,v))return false;
                s.page=ScriptPageWait;s.reads=uint32_t(v);
                steps[count++]=s;
                return true;
            }
            if(k!=1)return false;
            const std::size_t m=std::size_t(ends[0]-fields[0]);
            for(const auto& key:keys)if(std::strlen(key.text)==m && std::strncmp(key.text,fields[0],m)==0)s.page=key.bit;
            if(!s.page)return false;
            s.reads=1;
        } else return false;
        steps[count++]=s;
        return true;
    }
};
}
