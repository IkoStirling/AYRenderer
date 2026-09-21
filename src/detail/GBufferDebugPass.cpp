#include "detail/GBufferDebugPass.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GBufferPass.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/RenderPass.h"
#include "detail/ShadowPass.h"

#include <cstdio>
#include <string>

namespace ayt::render::detail
{

namespace {

struct alignas(16) FullscreenVertex {
    float x;
    float y;
    float u;
    float v;
};

constexpr FullscreenVertex kFullscreenTriangle[3] = {
    { -1.0f, -1.0f, 0.0f, 1.0f },
    {  3.0f, -1.0f, 2.0f, 1.0f },
    { -1.0f,  3.0f, 0.0f, -1.0f },
};

constexpr uint16_t kFullscreenIndices[3] = { 0, 1, 2 };

// Visible view-250 overlay. Channels 0..5 preserve the GBuffer contract;
// channels 6..9 use one CPU-selected auxiliary texture so adding diagnostics
// does not consume four more sampler stages.
constexpr const char* kGBufferDebugPhoskiaSource = R"(
material GBufferDebug {
    texture2d albedo
    texture2d normal
    texture2d worldPos
    texture2d materialSurface
    texture2d depthTex
    texture2d auxiliary
    uniform vec4 debugChannel
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let a = sample(albedo, uv)
        let n = sample(normal, uv)
        let w = sample(worldPos, uv)
        let s = sample(materialSurface, uv)
        let d = sample(depthTex, uv).x
        let x = sample(auxiliary, uv)
        let c = debugChannel.x
        let pick0 = 1.0 - step(0.5, c)
        let pick1 = step(0.5, c) * (1.0 - step(1.5, c))
        let pick2 = step(1.5, c) * (1.0 - step(2.5, c))
        let pick3 = step(2.5, c) * (1.0 - step(3.5, c))
        let pick4 = step(3.5, c) * (1.0 - step(4.5, c))
        let pick5 = step(4.5, c) * (1.0 - step(5.5, c))
        let pick6 = step(5.5, c) * (1.0 - step(6.5, c))
        let pick7 = step(6.5, c) * (1.0 - step(7.5, c))
        let pick8 = step(7.5, c) * (1.0 - step(8.5, c))
        let pick9 = step(8.5, c)
        let materialModel = floor(w.a * 0.5 + 0.0001)
        let materialAo = max(0.0, min(1.0, w.a - materialModel * 2.0))
        let modelNorm = min(materialModel, 1.0)
        let albedoView = vec4(a.rgb, 1.0)
        let normalView = vec4(n.xyz, 1.0)
        let worldView = vec4(w.xyz * 0.05 + vec3(0.5, 0.5, 0.5), 1.0)
        let materialView = vec4(a.a, n.a, materialAo, 1.0)
        let modelView = vec4(modelNorm, 1.0 - modelNorm, 0.25, 1.0)
        let depthView = vec4(1.0 - d, 1.0 - d, 1.0 - d, 1.0)
        let motionValid = step(0.5, x.a)
        let motionX = max(0.0, min(1.0, x.x * 16.0 + 0.5))
        let motionY = max(0.0, min(1.0, x.y * 16.0 + 0.5))
        let motionView = vec4(motionX, motionY, motionValid, 1.0)
        let ssaoView = vec4(x.x, x.x, x.x, 1.0)
        let historyView = vec4(x.rgb, 1.0)
        let shadowView = vec4(x.x, x.x, x.x, 1.0)
        let geometryView = (albedoView * pick0 + normalView * pick1
                         + worldView * pick2 + materialView * pick3
                         + modelView * pick5)
                         * step(0.5, s.a)
        return geometryView + depthView * pick4 + motionView * pick6
             + ssaoView * pick7 + historyView * pick8 + shadowView * pick9
    }
}
)";

constexpr const char* kGBufferDebugCacheKey =
    "gbufferdebug_v4_r6_diagnostic_textures";

} // namespace

// File-scope external definition lets contract tests pin the live shader key.
const char* const kGBufferDebugCacheKeyCStr = kGBufferDebugCacheKey;
const char* const kGBufferDebugPhoskiaSourceCStr = kGBufferDebugPhoskiaSource;

