#include "detail/Forward2DOpaquePass.h"

#include "detail/Draw2D.h"
#include "detail/FrameContext.h"
#include "detail/FrameDrawLists.h"
#include "detail/SceneColorPipeline.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace ayt::render::detail
{

std::vector<const DrawItem*> collectSortedOverlay2DItems(
    const RenderScene& scene)
{
    std::vector<const DrawItem*> sortedItems;
    sortedItems.reserve(scene.items().size());
    for (const DrawItem& item : scene.items()) {
        if (isOverlay2DItem(item) && !item.outlineHull) {
            sortedItems.push_back(&item);
        }
    }
    std::stable_sort(
        sortedItems.begin(), sortedItems.end(),
        [](const DrawItem* lhs, const DrawItem* rhs) {
            return lhs->payload->packedSortKey < rhs->payload->packedSortKey;
        });
    return sortedItems;
}

uint32_t Forward2DOpaquePass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    const uint8_t viewId = kOverlayViewId;
    const auto& meshes   = ctx.meshes;
    const auto& textures = ctx.textures;
    auto& materials      = ctx.materials;
    const uint16_t viewportX      = ctx.viewportX;
    const uint16_t viewportY      = ctx.viewportY;
    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    const RenderScene& scene = ctx.scene;

    // Mirror ForwardOpaquePass.cpp:168-170 — raw bgfx setViewTransform
    // on an uninitialized adapter is UB; Noop backend must still
    // reach the scene loop so tests can count "logical draw
    // submissions" (existing FO test semantics).
    if (!adapter.isInitialized()) {
        return 0;
    }

    const OverlayCamera2D& overlayCamera = scene.overlayCamera2D();
    adapter.setViewTransform(
        viewId,
        overlayCamera.valid ? overlayCamera.view : ctx.frame.view,
        overlayCamera.valid ? overlayCamera.projection : ctx.frame.projection);
    adapter.setViewMode(viewId, bgfx::ViewMode::Sequential);

    // Composite into the current scene-linear producer. In Deferred this is
    // Lighting/Haze color; in Forward it is sceneFbo. A mounted Deferred path
    // fails closed when its producer is stale. Invalid Forward sceneFbo keeps
    // the legacy backbuffer fallback.
    const bgfx::FrameBufferHandle compositeFbo = selectSceneColorSourceFbo(ctx);
    const bool deferredPath = ctx.gbufferPass != nullptr
        && ctx.lightingPass != nullptr;
    if (deferredPath && !BGFXAdapter::isValid(compositeFbo)) {
        return 0;
    }
    adapter.setViewFrameBuffer(viewId, compositeFbo);
    if (BGFXAdapter::isValid(compositeFbo)) {
        adapter.setViewRect(viewId, 0, 0, viewportWidth, viewportHeight);
    } else {
        adapter.setViewRect(viewId, viewportX, viewportY, viewportWidth, viewportHeight);
    }

    uint32_t drawCount = 0;

    FrameDrawLists fallbackDrawLists;
    const FrameDrawLists& drawLists =
        resolveFrameDrawLists(ctx, fallbackDrawLists);
    const std::vector<const DrawItem*>& sortedItems = drawLists.overlay2D;

    for (const DrawItem* itemPtr : sortedItems) {
        const DrawItem& item = *itemPtr;
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

        const DrawIndexRange drawRange = resolveDrawIndexRange(item, mesh);
        if (drawRange.indexCount == 0) {
            continue;
        }

        adapter.setTransform(item.world);
        adapter.setVertexBuffer(mesh.vertexBuffer);
        const bool wireframe = bindDrawIndexBuffer(
            adapter, mesh, drawRange, ctx.wireframe);
        // Blend-only: BGFX_STATE_BLEND_ALPHA, no WRITE_Z.
        // Camera-overlay sprites are planar artwork rather than closed 3D
        // surfaces.  Draw both sides so their visibility does not depend on
        // the engine's 3D front-face winding or on a reflected 2D transform.
        adapter.setStateAlphaBlend(wireframe, /*doubleSided=*/true);

        // Bind albedo textures (flushMaterial loop shape; shadowMap
        // slots skipped — 2D has no shadow path).
        for (const GpuMaterial::TextureSlot& slot : material.textures) {
            if (slot.name.empty() || !slot.texture.isValid()) {
                continue;
            }
            if (slot.name == "shadowMap") {
                continue;
            }
            const shader::BindingId binding =
                material.shader.getTextureBinding(slot.name);
            if (binding == shader::InvalidBinding) {
                continue;
            }
            const auto texIt = textures.find(slot.texture.id);
            if (texIt == textures.end()
                || !BGFXAdapter::isValid(texIt->second.handle)) {
                continue;
            }
            const uint8_t stage = material.shader.getTextureStage(binding);
            material.shader.setTexture(stage, binding,
                                       toShaderTexture(texIt->second.handle));
            if (slot.name == "albedoMap") {
                const shader::BindingId texelBinding =
                    material.shader.getUniformBinding("albedoTexel");
                if (texelBinding != shader::InvalidBinding
                    && texIt->second.width > 0 && texIt->second.height > 0) {
                    const float texel[4] = {
                        1.0f / static_cast<float>(texIt->second.width),
                        1.0f / static_cast<float>(texIt->second.height),
                        0.0f,
                        0.0f,
                    };
                    material.shader.setUniform(texelBinding, texel,
                                               sizeof(texel));
                }
            }
        }

        upload2DDrawUniforms(material.shader, *item.payload);

        ayt::shader::DrawCallContext drawCtx;
        drawCtx.viewId = viewId;
        drawCtx.state  = 0;  // Adapter owns state (design.md §2.5).
        material.shader.submit(drawCtx);
        ++drawCount;
    }

    return drawCount;
}

} // namespace ayt::render::detail
