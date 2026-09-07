#pragma once

#include "detail/FullscreenPassGeometry.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include "AYShader/ShaderResource.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace ayt::render::detail
{

// Display-referred SMAA 1x High. The pass owns the canonical three stages:
// color/luma edge detection, orthogonal + diagonal pattern blend-weight
// calculation, and neighborhood blending. Full Area/Search lookups are
// generated from the reference algorithms, avoiding loose texture files.
class SMAAPass final : public RenderPass {
public:
    // Reclaimed from the retained-UI offscreen range. RenderViewOrder places
    // these views after FXAA and before ColorGrading/Present regardless of the
    // numeric ids.
    static constexpr uint8_t kEdgeViewId = 247;
    static constexpr uint8_t kBlendWeightViewId = 248;
    static constexpr uint8_t kNeighborhoodViewId = 249;

    static constexpr uint16_t kAreaTextureWidth = 160;
    static constexpr uint16_t kAreaTextureHeight = 560;
    static constexpr uint16_t kSearchTextureWidth = 64;
    static constexpr uint16_t kSearchTextureHeight = 16;
    // Quarter-pixel endpoint decoding depends on bilinear interpolation.
    static constexpr bool kIntermediatePointSampled = false;
    static constexpr float kEdgeThreshold = 0.10f;
    static constexpr float kLocalContrastAdaptation = 2.0f;
    static constexpr uint8_t kMaxSearchSteps = 16;
    static constexpr uint8_t kMaxDiagonalSearchSteps = 8;
    static constexpr float kCornerRounding = 0.25f;

    std::string_view name() const override { return "SMAA"; }
    uint32_t execute(PassExecContext& ctx) override;

    bool isReady() const noexcept;
    void destroyResources(BGFXAdapter& adapter);

private:
    bool ensureLookupTextures(BGFXAdapter& adapter);
    void ensurePrograms(shader::ShaderResourcePool& pool);

    FullscreenPassGeometry _geometry;
    ayt::shader::ShaderResource _edgeProgram;
    ayt::shader::ShaderResource _weightProgram;
    ayt::shader::ShaderResource _neighborhoodProgram;
    bgfx::TextureHandle _areaTexture = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle _searchTexture = BGFX_INVALID_HANDLE;

    ayt::shader::BindingId _edgeInputColor = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _edgeMetrics = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _edgeParams = ayt::shader::InvalidBinding;

    ayt::shader::BindingId _weightEdges = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _weightArea = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _weightSearch = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _weightMetrics = ayt::shader::InvalidBinding;

    ayt::shader::BindingId _neighborhoodColor = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _neighborhoodBlend = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _neighborhoodMetrics = ayt::shader::InvalidBinding;

    uint16_t _programRetryFrames = 0;
    bool _firstDispatchLogged = false;
};

extern const char* const kSmaaEdgeCacheKeyCStr;
extern const char* const kSmaaWeightCacheKeyCStr;
extern const char* const kSmaaNeighborhoodCacheKeyCStr;
const char* smaaVaryingScForTests() noexcept;
const char* smaaVertexScForTests() noexcept;
const char* smaaEdgeFragmentScForTests() noexcept;
const char* smaaWeightFragmentScForTests() noexcept;
const char* smaaNeighborhoodFragmentScForTests() noexcept;
std::vector<uint8_t> generateSmaaAreaTextureRg8();
std::vector<uint8_t> generateSmaaSearchTextureR8();

} // namespace ayt::render::detail