uint32_t GBufferDebugPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;

    // The debug pass is part of the fixed pipeline but remains dormant until
    // the host asks for a GBuffer visualization.
    if (!ctx.frame.gbufferDebugEnabled) {
        return 0;
    }

    if (!adapter.isInitialized()) {
        rateLimitedEarlyReturn("GBufferDebugPass", "adapter not initialized");
        return 0;
    }
    if (adapter.isNoopBackend()) {
        rateLimitedEarlyReturn("GBufferDebugPass", "noop backend");
        return 0;
    }

    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    if (viewportWidth == 0 || viewportHeight == 0) {
        rateLimitedEarlyReturn("GBufferDebugPass", "viewport==0");
        return 0;
    }

    const uint8_t channel = ctx.frame.gbufferDebugChannel;
    const bool needsGBuffer = channel
        < static_cast<uint8_t>(GBufferDebugChannel::MotionVectors);
    BlackboardGBufferView blackboardGBuffer;
    const bool useBlackboard = ctx.resourceBlackboard != nullptr;
    const bool blackboardReady = useBlackboard
        && ctx.resourceBlackboard->resolveGBuffer(blackboardGBuffer);
    const bool legacyReady = !useBlackboard && ctx.gbufferPass != nullptr
        && ctx.gbufferPass->producedThisFrame()
        && ctx.gbufferPass->hasValidAttachments();
    if (needsGBuffer && !blackboardReady && !legacyReady) {
        rateLimitedEarlyReturn("GBufferDebugPass", "gbuffer not produced this frame");
        return 0;
    }

    bgfx::TextureHandle auxiliary = BGFX_INVALID_HANDLE;
    if (channel == static_cast<uint8_t>(
            GBufferDebugChannel::MotionVectors)) {
        const BlackboardResourceEntry* motion = useBlackboard
            ? ctx.resourceBlackboard->findProduced(
                  BlackboardResourceId::MotionVectors)
            : nullptr;
        auxiliary = motion != nullptr ? motion->texture
                                      : bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    } else if (channel == static_cast<uint8_t>(
                   GBufferDebugChannel::SsaoOcclusion)) {
        const BlackboardResourceEntry* ssao = useBlackboard
            ? ctx.resourceBlackboard->findProduced(
                  BlackboardResourceId::SsaoOcclusion)
            : nullptr;
        auxiliary = ssao != nullptr ? ssao->texture
                                    : bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    } else if (channel == static_cast<uint8_t>(
                   GBufferDebugChannel::TaaHistory)) {
        const BlackboardResourceEntry* history = useBlackboard
            ? ctx.resourceBlackboard->find(
                  BlackboardResourceId::TaaHistoryRead)
            : nullptr;
        if (history != nullptr && history->contentValid) {
            auxiliary = BGFXAdapter::isValid(history->texture)
                ? history->texture
                : adapter.getFboAttachment(history->framebuffer, 0);
        }
    } else if (channel == static_cast<uint8_t>(
                   GBufferDebugChannel::ShadowAtlas)) {
        auxiliary = ctx.shadowPass != nullptr
                && ctx.shadowPass->hasSampleableShadow()
            ? ctx.shadowPass->shadowSampleTexture()
            : bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    }

    if (!needsGBuffer && !BGFXAdapter::isValid(auxiliary)) {
        rateLimitedEarlyReturn(
            "GBufferDebugPass", "selected diagnostic texture unavailable");
        return 0;
    }

    const bgfx::TextureHandle safeTexture = BGFXAdapter::isValid(auxiliary)
        ? auxiliary
        : blackboardReady ? blackboardGBuffer.albedo
        : ctx.gbufferPass->gbufferAlbedoRt();
    const bgfx::TextureHandle albedoRt = blackboardReady
        ? blackboardGBuffer.albedo : safeTexture;
    const bgfx::TextureHandle normalRt = blackboardReady
        ? blackboardGBuffer.normal : safeTexture;
    const bgfx::TextureHandle worldPosRt = blackboardReady
        ? blackboardGBuffer.worldPosition : safeTexture;
    const bgfx::TextureHandle materialRt = blackboardReady
        ? blackboardGBuffer.material : safeTexture;
    const bgfx::TextureHandle depthRt = blackboardReady
        ? blackboardGBuffer.depth : safeTexture;
    if (!BGFXAdapter::isValid(auxiliary)) {
        auxiliary = safeTexture;
    }

    ensureFullscreenQuad(adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        rateLimitedEarlyReturn("GBufferDebugPass", "fullscreen VB/IB invalid");
        return 0;
    }

    ensureProgram(ctx.pool);
    const bool programReady = _program.isValid()
        && _uDebugChannel != ayt::shader::InvalidBinding
        && _tAlbedo       != ayt::shader::InvalidBinding
        && _tNormal       != ayt::shader::InvalidBinding
        && _tWorldPos     != ayt::shader::InvalidBinding
        && _tMaterial     != ayt::shader::InvalidBinding
        && _tDepth        != ayt::shader::InvalidBinding
        && _tAuxiliary    != ayt::shader::InvalidBinding;
    if (!programReady) {
        rateLimitedEarlyReturn("GBufferDebugPass", "program not ready");
        return 0;
    }

    constexpr uint8_t viewId = kGBufferDebugViewId;
    const ayt::math::Float4x4 identity = ayt::math::Float4x4::identity();
    adapter.setViewFrameBuffer(
        viewId, bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE});
    adapter.setViewRect(viewId, ctx.viewportX, ctx.viewportY,
                        viewportWidth, viewportHeight);
    adapter.setViewTransform(viewId, identity, identity);
    adapter.setViewClearRaw(viewId, BGFX_CLEAR_NONE, 0x00000000u);

    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    adapter.setIndexBuffer(_fullscreenIB, 0, 3);
    _program.setTexture(_program.getTextureStage(_tAlbedo), _tAlbedo,
                        toShaderTexture(albedoRt));
    _program.setTexture(_program.getTextureStage(_tNormal), _tNormal,
                        toShaderTexture(normalRt));
    _program.setTexture(_program.getTextureStage(_tWorldPos), _tWorldPos,
                        toShaderTexture(worldPosRt));
    _program.setTexture(_program.getTextureStage(_tMaterial), _tMaterial,
                        toShaderTexture(materialRt));
    _program.setTexture(_program.getTextureStage(_tDepth), _tDepth,
                        toShaderTexture(depthRt));
    _program.setTexture(_program.getTextureStage(_tAuxiliary), _tAuxiliary,
                        toShaderTexture(auxiliary));
    const float channelValue[4] = {
        static_cast<float>(ctx.frame.gbufferDebugChannel), 0.0f, 0.0f, 0.0f
    };
    _program.setUniform(
        _uDebugChannel, channelValue, sizeof(channelValue));

    ayt::shader::DrawCallContext sub;
    sub.viewId = viewId;
    sub.state  = 0;
    adapter.setStateDepthTestAlways();
    _program.submit(sub);

    // §P3 M5+L2 (2026-08-24) - was one-shot (`s_loggedFirst`
    // latch). Replaced with a rate-limited pattern (1 line per
    // 64 frames) so the first-dispatch signal stays loud
    // without flooding the console on long-running captures.
    static uint32_t s_firstFrame = 0;
    const uint32_t kFirstRateLimit = 64u;
    if (s_firstFrame == 0 || (s_firstFrame % kFirstRateLimit) == 0) {
        std::fprintf(stderr,
            "[GBufferDebugPass] frame=%u dispatch view=%u "
            "viewport=%ux%u enabled=%d channel=%u\n",
            s_firstFrame,
            static_cast<unsigned>(viewId),
            static_cast<unsigned>(viewportWidth),
            static_cast<unsigned>(viewportHeight),
            ctx.frame.gbufferDebugEnabled ? 1 : 0,
            static_cast<unsigned>(ctx.frame.gbufferDebugChannel));
    }
    ++s_firstFrame;
    return 1;
}

