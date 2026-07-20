#include "detail/TransparentPass.h"
#include "detail/GpuResources.h"

#include "AYShaderResource.h"  // for ShaderResource::setUniform

#include <algorithm>  // std::stable_sort
#include <bgfx/bgfx.h>
#include <vector>

namespace ayt::render::detail
{

namespace {

// BGFX state for transparent geometry. Differences from ForwardOpaquePass:
//   * STATE_BLEND_ALPHA added (standard non-premultiplied "over" —
//     assumes shader output is vec4(rgb, alpha), which is Phoskia default)
//   * STATE_WRITE_Z removed — depth-write from transparent draws causes
//     them to occlude each other when the scene isn't pre-sorted (U1.5
//     now sorts via DrawItem::sortKey, but we still skip WRITE_Z to
//     avoid breaking the test that all alpha fragments composite over
//     the opaque z-buffer regardless of order). Depth TEST remains so
//     we composite against the opaque z-buffer that ForwardOpaquePass
//     populated first.
const uint64_t kTransparentState = BGFX_STATE_WRITE_RGB
                                 | BGFX_STATE_WRITE_A
                                 | BGFX_STATE_BLEND_ALPHA
                                 | BGFX_STATE_DEPTH_TEST_LESS
                                 | BGFX_STATE_CULL_CW;

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

    adapter.setViewRect(viewId, viewportX, viewportY, viewportWidth, viewportHeight);
    adapter.setViewTransform(viewId, frame.view.ptr(), frame.projection.ptr());

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
        const ayt::math::Float4x4 modelViewProj = frame.projection * frame.view * item.world;
        trySetUniformMat4(material.shader, "u_modelViewProj", "modelViewProj", modelViewProj);

        trySetUniformVec3(material.shader, "cameraPos", frame.cameraPosition.ptr());

        const ayt::math::FVector3 toLight(
            -frame.lightDirection.x, -frame.lightDirection.y, -frame.lightDirection.z);
        const ayt::math::FVector3 toLightDir = toLight.normalize();
        trySetUniformVec3(material.shader, "lightDir", toLightDir.ptr());
        trySetUniformVec3(material.shader, "lightDirection", toLightDir.ptr());
        trySetUniformVec3(material.shader, "lightColor", frame.lightColor.ptr());

        // U1++ — color-uniform upload shared with ForwardOpaquePass
        // via RenderPass::resolveAndApplyColorUniforms.
        RenderPass::resolveAndApplyColorUniforms(material);

        ayt::shader::DrawCallContext ctx;
        ctx.viewId = viewId;
        ctx.state  = kTransparentState;
        material.shader.submit(ctx);
        ++drawCount;
    }

    return drawCount;
}

} // namespace ayt::render::detail
