#include "detail/ShadowPass.h"

#include "AYShadowConfig.h"
#include "detail/ShadowDebug.h"
#include "detail/ShadowDiagnostics.h"
#include "detail/ShadowMatrixBuilder.h"

#include <bgfx/bgfx.h>

#include <cmath>
#include <cstdio>

namespace ayt::render::detail
{

ShadowPass::~ShadowPass() = default;

uint32_t ShadowPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    ayt::shader::ShaderResourcePool& pool = ctx.pool;
    const uint8_t viewId = kShadowViewId;
    const auto& meshes    = ctx.meshes;
    const RenderScene& scene = ctx.scene;

    if (!adapter.isInitialized() || adapter.isNoopBackend()) {
        return 0;
    }

    const bgfx::Caps* caps = bgfx::getCaps();
    logShadowCapsIfNeeded(
        caps != nullptr ? static_cast<uint32_t>(caps->rendererType) : 999u,
        caps != nullptr && (caps->supported & BGFX_CAPS_TEXTURE_BLIT) != 0,
        caps != nullptr && (caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) != 0);

    ++_frameCounter;

    if (_requestedSize == 0) {
        _requestedSize = kDefaultShadowMapSize;
    }
    _mapResources.ensure(adapter, _requestedSize, ayt::render::kShadowPipelineBuildStamp);
    if (!_mapResources.isValid()) {
        if (ayt::render::ShadowDiagnostics::enabled(ayt::render::ShadowLogLevel::L1_Caps)) {
            std::fprintf(stderr,
                         "[ShadowPass] shadow FBO create failed at %ux%u; "
                         "shadow disabled for this frame\n",
                         _requestedSize, _requestedSize);
        }
        return 0;
    }

    _shadowCaster.ensureProgram(pool);
    const bool casterReady = _shadowCaster.isProgramReady();

    const bool homogeneousDepth = caps != nullptr && caps->homogeneousDepth;
    buildDirectionalShadowMatricesForScene(
        scene,
        meshes,
        ctx.frame.lightDirection,
        _lightView,
        _lightProj,
        _lightViewProj,
        _lightViewCol,
        _lightProjCol,
        _lightViewProjCol,
        homogeneousDepth);

    if (ayt::render::ShadowDiagnostics::enabled(ayt::render::ShadowLogLevel::L3_Probe)) {
        logShadowPassCpuDiag(ctx.frame.lightDirection, homogeneousDepth, _lightViewProjCol);
    }

    _mapResources.bindShadowView(adapter, viewId, _requestedSize);
    adapter.setViewTransformColumnMajor(viewId, _lightViewCol, _lightProjCol);
    bgfx::touch(viewId);

    const uint64_t casterState = ShadowMapResources::casterDrawState();
    adapter.setState(casterState);
    if (ayt::render::ShadowDiagnostics::enabled(ayt::render::ShadowLogLevel::L1_Caps)) {
        static uint32_t s_casterStateLog = 0;
        if (s_casterStateLog < 2) {
            std::fprintf(stderr,
                         "[ShadowDbg] caster depthTest=LESS writeZ=1 "
                         "(RGBA8 ndc01 + D24S8 ordering)\n");
            ++s_casterStateLog;
        }
    }

    const uint32_t drawCount = _shadowCaster.drawCasters(
        adapter, viewId, casterState, scene, meshes);

    _mapResources.resolveForSampling(adapter, kShadowResolveViewId);

    const bgfx::TextureHandle shadowColor = _mapResources.colorAttachment(adapter);
    const ShadowProjectSample probe =
        projectWorldThroughLvpColMajor(_lightViewProjCol,
                                       ayt::math::FVector3(0.0f, 0.0f, 0.0f));

    const float maxCoord = static_cast<float>(_requestedSize > 1 ? _requestedSize - 1 : 0);
    ayt::render::ShadowFrameStats stats{};
    stats.frameIndex  = _frameCounter;
    stats.casterDraws = drawCount;
    stats.casterReady = casterReady;
    stats.fboValid    = _mapResources.isValid();
    stats.blitOk      = lastBlitOk();
    stats.sampleReady = hasSampleableShadow();
    stats.probeU      = probe.shadowU;
    stats.probeV      = probe.shadowV;
    stats.expectEnc   = probe.refDepth;
    stats.probePxX    = static_cast<uint16_t>(std::lround(
        std::fmax(0.0f, std::fmin(probe.shadowU * maxCoord, maxCoord))));
    stats.probePxY    = static_cast<uint16_t>(std::lround(
        std::fmax(0.0f, std::fmin(probe.shadowV * maxCoord, maxCoord))));
    stats.rtIdx       = shadowColor.idx;
    stats.resolveIdx  = _mapResources.sampleTexture().idx;
    logShadowFrameStatsIfNeeded(stats);

    return drawCount;
}

void ShadowPass::destroyResources(BGFXAdapter& adapter)
{
    _mapResources.destroy(adapter);
    _shadowCaster.destroy(adapter);
}

} // namespace ayt::render::detail
