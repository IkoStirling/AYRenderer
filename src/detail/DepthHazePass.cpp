#include "detail/DepthHazePass.h"

#include "AYRenderer/DepthHazeShaderSources.h"
#include "detail/BGFXAdapter.h"
#include "detail/FgResource.h"
#include "detail/FrameContext.h"
#include "detail/GBufferPass.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"

#include <cstdio>
#include <string>

namespace ayt::render::detail
{

namespace {

struct alignas(16) FullscreenVertex {
    float x;
    float y;
    float u;
    float v;
};

constexpr FullscreenVertex kFullscreenTriangle[3] = {
    {-1.0f, -1.0f, 0.0f, 1.0f},
    { 3.0f, -1.0f, 2.0f, 1.0f},
    {-1.0f,  3.0f, 0.0f, -1.0f},
};
constexpr uint16_t kFullscreenIndices[3] = {0, 1, 2};

} // namespace

const char* const kDepthHazeCacheKeyCStr = ayt::render::kDepthHazeCacheKey;

std::string_view depthHazePhoskiaSourceForTests() noexcept
{
    return ayt::render::kDepthHazePhoskiaSource;
}

DepthHazePass::DepthHazePass() noexcept
    : _producedThisFrame(false)
{
}

uint32_t DepthHazePass::execute(PassExecContext& ctx)
{
    _producedThisFrame = false;

    BGFXAdapter& adapter = ctx.adapter;
    shader::ShaderResourcePool& pool = ctx.pool;
    const FrameContext& frame = ctx.frame;
    if (!adapter.isInitialized() || adapter.isNoopBackend()
        || ctx.frameGraph == nullptr
        || ctx.viewportWidth == 0 || ctx.viewportHeight == 0) {
        return 0;
    }

    const bgfx::FrameBufferHandle target =
        ctx.frameGraph->resolve(FgResourceId::HazeColor);
    if (!BGFXAdapter::isValid(target)) {
        return 0;
    }

    // resetFrameState() makes selectSourceFbo return the underlying scene here;
    // later consumers select HazeSource only after this submit succeeds.
    const bgfx::FrameBufferHandle sourceFbo =
        PostProcessPass::selectSourceFbo(ctx);
    if (!BGFXAdapter::isValid(sourceFbo)) {
        return 0;
    }
    const bgfx::TextureHandle sceneColor =
        adapter.getFboAttachment(sourceFbo, 0);
    if (!BGFXAdapter::isValid(sceneColor)
        || ctx.gbufferPass == nullptr
        || !ctx.gbufferPass->producedThisFrame()) {
        return 0;
    }

    const bgfx::TextureHandle worldPosition =
        ctx.gbufferPass->gbufferWorldPositionRt();
    const bgfx::TextureHandle geometryCoverage =
        ctx.gbufferPass->gbufferMaterialRt();
    if (!BGFXAdapter::isValid(worldPosition)
        || !BGFXAdapter::isValid(geometryCoverage)) {
        return 0;
    }

    ensureFullscreenQuad(adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        return 0;
    }
    ensureProgram(pool);
    const bool programReady = _program.isValid()
        && _uHazeDensity != shader::InvalidBinding
        && _uHazeStrength != shader::InvalidBinding
        && _uHazeColor != shader::InvalidBinding
        && _uCamPos != shader::InvalidBinding
        && _tSceneColor != shader::InvalidBinding
        && _tWorldPosition != shader::InvalidBinding
        && _tGeometryCoverage != shader::InvalidBinding;
    if (!programReady) {
        return 0;
    }

    constexpr uint8_t viewId = kDepthHazeViewId;
    const ayt::math::Float4x4 identity = ayt::math::Float4x4::identity();
    adapter.setViewFrameBuffer(viewId, target);
    adapter.setViewRect(viewId, 0, 0, ctx.viewportWidth, ctx.viewportHeight);
    adapter.setViewTransform(viewId, identity, identity);
    adapter.setViewClearRaw(viewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);

    const auto bindTexture = [&](ayt::shader::BindingId binding,
                                 bgfx::TextureHandle texture) {
        _program.setTexture(_program.getTextureStage(binding), binding,
                            toShaderTexture(texture));
    };
    bindTexture(_tSceneColor, sceneColor);
    bindTexture(_tWorldPosition, worldPosition);
    bindTexture(_tGeometryCoverage, geometryCoverage);

    const float density[4] = {frame.hazeDensity, 0.0f, 0.0f, 0.0f};
    const float strength[4] = {frame.hazeStrength, 0.0f, 0.0f, 0.0f};
    const float color[4] = {
        frame.hazeColor.x, frame.hazeColor.y, frame.hazeColor.z, 0.0f};
    const float camera[4] = {
        frame.cameraPosition.x, frame.cameraPosition.y,
        frame.cameraPosition.z, 0.0f};
    _program.setUniform(_uHazeDensity, density, sizeof(density));
    _program.setUniform(_uHazeStrength, strength, sizeof(strength));
    _program.setUniform(_uHazeColor, color, sizeof(color));
    _program.setUniform(_uCamPos, camera, sizeof(camera));

    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    adapter.setIndexBuffer(_fullscreenIB, 0, 3);
    adapter.setStateDepthTestAlways();
    ayt::shader::DrawCallContext submit;
    submit.viewId = viewId;
    submit.state = 0;
    _program.submit(submit);
    _producedThisFrame = true;
    return 1;
}

void DepthHazePass::ensureFullscreenQuad(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();
    _fullscreenVB = adapter.createVertexBuffer(
        kFullscreenTriangle, sizeof(kFullscreenTriangle), layout,
        BGFX_BUFFER_NONE);
    _fullscreenIB = adapter.createIndexBuffer(
        kFullscreenIndices, sizeof(kFullscreenIndices), BGFX_BUFFER_NONE);
}

void DepthHazePass::ensureProgram(shader::ShaderResourcePool& pool)
{
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != ayt::render::kDepthHazeCacheKey) {
        _program.reset();
        _programAcquireFailed = false;
        s_acquiredCacheKey = ayt::render::kDepthHazeCacheKey;
    }
    if (_program.isValid() || _programAcquireFailed) {
        return;
    }
    ayt::shader::ShaderResource acquired = pool.acquire(
        ayt::render::kDepthHazePhoskiaSource,
        ayt::render::kDepthHazeCacheKey);
    if (!acquired.isValid()) {
        _programAcquireFailed = true;
        std::fprintf(stderr,
                     "[DepthHazePass] shader acquire failed; pass skipped\n");
        for (const std::string& error : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[DepthHazePass]   %s\n", error.c_str());
        }
        return;
    }
    _program = acquired;
    _uHazeDensity = _program.getUniformBinding("hazeDensity");
    _uHazeStrength = _program.getUniformBinding("hazeStrength");
    _uHazeColor = _program.getUniformBinding("hazeColor");
    _uCamPos = _program.getUniformBinding("camPos");
    _tSceneColor = _program.getTextureBinding("sceneColor");
    _tWorldPosition = _program.getTextureBinding("worldPosition");
    _tGeometryCoverage = _program.getTextureBinding("geometryCoverage");
}

void DepthHazePass::destroyResources(BGFXAdapter& adapter)
{
    _producedThisFrame = false;
    if (BGFXAdapter::isValid(_fullscreenVB)) {
        adapter.destroy(_fullscreenVB);
        _fullscreenVB = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_fullscreenIB)) {
        adapter.destroy(_fullscreenIB);
        _fullscreenIB = BGFX_INVALID_HANDLE;
    }
    _program.reset();
    _uHazeDensity = shader::InvalidBinding;
    _uHazeStrength = shader::InvalidBinding;
    _uHazeColor = shader::InvalidBinding;
    _uCamPos = shader::InvalidBinding;
    _tSceneColor = shader::InvalidBinding;
    _tWorldPosition = shader::InvalidBinding;
    _tGeometryCoverage = shader::InvalidBinding;
    _programAcquireFailed = false;
}

} // namespace ayt::render::detail
