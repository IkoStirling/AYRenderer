#include "detail/TransparentPass.h"
#include "detail/RasterConvention.h"
#include "detail/GpuResources.h"
#include "detail/ShadowPass.h"
#include "detail/LightingPass.h"
#include "detail/GBufferPass.h"
#include "detail/SkyboxPass.h"
#include "detail/DepthHazePass.h"
#include "detail/FrameDrawLists.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/SceneColorPipeline.h"

#include "AYRenderer/RenderTypes.h"
#include "AYShader/ShaderResource.h"
#include <AYIO/Env.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
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

uint64_t selectionMaskState(bool doubleSided,
                            bool reverseWinding) noexcept
{
    uint64_t state = BGFX_STATE_WRITE_A
                   | BGFX_STATE_DEPTH_TEST_ALWAYS;
    if (!doubleSided) {
        state |= reverseWinding ? kCullFrontFaces : kCullBackFaces;
    }
    return state;
}

uint64_t selectionVisibleMaskState(bool doubleSided,
                                   bool reverseWinding) noexcept
{
    uint64_t state = BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_DEPTH_TEST_LEQUAL;
    if (!doubleSided) {
        state |= reverseWinding ? kCullFrontFaces : kCullBackFaces;
    }
    return state;
}

uint64_t selectionCompositeState() noexcept
{
    return BGFX_STATE_WRITE_RGB
         | BGFX_STATE_WRITE_A
         | BGFX_STATE_BLEND_ALPHA
         | BGFX_STATE_DEPTH_TEST_ALWAYS;
}

namespace {

constexpr const char* kSelectionOutlinePhoskiaSource = R"(
material EditorSelectionOutline {
    texture2d selectionMask
    uniform vec4 selectionTexelSize
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let stableUv = vec2(vUv.x, 1.0 - vUv.y)
        let visibleUv = stableUv + selectionTexelSize.zw
        let radius = selectionTexelSize.xy * 2.25
        let diagonal = radius * 0.70710678
        let center = sample(selectionMask, stableUv).w
        let a0 = sample(selectionMask, stableUv + vec2( radius.x, 0.0)).w
        let a1 = sample(selectionMask, stableUv + vec2(-radius.x, 0.0)).w
        let a2 = sample(selectionMask, stableUv + vec2(0.0,  radius.y)).w
        let a3 = sample(selectionMask, stableUv + vec2(0.0, -radius.y)).w
        let a4 = sample(selectionMask, stableUv + vec2( diagonal.x,  diagonal.y)).w
        let a5 = sample(selectionMask, stableUv + vec2(-diagonal.x,  diagonal.y)).w
        let a6 = sample(selectionMask, stableUv + vec2( diagonal.x, -diagonal.y)).w
        let a7 = sample(selectionMask, stableUv + vec2(-diagonal.x, -diagonal.y)).w
        let v0 = sample(selectionMask, visibleUv + vec2( radius.x, 0.0)).x
        let v1 = sample(selectionMask, visibleUv + vec2(-radius.x, 0.0)).x
        let v2 = sample(selectionMask, visibleUv + vec2(0.0,  radius.y)).x
        let v3 = sample(selectionMask, visibleUv + vec2(0.0, -radius.y)).x
        let v4 = sample(selectionMask, visibleUv + vec2( diagonal.x,  diagonal.y)).x
        let v5 = sample(selectionMask, visibleUv + vec2(-diagonal.x,  diagonal.y)).x
        let v6 = sample(selectionMask, visibleUv + vec2( diagonal.x, -diagonal.y)).x
        let v7 = sample(selectionMask, visibleUv + vec2(-diagonal.x, -diagonal.y)).x
        let neighborSum = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7
        let visibleSum = v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7
        // An eight-tap circular ring keeps the diagonal radius equal to the
        // axial radius. Fractional offsets engage linear filtering so the
        // stable post-TAA mask produces sub-pixel coverage instead of the
        // square, binary staircase of the former 2x2 dilation kernel.
        let ringCoverage = neighborSum * 0.125
        let visibleRingCoverage = visibleSum * 0.125
        let outerCoverage = smoothstep(0.025, 0.30, ringCoverage)
        let visibleCoverage = smoothstep(0.025, 0.30,
                                         visibleRingCoverage)
        let centerCoverage = smoothstep(0.20, 0.80, center)
        let edge = outerCoverage * (1.0 - centerCoverage)
                 * visibleCoverage
        return vec4(1.0, 0.55, 0.12, edge)
    }
}
)";

