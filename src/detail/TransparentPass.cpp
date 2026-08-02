#include "detail/TransparentPass.h"
#include "detail/GpuResources.h"
#include "detail/ShadowPass.h"
#include "detail/LightingPass.h"
#include "detail/GBufferPass.h"

#include "AYShaderResource.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace ayt::render::detail
{

namespace {

struct SortKeyDescending {
    bool operator()(const DrawItem* a, const DrawItem* b) const {
        return a->sortKey > b->sortKey;
    }
};

} // namespace

bool TransparentPass::submitItem(
    BGFXAdapter& adapter,
    PassExecContext& ctx,
    const FrameContext& frame,
    const DrawItem& item,
    uint8_t viewId,
    const ayt::math::Float4x4* worldOverride)
{
    const auto& meshes   = ctx.meshes;
    const auto& textures = ctx.textures;
    auto& materials      = ctx.materials;

    if (!item.mesh.isValid() || !item.material.isValid()) {
        return false;
    }

    const auto meshIt = meshes.find(item.mesh.id);
    const auto matIt  = materials.find(item.material.id);
    if (meshIt == meshes.end() || matIt == materials.end()) {
        return false;
    }

    const GpuMesh& mesh = meshIt->second;
    if (!BGFXAdapter::isValid(mesh.vertexBuffer)
        || !BGFXAdapter::isValid(mesh.indexBuffer)) {
        return false;
    }

    GpuMaterial& material = matIt->second;
    if (!material.shader.isValid()) {
        return false;
    }

    if (material.blendMode != ayt::render::BlendMode::Alpha) {
        return false;
    }

    adapter.setTransform(worldOverride != nullptr ? *worldOverride : item.world);
    adapter.setVertexBuffer(mesh.vertexBuffer);
    adapter.setIndexBuffer(mesh.indexBuffer, 0, mesh.indexCount);

    trySetUniformVec3(material.shader, "cameraPos", frame.cameraPosition.ptr());

    const ayt::math::FVector3 toLight(
        -frame.lightDirection.x, -frame.lightDirection.y, -frame.lightDirection.z);
    const ayt::math::FVector3 toLightDir = toLight.normalize();
    trySetUniformVec3(material.shader, "lightDir", toLightDir.ptr());
    trySetUniformVec3(material.shader, "lightDirection", toLightDir.ptr());
    trySetUniformVec3(material.shader, "lightColor", frame.lightColor.ptr());

    tryBindShadowSampler(material.shader, adapter, ctx.shadowPass,
                      item.shadowFlags, frame.shadowBias);

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

    resolveAndApplyColorUniforms(material);

    ayt::shader::DrawCallContext drawCtx;
    drawCtx.viewId = viewId;
    drawCtx.state  = 0;
    material.shader.submit(drawCtx);
    return true;
}

uint32_t TransparentPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    const FrameContext& frame = ctx.frame;
    constexpr uint8_t kDeferredTransparentViewId = 9;
    const bool deferredLitComposite = (ctx.lightingPass != nullptr) &&
        bgfx::isValid(ctx.lightingPass->lightingOutputFbo());
    const uint8_t viewId = deferredLitComposite
                               ? kDeferredTransparentViewId
                               : ctx.viewId;
    const uint16_t viewportX      = ctx.viewportX;
    const uint16_t viewportY      = ctx.viewportY;
    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    const RenderScene& scene = ctx.scene;

    if (!adapter.isInitialized()) {
        return 0;
    }

    adapter.setViewTransform(viewId, frame.view, frame.projection);

    // Deferred LightingOutput is color-only. Borrow GBuffer depth so
    // glass can DEPTH_TEST_LESS against opaque geometry.
    bgfx::FrameBufferHandle compositeFbo = ctx.sceneFbo;
    bgfx::FrameBufferHandle borrowedDepthFbo = BGFX_INVALID_HANDLE;
    bool ownsBorrowedDepthFbo = false;
    if (deferredLitComposite) {
        compositeFbo = ctx.lightingPass->lightingOutputFbo();
        if (ctx.gbufferPass != nullptr) {
            const bgfx::TextureHandle color =
                adapter.getFboAttachment(compositeFbo, 0);
            const bgfx::TextureHandle depth = ctx.gbufferPass->gbufferDepthRt();
            if (BGFXAdapter::isValid(color) && BGFXAdapter::isValid(depth)) {
                borrowedDepthFbo =
                    adapter.createBorrowedColorDepthFrameBuffer(color, depth);
                ownsBorrowedDepthFbo = BGFXAdapter::isValid(borrowedDepthFbo);
            }
        }
        if (ownsBorrowedDepthFbo) {
            compositeFbo = borrowedDepthFbo;
        } else {
            static bool s_logged = false;
            if (!s_logged) {
                std::fprintf(stderr,
                    "[TransparentPass] deferred depth FBO unavailable; "
                    "glass will not occlude against opaque\n");
                s_logged = true;
            }
        }
    }

    const auto& items = scene.items();
    std::vector<const DrawItem*> sortedItems;
    sortedItems.reserve(items.size());
    for (const DrawItem& item : items) {
        if (item.outlineHull) {
            continue;
        }
        sortedItems.push_back(&item);
    }
    if (sortedItems.empty()) {
        if (ownsBorrowedDepthFbo) {
            adapter.destroy(borrowedDepthFbo);
        }
        return 0;
    }
    std::stable_sort(sortedItems.begin(), sortedItems.end(), SortKeyDescending{});

    uint32_t drawCount = 0;

    adapter.setViewFrameBuffer(viewId, compositeFbo);
    if (BGFXAdapter::isValid(compositeFbo)) {
        adapter.setViewRect(viewId, 0, 0, viewportWidth, viewportHeight);
    } else {
        adapter.setViewRect(viewId, viewportX, viewportY, viewportWidth, viewportHeight);
    }

    if (deferredLitComposite && !ownsBorrowedDepthFbo) {
        adapter.setState(BGFX_STATE_WRITE_RGB
                       | BGFX_STATE_WRITE_A
                       | BGFX_STATE_BLEND_ALPHA
                       | BGFX_STATE_DEPTH_TEST_ALWAYS
                       | BGFX_STATE_CULL_CW);
    } else {
        adapter.setStateAlphaBlend();
    }

    for (const DrawItem* pItem : sortedItems) {
        if (submitItem(adapter, ctx, frame, *pItem, viewId)) {
            ++drawCount;
        }
    }

    if (ownsBorrowedDepthFbo) {
        adapter.destroy(borrowedDepthFbo);
    }

    return drawCount;
}

} // namespace ayt::render::detail
