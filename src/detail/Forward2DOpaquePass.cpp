#include "detail/Forward2DOpaquePass.h"

#include "detail/FrameContext.h"

#include <cstdio>

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
}

} // namespace

uint32_t Forward2DOpaquePass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    const uint8_t viewId = ctx.viewId;
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

    adapter.setViewTransform(viewId, ctx.frame.view, ctx.frame.projection);

    // Bind the shared scene FBO so the 2D lane composites into the
    // same offscreen color/depth that PostProcessPass samples
    // (ForwardOpaquePass mirror). BGFX_INVALID_HANDLE ⇒ default
    // backbuffer (headless test path). The 2D pass does NOT clear —
    // ForwardOpaquePass already cleared this view (mirror
    // TransparentPass, which also draws without clearing).
    adapter.setViewFrameBuffer(viewId, ctx.sceneFbo);
    if (BGFXAdapter::isValid(ctx.sceneFbo)) {
        adapter.setViewRect(viewId, 0, 0, viewportWidth, viewportHeight);
    } else {
        adapter.setViewRect(viewId, viewportX, viewportY, viewportWidth, viewportHeight);
    }

    // Blend-only: BGFX_STATE_BLEND_ALPHA, no WRITE_Z, no DEPTH_TEST.
    // CPU packedSortKey order is the final order (see class doc).
    adapter.setStateAlphaBlend();

    uint32_t drawCount = 0;

    for (const DrawItem& item : scene.items()) {
        if (item.payload == nullptr) {
            continue;  // 3D item — other passes own it.
        }
        if (item.outlineHull) {
            continue;
        }
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

        adapter.setTransform(item.world);
        adapter.setVertexBuffer(mesh.vertexBuffer);
        adapter.setIndexBuffer(mesh.indexBuffer, 0, mesh.indexCount);

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
