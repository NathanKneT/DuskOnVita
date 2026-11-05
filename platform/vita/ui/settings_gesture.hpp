#pragma once
// The gesture that opens the settings page (ui/settings_page.hpp). It never uses a game button:
// the game reads the Vita's buttons and sticks, and the front panel only as a menu pointer (one
// finger); the rear panel is not read at all.
//   rear  (default)  two fingers held on the rear touch pad for 1 s: a pair of fingers that landed
//                    within 0.5 s of each other, under 65 % of the pad's width (60 % of its height)
//                    apart, neither sliding more than 10 % of the pad. Other fingers on the pad (a
//                    finger resting there while holding the Vita, a third one) are ignored: they
//                    neither count nor spoil. Fingers at both edges of the pad (holding the Vita,
//                    about 80 % apart) do not open it. Console b3: with stricter rules nothing opened
//                    (any other finger blocked every attempt); b3g: the player's natural pair was
//                    47-50 % of the pad's width apart.
//   front            three fingers on the screen within 0.5 s (Dusklight's own menu gesture).
// Fingers are in SDL's 0..1 panel coordinates; times in microseconds. Counters (rearStats) say why
// rear pairs did not open the page, for the receipts. SDK-free and host-tested
// (tests/settings_page_test.cpp).
#include <cstdint>

namespace twv::ui {

enum class TouchPanel : uint8_t { Front = 1, Rear = 2 }; // SDL's Vita touch IDs
enum class TouchPhase : uint8_t { Down, Motion, Up, Canceled };

struct GestureRules {
    uint64_t holdUs = 1000000;      // rear: hold time after the pair's second finger lands
    uint64_t landWindowUs = 500000; // both panels: fingers that land within this count together
    float rearMaxApartX = 0.65f, rearMaxApartY = 0.60f, rearMaxSlide = 0.10f;
};

// Rear pairs that ended without opening the page, by the first rule they broke (receipts).
struct RearGestureStats {
    unsigned downs = 0, pairs = 0, opened = 0, apart = 0, slid = 0, shortHold = 0, maxFingers = 0;
    float lastApartX = 0, lastApartY = 0, lastSlide = 0;
};

class OpenGestureRecogniser {
public:
    bool rear = true, front = false; // which gestures open the page
    GestureRules rules;
    RearGestureStats rearStats;

    void touch(TouchPanel panel, TouchPhase phase, uint64_t id, float x, float y, uint64_t nowUs) {
        if (panel == TouchPanel::Rear) {
            rear_touch(phase, id, x, y, nowUs);
            return;
        }
        Side& s = mFront;
        switch (phase) {
        case TouchPhase::Down: {
            if (s.count == 0) {
                s.spoiled = false;
                s.fired = false;
                s.first = nowUs;
                s.total = 0;
            }
            ++s.total;
            if (s.count >= MaxFingers) { s.spoiled = true; return; }
            if (s.total > 3 || (nowUs > s.first && nowUs - s.first > rules.landWindowUs)) s.spoiled = true;
            s.fingers[s.count++] = {id, x, y, nowUs, 0, false};
            if (front && !s.spoiled && !s.fired && s.count == 3) {
                s.fired = true;
                mPending = true;
            }
            return;
        }
        case TouchPhase::Motion: return;
        case TouchPhase::Up:
        case TouchPhase::Canceled:
            remove(s, id);
            if (s.count != 0) s.spoiled = true; // the next attempt starts when every finger is up
            return;
        }
    }

    // True once when a gesture has completed; call every frame.
    bool poll(uint64_t nowUs) {
        if (rear) {
            Side& r = mRear;
            for (unsigned i = 0; i < r.count; ++i)
                for (unsigned j = i + 1; j < r.count; ++j) {
                    Finger& a = r.fingers[i];
                    Finger& b = r.fingers[j];
                    if (a.fired || b.fired || !together(a, b) || broken(a, b) != Broken::None) continue;
                    const uint64_t second = a.down > b.down ? a.down : b.down;
                    if (nowUs >= second && nowUs - second >= rules.holdUs) {
                        a.fired = b.fired = true; // once, until one of the two lifts
                        ++rearStats.opened;
                        mPending = true;
                    }
                }
        }
        const bool fire = mPending;
        mPending = false;
        return fire;
    }