constexpr const char* kSelectionOutlineCacheKey =
    "editor_selection_mask_circular_coverage_v7_stable_silhouette";

} // namespace

const char* selectionOutlinePhoskiaSourceForTests() noexcept
{
    return kSelectionOutlinePhoskiaSource;
}

namespace {

void bindTransparentEnvironment(shader::ShaderResource& shader,
                                const PassExecContext& ctx)
{
    bool cubeActive = false;
    bool iblV2Active = false;
    float iblMaxLod = 0.0f;
    if (ctx.skyboxPass != nullptr && ctx.skyboxPass->hasCubeTexture()) {
        const uint64_t cubeTextureId = ctx.skyboxPass->cubeTexture().id;
        const auto cubeIt = ctx.textures.find(cubeTextureId);
        const shader::BindingId cubeBinding = shader.getTextureBinding("envCube");
        if (cubeBinding != shader::InvalidBinding
            && cubeIt != ctx.textures.end()
            && BGFXAdapter::isValid(cubeIt->second.handle)) {
            shader.setTexture(shader.getTextureStage(cubeBinding), cubeBinding,
                              toShaderTexture(cubeIt->second.handle));
            cubeActive = true;
        }
        if (ctx.lightingPass != nullptr
            && ctx.lightingPass->iblV2ReadyFor(cubeTextureId)) {
            const shader::BindingId irradianceBinding =
                shader.getTextureBinding("irradianceCube");
            const shader::BindingId prefilterBinding =
                shader.getTextureBinding("prefilteredSpecularCube");
            const shader::BindingId brdfBinding =
                shader.getTextureBinding("brdfLut");
            if (irradianceBinding != shader::InvalidBinding
                && prefilterBinding != shader::InvalidBinding
                && brdfBinding != shader::InvalidBinding) {
                shader.setTexture(
                    shader.getTextureStage(irradianceBinding),
                    irradianceBinding,
                    toShaderTexture(
                        ctx.lightingPass->iblIrradianceTexture()));
                shader.setTexture(
                    shader.getTextureStage(prefilterBinding),
                    prefilterBinding,
                    toShaderTexture(
                        ctx.lightingPass->iblPrefilteredSpecularTexture()));
                shader.setTexture(
                    shader.getTextureStage(brdfBinding), brdfBinding,
                    toShaderTexture(ctx.lightingPass->iblBrdfLutTexture()));
                iblV2Active = true;
                iblMaxLod = ctx.lightingPass->iblMaxSpecularLod();
            }
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
    const float iblV2Value[4] = {
        iblV2Active ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f
    };
    const float iblLodValue[4] = { iblMaxLod, 0.0f, 0.0f, 0.0f };
    trySetUniformVec4(shader, "cubeActive", cubeActiveValue);
    trySetUniformVec4(shader, "iblV2Active", iblV2Value);
    trySetUniformVec4(shader, "iblMaxLod", iblLodValue);
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
    if (item.particleBatch) {
        if (!adapter.bindParticleBatch(*item.particleBatch)) { result.skip = true; return result; }
    } else adapter.setVertexBuffer(mesh.vertexBuffer);
    const bool wireframe = item.particleBatch ? false : bindDrawIndexBuffer(
        adapter, mesh, drawRange,
        mode == SubmitMode::TransparentSurface && ctx.wireframe);
    if (mode == SubmitMode::TransparentSurface) {
        uint64_t state = transparentDrawState(
            material.blendMode,
            material.premultipliedAlpha,
            material.doubleSided,
            reversesWinding(item.world));
        if (item.particleBatch && material.blendMode==BlendMode::Additive) {
            state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL
                | BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE,
                    BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ONE);
        }
        if (wireframe) state |= BGFX_STATE_PT_LINES;
        adapter.setState(state);
    }

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

    if (mode == SubmitMode::SelectionMask) {
        // Produce an opaque white visibility mask regardless of the selected
        // material's albedo/opacity. The mask is never shown directly; a
        // fullscreen dilation pass turns only its outer edge orange.
        tryBindWhiteTexture(material.shader, adapter, "baseColorTexture", false);
        tryBindWhiteTexture(material.shader, adapter, "albedoMap", false);
        tryBindWhiteTexture(material.shader, adapter, "opacityTexture", false);
        if (material.colorBinding != shader::InvalidBinding
            && material.shader.hasUniformBinding(material.colorBinding)) {
            constexpr float selectionColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
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
    const bool hazeActive = ctx.resourceBlackboard != nullptr
        ? ctx.resourceBlackboard->findProduced(
              BlackboardResourceId::DepthHazeColor) != nullptr
        : ctx.depthHazePass != nullptr
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

void TransparentPass::destroySelectionTarget(BGFXAdapter& adapter) noexcept
{
    if (BGFXAdapter::isValid(_selectionStableMaskFbo)) {
        adapter.destroy(_selectionStableMaskFbo);
    }
    if (BGFXAdapter::isValid(_selectionMaskFbo)) {
        adapter.destroy(_selectionMaskFbo);
    }
    if (BGFXAdapter::isValid(_selectionMaskTexture)) {
        adapter.destroy(_selectionMaskTexture);
    }
    _selectionStableMaskFbo = BGFX_INVALID_HANDLE;
    _selectionMaskFbo = BGFX_INVALID_HANDLE;
    _selectionMaskTexture = BGFX_INVALID_HANDLE;
    _selectionSceneDepth = BGFX_INVALID_HANDLE;
    _selectionWidth = 0;
    _selectionHeight = 0;
}

void TransparentPass::ensureSelectionCompositeProgram(
    shader::ShaderResourcePool& pool)
{
    if (_selectionCompositeProgram.isValid()) {
        return;
    }
    if (_selectionProgramRetryFrames > 0) {
        --_selectionProgramRetryFrames;
        return;
    }

    shader::ShaderResource program = pool.acquire(
        kSelectionOutlinePhoskiaSource, kSelectionOutlineCacheKey);
    const shader::BindingId maskBinding = program.isValid()
        ? program.getTextureBinding("selectionMask")
        : shader::InvalidBinding;
    const shader::BindingId texelBinding = program.isValid()
        ? program.getUniformBinding("selectionTexelSize")
        : shader::InvalidBinding;
    if (!program.isValid()
        || maskBinding == shader::InvalidBinding
        || texelBinding == shader::InvalidBinding) {
        _selectionProgramRetryFrames = 120;
        std::fprintf(stderr,
                     "[TransparentPass] selection-mask composite shader "
                     "unavailable; retrying in 120 frames.\n");
        for (const std::string& error : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[TransparentPass]   %s\n", error.c_str());
        }
        return;
    }

    _selectionCompositeProgram = std::move(program);
    _selectionMaskBinding = maskBinding;
    _selectionTexelSizeBinding = texelBinding;
    _selectionProgramRetryFrames = 0;
}

bool TransparentPass::ensureSelectionResources(
    PassExecContext& ctx,
    bgfx::TextureHandle sceneDepth)
{
    BGFXAdapter& adapter = ctx.adapter;
    if (!BGFXAdapter::isValid(sceneDepth)
        || ctx.viewportWidth == 0 || ctx.viewportHeight == 0) {
        return false;
    }

    const bool targetChanged = _selectionWidth != ctx.viewportWidth
        || _selectionHeight != ctx.viewportHeight
        || !BGFXAdapter::isValid(_selectionMaskTexture)
        || !BGFXAdapter::isValid(_selectionStableMaskFbo)
        || !BGFXAdapter::isValid(_selectionMaskFbo)
        || !BGFXAdapter::isValid(_selectionSceneDepth)
        || _selectionSceneDepth.idx != sceneDepth.idx;
    if (targetChanged) {
        destroySelectionTarget(adapter);
    }

    if (!BGFXAdapter::isValid(_selectionMaskTexture)) {
        const uint64_t flags = BGFX_TEXTURE_RT
            | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        _selectionMaskTexture = adapter.createDynamicTexture2D(
            ctx.viewportWidth, ctx.viewportHeight, flags);
        if (BGFXAdapter::isValid(_selectionMaskTexture)) {
            _selectionStableMaskFbo =
                adapter.createBorrowedColorFrameBuffer(_selectionMaskTexture);
            _selectionMaskFbo = adapter.createBorrowedColorDepthFrameBuffer(
                _selectionMaskTexture, sceneDepth);
        }
        if (!BGFXAdapter::isValid(_selectionMaskTexture)
            || !BGFXAdapter::isValid(_selectionStableMaskFbo)
            || !BGFXAdapter::isValid(_selectionMaskFbo)) {
            destroySelectionTarget(adapter);
            return false;
        }
        _selectionSceneDepth = sceneDepth;
        _selectionWidth = ctx.viewportWidth;
        _selectionHeight = ctx.viewportHeight;
    }

    if (!_selectionCompositeGeometry.ensure(adapter)) {
        return false;
    }
    ensureSelectionCompositeProgram(ctx.pool);
    return _selectionCompositeProgram.isValid()
        && _selectionMaskBinding != shader::InvalidBinding
        && _selectionTexelSizeBinding != shader::InvalidBinding;
}

void TransparentPass::destroyResources(BGFXAdapter& adapter) noexcept
{
    destroySelectionTarget(adapter);
    _selectionCompositeGeometry.destroy(adapter);
    _selectionCompositeProgram.reset();
    _selectionMaskBinding = shader::InvalidBinding;
    _selectionTexelSizeBinding = shader::InvalidBinding;
    _selectionProgramRetryFrames = 0;

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

    BlackboardGBufferView blackboardGBuffer;
    const bool gbufferProduced = ctx.resourceBlackboard != nullptr
        ? ctx.resourceBlackboard->resolveGBuffer(blackboardGBuffer)
        : ctx.gbufferPass != nullptr
            && ctx.gbufferPass->producedThisFrame();
    const BlackboardResourceEntry* lighting =
        ctx.resourceBlackboard != nullptr
            ? ctx.resourceBlackboard->findProduced(
                  BlackboardResourceId::LightingColor)
            : nullptr;
    const bool lightingProduced = ctx.resourceBlackboard != nullptr
        ? lighting != nullptr
        : ctx.lightingPass != nullptr
            && ctx.lightingPass->producedThisFrame();
    const bool lightingTargetValid = ctx.resourceBlackboard != nullptr
        ? lighting != nullptr
            && BGFXAdapter::isValid(lighting->framebuffer)
        : ctx.lightingPass != nullptr
            && BGFXAdapter::isValid(ctx.lightingPass->lightingOutputFbo());
    const TransparentRoute route = selectTransparentRoute(
        ctx.gbufferPass != nullptr,
        ctx.lightingPass != nullptr,
        gbufferProduced,
        lightingProduced,
        lightingTargetValid);
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

    // Classification and sorting are shared with all other geometry passes.
    // Selection remains an additional silhouette draw, never a replacement.
    FrameDrawLists fallbackDrawLists;
    const FrameDrawLists& drawLists =
        resolveFrameDrawLists(ctx, fallbackDrawLists);
    const auto& sortedItems = drawLists.transparent3D;
    const auto& allOutlineItems = drawLists.selectionOutlines;
    static const std::vector<const DrawItem*> kNoOutlineItems;
    const auto& outlineItems = outlineEnabled
        ? allOutlineItems : kNoOutlineItems;
    if (sortedItems.empty() && outlineItems.empty()) {
        return 0;
    }

    bgfx::FrameBufferHandle compositeFbo = ctx.sceneFbo;
    bool borrowedDepth = false;
    if (deferredLitComposite) {
        const bgfx::FrameBufferHandle lightingFbo =
            selectSceneColorSourceFbo(ctx);
        const bgfx::TextureHandle color =
            adapter.getFboAttachment(lightingFbo, 0);
        const bgfx::TextureHandle depth = ctx.resourceBlackboard != nullptr
            ? blackboardGBuffer.depth : ctx.gbufferPass->gbufferDepthRt();
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
        for (const SortedTransparentItem& entry : sortedItems) {
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

    for (const SortedTransparentItem& entry : sortedItems) {
        const DrawItem* pItem = entry.item;
        if (pItem->particleBatch && pItem->particleBatch->world3D
            && pItem->particleBatch->gpuStream && pItem->particleBatch->submitGpu) {
            pItem->particleBatch->submitGpu(pItem->particleBatch->gpuStream, viewId);
            ++drawCount;
            continue;
        }
        const auto matIt = ctx.materials.find(pItem->material.id);
        if (matIt == ctx.materials.end()) {
            continue;
        }
        const GpuMaterial& material = matIt->second;

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

    // Alpha stores an unjittered silhouette, while RGB stores the selected
    // surface rasterized with the same jittered projection and depth as the
    // scene. The composite samples those channels on their respective grids:
    // stable alpha fixes the rim in output pixels and jitter-aligned RGB keeps
    // foreground occlusion. This avoids feeding editor chrome through TAA.
    uint32_t selectionMaskDraws = 0;
    uint32_t selectionVisibleMaskDraws = 0;
    const bgfx::TextureHandle sceneDepth = BGFXAdapter::isValid(compositeFbo)
        ? adapter.getFboAttachment(compositeFbo, 1)
        : bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    if (!outlineItems.empty()
        && ensureSelectionResources(ctx, sceneDepth)) {
        constexpr uint8_t stableMaskViewId = kSelectionStableMaskViewId;
        constexpr uint8_t maskViewId = kSelectionMaskViewId;
        adapter.setViewFrameBuffer(stableMaskViewId, _selectionStableMaskFbo);
        adapter.setViewRect(stableMaskViewId, 0, 0,
                            viewportWidth, viewportHeight);
        adapter.setViewTransform(stableMaskViewId, frame.view,
                                 _selectionUnjitteredProjection);
        adapter.setViewMode(stableMaskViewId, bgfx::ViewMode::Sequential);
        adapter.setViewClearRaw(stableMaskViewId, BGFX_CLEAR_COLOR,
                                0x00000000u, 1.0f, 0);
        adapter.touch(stableMaskViewId);

        adapter.setViewFrameBuffer(maskViewId, _selectionMaskFbo);
        adapter.setViewRect(maskViewId, 0, 0, viewportWidth, viewportHeight);
        // Only RGB visibility borrows scene depth, so it must use the exact
        // same jittered projection. Alpha was already written without depth
        // on the stable view and is preserved because this view does not clear.
        adapter.setViewTransform(maskViewId, frame.view, frame.projection);
        adapter.setViewMode(maskViewId, bgfx::ViewMode::Sequential);
        adapter.touch(maskViewId);

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

            DrawItem maskItem = *pItem;
            maskItem.world = pItem->hasOutlineDepthWorld
                ? pItem->outlineDepthWorld : pItem->world;
            adapter.setState(selectionMaskState(
                material.doubleSided, reversesWinding(maskItem.world)));
            const SubmitResult maskResult = submitItem(
                adapter, ctx, frame, maskItem, stableMaskViewId,
                packedLights, packedShadows, SubmitMode::SelectionMask);
            if (maskResult.accepted) {
                ++selectionMaskDraws;
                ++drawCount;
            }

            adapter.setState(selectionVisibleMaskState(
                material.doubleSided, reversesWinding(maskItem.world)));
            const SubmitResult visibleMaskResult = submitItem(
                adapter, ctx, frame, maskItem, maskViewId,
                packedLights, packedShadows, SubmitMode::SelectionMask);
            if (visibleMaskResult.accepted) {
                ++selectionVisibleMaskDraws;
                ++drawCount;
            }
        }

        if (selectionMaskDraws != 0 && selectionVisibleMaskDraws != 0) {
            configureFullscreenPassView(
                adapter, kSelectionCompositeViewId,
                bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE},
                viewportX, viewportY, viewportWidth, viewportHeight);
            _selectionCompositeGeometry.bind(adapter);
            _selectionCompositeProgram.setTexture(
                0, _selectionMaskBinding,
                toShaderTexture(_selectionMaskTexture));
            const float texelSize[4] = {
                1.0f / static_cast<float>(viewportWidth),
                1.0f / static_cast<float>(viewportHeight),
                _selectionJitterXPixels / static_cast<float>(viewportWidth),
                _selectionJitterYPixels / static_cast<float>(viewportHeight)
            };
            _selectionCompositeProgram.setUniform(
                _selectionTexelSizeBinding, texelSize, sizeof(texelSize));
            adapter.setState(selectionCompositeState());

            ayt::shader::DrawCallContext compositeDraw;
            compositeDraw.viewId = kSelectionCompositeViewId;
            compositeDraw.state = 0;
            _selectionCompositeProgram.submit(compositeDraw);
            ++drawCount;
        }
    }

    static uint32_t s_routeLogFrame = 0;
    if (s_routeLogFrame < 8) {
        std::fprintf(stderr,
                     "[TransparentRoute] frame=%u items=%zu candidates=%zu "
                     "draws=%u maskDraws=%u visibleMaskDraws=%u "
                     "deferred=%d borrowedDepth=%d\n",
                     s_routeLogFrame, items.size(), sortedItems.size(), drawCount,
                     selectionMaskDraws, selectionVisibleMaskDraws,
                     deferredLitComposite ? 1 : 0,
                     borrowedDepth ? 1 : 0);
        ++s_routeLogFrame;
    }

    return drawCount;
}

} // namespace ayt::render::detail
