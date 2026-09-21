#pragma once

#include "AYShader/ShaderResource.h"
#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

// Deferred-only screen-space ambient-occlusion producer.
//
// Ownership:
//   - this pass owns its fullscreen VB/IB and ShaderResource handle;
//   - FrameGraph owns SSAOTexture;
//   - GBuffer owns world-position, normal and geometry-coverage inputs.
//
// Execution order is GBuffer -> SSAO -> Lighting. A successful submit
// publishes the transient occlusion texture through the resource blackboard;
// Lighting applies it to StandardLit ambient/IBL only.
class SSAOPass final : public RenderPass {
public:
    // Stable ABI/debug id. RenderViewOrder places this between GBuffer view 7
    // and Lighting view 8 even though the numeric id remains append-only.
    static constexpr uint8_t kSsaoViewId = 14;

    SSAOPass() = default;
    ~SSAOPass() override = default;

    std::string_view name() const override { return "SSAO"; }
    uint32_t execute(PassExecContext& ctx) override;

    bool isReady() const noexcept { return _program.isValid(); }
    bool producedThisFrame() const noexcept { return _producedThisFrame; }
    void resetFrameState() noexcept { _producedThisFrame = false; }

    // Must run while the adapter is alive, before pipeline.clear() or adapter
    // shutdown. The destructor intentionally does not touch bgfx handles.
    void destroyResources(BGFXAdapter& adapter);

private:
    bgfx::VertexBufferHandle _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle _fullscreenIB = BGFX_INVALID_HANDLE;
    ayt::shader::ShaderResource _program;

    ayt::shader::BindingId _uSSAORadius = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uSSAOBias = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _tWorldPosition = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _tWorldNormal = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _tGeometryCoverage = ayt::shader::InvalidBinding;

    bool _programAcquireFailed = false;
    bool _producedThisFrame = false;

    void ensureFullscreenQuad(BGFXAdapter& adapter);
    void ensureProgram(shader::ShaderResourcePool& pool);
};

extern const char* const kSSAOCacheKeyCStr;

} // namespace ayt::render::detail
