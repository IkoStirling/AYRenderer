#include "detail/SSAOPass.h"

#include "detail/BGFXAdapter.h"
#include "detail/FgResource.h"        // §A1 SSAO MVP (2026-07-24) — FrameGraph SSAOTexture resolve gate
#include "detail/FrameContext.h"
#include "detail/GBufferPass.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"

#include "AYShaderResource.h"

#include <cstdio>

namespace ayt::render::detail
{

namespace {

// §A1 (2026-07-23) — fullscreen-triangle layout mirror
// (DepthHazePass.cpp:25-38 / BloomExtractPass).
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

// §A3 SSAO MVP — 8-tap worldPos Phoskia source.
//
// v4 quality fix (2026-07-24) — v3 still false-darkened every
// shaded pixel: perspective makes nearby coplanar ground look
// "closer", so center-depth compare fires everywhere geometry
// exists (matches user report: sky clean, ground/cube speckled,
// fills viewport when close). Fixes:
//   1) Off-plane gate: reject occluders with
//      |dot(occluder-center, N)| < bias (kills coplanar hits).
//   2) Relative depth bias: bias + 1% of cam distance.
//   3) Fixed kernel (no noise rotation) — noise was the "dot
//      texture"; weak residual AO is thresholded away.
//   4) Keep hemi flip + soft range + skyGate.
//
// Phoskia constraints (lessons §3.1):
//   - Scalars as `uniform vec4` with `.x` carry.
//   - No `saturate` — use `clamp`.
//   - No `inverse()` — `viewProjectionMatrix * vec4` then /w.
//   - 8 taps unrolled.
constexpr const char* kSSAOPhoskiaSource = R"(
material SSAO {
    texture2d worldPosition
    texture2d worldNormal
    uniform vec4 ssaoStrength
    uniform vec4 ssaoRadius
    uniform vec4 ssaoBias
    uniform vec4 camPos
    uniform vec4 viewportTexel
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let centerWorld = sample(worldPosition, uv)
        let skyGate = step(0.0001, centerWorld.w)
        let nEnc = sample(worldNormal, uv)
        let Nraw = nEnc.xyz * 2.0 - vec3(1.0, 1.0, 1.0)
        let Nn = Nraw * (1.0 / max(length(Nraw), 0.0001))
        let distC = length(centerWorld.xyz - camPos.xyz)
        let rad = max(ssaoRadius.x, 0.0001)
        let depthBias = ssaoBias.x + distC * 0.01

        let dx0raw = vec3(0.4, 0.4, 0.6)
        let dx0 = mix(dx0raw * -1.0, dx0raw, step(0.0, dot(dx0raw, Nn)))
        let sampleWorld0 = centerWorld.xyz + dx0 * rad
        let p0 = viewProjectionMatrix * vec4(sampleWorld0, 1.0)
        let ndc0 = p0.xyz / max(p0.w, 0.0001)
        let uv0 = vec2(ndc0.x * 0.5 + 0.5, 1.0 - (ndc0.y * 0.5 + 0.5))
        let w0 = sample(worldPosition, uv0)
        let to0 = w0.xyz - centerWorld.xyz
        let distS0 = length(w0.xyz - camPos.xyz)
        let range0 = clamp(1.0 - length(to0) / rad, 0.0, 1.0)
        let offPlane0 = step(ssaoBias.x, abs(dot(to0, Nn)))
        let bound0 = step(0.0, uv0.x) * step(uv0.x, 1.0) * step(0.0, uv0.y) * step(uv0.y, 1.0)
        let occ0 = step(0.0001, p0.w) * step(0.0001, w0.w) * bound0 * range0 * offPlane0 * step(distS0 + depthBias, distC)

        let dx1raw = vec3(0.6, -0.4, 0.5)
        let dx1 = mix(dx1raw * -1.0, dx1raw, step(0.0, dot(dx1raw, Nn)))
        let sampleWorld1 = centerWorld.xyz + dx1 * rad
        let p1 = viewProjectionMatrix * vec4(sampleWorld1, 1.0)
        let ndc1 = p1.xyz / max(p1.w, 0.0001)
        let uv1 = vec2(ndc1.x * 0.5 + 0.5, 1.0 - (ndc1.y * 0.5 + 0.5))
        let w1 = sample(worldPosition, uv1)
        let to1 = w1.xyz - centerWorld.xyz
        let distS1 = length(w1.xyz - camPos.xyz)
        let range1 = clamp(1.0 - length(to1) / rad, 0.0, 1.0)
        let offPlane1 = step(ssaoBias.x, abs(dot(to1, Nn)))
        let bound1 = step(0.0, uv1.x) * step(uv1.x, 1.0) * step(0.0, uv1.y) * step(uv1.y, 1.0)
        let occ1 = step(0.0001, p1.w) * step(0.0001, w1.w) * bound1 * range1 * offPlane1 * step(distS1 + depthBias, distC)

        let dx2raw = vec3(-0.5, 0.5, 0.5)
        let dx2 = mix(dx2raw * -1.0, dx2raw, step(0.0, dot(dx2raw, Nn)))
        let sampleWorld2 = centerWorld.xyz + dx2 * rad
        let p2 = viewProjectionMatrix * vec4(sampleWorld2, 1.0)
        let ndc2 = p2.xyz / max(p2.w, 0.0001)
        let uv2 = vec2(ndc2.x * 0.5 + 0.5, 1.0 - (ndc2.y * 0.5 + 0.5))
        let w2 = sample(worldPosition, uv2)
        let to2 = w2.xyz - centerWorld.xyz
        let distS2 = length(w2.xyz - camPos.xyz)
        let range2 = clamp(1.0 - length(to2) / rad, 0.0, 1.0)
        let offPlane2 = step(ssaoBias.x, abs(dot(to2, Nn)))
        let bound2 = step(0.0, uv2.x) * step(uv2.x, 1.0) * step(0.0, uv2.y) * step(uv2.y, 1.0)
        let occ2 = step(0.0001, p2.w) * step(0.0001, w2.w) * bound2 * range2 * offPlane2 * step(distS2 + depthBias, distC)

        let dx3raw = vec3(-0.4, -0.6, 0.4)
        let dx3 = mix(dx3raw * -1.0, dx3raw, step(0.0, dot(dx3raw, Nn)))
        let sampleWorld3 = centerWorld.xyz + dx3 * rad
        let p3 = viewProjectionMatrix * vec4(sampleWorld3, 1.0)
        let ndc3 = p3.xyz / max(p3.w, 0.0001)
        let uv3 = vec2(ndc3.x * 0.5 + 0.5, 1.0 - (ndc3.y * 0.5 + 0.5))
        let w3 = sample(worldPosition, uv3)
        let to3 = w3.xyz - centerWorld.xyz
        let distS3 = length(w3.xyz - camPos.xyz)
        let range3 = clamp(1.0 - length(to3) / rad, 0.0, 1.0)
        let offPlane3 = step(ssaoBias.x, abs(dot(to3, Nn)))
        let bound3 = step(0.0, uv3.x) * step(uv3.x, 1.0) * step(0.0, uv3.y) * step(uv3.y, 1.0)
        let occ3 = step(0.0001, p3.w) * step(0.0001, w3.w) * bound3 * range3 * offPlane3 * step(distS3 + depthBias, distC)

        let dx4raw = vec3(0.7, 0.0, 0.3)
        let dx4 = mix(dx4raw * -1.0, dx4raw, step(0.0, dot(dx4raw, Nn)))
        let sampleWorld4 = centerWorld.xyz + dx4 * rad
        let p4 = viewProjectionMatrix * vec4(sampleWorld4, 1.0)
        let ndc4 = p4.xyz / max(p4.w, 0.0001)
        let uv4 = vec2(ndc4.x * 0.5 + 0.5, 1.0 - (ndc4.y * 0.5 + 0.5))
        let w4 = sample(worldPosition, uv4)
        let to4 = w4.xyz - centerWorld.xyz
        let distS4 = length(w4.xyz - camPos.xyz)
        let range4 = clamp(1.0 - length(to4) / rad, 0.0, 1.0)
        let offPlane4 = step(ssaoBias.x, abs(dot(to4, Nn)))
        let bound4 = step(0.0, uv4.x) * step(uv4.x, 1.0) * step(0.0, uv4.y) * step(uv4.y, 1.0)
        let occ4 = step(0.0001, p4.w) * step(0.0001, w4.w) * bound4 * range4 * offPlane4 * step(distS4 + depthBias, distC)

        let dx5raw = vec3(0.0, 0.7, 0.3)
        let dx5 = mix(dx5raw * -1.0, dx5raw, step(0.0, dot(dx5raw, Nn)))
        let sampleWorld5 = centerWorld.xyz + dx5 * rad
        let p5 = viewProjectionMatrix * vec4(sampleWorld5, 1.0)
        let ndc5 = p5.xyz / max(p5.w, 0.0001)
        let uv5 = vec2(ndc5.x * 0.5 + 0.5, 1.0 - (ndc5.y * 0.5 + 0.5))
        let w5 = sample(worldPosition, uv5)
        let to5 = w5.xyz - centerWorld.xyz
        let distS5 = length(w5.xyz - camPos.xyz)
        let range5 = clamp(1.0 - length(to5) / rad, 0.0, 1.0)
        let offPlane5 = step(ssaoBias.x, abs(dot(to5, Nn)))
        let bound5 = step(0.0, uv5.x) * step(uv5.x, 1.0) * step(0.0, uv5.y) * step(uv5.y, 1.0)
        let occ5 = step(0.0001, p5.w) * step(0.0001, w5.w) * bound5 * range5 * offPlane5 * step(distS5 + depthBias, distC)

        let dx6raw = vec3(-0.6, 0.2, 0.5)
        let dx6 = mix(dx6raw * -1.0, dx6raw, step(0.0, dot(dx6raw, Nn)))
        let sampleWorld6 = centerWorld.xyz + dx6 * rad
        let p6 = viewProjectionMatrix * vec4(sampleWorld6, 1.0)
        let ndc6 = p6.xyz / max(p6.w, 0.0001)
        let uv6 = vec2(ndc6.x * 0.5 + 0.5, 1.0 - (ndc6.y * 0.5 + 0.5))
        let w6 = sample(worldPosition, uv6)
        let to6 = w6.xyz - centerWorld.xyz
        let distS6 = length(w6.xyz - camPos.xyz)
        let range6 = clamp(1.0 - length(to6) / rad, 0.0, 1.0)
        let offPlane6 = step(ssaoBias.x, abs(dot(to6, Nn)))
        let bound6 = step(0.0, uv6.x) * step(uv6.x, 1.0) * step(0.0, uv6.y) * step(uv6.y, 1.0)
        let occ6 = step(0.0001, p6.w) * step(0.0001, w6.w) * bound6 * range6 * offPlane6 * step(distS6 + depthBias, distC)

        let dx7raw = vec3(0.2, -0.6, 0.5)
        let dx7 = mix(dx7raw * -1.0, dx7raw, step(0.0, dot(dx7raw, Nn)))
        let sampleWorld7 = centerWorld.xyz + dx7 * rad
        let p7 = viewProjectionMatrix * vec4(sampleWorld7, 1.0)
        let ndc7 = p7.xyz / max(p7.w, 0.0001)
        let uv7 = vec2(ndc7.x * 0.5 + 0.5, 1.0 - (ndc7.y * 0.5 + 0.5))
        let w7 = sample(worldPosition, uv7)
        let to7 = w7.xyz - centerWorld.xyz
        let distS7 = length(w7.xyz - camPos.xyz)
        let range7 = clamp(1.0 - length(to7) / rad, 0.0, 1.0)
        let offPlane7 = step(ssaoBias.x, abs(dot(to7, Nn)))
        let bound7 = step(0.0, uv7.x) * step(uv7.x, 1.0) * step(0.0, uv7.y) * step(uv7.y, 1.0)
        let occ7 = step(0.0001, p7.w) * step(0.0001, w7.w) * bound7 * range7 * offPlane7 * step(distS7 + depthBias, distC)

        let occSum = occ0 + occ1 + occ2 + occ3 + occ4 + occ5 + occ6 + occ7
        let occFraction = clamp(occSum * 0.125, 0.0, 1.0)
        // Kill weak residual hits (flat-surface / edge noise).
        let aoOcclusion = clamp((occFraction - 0.2) * 1.25, 0.0, 1.0)
        return vec4(aoOcclusion * skyGate, 0.0, 0.0, 1.0)
    }
}
)";