void GBufferDebugPass::ensureFullscreenQuad(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();
    (void)ensureFullscreenTriangleBuffers(
        adapter, _fullscreenVB, _fullscreenIB,
        kFullscreenTriangle, sizeof(kFullscreenTriangle), layout,
        kFullscreenIndices, sizeof(kFullscreenIndices));
}

void GBufferDebugPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != kGBufferDebugCacheKey) {
        _program.reset();
        _programAcquireFailed = false;
        s_acquiredCacheKey = kGBufferDebugCacheKey;
    }

    if (_program.isValid() || _programAcquireFailed) {
        return;
    }

    ayt::shader::ShaderResource acquired =
        pool.acquire(kGBufferDebugPhoskiaSource, kGBufferDebugCacheKey);
    if (!acquired.isValid()) {
        _programAcquireFailed = true;
        std::fprintf(stderr,
                     "[GBufferDebugPass] Phoskia acquire failed; debug overlay skipped\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[GBufferDebugPass]   %s\n", err.c_str());
        }
        return;
    }
    _program       = acquired;
    _uDebugChannel = _program.getUniformBinding("debugChannel");
    _tAlbedo       = _program.getTextureBinding("albedo");
    _tNormal       = _program.getTextureBinding("normal");
    _tWorldPos     = _program.getTextureBinding("worldPos");
    _tMaterial     = _program.getTextureBinding("materialSurface");
    _tDepth        = _program.getTextureBinding("depthTex");
    _tAuxiliary    = _program.getTextureBinding("auxiliary");
}

void GBufferDebugPass::destroyResources(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fullscreenVB)) {
        adapter.destroy(_fullscreenVB);
        _fullscreenVB = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_fullscreenIB)) {
        adapter.destroy(_fullscreenIB);
        _fullscreenIB = BGFX_INVALID_HANDLE;
    }
    if (_program.isValid()) {
        _program.reset();
    }
    _uDebugChannel   = ayt::shader::InvalidBinding;
    _tAlbedo         = ayt::shader::InvalidBinding;
    _tNormal         = ayt::shader::InvalidBinding;
    _tWorldPos       = ayt::shader::InvalidBinding;
    _tMaterial       = ayt::shader::InvalidBinding;
    _tDepth          = ayt::shader::InvalidBinding;
    _tAuxiliary      = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
}

} // namespace ayt::render::detail
