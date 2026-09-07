#pragma once

#include "detail/PassExecContext.h"
#include "detail/FullscreenPassGeometry.h"
#include "detail/RenderPass.h"
#include "detail/SceneLighting.h"
#include "AYRenderer/RenderTypes.h"  // ayt::render::BlendMode
#include "AYShader/ShaderResource.h"

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

// Selection mask alpha stores the full silhouette while RGB stores its
// depth-visible coverage. Screen-space dilation uses alpha for the original
// projected border and RGB only to suppress occluded segments. The border is
// fixed-width without outlining unrelated foreground occluders.
uint64_t selectionMaskState(bool doubleSided,
                            bool reverseWinding) noexcept;
uint64_t selectionVisibleMaskState(bool doubleSided,
                                   bool reverseWinding) noexcept;
uint64_t selectionCompositeState() noexcept;
const char* selectionOutlinePhoskiaSourceForTests() noexcept;

// Forward path composites into sceneFbo after ForwardOpaque. Deferred path
// uses a dedicated view and a cached FBO that borrows LightingOutput color and
// GBuffer depth. Selection borrows that depth for the RGB visibility channel
// while alpha remains the complete silhouette. The mask projection exactly
// matches the borrowed jittered depth; its UV is realigned and the fixed-width
// outer edge is composited on the backbuffer after Present, so TAA never
// accumulates editor chrome. SceneLights
// and shadow-atlas arrays share the same CPU packing contract as LightingPass.
class TransparentPass : public RenderPass {
public:
    static constexpr uint8_t kSelectionMaskViewId = 253;
    static constexpr uint8_t kSelectionCompositeViewId = 254;

    std::string_view name() const override { return "Transparent"; }

    uint32_t execute(PassExecContext& ctx) override;
    void destroyResources(BGFXAdapter& adapter) noexcept;
    void setSelectionProjectionJitter(float xPixels,
                                      float yPixels) noexcept {
        _selectionJitterXPixels = xPixels;
        _selectionJitterYPixels = yPixels;
    }

private:
    enum class SubmitMode : uint8_t {
        TransparentSurface,
        SelectionMask,
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
    bool ensureSelectionResources(PassExecContext& ctx,
                                  bgfx::TextureHandle sceneDepth);
    void destroySelectionTarget(BGFXAdapter& adapter) noexcept;
    void ensureSelectionCompositeProgram(shader::ShaderResourcePool& pool);

    bgfx::FrameBufferHandle _deferredCompositeFbo =
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle _deferredColor =
        bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle _deferredDepth =
        bgfx::TextureHandle{BGFX_INVALID_HANDLE};

    bgfx::FrameBufferHandle _selectionMaskFbo =
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle _selectionMaskTexture =
        bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle _selectionSceneDepth =
        bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    uint16_t _selectionWidth = 0;
    uint16_t _selectionHeight = 0;

    FullscreenPassGeometry _selectionCompositeGeometry;
    ayt::shader::ShaderResource _selectionCompositeProgram;
    ayt::shader::BindingId _selectionMaskBinding =
        ayt::shader::InvalidBinding;
    ayt::shader::BindingId _selectionTexelSizeBinding =
        ayt::shader::InvalidBinding;
    uint16_t _selectionProgramRetryFrames = 0;
    float _selectionJitterXPixels = 0.0f;
    float _selectionJitterYPixels = 0.0f;
};

} // namespace ayt::render::detail
