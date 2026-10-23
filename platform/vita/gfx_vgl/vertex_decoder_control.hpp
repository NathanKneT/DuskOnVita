#pragma once

namespace twv::vgl {
// Startup-only selection. Failure cannot be undone after decoding has begun.
class VertexDecoderControl {
    bool compiled_,selected_=false,started_=false,enabled_=false;
public:
    explicit constexpr VertexDecoderControl(bool compiled) : compiled_(compiled) {}
    bool select(int enabled) noexcept {
        if((enabled!=0 && enabled!=1) || selected_ || started_ || (enabled && !compiled_))return false;
        selected_=true;enabled_=enabled;return true;
    }
    void begin() noexcept {started_=true;}
    void fail() noexcept {enabled_=false;started_=true;}
    bool enabled() const noexcept {return enabled_;}
    bool compiled() const noexcept {return compiled_;}
    bool selected() const noexcept {return selected_;}
};
static_assert(sizeof(VertexDecoderControl)<=4);
} // namespace twv::vgl