    // Fingers down now on a panel (the page ignores those that were down when it opened).
    unsigned down(TouchPanel panel) const { return panel == TouchPanel::Rear ? mRear.count : mFront.count; }
    // True once after every rear finger has lifted (the shell writes a receipt per attempt).
    bool take_rear_attempt_end() {
        const bool ended = mRearEnded;
        mRearEnded = false;
        return ended;
    }

    void reset();

private:
    static constexpr unsigned MaxFingers = 8;
    struct Finger {
        uint64_t id = 0;
        float x0 = 0, y0 = 0;
        uint64_t down = 0;
        float slide = 0; // largest distance from where it landed, per axis
        bool fired = false;
        float x = 0, y = 0;
    };
    struct Side {
        Finger fingers[MaxFingers]{};
        unsigned count = 0, total = 0; // front: down now; landed since the attempt began
        uint64_t first = 0;
        bool spoiled = false, fired = false;
    };
    enum class Broken : uint8_t { None, Apart, Slid };

    static float abs_of(float v) { return v < 0 ? -v : v; }
    bool together(const Finger& a, const Finger& b) const {
        const uint64_t gap = a.down > b.down ? a.down - b.down : b.down - a.down;
        return gap <= rules.landWindowUs;
    }
    Broken broken(const Finger& a, const Finger& b) const {
        if (abs_of(a.x - b.x) >= rules.rearMaxApartX || abs_of(a.y - b.y) >= rules.rearMaxApartY) return Broken::Apart;
        if (a.slide > rules.rearMaxSlide || b.slide > rules.rearMaxSlide) return Broken::Slid;
        return Broken::None;
    }
    static void remove(Side& s, uint64_t id) {
        for (unsigned i = 0; i < s.count; ++i)
            if (s.fingers[i].id == id) {
                s.fingers[i] = s.fingers[--s.count];
                return;
            }
    }

    void rear_touch(TouchPhase phase, uint64_t id, float x, float y, uint64_t nowUs) {
        Side& s = mRear;
        Finger* f = nullptr;
        for (unsigned i = 0; i < s.count; ++i)
            if (s.fingers[i].id == id) f = &s.fingers[i];
        switch (phase) {
        case TouchPhase::Down:
            ++rearStats.downs;
            if (f || s.count >= MaxFingers) return;
            s.fingers[s.count++] = {id, x, y, nowUs, 0, false, x, y};
            if (s.count > rearStats.maxFingers) rearStats.maxFingers = s.count;
            return;
        case TouchPhase::Motion:
            if (f) {
                f->x = x;
                f->y = y;
                const float d = abs_of(x - f->x0) > abs_of(y - f->y0) ? abs_of(x - f->x0) : abs_of(y - f->y0);
                if (d > f->slide) f->slide = d;
            }
            return;
        case TouchPhase::Up:
        case TouchPhase::Canceled:
            if (!f) return;
            // The pairs this finger made that did not open the page: why not.
            for (unsigned i = 0; i < s.count; ++i) {
                const Finger& o = s.fingers[i];
                if (&o == f || !together(*f, o)) continue;
                ++rearStats.pairs;
                rearStats.lastApartX = abs_of(f->x - o.x);
                rearStats.lastApartY = abs_of(f->y - o.y);
                rearStats.lastSlide = f->slide > o.slide ? f->slide : o.slide;
                if (f->fired && o.fired) continue;
                switch (broken(*f, o)) {
                case Broken::Apart: ++rearStats.apart; break;
                case Broken::Slid: ++rearStats.slid; break;
                case Broken::None: ++rearStats.shortHold; break;
                }
            }
            remove(s, id);
            if (s.count == 0) mRearEnded = true;
            return;
        }
    }

    Side mRear, mFront;
    bool mPending = false, mRearEnded = false;
};

// Outside the class: GCC 13 cannot use Side's default member initializers before the enclosing class
// is complete.
inline void OpenGestureRecogniser::reset() {
    mRear = Side{};
    mFront = Side{};
    mPending = false;
    mRearEnded = false;
}

} // namespace twv::ui
