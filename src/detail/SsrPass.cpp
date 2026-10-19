#include "detail/SsrPass.h"
#include "AYRenderer/SsrShaderSources.h"
#include "detail/FrameContext.h"
#include "detail/PassExecContext.h"
#include "detail/LightingPass.h"
#include "detail/FgResource.h"
#include "detail/GpuResources.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/SceneColorPipeline.h"
#include "detail/BgfxMatrix.h"
#include "detail/IblPrecompute.h"
#include <array>
namespace ayt::render::detail {
uint32_t SsrPass::execute(PassExecContext& ctx) {
    auto& adapter = ctx.adapter;
    const auto s = sanitizeSsrSettings(ctx.frame.ssr);
    const auto gi = sanitizeScreenSpaceIndirect(ctx.frame.screenIndirect);
    if ((!s.enabled || s.strength <= 0) && (!gi.enabled || gi.strength <= 0)) return 0;
    if (!adapter.isInitialized() || adapter.isNoopBackend()
        || !ctx.frameGraph || !ctx.resourceBlackboard || !ctx.lightingPass
        || !ctx.viewportWidth || !ctx.viewportHeight) return 0;
    BlackboardGBufferView gb;
    if (!ctx.resourceBlackboard->resolveGBuffer(gb)
        || !ctx.resourceBlackboard->findProduced(BlackboardResourceId::LightingColor)) return 0;
    const bool hasIbl = ctx.lightingPass->iblActiveThisFrame();
    auto cube = hasIbl ? ctx.lightingPass->iblPrefilteredSpecularTexture() : _blackCube;
    auto lut = hasIbl ? ctx.lightingPass->iblBrdfLutTexture() : _fallbackBrdf;
    if (!hasIbl && (!BGFXAdapter::isValid(cube) || !BGFXAdapter::isValid(lut))) {
        // Reuse the same split-sum integration as IBL; no environment asset is
        // required to reflect already illuminated screen-space receivers.
        const std::array<uint8_t, 24> black{};
        if (!BGFXAdapter::isValid(_blackCube)) _blackCube = adapter.createTextureCube(1, black.data());
        if (!BGFXAdapter::isValid(_fallbackBrdf)) {
            const auto data = buildIblCpuSet(black.data(), 1, 1, 1, 64, 64);
            _fallbackBrdf = adapter.createTexture2DFromData(data.brdfLutSize, data.brdfLutSize,
                bgfx::TextureFormat::RGBA8, data.brdfLutRgba8.data(),
                static_cast<uint32_t>(data.brdfLutRgba8.size()), BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        }
        cube = _blackCube; lut = _fallbackBrdf;
    }
    if (!BGFXAdapter::isValid(cube) || !BGFXAdapter::isValid(lut)) return 0;
    auto input = adapter.getFboAttachment(selectSceneColorSourceFbo(ctx), 0);
    auto target = ctx.frameGraph->resolve(FgResourceId::SsrColor);
    if (!BGFXAdapter::isValid(input) || !BGFXAdapter::isValid(target)) return 0;
    auto output = adapter.getFboAttachment(target, 0);
    if (!BGFXAdapter::isValid(output) || input.idx == output.idx || !_geometry.ensure(adapter)) return 0;
    if (!_program.isValid()) _program = ctx.pool.acquire(kSsrPhoskiaSource, kSsrCacheKey);
    if (!_program.isValid()) return 0;
    auto texture = [&](const char* name, bgfx::TextureHandle handle) {
        auto b = _program.getTextureBinding(name);
        if (b == shader::InvalidBinding) return false;
        _program.setTexture(_program.getTextureStage(b), b, toShaderTexture(handle));
        return true;
    };
    auto uniform = [&](const char* name, const void* value, uint32_t size) {
        auto b = _program.getUniformBinding(name);
        if (b == shader::InvalidBinding) return false;
        _program.setUniform(b, value, size); return true;
    };
    const auto* ao = ctx.resourceBlackboard->findProduced(BlackboardResourceId::SsaoOcclusion);
    const float camera[4] = {ctx.frame.cameraPosition.x, ctx.frame.cameraPosition.y, ctx.frame.cameraPosition.z, 0};
    const float trace[4] = {s.enabled ? s.strength : 0.0f, s.maxDistance, s.thickness, float(s.steps)};
    const float material[4] = {0, s.maxRoughness, s.enabled ? float(s.debugView) : 0.0f, 0};
    const float indirect[4] = {gi.enabled ? gi.strength : 0.0f, gi.maxDistance, gi.thickness, float(gi.steps)};
    const float indirectSamples[4] = {float(gi.rays), 0, 0, 0};
    const float ibl[4] = {hasIbl ? ctx.lightingPass->ambientStrength() : 0.0f, hasIbl ? ctx.lightingPass->iblMaxSpecularLod() : 0.0f, 0, 0};
    const float ssao[4] = {ao ? ctx.frame.ssaoStrength : 0, 1.0f / ctx.viewportWidth, 1.0f / ctx.viewportHeight, 0};
    float view[16], projection[16];
    toBgfxColumnMajor(ctx.frame.view, view);
    toBgfxColumnMajor(ctx.frame.projection, projection);
    if (!texture("sceneColor", input) || !texture("worldPosition", gb.worldPosition)
        || !texture("geometryCoverage", gb.material) || !texture("normalRoughness", gb.normal)
        || !texture("albedoMetallic", gb.albedo) || !texture("ssaoTexture", ao ? ao->texture : gb.material)
        || !texture("prefilteredSpecularCube", cube) || !texture("brdfLut", lut)
        || !uniform("ssrViewMatrix", view, 64) || !uniform("ssrProjectionMatrix", projection, 64)
        || !uniform("cameraPosition", camera, 16) || !uniform("traceParams", trace, 16)
        || !uniform("materialParams", material, 16) || !uniform("iblParams", ibl, 16)
        || !uniform("ssaoParams", ssao, 16) || !uniform("indirectParams", indirect, 16)
        || !uniform("indirectSamples", indirectSamples, 16)) return 0;
    configureFullscreenPassView(adapter, kViewId, target, 0, 0, ctx.viewportWidth, ctx.viewportHeight);
    _geometry.bind(adapter); adapter.setStateDepthTestAlways();
    shader::DrawCallContext submit; submit.viewId = kViewId; submit.state = 0;
    _program.submit(submit);
    ctx.frameGraph->markProduced(FgResourceId::SsrColor);
    ctx.resourceBlackboard->publishProduced(BlackboardResourceId::SsrColor, BlackboardResourceLifetime::Transient,
        target, output, ctx.viewportWidth, ctx.viewportHeight, 0);
    return 1;
}
void SsrPass::destroyResources(BGFXAdapter& adapter) {
    _program.reset(); _geometry.destroy(adapter);
    if (BGFXAdapter::isValid(_blackCube)) adapter.destroy(_blackCube);
    if (BGFXAdapter::isValid(_fallbackBrdf)) adapter.destroy(_fallbackBrdf);
    _blackCube = BGFX_INVALID_HANDLE; _fallbackBrdf = BGFX_INVALID_HANDLE;
}
}
