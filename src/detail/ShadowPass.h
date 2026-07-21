#pragma once

#include "AYRenderScene.h"
#include "AYShaderResourcePool.h"
#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include "aymath/MathTypes.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

// PR-F1' (safe Shadow cut-2 slice, 2026-07-20) — depth caster with real
// directional light-space ortho. See docs/execution-plan.md §5.5:
// the blocked C' F1 (RenderScene Light + FrameContext shadowFbo/
// lightViewProj + default-enabled addPass) SIGSEGV'd; this cut keeps
// ALL shadow state on the pass itself and does NOT register into the
// default Renderer pipeline.
//
// Behavior:
//   - Depth-only FBO (createDepthOnlyFrameBuffer), size host-overridable.
//   - Light VP from FrameContext::lightDirection via
//     buildDirectionalShadowMatrices (fixed 50-unit radius at origin).
//   - lightView / lightProj / lightViewProj cached on this pass for F2
//     sampling (bypass getter — never written into FrameContext).
//   - Noop / uninitialized → 0 draws, isReady stays false.
//
// Explicit non-goals (still blocked / deferred):
//   - Default pipeline addPass (enabled or disabled) until §5.4 E4 alone
//     is recorded 3/3 PASS after the F1 rollback.
//   - FrameContext::shadowFbo / lightViewProj / Renderer lastFrameShadow*
//   - RenderScene Light struct / addLight
//   - Forward/Transparent sampling (PR-F2)
//   - Cascades, PCF, point/spot, Phoskia shadow_caster
class ShadowPass : public RenderPass {
public:
    static constexpr uint16_t kDefaultShadowMapSize = 1024;
    static constexpr float    kDefaultFrustumRadius = 50.0f;

    ShadowPass() = default;
    ~ShadowPass() override;

    std::string_view name() const override { return "Shadow"; }

    uint32_t execute(PassExecContext& ctx) override;

    bool isReady() const noexcept { return bgfx::isValid(_shadowFbo); }

    void setShadowMapSize(uint16_t size) noexcept { _requestedSize = size; }
    uint16_t shadowMapSize() const noexcept { return _requestedSize; }

    // F1' — last successfully built light-space matrices (identity until
    // the first non-Noop execute that creates an FBO). F2 sampling should
    // read these via a Renderer/pass getter, not FrameContext.
    const ayt::math::Float4x4& lightView() const noexcept { return _lightView; }
    const ayt::math::Float4x4& lightProj() const noexcept { return _lightProj; }
    const ayt::math::Float4x4& lightViewProj() const noexcept { return _lightViewProj; }

    bgfx::FrameBufferHandle shadowFbo() const noexcept { return _shadowFbo; }

    void destroyResources(BGFXAdapter& adapter);

private:
    void ensureShadowFbo(BGFXAdapter& adapter, uint16_t size);
    void ensureCasterProgram(ayt::shader::ShaderResourcePool& pool);

    bgfx::FrameBufferHandle    _shadowFbo        = BGFX_INVALID_HANDLE;
    uint16_t                   _shadowSize       = 0;
    uint16_t                   _requestedSize    = kDefaultShadowMapSize;

    ayt::math::Float4x4        _lightView        = ayt::math::Float4x4::identity();
    ayt::math::Float4x4        _lightProj        = ayt::math::Float4x4::identity();
    ayt::math::Float4x4        _lightViewProj    = ayt::math::Float4x4::identity();

    // PR-F3 (2026-07-21) — depth-only caster program with conditional
    // `castSkinned` segment so skinned + static geometry can share
    // one VS shader. Acquired lazily from the ShaderResourcePool
    // (mirrors PostProcessPass::ensureProgram); when pool has no
    // shaderc / compile fails, the pass falls back to the BGFXAdapter
    // INVALID-program submit path (records a draw but writes
    // nothing — same fallback shape as PostProcessPass does when its
    // post-process material fails to compile).
    ayt::shader::ShaderResource _caster;
    ayt::shader::BindingId      _casterSkeletonBinding = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _casterCastSkinned     = ayt::shader::InvalidBinding;
};

} // namespace ayt::render::detail
