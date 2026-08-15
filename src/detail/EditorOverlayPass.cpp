#include "detail/EditorOverlayPass.h"

#include "detail/GpuResources.h"
#include "detail/ShadowPass.h"

#include "AYShaderResource.h"

#include <algorithm>
#include <AYIO/Env.h>
#include <cstdio>
#include <cstring>
#include <string>
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

bool EditorOverlayPass::submitOutlineItem(
    BGFXAdapter& adapter,
    PassExecContext& ctx,
    const FrameContext& frame,
    const DrawItem& item,
    uint8_t viewId)
{
    if (!item.mesh.isValid() || !item.material.isValid()) {
        return false;
    }

    const auto meshIt = ctx.meshes.find(item.mesh.id);
    const auto matIt  = ctx.materials.find(item.material.id);
    if (meshIt == ctx.meshes.end() || matIt == ctx.materials.end()) {
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

    adapter.setTransform(item.world);
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
        if (slot.name.empty() || !slot.texture.isValid() || slot.name == "shadowMap") {
            continue;
        }
        const shader::BindingId binding = material.shader.getTextureBinding(slot.name);
        if (binding == shader::InvalidBinding) {
            continue;
        }
        const auto texIt = ctx.textures.find(slot.texture.id);
        if (texIt == ctx.textures.end()
            || !BGFXAdapter::isValid(texIt->second.handle)) {
            continue;
        }
        const uint8_t stage = material.shader.getTextureStage(binding);
        material.shader.setTexture(stage, binding,
                                   toShaderTexture(texIt->second.handle));
    }

    resolveAndApplyColorUniforms(material);

    if (material.mat4Binding != shader::InvalidBinding && material.hasMat4Override) {
        if (material.shader.hasUniformBinding(material.mat4Binding)) {
            float colMajor[16];
            toBgfxColumnMajor(material.mat4Override, colMajor);
            material.shader.setUniform(material.mat4Binding, colMajor, sizeof(colMajor));
        } else {
            material.mat4Binding = shader::InvalidBinding;
        }
    }

    for (const GpuMaterial::UniformSlot& slot : material.uniformSlots) {
        if (slot.name.empty() || slot.size == 0) {
            continue;
        }
        const shader::BindingId binding = material.shader.getUniformBinding(slot.name);
        if (binding == shader::InvalidBinding) {
            continue;
        }
        material.shader.setUniform(binding, slot.data, slot.size);
    }

    if (material.shader.isValid()) {
        if (material.boneBlockBinding == shader::InvalidBinding) {
            material.boneBlockBinding =
                material.shader.getUniformBlockBinding("Skeleton");
        }
        tryUploadBonePalette(material.shader,
                             material.boneBlockBinding,
                             /*castSkinnedBinding=*/shader::InvalidBinding,
                             /*castSkinnedValue=*/0u,
                             item);
    }

    ayt::shader::DrawCallContext drawCtx;
    drawCtx.viewId = viewId;
    drawCtx.state  = 0;
    material.shader.submit(drawCtx);
    return true;
}

uint32_t EditorOverlayPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    const FrameContext& frame = ctx.frame;
    constexpr uint8_t viewId = kBlitViewId;
    const uint16_t viewportX      = ctx.viewportX;
    const uint16_t viewportY      = ctx.viewportY;
    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    const RenderScene& scene = ctx.scene;

    if (!adapter.isInitialized() || adapter.isNoopBackend()) {
        return 0;
    }
    const std::string outlineEnv = ayt::io::env::get("AY_EDITOR_OUTLINE").value_or("");
    if (!outlineEnv.empty() && outlineEnv == "0") {
        return 0;
    }
    if (viewportWidth == 0 || viewportHeight == 0) {
        return 0;
    }

    const auto& items = scene.items();
    std::vector<const DrawItem*> outlineItems;
    outlineItems.reserve(items.size());
    for (const DrawItem& item : items) {
        if (item.outlineHull) {
            outlineItems.push_back(&item);
        }
    }
    if (outlineItems.empty()) {
        return 0;
    }

    // Post-process already wrote the lit scene into the Game View hole.
    // Overlay selection with the proven mesh material path — no RT/blit.
    adapter.setViewFrameBuffer(viewId, BGFX_INVALID_HANDLE);
    adapter.setViewRect(viewId, viewportX, viewportY, viewportWidth, viewportHeight);
    adapter.setViewTransform(viewId, frame.view, frame.projection);
    adapter.setViewClearRaw(viewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);

    adapter.setState(BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_WRITE_A
                   | BGFX_STATE_BLEND_ALPHA
                   | BGFX_STATE_DEPTH_TEST_ALWAYS
                   | BGFX_STATE_CULL_CW);

    std::stable_sort(outlineItems.begin(), outlineItems.end(), SortKeyDescending{});

    uint32_t drawCount = 0;
    for (const DrawItem* pItem : outlineItems) {
        if (submitOutlineItem(adapter, ctx, frame, *pItem, viewId)) {
            ++drawCount;
        }
    }

    static bool s_loggedOk = false;
    if (drawCount > 0 && !s_loggedOk) {
        std::fprintf(stderr,
            "[EditorOverlayPass] outline active: material overlay(v17, stable) "
            "(draws=%u)\n", drawCount);
        s_loggedOk = true;
    }

    return drawCount;
}

} // namespace ayt::render::detail
