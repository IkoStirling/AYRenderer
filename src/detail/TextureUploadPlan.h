#pragma once

#include "AYResource/assetsDefs/ITexture.h"
#include <bgfx/bgfx.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ayt::render::detail {

// CPU preparation owns bytes until the adapter copies them. No GPU allocation,
// resource publication or mutation of the borrowed L2 asset occurs here.
struct TextureUploadPlan {
    static constexpr uint64_t byteLimit = 256ull * 1024u * 1024u;
    uint16_t width = 0, height = 0;
    bgfx::TextureFormat::Enum format = bgfx::TextureFormat::Unknown;
    uint8_t sourceMipCount = 0, mipCount = 0;
    uint64_t flags = BGFX_TEXTURE_NONE;
    std::vector<uint8_t> bytes;
    // Valid partial chains fall back to base-only rather than expose undefined
    // levels: bgfx's 2D creation API allocates either one or the full chain.
    bool partialChainFallback = false;
    bool samplerFallback = false;
    std::string diagnostic;
    bool isValid() const noexcept { return width && height && mipCount && !bytes.empty(); }
};

// Validate every supplied level before copying. Preserve encoded levels exactly;
// RGB8/BC4/BC5 sRGB views and non-block-aligned compressed images expand to
// RGBA8. Does not generate missing mip levels.
TextureUploadPlan prepareTextureUpload(const ayt::resource::ITexture& texture, bool srgb);

} // namespace ayt::render::detail
