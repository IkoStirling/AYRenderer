#include "detail/ShadowPass.h"

#include <bgfx/bgfx.h>

#include <cstdio>

namespace ayt::render::detail
{

namespace {

// R5+ (Phase Shadow) — fixed light-space transform. The first cut
// uses the identity view + identity projection so the depth FBO
// records world-space depth (cut 2 replaces this with a proper
// orthographic light-space matrix once the Light struct + scene-
// AABB fit land). Pinning the no-op behavior here makes the slot
// auditable in tests.
constexpr float kIdentityRow[4] = {1.0f, 0.0f, 0.0f, 0.0f};

} // namespace

ShadowPass::~ShadowPass()
{
    // R5+ — dtor intentionally does not touch bgfx handles. The
    // bgfx::shutdown() in BGFXAdapter::shutdown() invalidates all
    // handles globally, so releasing at that point is implicit.
    // Hosts needing mid-frame adapter teardown should call
    // destroyResources() explicitly before the pass is destroyed.
    // Same pattern as PostProcessPass::destroyResources().
}

uint32_t ShadowPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    const uint8_t viewId = ctx.viewId;
    const auto& meshes    = ctx.meshes;
    const RenderScene& scene = ctx.scene;

    // R5+ — mirror PostProcessPass::execute guards. The pass is a
    // hard no-op on the headless test path (Noop backend / not
    // initialized) so the unit tests don't need a real GPU.
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

    // R5+ — bind the shadow FBO. Identity view + identity projection
    // (cut 1 stub; cut 2 replaces with light-space orthographic).
    adapter.setViewFrameBuffer(viewId, _shadowFbo);
    adapter.setViewRect(viewId, 0, 0, _requestedSize, _requestedSize);
    adapter.setViewTransform(viewId, kIdentityRow, kIdentityRow);
    // Clear depth to 1.0 (far plane) so untouched pixels don't sample
    // as "in shadow". Color doesn't matter for a depth-only FBO.
    adapter.setViewClearDepthOnly(viewId, /*depth=*/1.0f);

    // R5+ — depth-only state. WRITE_Z + DEPTH_TEST_LESS + DISCARD_ALL
    // (fragment skipped = no color writes, just depth).
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

    // R5+ — restore default backbuffer so the next pass (ForwardOpaque
    // or future GBuffer) sees the regular target.
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