// v4: off-plane gate + fixed kernel (no noise).
constexpr const char* kSSAOCacheKey = "ssao_v4_8tap_offplane_fixed_fs";

} // namespace

const char* const kSSAOCacheKeyCStr = kSSAOCacheKey;

uint32_t SSAOPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    shader::ShaderResourcePool& pool = ctx.pool;
    const FrameContext& frame = ctx.frame;

    if (!adapter.isInitialized()) {
        return 0;
    }
    if (adapter.isNoopBackend()) {
        return 0;
    }

    if (ctx.frameGraph == nullptr) {
        return 0;
    }

    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    if (viewportWidth == 0 || viewportHeight == 0) {
        return 0;
    }

    const bgfx::FrameBufferHandle target =
        ctx.frameGraph->resolve(FgResourceId::SSAOTexture);
    if (!BGFXAdapter::isValid(target)) {
        return 0;
    }

    if (ctx.gbufferPass == nullptr) {
        return 0;
    }
    const bgfx::TextureHandle worldPosRt = ctx.gbufferPass->gbufferMotionRt();
    const bgfx::TextureHandle worldNrmRt = ctx.gbufferPass->gbufferNormalRt();
    if (!BGFXAdapter::isValid(worldPosRt) || !BGFXAdapter::isValid(worldNrmRt)) {
        return 0;
    }

    ensureFullscreenQuad(adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        return 0;
    }

    ensureProgram(pool);
    const bool programReady = _program.isValid()
        && _uSSAOStrength   != ayt::shader::InvalidBinding
        && _uSSAORadius     != ayt::shader::InvalidBinding
        && _uSSAOBias       != ayt::shader::InvalidBinding
        && _uCamPos         != ayt::shader::InvalidBinding
        && _uViewportTexel  != ayt::shader::InvalidBinding
        && _tWorldPosition  != ayt::shader::InvalidBinding
        && _tWorldNormal    != ayt::shader::InvalidBinding;
    if (!programReady) {
        return 0;
    }

    constexpr uint8_t viewId = kSsaoViewId;

    adapter.setViewFrameBuffer(viewId, target);
    adapter.setViewRect(viewId, 0, 0, viewportWidth, viewportHeight);
    adapter.setViewTransform(viewId, frame.view, frame.projection);
    adapter.setViewClearRaw(viewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);

    const ayt::shader::TextureHandle worldPosHandle =
        ayt::render::detail::toShaderTexture(worldPosRt);
    const ayt::shader::TextureHandle worldNrmHandle =
        ayt::render::detail::toShaderTexture(worldNrmRt);

    const float strengthPad[4] = {frame.ssaoStrength, 0.0f, 0.0f, 0.0f};
    const float radiusPad[4]   = {frame.ssaoRadius,   0.0f, 0.0f, 0.0f};
    const float biasPad[4]     = {frame.ssaoBias,     0.0f, 0.0f, 0.0f};
    const float camPosPad[4]   = {
        frame.cameraPosition.x, frame.cameraPosition.y,
        frame.cameraPosition.z, 0.0f};
    const float vpTexelPad[4]  = {
        1.0f / static_cast<float>(viewportWidth),
        1.0f / static_cast<float>(viewportHeight),
        0.0f, 0.0f};

    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    adapter.setIndexBuffer(_fullscreenIB, 0, 3);
    {
        const uint8_t stageWorld = _program.getTextureStage(_tWorldPosition);
        const uint8_t stageNrm   = _program.getTextureStage(_tWorldNormal);
        _program.setTexture(stageWorld, _tWorldPosition, worldPosHandle);
        _program.setTexture(stageNrm,   _tWorldNormal,   worldNrmHandle);
    }
    _program.setUniform(_uSSAOStrength,  strengthPad, sizeof(strengthPad));
    _program.setUniform(_uSSAORadius,    radiusPad,   sizeof(radiusPad));
    _program.setUniform(_uSSAOBias,      biasPad,     sizeof(biasPad));
    _program.setUniform(_uCamPos,        camPosPad,   sizeof(camPosPad));
    _program.setUniform(_uViewportTexel, vpTexelPad,  sizeof(vpTexelPad));

    ayt::shader::DrawCallContext sub;
    sub.viewId = viewId;
    sub.state  = 0;
    adapter.setStateDepthTestAlways();
    _program.submit(sub);

    static bool s_loggedFirst = false;
    if (!s_loggedFirst) {
        std::fprintf(stderr,
            "[SSAOPass] A3 first dispatch view=%u viewport=%ux%u "
            "enabled=%d strength=%.2f radius=%.2f bias=%.3f\n",
            static_cast<unsigned>(viewId),
            static_cast<unsigned>(viewportWidth),
            static_cast<unsigned>(viewportHeight),
            frame.ssaoEnabled ? 1 : 0,
            frame.ssaoStrength,
            frame.ssaoRadius,
            frame.ssaoBias);
        s_loggedFirst = true;
    }
    return 1;
}

