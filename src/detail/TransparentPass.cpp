#include "detail/TransparentPass.h"
#include "detail/RasterConvention.h"
#include "detail/GpuResources.h"
#include "detail/ShadowPass.h"
#include "detail/LightingPass.h"
#include "detail/GBufferPass.h"
#include "detail/SkyboxPass.h"
#include "detail/DepthHazePass.h"
#include "detail/SceneColorPipeline.h"

#include "AYRenderer/RenderTypes.h"
#include "AYShader/ShaderResource.h"
#include <AYIO/Env.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ayt::render::detail
{

float transparentDistanceSquared(
    const DrawItem& item,
    const ayt::math::FVector3& cameraPosition) noexcept
{
    const float dx = item.world(0, 3) - cameraPosition.x;
    const float dy = item.world(1, 3) - cameraPosition.y;
    const float dz = item.world(2, 3) - cameraPosition.z;
    const float distance = dx * dx + dy * dy + dz * dz;
    return std::isfinite(distance) ? distance : 0.0f;
}

bool transparentSortBefore(const TransparentSortEntry& a,
                           const TransparentSortEntry& b) noexcept
{
    if (a.item == nullptr || b.item == nullptr) {
        return a.item != nullptr;
    }
    // Explicit sortKey remains the primary, host-controlled layer. Equal
    // layers (including the default zero layer) sort by camera distance.
    if (a.item->sortKey != b.item->sortKey) {
        return a.item->sortKey > b.item->sortKey;
    }
    return a.distanceSquared > b.distanceSquared;
}

TransparentRoute selectTransparentRoute(bool hasGBufferPass,
                                        bool hasLightingPass,
                                        bool gbufferProduced,
                                        bool lightingProduced,
                                        bool lightingFboValid) noexcept
{
    const bool hasDeferredProducer = hasGBufferPass || hasLightingPass;
    if (!hasDeferredProducer) {
        return TransparentRoute::Forward;
    }
    if (!hasGBufferPass || !hasLightingPass || !gbufferProduced
        || !lightingProduced || !lightingFboValid) {
        return TransparentRoute::Skip;
    }
    return TransparentRoute::Deferred;
}

uint64_t transparentDrawState(BlendMode blendMode,
                              bool premultipliedAlpha,
                              bool doubleSided,
                              bool reverseWinding) noexcept
{
    uint64_t state = BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_WRITE_A
                   | BGFX_STATE_DEPTH_TEST_LEQUAL;
    if (blendMode == BlendMode::Additive) {
        // Add RGB while preserving destination coverage alpha.
        state |= BGFX_STATE_BLEND_FUNC_SEPARATE(
            BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE,
            BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ONE);
    } else if (premultipliedAlpha) {
        state |= BGFX_STATE_BLEND_FUNC_SEPARATE(
            BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA,
            BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
    } else {
        // Standard over: RGB uses source alpha; alpha itself uses
        // srcA + dstA*(1-srcA), not srcA*srcA.
        state |= BGFX_STATE_BLEND_FUNC_SEPARATE(
            BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA,
            BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
    }
    if (!doubleSided) {
        state |= reverseWinding ? kCullFrontFaces : kCullBackFaces;
    }
    return state;
}

uint64_t selectionDepthState(bool doubleSided,
                             bool reverseWinding) noexcept
{
    uint64_t state = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LEQUAL;
    if (!doubleSided) {
        state |= reverseWinding ? kCullFrontFaces : kCullBackFaces;
    }
    return state;
}

uint64_t selectionHullState(bool reverseWinding) noexcept
{
    return BGFX_STATE_WRITE_RGB
         | BGFX_STATE_WRITE_A
         | BGFX_STATE_DEPTH_TEST_LESS
         | (reverseWinding ? kCullBackFaces : kCullFrontFaces);
}

ayt::math::Float4x4 makeSelectionHullWorld(const DrawItem& item,
                                           float expansion) noexcept
{
    // Older hosts may already provide a padded world plus the original world
    // in outlineDepthWorld. Newer editor selection never mutates Transform;
    // expand locally around the mesh origin instead.
    if (item.hasOutlineDepthWorld) {
        return item.world;
    }
    const float safeExpansion = std::isfinite(expansion)
        ? std::max(1.0f, expansion) : 1.0f;
    return item.world * ayt::math::Float4x4::scaling(
        ayt::math::FVector3(safeExpansion, safeExpansion, safeExpansion));
}

namespace {

void bindTransparentEnvironment(shader::ShaderResource& shader,
                                const PassExecContext& ctx)
{
    bool cubeActive = false;
    if (ctx.skyboxPass != nullptr && ctx.skyboxPass->hasCubeTexture()) {
        const auto cubeIt = ctx.textures.find(ctx.skyboxPass->cubeTexture().id);
        const shader::BindingId cubeBinding = shader.getTextureBinding("envCube");
        if (cubeBinding != shader::InvalidBinding
            && cubeIt != ctx.textures.end()
            && BGFXAdapter::isValid(cubeIt->second.handle)) {
            shader.setTexture(shader.getTextureStage(cubeBinding), cubeBinding,
                              toShaderTexture(cubeIt->second.handle));
            cubeActive = true;
        }
    }

    const float cubeActiveValue[4] = {
        cubeActive ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f
    };
    const float ambientValue[4] = {
        ctx.lightingPass != nullptr
            ? ctx.lightingPass->ambientStrength()
            : ayt::render::kDefaultAmbientStrength,
        0.0f, 0.0f, 0.0f
    };
    trySetUniformVec4(shader, "cubeActive", cubeActiveValue);
    trySetUniformVec4(shader, "ambientStrength", ambientValue);
}

} // namespace

TransparentPass::SubmitResult TransparentPass::submitItem(
    BGFXAdapter& adapter,
    PassExecContext& ctx,
    const FrameContext& frame,
    const DrawItem& item,
    uint8_t viewId,
    const PackedSceneLighting& lights,
    const PackedShadowAtlas& shadows,
    SubmitMode mode)
{
    SubmitResult result;
    const auto& meshes   = ctx.meshes;
    const auto& textures = ctx.textures;
    auto& materials      = ctx.materials;

    // §P2 M5 (2026-08-24) — single material lookup per item. Caller
    // pre-resolved the material in the dispatch loop and passes the
    // pre-computed state bits; submitItem now only validates + submits.
    if (!item.mesh.isValid() || !item.material.isValid()) {
        result.skip = true;
        return result;
    }

    const auto meshIt = meshes.find(item.mesh.id);
    if (meshIt == meshes.end()) {
        result.skip = true;
        return result;
    }

    const GpuMesh& mesh = meshIt->second;
    if (!BGFXAdapter::isValid(mesh.vertexBuffer)
        || !BGFXAdapter::isValid(mesh.indexBuffer)) {
        result.skip = true;
        return result;
    }

    const auto matIt  = materials.find(item.material.id);
    if (matIt == materials.end()) {
        result.skip = true;
        return result;
    }

    GpuMaterial& material = matIt->second;
    if (!material.shader.isValid()) {
        result.skip = true;
        return result;
    }

    if (mode == SubmitMode::TransparentSurface
        && !ayt::render::isTransparentBlendMode(material.blendMode)) {
        result.skip = true;
        return result;
    }

    const DrawIndexRange drawRange = resolveDrawIndexRange(item, mesh);
    if (drawRange.indexCount == 0) {
        result.skip = true;
        return result;
    }

    // §P2 L5 (2026-08-24) — `worldOverride` arg dropped (was unused).
    adapter.setTransform(item.world);
    adapter.setVertexBuffer(mesh.vertexBuffer);
    adapter.setIndexBuffer(mesh.indexBuffer, drawRange.firstIndex,
                           drawRange.indexCount);

    // §P2 supplemental (2026-08-24) — hoisted to RenderPass::tryUploadLightUniforms
    // to dedup with ForwardOpaquePass::flushMaterial.
    tryUploadLightUniforms(material.shader, frame);

    tryBindShadowSampler(material.shader, adapter, ctx.shadowPass,
                      item.shadowFlags, frame.shadowBias);

    bool baseColorTextureBound = false;
    bool opacityTextureBound = false;
    bool albedoMapBound = false;
    bool normalTextureBound = false;
    bool metallicTextureBound = false;
    bool roughnessTextureBound = false;
    bool aoTextureBound = false;
    bool emissiveTextureBound = false;
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
        baseColorTextureBound = baseColorTextureBound || slot.name == "baseColorTexture";
        opacityTextureBound = opacityTextureBound || slot.name == "opacityTexture";
        albedoMapBound = albedoMapBound || slot.name == "albedoMap";
        normalTextureBound = normalTextureBound || slot.name == "normalTexture";
        metallicTextureBound = metallicTextureBound || slot.name == "metallicTexture";
        roughnessTextureBound = roughnessTextureBound || slot.name == "roughnessTexture";
        aoTextureBound = aoTextureBound || slot.name == "aoTexture";
        emissiveTextureBound = emissiveTextureBound || slot.name == "emissiveTexture";
    }
    tryBindWhiteTexture(material.shader, adapter, "baseColorTexture",
                        baseColorTextureBound);
    tryBindWhiteTexture(material.shader, adapter, "opacityTexture",
                        opacityTextureBound);
    tryBindWhiteTexture(material.shader, adapter, "albedoMap", albedoMapBound);
    tryBindFlatNormalTexture(material.shader, adapter, "normalTexture",
                             normalTextureBound);
    tryBindWhiteTexture(material.shader, adapter, "metallicTexture",
                        metallicTextureBound);
    tryBindWhiteTexture(material.shader, adapter, "roughnessTexture",
                        roughnessTextureBound);
    tryBindWhiteTexture(material.shader, adapter, "aoTexture", aoTextureBound);
    tryBindWhiteTexture(material.shader, adapter, "emissiveTexture",
                        emissiveTextureBound);

    resolveAndApplyColorUniforms(material);

    for (const GpuMaterial::UniformSlot& slot : material.uniformSlots) {
        if (slot.name.empty() || slot.size == 0) {
            continue;
        }
        const shader::BindingId binding =
            material.shader.getUniformBinding(slot.name);
        if (binding != shader::InvalidBinding) {
            material.shader.setUniform(binding, slot.data, slot.size);
        }
    }

    if (mode == SubmitMode::SelectionHull) {
        // Override both common texture paths and the material tint after its
        // own slots were uploaded. This keeps the rim readable and prevents a
        // selected material from turning the complete object white/red/black.
        tryBindWhiteTexture(material.shader, adapter, "baseColorTexture", false);
        tryBindWhiteTexture(material.shader, adapter, "albedoMap", false);
        tryBindWhiteTexture(material.shader, adapter, "opacityTexture", false);
        if (material.colorBinding != shader::InvalidBinding
            && material.shader.hasUniformBinding(material.colorBinding)) {
            constexpr float selectionColor[4] = {
                1.0f, 0.55f, 0.12f, 1.0f
            };
            material.shader.setUniform(material.colorBinding,
                                       selectionColor,
                                       sizeof(selectionColor));
        }
    }

    // Frame-owned lighting state is uploaded after material slots so an asset
    // cannot accidentally leave stale SceneLights/atlas data on a shared
    // program. Shaders without the new arrays keep the legacy uniforms above.
    (void)uploadSceneLighting(material.shader, lights);
    (void)uploadShadowAtlas(material.shader, shadows);
    bindTransparentEnvironment(material.shader, ctx);

    // Built-in PBR transparents evaluate fog from their own fragment world
    // position after opaque haze has completed. Optional bindings keep custom
    // shaders source-compatible; an absent uniform simply skips this feature.
    const bool hazeActive = ctx.depthHazePass != nullptr
        && ctx.depthHazePass->producedThisFrame();
    const float depthHaze[4] = {
        frame.hazeDensity,
        hazeActive ? frame.hazeStrength : 0.0f,
        material.blendMode == BlendMode::Additive ? 1.0f : 0.0f,
        0.0f
    };
    const float depthHazeColor[4] = {
        frame.hazeColor.x, frame.hazeColor.y, frame.hazeColor.z, 0.0f
    };
    trySetUniformVec4(material.shader, "depthHaze", depthHaze);
    trySetUniformVec4(material.shader, "depthHazeColor", depthHazeColor);

    if (material.boneBlockBinding == shader::InvalidBinding) {
        material.boneBlockBinding =
            material.shader.getUniformBlockBinding("Skeleton");
    }
    tryUploadBonePalette(material.shader,
                         material.boneBlockBinding,
                         material.shader.getUniformBinding("castSkinned"),
                         /*castSkinnedValue=*/1u,
                         item);

    ayt::shader::DrawCallContext drawCtx;
    drawCtx.viewId = viewId;
    drawCtx.state  = 0;
    material.shader.submit(drawCtx);

    result.accepted = true;
    result.drawnIndexCount = drawRange.indexCount;
    return result;
}

bgfx::FrameBufferHandle TransparentPass::ensureDeferredCompositeFbo(
    BGFXAdapter& adapter,
    bgfx::TextureHandle color,
    bgfx::TextureHandle depth)
{
    if (!BGFXAdapter::isValid(color) || !BGFXAdapter::isValid(depth)) {
        destroyResources(adapter);
        return BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_deferredCompositeFbo)
        && _deferredColor.idx == color.idx
        && _deferredDepth.idx == depth.idx) {
        return _deferredCompositeFbo;
    }

    destroyResources(adapter);
    _deferredCompositeFbo =
        adapter.createBorrowedColorDepthFrameBuffer(color, depth);
    if (BGFXAdapter::isValid(_deferredCompositeFbo)) {
        _deferredColor = color;
        _deferredDepth = depth;
    }
    return _deferredCompositeFbo;
}

void TransparentPass::destroyResources(BGFXAdapter& adapter) noexcept
{
    if (BGFXAdapter::isValid(_deferredCompositeFbo)) {
        adapter.destroy(_deferredCompositeFbo);
    }
    _deferredCompositeFbo = BGFX_INVALID_HANDLE;
    _deferredColor = BGFX_INVALID_HANDLE;
    _deferredDepth = BGFX_INVALID_HANDLE;
}

uint32_t TransparentPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    const FrameContext& frame = ctx.frame;
    const uint16_t viewportX      = ctx.viewportX;
    const uint16_t viewportY      = ctx.viewportY;
    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    const RenderScene& scene = ctx.scene;

    if (!adapter.isInitialized()) {
        return 0;
    }

    const TransparentRoute route = selectTransparentRoute(
        ctx.gbufferPass != nullptr,
        ctx.lightingPass != nullptr,
        ctx.gbufferPass != nullptr && ctx.gbufferPass->producedThisFrame(),
        ctx.lightingPass != nullptr && ctx.lightingPass->producedThisFrame(),
        ctx.lightingPass != nullptr
            && BGFXAdapter::isValid(ctx.lightingPass->lightingOutputFbo()));
    if (route == TransparentRoute::Skip) {
        rateLimitedEarlyReturn(
            "TransparentPass",
            "deferred producer missing/stale — transparent composite skipped");
        return 0;
    }
    const bool deferredLitComposite = route == TransparentRoute::Deferred;
    const uint8_t viewId = deferredLitComposite
        ? ayt::render::kTransparentDeferredViewId
        : ctx.viewId;

    const auto& items = scene.items();
    const std::string outlineEnv =
        ayt::io::env::get("AY_EDITOR_OUTLINE").value_or("");
    const bool outlineEnabled = outlineEnv.empty() || outlineEnv != "0";

    // Filter Alpha-only before sorting, but never remove a selected Alpha
    // surface from its normal draw. Selection is an additional depth-aware
    // hull below, not a replacement material pass.
    std::vector<TransparentSortEntry> sortedItems;
    std::vector<const DrawItem*> outlineItems;
    sortedItems.reserve(items.size());
    outlineItems.reserve(items.size());
    for (const DrawItem& item : items) {
        if (outlineEnabled && item.outlineHull) {
            outlineItems.push_back(&item);
        }
        const auto matIt = ctx.materials.find(item.material.id);
        if (matIt == ctx.materials.end()) {
            continue;
        }
        if (!ayt::render::isTransparentBlendMode(matIt->second.blendMode)) {
            continue;
        }
        sortedItems.push_back({
            &item, transparentDistanceSquared(item, frame.cameraPosition)
        });
    }
    if (sortedItems.empty() && outlineItems.empty()) {
        return 0;
    }
    std::stable_sort(sortedItems.begin(), sortedItems.end(),
                     transparentSortBefore);

    bgfx::FrameBufferHandle compositeFbo = ctx.sceneFbo;
    bool borrowedDepth = false;
    if (deferredLitComposite) {
        const bgfx::FrameBufferHandle lightingFbo =
            selectSceneColorSourceFbo(ctx);
        const bgfx::TextureHandle color =
            adapter.getFboAttachment(lightingFbo, 0);
        const bgfx::TextureHandle depth = ctx.gbufferPass->gbufferDepthRt();
        compositeFbo = ensureDeferredCompositeFbo(adapter, color, depth);
        borrowedDepth = BGFXAdapter::isValid(compositeFbo);
        if (!borrowedDepth) {
            rateLimitedEarlyReturn(
                "TransparentPass",
                "deferred color+depth FBO unavailable — fail closed");
            return 0;
        }
    }

    adapter.setViewTransform(viewId, frame.view, frame.projection);
    // CPU back-to-front order must survive bgfx's program/state sorting.
    adapter.setViewMode(viewId, bgfx::ViewMode::Sequential);

    uint32_t drawCount = 0;

    adapter.setViewFrameBuffer(viewId, compositeFbo);
    if (BGFXAdapter::isValid(compositeFbo)) {
        adapter.setViewRect(viewId, 0, 0, viewportWidth, viewportHeight);
    } else {
        adapter.setViewRect(viewId, viewportX, viewportY, viewportWidth, viewportHeight);
    }
    static uint32_t s_orderLogFrames = 0;
    if (s_orderLogFrames < 3) {
        uint32_t rank = 0;
        for (const TransparentSortEntry& entry : sortedItems) {
            const DrawItem* item = entry.item;
            std::fprintf(stderr,
                         "[TransparentOrder] frame=%u rank=%u material=%llu "
                         "sortKey=%d distance2=%.3f first=%u count=%u\n",
                         s_orderLogFrames, rank++,
                         static_cast<unsigned long long>(item->material.id),
                         item->sortKey, entry.distanceSquared,
                         item->firstIndex, item->indexCount);
        }
        ++s_orderLogFrames;
    }

    const PackedSceneLighting packedLights =
        packSceneLighting(ctx.sceneLights, frame);
    const PackedShadowAtlas receiverShadows =
        packShadowAtlas(ctx.shadowPass, true);
    const PackedShadowAtlas litFallbackShadows =
        packShadowAtlas(nullptr, false);

    for (const TransparentSortEntry& entry : sortedItems) {
        const DrawItem* pItem = entry.item;
        const auto matIt = ctx.materials.find(pItem->material.id);
        if (matIt == ctx.materials.end()) {
            continue;
        }
        const GpuMaterial& material = matIt->second;

        adapter.setState(transparentDrawState(
            material.blendMode,
            material.premultipliedAlpha,
            material.doubleSided,
            reversesWinding(pItem->world)));

        const PackedShadowAtlas& packedShadows =
            receivesShadow(pItem->shadowFlags)
                ? receiverShadows
                : litFallbackShadows;

        const SubmitResult res = submitItem(
            adapter, ctx, frame, *pItem, viewId,
            packedLights, packedShadows,
            SubmitMode::TransparentSurface);
        if (res.accepted) {
            ++drawCount;
        }
    }

    // Draw selection before PostProcess/Present while this view still owns
    // both the scene color and its depth attachment. Transparent selections
    // need the first depth-only submission because their normal surface does
    // not write Z; opaque selections harmlessly refresh the existing depth.
    for (const DrawItem* pItem : outlineItems) {
        const auto matIt = ctx.materials.find(pItem->material.id);
        if (matIt == ctx.materials.end()) {
            continue;
        }
        const GpuMaterial& material = matIt->second;
        const PackedShadowAtlas& packedShadows =
            receivesShadow(pItem->shadowFlags)
                ? receiverShadows
                : litFallbackShadows;

        DrawItem depthItem = *pItem;
        depthItem.world = pItem->hasOutlineDepthWorld
            ? pItem->outlineDepthWorld : pItem->world;
        adapter.setState(selectionDepthState(
            material.doubleSided, reversesWinding(depthItem.world)));
        const SubmitResult depthResult = submitItem(
            adapter, ctx, frame, depthItem, viewId,
            packedLights, packedShadows, SubmitMode::SelectionDepth);
        if (depthResult.accepted) {
            ++drawCount;
        }

        DrawItem hullItem = *pItem;
        hullItem.world = makeSelectionHullWorld(*pItem);
        adapter.setState(selectionHullState(reversesWinding(hullItem.world)));
        const SubmitResult hullResult = submitItem(
            adapter, ctx, frame, hullItem, viewId,
            packedLights, packedShadows, SubmitMode::SelectionHull);
        if (hullResult.accepted) {
            ++drawCount;
        }
    }

    static uint32_t s_routeLogFrame = 0;
    if (s_routeLogFrame < 8) {
        std::fprintf(stderr,
                     "[TransparentRoute] frame=%u items=%zu candidates=%zu "
                     "draws=%u deferred=%d borrowedDepth=%d\n",
                     s_routeLogFrame, items.size(), sortedItems.size(), drawCount,
                     deferredLitComposite ? 1 : 0,
                     borrowedDepth ? 1 : 0);
        ++s_routeLogFrame;
    }

    return drawCount;
}

} // namespace ayt::render::detail
