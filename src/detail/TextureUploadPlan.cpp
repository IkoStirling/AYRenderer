#include "detail/TextureUploadPlan.h"
#include "detail/TextureImageLoader.h"

#include <algorithm>
#include <cmath>

namespace ayt::render::detail {
namespace {
using ayt::resource::TextureFilter;
using ayt::resource::TextureFormat;
using ayt::resource::TextureWrap;

bool describeFormat(TextureFormat source, bgfx::TextureFormat::Enum& gpu,
                    uint32_t& pixelBytes, uint32_t& blockBytes) {
    pixelBytes = blockBytes = 0;
    switch (source) {
    case TextureFormat::RGBA8: gpu = bgfx::TextureFormat::RGBA8; pixelBytes = 4; return true;
    case TextureFormat::RGB8: gpu = bgfx::TextureFormat::RGB8; pixelBytes = 3; return true;
    case TextureFormat::BC1: gpu = bgfx::TextureFormat::BC1; blockBytes = 8; return true;
    case TextureFormat::BC3: gpu = bgfx::TextureFormat::BC3; blockBytes = 16; return true;
    case TextureFormat::BC4: gpu = bgfx::TextureFormat::BC4; blockBytes = 8; return true;
    case TextureFormat::BC5: gpu = bgfx::TextureFormat::BC5; blockBytes = 16; return true;
    case TextureFormat::BC7: gpu = bgfx::TextureFormat::BC7; blockBytes = 16; return true;
    default: return false;
    }
}

void note(TextureUploadPlan& plan, const char* text) {
    if (!plan.diagnostic.empty()) plan.diagnostic += "; ";
    plan.diagnostic += text;
}

uint64_t wrapFlag(TextureWrap wrap, unsigned shift, TextureUploadPlan& plan) {
    uint64_t value = 0;
    switch (wrap) {
    case TextureWrap::Repeat: break;
    case TextureWrap::Mirror: value = BGFX_SAMPLER_U_MIRROR; break;
    case TextureWrap::Clamp: value = BGFX_SAMPLER_U_CLAMP; break;
    case TextureWrap::Border:
        // ITexture has no authored border color. Avoid a dependency on the
        // device-global bgfx palette: use a documented clamp fallback.
        value = BGFX_SAMPLER_U_CLAMP;
        plan.samplerFallback = true; note(plan, "border addressing uses clamp"); break;
    default:
        plan.samplerFallback = true; note(plan, "invalid addressing uses repeat"); break;
    }
    return value << shift;
}

void prepareSampler(const ayt::resource::TextureSampler& sampler, TextureUploadPlan& plan) {
    plan.flags |= wrapFlag(sampler.wrapU, BGFX_SAMPLER_U_SHIFT, plan)
        | wrapFlag(sampler.wrapV, BGFX_SAMPLER_V_SHIFT, plan)
        | wrapFlag(sampler.wrapW, BGFX_SAMPLER_W_SHIFT, plan);
    switch (sampler.minFilter) {
    case TextureFilter::None:
    case TextureFilter::Point: plan.flags |= BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MIP_POINT; break;
    case TextureFilter::Bilinear: plan.flags |= BGFX_SAMPLER_MIP_POINT; break;
    case TextureFilter::Trilinear: break;
    case TextureFilter::Anisotropic:
        if (std::isfinite(sampler.anisotropy) && sampler.anisotropy > 1) {
            plan.flags |= BGFX_SAMPLER_MIN_ANISOTROPIC;
            // bgfx exposes a device-wide maximum, not a per-texture 2x/4x/8x.
            plan.samplerFallback = true; note(plan, "anisotropy uses device maximum");
        } else {
            plan.samplerFallback = true; note(plan, "anisotropy <= 1 or nonfinite uses trilinear");
        }
        break;
    default:
        plan.flags |= BGFX_SAMPLER_MIP_POINT;
        plan.samplerFallback = true; note(plan, "invalid min filter uses bilinear"); break;
    }
    switch (sampler.magFilter) {
    case TextureFilter::None:
    case TextureFilter::Point:
        if (plan.flags & BGFX_SAMPLER_MIN_ANISOTROPIC) {
            plan.samplerFallback = true; note(plan,"anisotropic filtering uses linear magnification");
        } else plan.flags |= BGFX_SAMPLER_MAG_POINT;
        break;
    case TextureFilter::Bilinear: break;
    case TextureFilter::Trilinear:
    case TextureFilter::Anisotropic:
        plan.samplerFallback = true; note(plan, "magnification uses bilinear"); break;
    default:
        plan.samplerFallback = true; note(plan, "invalid mag filter uses bilinear"); break;
    }
    if (sampler.lodBias != 0) {
        plan.samplerFallback = true; note(plan, "LOD bias is unsupported and uses zero");
    }
}
} // namespace

TextureUploadPlan prepareTextureUpload(const ayt::resource::ITexture& texture, bool srgb) {
    TextureUploadPlan plan;
    const uint32_t width = texture.getWidth(), height = texture.getHeight();
    const uint32_t count = texture.getMipmapCount();
    if (!width || !height || width > UINT16_MAX || height > UINT16_MAX || !count) return {};
    uint32_t fullCount = 1;
    for (uint32_t side = std::max(width, height); side > 1; side >>= 1) ++fullCount;
    if (count > fullCount) return {};
    uint32_t pixelBytes = 0, blockBytes = 0;
    if (!describeFormat(texture.getFormat(), plan.format, pixelBytes, blockBytes)) return {};
    const bool useSrgb = srgb || texture.getSampler().sRGB;
    const bool nonAlignedBlocks = blockBytes && (width%4u || height%4u);
    const bool expandChannels = (useSrgb || nonAlignedBlocks) && (texture.getFormat() == TextureFormat::BC4
                                               || texture.getFormat() == TextureFormat::BC5);
    const bool expandColor = nonAlignedBlocks && !expandChannels;
    const bool expand = expandChannels || expandColor;
    // RGB8 has no portable sRGB device view. Preserve RGB and supply opaque
    // alpha explicitly, rather than submit unsupported RGB8+sRGB flags.
    const bool expandRgb = useSrgb && texture.getFormat() == TextureFormat::RGB8;
    const uint32_t usedCount = count == fullCount ? count : 1;
    uint64_t total = 0;
    uint32_t w = width, h = height;
    for (uint32_t level = 0; level < count; ++level) {
        const uint64_t expected = blockBytes
            ? uint64_t((w + 3u) / 4u) * ((h + 3u) / 4u) * blockBytes
            : uint64_t(w) * h * pixelBytes;
        if (expected > TextureUploadPlan::byteLimit || expected != texture.getMipmapSize(level)
            || !texture.getMipmapData(level)) return {};
        if (level < usedCount) {
            total += expand || expandRgb ? uint64_t(w) * h * 4u : expected;
            if (total > TextureUploadPlan::byteLimit) return {};
        }
        w = std::max(1u, w / 2u); h = std::max(1u, h / 2u);
    }
    plan.width = static_cast<uint16_t>(width); plan.height = static_cast<uint16_t>(height);
    plan.sourceMipCount = static_cast<uint8_t>(count);
    plan.mipCount = static_cast<uint8_t>(usedCount);
    plan.partialChainFallback = count > 1 && count < fullCount;
    if (plan.partialChainFallback) note(plan, "partial mip chain uses base level only");
    if (nonAlignedBlocks) note(plan,"non-block-aligned compressed texture uses RGBA8 to preserve exact dimensions and mip levels");
    plan.flags = useSrgb ? BGFX_TEXTURE_SRGB : BGFX_TEXTURE_NONE;
    prepareSampler(texture.getSampler(), plan);
    plan.bytes.reserve(static_cast<size_t>(total));
    w = width; h = height;
    for (uint32_t level = 0; level < usedCount; ++level) {
        if (expand) {
            auto image = expandChannels
                ? decodeUnsignedBcChannels(w,h,texture.getMipmapData(level),texture.getMipmapSize(level),texture.getFormat() == TextureFormat::BC5)
                : decodeBcColor(w,h,texture.getMipmapData(level),texture.getMipmapSize(level),
                    texture.getFormat() == TextureFormat::BC1 ? BcColorFormat::BC1
                        : texture.getFormat() == TextureFormat::BC3 ? BcColorFormat::BC3 : BcColorFormat::BC7);
            if (!image.isValid()) return {};
            plan.bytes.insert(plan.bytes.end(), image.rgba8.begin(), image.rgba8.end());
        } else if (expandRgb) {
            const auto* pixels = texture.getMipmapData(level);
            for (size_t i = 0; i < size_t(w)*h; ++i) {
                plan.bytes.insert(plan.bytes.end(),pixels+i*3,pixels+i*3+3);
                plan.bytes.push_back(255);
            }
        } else {
            const auto* begin = texture.getMipmapData(level);
            plan.bytes.insert(plan.bytes.end(), begin, begin + texture.getMipmapSize(level));
        }
        w = std::max(1u, w / 2u); h = std::max(1u, h / 2u);
    }
    if (expand || expandRgb) plan.format = bgfx::TextureFormat::RGBA8;
    return plan;
}

} // namespace ayt::render::detail
