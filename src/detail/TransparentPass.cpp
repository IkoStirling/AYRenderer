#include "detail/TransparentPass.h"
#include "detail/GpuResources.h"
#include "detail/ShadowPass.h"

#include "AYShaderResource.h"  // for ShaderResource::setUniform

#include <algorithm>  // std::stable_sort
#include <vector>

namespace ayt::render::detail
{

namespace {

// U1.5 — comparator: higher sortKey = drawn first (back-to-front
// compositing). std::stable_sort + std::greater preserves insertion
// order between equal sortKey items, which keeps the behavior of
// "all sortKey=0 == current insertion-order" exactly identical to
// the pre-sort code path.
struct SortKeyDescending {
    bool operator()(const DrawItem* a, const DrawItem* b) const {
        return a->sortKey > b->sortKey;
    }
};

} // namespace

uint32_t TransparentPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    const FrameContext& frame = ctx.frame;
    const uint8_t viewId = ctx.viewId;
    const auto& meshes    = ctx.meshes;
    auto& materials       = ctx.materials;
    const uint16_t viewportX      = ctx.viewportX;
    const uint16_t viewportY      = ctx.viewportY;
    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    const RenderScene& scene = ctx.scene;

    // §5.4 (2026-07-22) — `isInitialized()` guard fixes the
    // pre-existing landmine where Transparent went straight into
    // `adapter.setViewTransform(viewId, frame.view, frame.projection)`
    // on an uninitialized adapter. Same rationale as
    // ForwardOpaquePass.cpp:147 — see that comment for the full
    // "no isNoopBackend() check" justification (Noop backend's
    // adapter-internal short-circuits are safe; the test sandbox
    // relies on the scene-items loop running to count logical
    // draw submissions).
    if (!adapter.isInitialized()) {
        return 0;
    }

    adapter.setViewTransform(viewId, frame.view, frame.projection);

    // P6.5 (2026-07-22) — preset state combination replaces the
    // pre-P6.5 inline `BGFX_STATE_WRITE_RGB | WRITE_A | BLEND_ALPHA |
    // DEPTH_TEST_LESS | CULL_CW`. Bit combination identical; called
    // once per execute() because every draw in the loop below uses
    // the same state (draw-state-0 in DrawCallContext signals
    // "use the Adapter-set state").
    adapter.setStateAlphaBlend();

    // P2 (PR-D, 2026-07-20) — bind the shared scene FBO so transparent
    // composite over the offscreen scene color (matches
    // ForwardOpaquePass so the depth they wrote lines up with what
    // PostProcessPass samples). BGFX_INVALID_HANDLE ⇒ backbuffer
    // fallback (no-op on the headless test path / SceneRT-off hosts).
    // View rect origin is (0,0) against the panel-sized scene FBO;
    // panel offset is for backbuffer compositing only (see FO).
    adapter.setViewFrameBuffer(viewId, ctx.sceneFbo);
    if (BGFXAdapter::isValid(ctx.sceneFbo)) {
        adapter.setViewRect(viewId, 0, 0, viewportWidth, viewportHeight);
    } else {
        adapter.setViewRect(viewId, viewportX, viewportY, viewportWidth, viewportHeight);
    }

    // U1.5 — back-to-front sort via DrawItem::sortKey. We don't
    // mutate the scene's _items vector; instead we build a transient
    // pointer list, stable_sort it, and iterate that. The default
    // sortKey=0 keeps the iteration order identical to the prior
    // "for (item : scene.items())" path, so this is a strict no-op
    // for callers that haven't opted into sorting.
    const auto& items = scene.items();
    std::vector<const DrawItem*> sortedItems;
    sortedItems.reserve(items.size());
    for (const DrawItem& item : items) {
        sortedItems.push_back(&item);
    }
    std::stable_sort(sortedItems.begin(), sortedItems.end(), SortKeyDescending{});

    uint32_t drawCount = 0;

    for (const DrawItem* pItem : sortedItems) {
        const DrawItem& item = *pItem;
        if (!item.mesh.isValid() || !item.material.isValid()) {
            continue;
        }

        const auto meshIt = meshes.find(item.mesh.id);
        const auto matIt  = materials.find(item.material.id);
        if (meshIt == meshes.end() || matIt == materials.end()) {
            continue;
        }

        const GpuMesh& mesh = meshIt->second;
        if (!BGFXAdapter::isValid(mesh.vertexBuffer)
            || !BGFXAdapter::isValid(mesh.indexBuffer)) {
            continue;
        }

        GpuMaterial& material = matIt->second;
        if (!material.shader.isValid()) {
            continue;
        }

        // U1 tag check — only Alpha materials enter this pass.
        // ForwardOpaquePass draws the Opaque ones (default for all
        // pre-existing materials).
        if (material.blendMode != ayt::render::BlendMode::Alpha) {
            continue;
        }

        adapter.setTransform(item.world);
        adapter.setVertexBuffer(mesh.vertexBuffer);
        adapter.setIndexBuffer(mesh.indexBuffer, 0, mesh.indexCount);

        // U1.5 — MVP / cameraPos / lightDir / lightColor upload now
        // matches ForwardOpaquePass::flushMaterial. Prior to U1.5,
        // TransparentPass relied entirely on Phoskia's Unlit test
        // shader NOT sampling these uniforms (so the missing upload
        // wasn't observable in tests). Any Lit / Glass / PBR alpha
        // material would have rendered against the default-zero
        // view-projection matrix and been culled to a black quad at
        // the origin. U1.5 fixes that latent bug by lifting the
        // MVP/light upload — helpers live as inline free fns in
        // detail/RenderPass.h so they stay byte-for-byte identical
        // across both passes (no risk of drift).
        // MVP comes from setViewTransform + setTransform (bgfx builtin).
        // Do not manually overwrite u_modelViewProj.

        trySetUniformVec3(material.shader, "cameraPos", frame.cameraPosition.ptr());

        const ayt::math::FVector3 toLight(
            -frame.lightDirection.x, -frame.lightDirection.y, -frame.lightDirection.z);
        const ayt::math::FVector3 toLightDir = toLight.normalize();
        trySetUniformVec3(material.shader, "lightDir", toLightDir.ptr());
        trySetUniformVec3(material.shader, "lightDirection", toLightDir.ptr());
        trySetUniformVec3(material.shader, "lightColor", frame.lightColor.ptr());

        // PR-F2 (2026-07-21) — same plumbing as ForwardOpaquePass's
        // call to tryBindShadowSampler: when ctx.shadowPass has a
        // ready FBO, upload `u_lightViewProj` and bind the depth
        // attachment as `shadowMap`. Inline call (TransparentPass
        // does not share FO's flushMaterial) keeps the helper's
        // single source-of-truth for the upload shape; mirror site is
        // ForwardOpaquePass::flushMaterial.
        tryBindShadowSampler(material.shader, adapter, ctx.shadowPass,
                          item.shadowFlags, frame.shadowBias);

        // U1++ — color-uniform upload shared with ForwardOpaquePass
        // via RenderPass::resolveAndApplyColorUniforms.
        RenderPass::resolveAndApplyColorUniforms(material);

        ayt::shader::DrawCallContext ctx;
        ctx.viewId = viewId;
        ctx.state  = 0;  // P6.5: per-draw state owned by Adapter (see
                          // setStateAlphaBlend() called once at the top
                          // of execute() below).
        material.shader.submit(ctx);
        ++drawCount;
    }

    return drawCount;
}

} // namespace ayt::render::detail
