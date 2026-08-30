#pragma once

#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"
#include "detail/SceneLighting.h"
#include "AYRenderer/RenderTypes.h"  // ayt::render::BlendMode

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace ayt::render::detail
{

struct TransparentSortEntry {
    const DrawItem* item = nullptr;
    float distanceSquared = 0.0f;
};

float transparentDistanceSquared(const DrawItem& item,
                                 const ayt::math::FVector3& cameraPosition) noexcept;
bool transparentSortBefore(const TransparentSortEntry& a,
                           const TransparentSortEntry& b) noexcept;

enum class TransparentRoute : uint8_t {
    Skip,
    Forward,
    Deferred,
};

TransparentRoute selectTransparentRoute(bool hasGBufferPass,
                                        bool hasLightingPass,
                                        bool gbufferProduced,
                                        bool lightingProduced,
                                        bool lightingFboValid) noexcept;

uint64_t transparentDrawState(BlendMode blendMode,
                              bool premultipliedAlpha,
                              bool doubleSided,
                              bool reverseWinding) noexcept;

// Selection is rendered while the scene color and depth attachments are
// still bound.  The original surface first seeds depth when necessary, then
// an expanded back-face hull draws only where it is nearer than scene depth.
uint64_t selectionDepthState(bool doubleSided,
                             bool reverseWinding) noexcept;
uint64_t selectionHullState(bool reverseWinding) noexcept;
ayt::math::Float4x4 makeSelectionHullWorld(
    const DrawItem& item,
    float expansion = 1.025f) noexcept;

// Forward path composites into sceneFbo after ForwardOpaque. Deferred path
// uses a dedicated view and a cached FBO that borrows LightingOutput color and
// GBuffer depth. Normal transparent surfaces never write depth; a selected
// surface uses a local depth prepass before its inverted hull. SceneLights and
// shadow-atlas arrays share the same CPU packing contract as LightingPass.
class TransparentPass : public RenderPass {
public:
    std::string_view name() const override { return "Transparent"; }

    uint32_t execute(PassExecContext& ctx) override;
    void destroyResources(BGFXAdapter& adapter) noexcept;

private:
    enum class SubmitMode : uint8_t {
        TransparentSurface,
        SelectionDepth,
        SelectionHull,
    };

    struct SubmitResult {
        bool accepted = false;
        bool skip = false;
        uint32_t drawnIndexCount = 0;
    };

    static SubmitResult submitItem(BGFXAdapter& adapter,
                                   PassExecContext& ctx,
                                   const FrameContext& frame,
                                   const DrawItem& item,
                                   uint8_t viewId,
                                   const PackedSceneLighting& lights,
                                   const PackedShadowAtlas& shadows,
                                   SubmitMode mode);

    bgfx::FrameBufferHandle ensureDeferredCompositeFbo(
        BGFXAdapter& adapter,
        bgfx::TextureHandle color,
        bgfx::TextureHandle depth);

    bgfx::FrameBufferHandle _deferredCompositeFbo =
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle _deferredColor =
        bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle _deferredDepth =
        bgfx::TextureHandle{BGFX_INVALID_HANDLE};
};

} // namespace ayt::render::detail
