module;
#include "native.hpp"

export module wavsen.video.apple;

import rstd;
import wavsen.video;

using namespace rstd::prelude;

export namespace wavsen::video
{

// Keep this owner alive until external GPU use of native_handle() has completed.
class MetalVideoTexture {
public:
    MetalVideoTexture() noexcept                                   = default;
    MetalVideoTexture(const MetalVideoTexture&)                    = delete;
    auto operator=(const MetalVideoTexture&) -> MetalVideoTexture& = delete;
    MetalVideoTexture(MetalVideoTexture&& other) noexcept;
    auto operator=(MetalVideoTexture&& other) noexcept -> MetalVideoTexture&;
    ~MetalVideoTexture();
    auto native_handle() const noexcept -> void*;

private:
    friend class MetalVideoAdapter;
    WavsenMetalTexture* state_ {};
};

// Call from one control thread; retire external GPU use before updating a texture.
class MetalVideoAdapter {
public:
    static auto create(void* device) -> Result<MetalVideoAdapter, Error>;
    MetalVideoAdapter(const MetalVideoAdapter&)                    = delete;
    auto operator=(const MetalVideoAdapter&) -> MetalVideoAdapter& = delete;
    MetalVideoAdapter(MetalVideoAdapter&& other) noexcept;
    auto operator=(MetalVideoAdapter&& other) noexcept -> MetalVideoAdapter&;
    ~MetalVideoAdapter();
    auto import_frame(const AppleFrameLease& frame) -> Result<MetalVideoTexture, Error>;
    auto update(const AppleFrameLease& frame, MetalVideoTexture& texture) -> Result<empty, Error>;

private:
    explicit MetalVideoAdapter(WavsenMetalAdapter* state) noexcept: state_(state) {}
    WavsenMetalAdapter* state_ {};
};

} // namespace wavsen::video
