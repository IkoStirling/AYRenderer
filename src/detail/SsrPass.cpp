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
namespace ayt::render::detail {
uint32_t SsrPass::execute(PassExecContext& ctx) {
    auto& adapter = ctx.adapter;
    const auto s = sanitizeSsrSettings(ctx.frame.ssr);
    if (!s.enabled || s.strength <= 0 || !adapter.isInitialized() || adapter.isNoopBackend()
        || !ctx.frameGraph || !ctx.resourceBlackboard || !ctx.lightingPass
        || !ctx.viewportWidth || !ctx.viewportHeight) return 0;
    BlackboardGBufferView gb;
    if (!ctx.resourceBlackboard->resolveGBuffer(gb)
        || !ctx.resourceBlackboard->findProduced(BlackboardResourceId::LightingColor)) return 0;
    if (!ctx.lightingPass->iblActiveThisFrame()) return 0;
    auto cube = ctx.lightingPass->iblPrefilteredSpecularTexture();
    auto lut = ctx.lightingPass->iblBrdfLutTexture();
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
    const float trace[4] = {s.strength, s.maxDistance, s.thickness, float(s.steps)};
    const float material[4] = {0, s.maxRoughness, float(s.debugView), 0};
    const float ibl[4] = {ctx.lightingPass->ambientStrength(), ctx.lightingPass->iblMaxSpecularLod(), 0, 0};
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
        || !uniform("ssaoParams", ssao, 16)) return 0;
    configureFullscreenPassView(adapter, kViewId, target, 0, 0, ctx.viewportWidth, ctx.viewportHeight);
    _geometry.bind(adapter); adapter.setStateDepthTestAlways();
    shader::DrawCallContext submit; submit.viewId = kViewId; submit.state = 0;
    _program.submit(submit);
    ctx.frameGraph->markProduced(FgResourceId::SsrColor);
    ctx.resourceBlackboard->publishProduced(BlackboardResourceId::SsrColor, BlackboardResourceLifetime::Transient,
        target, output, ctx.viewportWidth, ctx.viewportHeight, 0);
    return 1;
}
void SsrPass::destroyResources(BGFXAdapter& adapter) { _program.reset(); _geometry.destroy(adapter); }
}