void SSAOPass::ensureFullscreenQuad(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();
    _fullscreenVB = adapter.createVertexBuffer(kFullscreenTriangle,
                                                sizeof(kFullscreenTriangle),
                                                layout,
                                                BGFX_BUFFER_NONE);
    _fullscreenIB = adapter.createIndexBuffer(kFullscreenIndices,
                                              sizeof(kFullscreenIndices),
                                              BGFX_BUFFER_NONE);
}

void SSAOPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != kSSAOCacheKey) {
        _program.reset();
        _programAcquireFailed = false;
        s_acquiredCacheKey = kSSAOCacheKey;
    }

    if (_program.isValid() || _programAcquireFailed) {
        return;
    }
    ayt::shader::ShaderResource acquired =
        pool.acquire(kSSAOPhoskiaSource, kSSAOCacheKey);
    if (!acquired.isValid()) {
        _programAcquireFailed = true;
        std::fprintf(stderr,
                     "[SSAOPass] Phoskia acquire failed; "
                     "SSAO will skip (A3 = 0 draw).\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[SSAOPass]   %s\n", err.c_str());
        }
        return;
    }
    _program           = acquired;
    _uSSAOStrength     = _program.getUniformBinding("ssaoStrength");
    _uSSAORadius       = _program.getUniformBinding("ssaoRadius");
    _uSSAOBias         = _program.getUniformBinding("ssaoBias");
    _uCamPos           = _program.getUniformBinding("camPos");
    _uViewportTexel    = _program.getUniformBinding("viewportTexel");
    _tWorldPosition    = _program.getTextureBinding("worldPosition");
    _tWorldNormal      = _program.getTextureBinding("worldNormal");
    _tNoise            = ayt::shader::InvalidBinding;  // v4: fixed kernel, no noise
}

void SSAOPass::ensureNoise(BGFXAdapter& adapter)
{
    // v4: fixed kernel — noise texture unused. Keep stub so destroy /
    // field layout stay stable.
    (void)adapter;
}

void SSAOPass::destroyResources(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fullscreenVB)) {
        adapter.destroy(_fullscreenVB);
        _fullscreenVB = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_fullscreenIB)) {
        adapter.destroy(_fullscreenIB);
        _fullscreenIB = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_noiseTex)) {
        adapter.destroy(_noiseTex);
        _noiseTex = BGFX_INVALID_HANDLE;
        _noiseUploaded = false;
    }
    if (_program.isValid()) {
        _program.reset();
    }
    _uSSAOStrength    = ayt::shader::InvalidBinding;
    _uSSAORadius      = ayt::shader::InvalidBinding;
    _uSSAOBias        = ayt::shader::InvalidBinding;
    _uCamPos          = ayt::shader::InvalidBinding;
    _uViewportTexel   = ayt::shader::InvalidBinding;
    _tWorldPosition   = ayt::shader::InvalidBinding;
    _tWorldNormal     = ayt::shader::InvalidBinding;
    _tNoise           = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
}

} // namespace ayt::render::detail
