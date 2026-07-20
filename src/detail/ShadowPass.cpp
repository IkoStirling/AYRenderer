#include "detail/ShadowPass.h"

#include "detail/ShadowLightMatrix.h"

#include <bgfx/bgfx.h>

#include <cstdio>

namespace ayt::render::detail
{

ShadowPass::~ShadowPass()
{
    // dtor intentionally does not touch bgfx handles — same pattern as
    // PostProcessPass. Call destroyResources() before mid-lifetime teardown.
}

uint32_t ShadowPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    const uint8_t viewId = ctx.viewId;
    const auto& meshes    = ctx.meshes;
    const RenderScene& scene = ctx.scene;

    if (!adapter.isInitialized() || adapter.isNoopBackend()) {
        return 0;
    }

    if (_requestedSize == 0) {
        _requestedSize = kDefaultShadowMapSize;
    }
    ensureShadowFbo(adapter, _requestedSize);
    if (!BGFXAdapter::isValid(_shadowFbo)) {
        std::fprintf(stderr,
                     "[ShadowPass] shadow FBO create failed at %ux%u; "
                     "shadow disabled for this frame\n",
                     _requestedSize, _requestedSize);
        return 0;
    }

    const bool homogeneousDepth =
        bgfx::getCaps() != nullptr && bgfx::getCaps()->homogeneousDepth;
    buildDirectionalShadowMatrices(
        ctx.frame.lightDirection,
        _lightView,
        _lightProj,
        ayt::math::FVector3(0.0f, 0.0f, 0.0f),
        kDefaultFrustumRadius,
        homogeneousDepth);
    _lightViewProj = _lightProj * _lightView;

    adapter.setViewFrameBuffer(viewId, _shadowFbo);
    adapter.setViewRect(viewId, 0, 0, _requestedSize, _requestedSize);
    adapter.setViewTransform(viewId, _lightView.ptr(), _lightProj.ptr());
    adapter.setViewClearDepthOnly(viewId, /*depth=*/1.0f);

    const uint64_t depthOnlyState = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                                  | BGFX_STATE_CULL_CW;
    adapter.setState(depthOnlyState);

    uint32_t drawCount = 0;
    for (const DrawItem& item : scene.items()) {
        if (!item.mesh.isValid()) {
            continue;
        }
        const auto meshIt = meshes.find(item.mesh.id);
        if (meshIt == meshes.end()) {
            continue;
        }
        const GpuMesh& mesh = meshIt->second;
        if (!BGFXAdapter::isValid(mesh.vertexBuffer)
            || !BGFXAdapter::isValid(mesh.indexBuffer)) {
            continue;
        }

        adapter.setTransform(item.world);
        adapter.setVertexBuffer(mesh.vertexBuffer, 0, UINT32_MAX);
        adapter.setIndexBuffer(mesh.indexBuffer, 0, mesh.indexCount);
        adapter.submit(viewId,
                       bgfx::ProgramHandle{BGFX_INVALID_HANDLE},
                       /*depth=*/0,
                       BGFX_DISCARD_ALL);
        ++drawCount;
    }

    adapter.setViewFrameBuffer(viewId, BGFX_INVALID_HANDLE);
    return drawCount;
}

void ShadowPass::ensureShadowFbo(BGFXAdapter& adapter, uint16_t size)
{
    if (BGFXAdapter::isValid(_shadowFbo) && _shadowSize == size) {
        return;
    }
    if (BGFXAdapter::isValid(_shadowFbo)) {
        adapter.destroy(_shadowFbo);
        _shadowFbo  = BGFX_INVALID_HANDLE;
        _shadowSize = 0;
    }
    _shadowFbo = adapter.createDepthOnlyFrameBuffer(size, size);
    if (BGFXAdapter::isValid(_shadowFbo)) {
        _shadowSize = size;
    }
}

void ShadowPass::destroyResources(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_shadowFbo)) {
        adapter.destroy(_shadowFbo);
        _shadowFbo  = BGFX_INVALID_HANDLE;
        _shadowSize = 0;
    }
}

} // namespace ayt::render::detail
