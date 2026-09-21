#pragma once

#include "AYShader/ShaderResource.h"
#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

// Deferred opaque haze produces and publishes a full-resolution scene-linear
// color target through the renderer resource blackboard.
// Stable view/resource numeric ids remain unchanged; explicit view ordering
// places Lighting before this pass and transparent/bloom after it.
class DepthHazePass final : public RenderPass {
public:
    static constexpr uint8_t kDepthHazeViewId = 13;

    DepthHazePass() noexcept;

    std::string_view name() const override { return "DepthHaze"; }
    uint32_t execute(PassExecContext& ctx) override;

    bool isReady() const noexcept { return _program.isValid(); }
    bool producedThisFrame() const noexcept { return _producedThisFrame; }
    void resetFrameState() noexcept { _producedThisFrame = false; }

    // Legacy source-compatible accessors. The result is now FrameGraph-owned,
    // full resolution, and consumed through FgSemantic::HazeSource.
    uint16_t halfWidth() const noexcept { return 0; }
    uint16_t halfHeight() const noexcept { return 0; }
    bgfx::FrameBufferHandle halfResFbo() const noexcept {
        return BGFX_INVALID_HANDLE;
    }

    void destroyResources(BGFXAdapter& adapter);

private:
    bgfx::VertexBufferHandle _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle _fullscreenIB = BGFX_INVALID_HANDLE;
    ayt::shader::ShaderResource _program;

    ayt::shader::BindingId _uHazeDensity = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uHazeStrength = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uHazeColor = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uCamPos = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _tSceneColor = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _tWorldPosition = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _tGeometryCoverage = ayt::shader::InvalidBinding;

    bool _programAcquireFailed = false;
    bool _producedThisFrame = false;

    void ensureFullscreenQuad(BGFXAdapter& adapter);
    void ensureProgram(shader::ShaderResourcePool& pool);
};

extern const char* const kDepthHazeCacheKeyCStr;
std::string_view depthHazePhoskiaSourceForTests() noexcept;

} // namespace ayt::render::detail
