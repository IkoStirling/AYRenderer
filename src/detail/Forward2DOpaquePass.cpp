#include "detail/Forward2DOpaquePass.h"

#include "detail/FrameContext.h"
#include "detail/SceneColorPipeline.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace ayt::render::detail
{

namespace {

// Upload the three 2D per-draw uniforms from the payload. Missing
// bindings are silent no-ops (the Tilemap2D shader declares them,
// so this only no-ops on a mismatched custom shader).
void upload2DUniforms(shader::ShaderResource& shader, const DrawPayload2D& payload)
{
    const float srcRect[4] = {
        payload.sourceRectMin.x, payload.sourceRectMin.y,
        payload.sourceRectMax.x, payload.sourceRectMax.y,
    };
    const float tint[4] = {
        payload.tintRGBA.x, payload.tintRGBA.y,
        payload.tintRGBA.z, payload.tintRGBA.w,
    };
    // SpriteFlip bit semantics: 1 = horizontal, 2 = vertical.
    const float flip[4] = {
        static_cast<float>(payload.flip & 0x01u),
        static_cast<float>((payload.flip >> 1) & 0x01u),
        0.0f, 0.0f,
    };
    const float atlasTexel[4] = {
        payload.atlasTexelSize.x, payload.atlasTexelSize.y, 0.0f, 0.0f,
    };

    const shader::BindingId srcRectBinding = shader.getUniformBinding("srcRect");
    if (srcRectBinding != shader::InvalidBinding) {
        shader.setUniform(srcRectBinding, srcRect, sizeof(srcRect));
    }
    const shader::BindingId tintBinding = shader.getUniformBinding("tint");
    if (tintBinding != shader::InvalidBinding) {
        shader.setUniform(tintBinding, tint, sizeof(tint));
    }
    const shader::BindingId flipBinding = shader.getUniformBinding("flip");
    if (flipBinding != shader::InvalidBinding) {
        shader.setUniform(flipBinding, flip, sizeof(flip));
    }
    const shader::BindingId atlasTexelBinding =
        shader.getUniformBinding("atlasTexel");
    if (atlasTexelBinding != shader::InvalidBinding) {
        shader.setUniform(atlasTexelBinding, atlasTexel, sizeof(atlasTexel));
    }
}

} // namespace

std::vector<const DrawItem*> collectSortedOverlay2DItems(
    const RenderScene& scene)
{
    std::vector<const DrawItem*> sortedItems;
    sortedItems.reserve(scene.items().size());
    for (const DrawItem& item : scene.items()) {
        if (item.payload != nullptr && !item.outlineHull) {
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

    // Blend-only: BGFX_STATE_BLEND_ALPHA, no WRITE_Z, no DEPTH_TEST.
    adapter.setStateAlphaBlend();

    uint32_t drawCount = 0;

    const std::vector<const DrawItem*> sortedItems =
        collectSortedOverlay2DItems(scene);

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
        adapter.setIndexBuffer(mesh.indexBuffer, drawRange.firstIndex,
                               drawRange.indexCount);

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
        }

        upload2DUniforms(material.shader, *item.payload);

        ayt::shader::DrawCallContext drawCtx;
        drawCtx.viewId = viewId;
        drawCtx.state  = 0;  // Adapter owns state (design.md §2.5).
        material.shader.submit(drawCtx);
        ++drawCount;
    }

    return drawCount;
}

} // namespace ayt::render::detail
