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

// §A3 SSAO MVP (2026-07-24, mid-term FG MVP SSAO Gate) — 8-tap
// worldPos-sphere Phoskia source.
//
// Algorithm (主人拍板 B = 8-tap worldPos sphere; cutsheet §S2):
//   1) Reconstruct the SCREEN-space tangent basis from a small
//      tile of a 4×4 RGBA8 noise texture (Phoskia has no for-loop
//      / array-indexing, so the 8 sphere directions are unrolled
//      to literal expressions).
//   2) For each of the 8 directions, sample the worldPos GBuffer
//      RT at `viewProjectionMatrix * samplePos` ⇒ reject if the
//      sample's `.w > 0` (sky) or the depth-difference exceeds
//      `ssaoBias.x` ⇒ add to `occSum`.
//   3) `aoOcclusion = clamp(1 - pow(1 - occFraction, 4), 0, 1)`
//      (cutsheet §S2 visibility → occlusion conversion).
//   4) Sky reject via `step(0.0001, worldPos.w)` (Phoskia has no
//      `if` expression).
//
// Phoskia constraints (lessons §3.1, §S4 cutsheet):
//   - All scalars as `uniform vec4` with `.x` carry (bgfx Vec4).
//   - No `saturate` builtin — use `clamp(1.0 - x, 0.0, 1.0)`.
//   - No `inverse()` builtin — use `viewProjectionMatrix * vec4
//     (samplePos, 1.0)` directly, then divide by `.w`.
//   - 8 taps unrolled explicitly.
constexpr const char* kSSAOPhoskiaSource = R"(
material SSAO {
    texture2d sceneColor
    texture2d worldPosition
    texture2d noiseTexture
    uniform vec4 ssaoStrength
    uniform vec4 ssaoRadius
    uniform vec4 ssaoBias
    uniform vec4 projection
    uniform vec4 viewportTexel
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        // Center world position from GBuffer RT2 (RGBA16F; w=1 for geometry, w=0 for sky)
        let centerWorld = sample(worldPosition, uv)
        // Reject sky pre-emptively (return occlusion=0 ⇒ no darkening)
        let skyGate = step(0.0001, centerWorld.w)
        let noise = sample(noiseTexture, uv * vec2(viewportTexel.x * 0.25, viewportTexel.y * 0.25))
        let rot = (noise.rg - vec2(0.5)) * 6.2831853
        let c = cos(rot.x)
        let s = sin(rot.x)
        // 8 sphere-tap directions unrolled. Each direction is a
        // 3D vector scaled by ssaoRadius; samples get converted to
        // NDC via the camera viewProjectionMatrix; then the depth-
        // difference gate decides if that sample is "occluded".
        // Phoskia forbids for-loops in the FS so this is literal.
        let dx0 = vec3(c * 0.4, s * 0.4, 0.6)
        let p0 = viewProjectionMatrix * vec4(centerWorld.xyz + dx0 * ssaoRadius.x, 1.0)
        let uv0 = (p0.xyz / p0.w) * 0.5 + vec3(0.5, 0.5, 0.5)
        let w0 = sample(worldPosition, vec2(uv0.x, 1.0 - uv0.y))
        let occ0 = step(0.0001, w0.w) * step(ssaoBias.x, abs(p0.w - w0.w))

        let dx1 = vec3(s * 0.6, c * 0.6, 0.4)
        let p1 = viewProjectionMatrix * vec4(centerWorld.xyz + dx1 * ssaoRadius.x, 1.0)
        let uv1 = (p1.xyz / p1.w) * 0.5 + vec3(0.5, 0.5, 0.5)
        let w1 = sample(worldPosition, vec2(uv1.x, 1.0 - uv1.y))
        let occ1 = step(0.0001, w1.w) * step(ssaoBias.x, abs(p1.w - w1.w))

        let dx2 = vec3(-c * 0.4, -s * 0.4, -0.6)
        let p2 = viewProjectionMatrix * vec4(centerWorld.xyz + dx2 * ssaoRadius.x, 1.0)
        let uv2 = (p2.xyz / p2.w) * 0.5 + vec3(0.5, 0.5, 0.5)
        let w2 = sample(worldPosition, vec2(uv2.x, 1.0 - uv2.y))
        let occ2 = step(0.0001, w2.w) * step(ssaoBias.x, abs(p2.w - w2.w))

        let dx3 = vec3(-s * 0.6, -c * 0.6, -0.4)
        let p3 = viewProjectionMatrix * vec4(centerWorld.xyz + dx3 * ssaoRadius.x, 1.0)
        let uv3 = (p3.xyz / p3.w) * 0.5 + vec3(0.5, 0.5, 0.5)
        let w3 = sample(worldPosition, vec2(uv3.x, 1.0 - uv3.y))
        let occ3 = step(0.0001, w3.w) * step(ssaoBias.x, abs(p3.w - w3.w))

        let dx4 = vec3(c * 0.7, -s * 0.7, 0.3)
        let p4 = viewProjectionMatrix * vec4(centerWorld.xyz + dx4 * ssaoRadius.x, 1.0)
        let uv4 = (p4.xyz / p4.w) * 0.5 + vec3(0.5, 0.5, 0.5)
        let w4 = sample(worldPosition, vec2(uv4.x, 1.0 - uv4.y))
        let occ4 = step(0.0001, w4.w) * step(ssaoBias.x, abs(p4.w - w4.w))

        let dx5 = vec3(s * 0.3, c * 0.3, -0.7)
        let p5 = viewProjectionMatrix * vec4(centerWorld.xyz + dx5 * ssaoRadius.x, 1.0)
        let uv5 = (p5.xyz / p5.w) * 0.5 + vec3(0.5, 0.5, 0.5)
        let w5 = sample(worldPosition, vec2(uv5.x, 1.0 - uv5.y))
        let occ5 = step(0.0001, w5.w) * step(ssaoBias.x, abs(p5.w - w5.w))

        let dx6 = vec3(-c * 0.5, s * 0.5, 0.5)
        let p6 = viewProjectionMatrix * vec4(centerWorld.xyz + dx6 * ssaoRadius.x, 1.0)
        let uv6 = (p6.xyz / p6.w) * 0.5 + vec3(0.5, 0.5, 0.5)
        let w6 = sample(worldPosition, vec2(uv6.x, 1.0 - uv6.y))
        let occ6 = step(0.0001, w6.w) * step(ssaoBias.x, abs(p6.w - w6.w))

        let dx7 = vec3(-s * 0.5, -c * 0.5, -0.5)
        let p7 = viewProjectionMatrix * vec4(centerWorld.xyz + dx7 * ssaoRadius.x, 1.0)
        let uv7 = (p7.xyz / p7.w) * 0.5 + vec3(0.5, 0.5, 0.5)
        let w7 = sample(worldPosition, vec2(uv7.x, 1.0 - uv7.y))
        let occ7 = step(0.0001, w7.w) * step(ssaoBias.x, abs(p7.w - w7.w))

        let occSum = occ0 + occ1 + occ2 + occ3 + occ4 + occ5 + occ6 + occ7
        let occFraction = clamp(occSum * 0.125, 0.0, 1.0)
        // cutsheet §S2 visibility → occlusion: clamp(1 - pow(1 - v, 4), 0, 1)
        let aoOcclusion = clamp(1.0 - pow(clamp(1.0 - occFraction, 0.0, 1.0), 4.0), 0.0, 1.0)
        return vec4(aoOcclusion * skyGate, 0.0, 0.0, 1.0)
    }
}
)";

// §A3 SSAO MVP (2026-07-24) — cache-key bump v0 placeholder → v1
// real 8-tap sphere shader. Bug-fix-#3 mirror: any future FS
// changes that forget to bump the cache key would be caught by
// the extern-from-test drift detection in `kSSAOCacheKeyCStr`.
constexpr const char* kSSAOCacheKey = "ssao_v1_8tap_worldpos_sphere_fs";

// §A3 SSAO MVP (2026-07-24) — noise table (tangent-rotation
// look-up). RGBA8 4×4 = 64 bytes lazy-uploaded once on first
// execute() after adapter init. Per-pixel noise.rg is sampled
// by the SSAO FS at `uv * viewportTexel * 0.25` (so the noise
// tiles every 4 pixels regardless of viewport size).
const uint8_t kSSAONoiseData[4 * 4 * 4] = {
    0xC8, 0x29, 0x54, 0xFF, 0xA4, 0x0E, 0xF1, 0xC0, 0x76, 0xE5, 0x10, 0xA0, 0x39, 0xC2, 0x88, 0xFF,
    0x1A, 0xA1, 0xB7, 0xC0, 0x6F, 0x82, 0xD1, 0xFF, 0x39, 0x0E, 0x55, 0xA4, 0xE8, 0x76, 0x29, 0xFF,
    0xE7, 0xC5, 0x96, 0xC0, 0xA4, 0x76, 0x10, 0xFF, 0x73, 0x39, 0xD2, 0xC0, 0x40, 0x55, 0x88, 0xFF,
    0xB1, 0xE1, 0x29, 0xC0, 0x6F, 0xC2, 0x54, 0xFF, 0x39, 0x96, 0x10, 0xA0, 0xE7, 0x29, 0x76, 0xFF
};

} // namespace

// §A1 (2026-07-24) — cache-key externalize (mirror DepthHazePass /
// BloomExtractPass / BloomBlurPass / SkyboxPass / LightingPass
// Bug-fix-#3 pattern). The header's `extern const char* const
// kSSAOCacheKeyCStr` is bound to this literal so tests can
// `assert(kSSAOCacheKeyCStr == mirror)` and drift breaks
// immediately.
const char* const kSSAOCacheKeyCStr = kSSAOCacheKey;

uint32_t SSAOPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    shader::ShaderResourcePool& pool = ctx.pool;
    const FrameContext& frame = ctx.frame;

    // Mirror Noop + uninit short-circuits (mirror DepthHazePass /
    // BloomExtractPass contract).
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

    // §A2 FG gate (2026-07-24) — render() central ssaoPassEnabled
    // is the canonical "this pass should run" signal. When it is
    // false, FG compile culls SSAOTexture ⇒ resolve returns
    // invalid ⇒ execute early-returns 0 (0 draw, 0 alloc).
    const bgfx::FrameBufferHandle target =
        ctx.frameGraph->resolve(FgResourceId::SSAOTexture);
    if (!BGFXAdapter::isValid(target)) {
        return 0;
    }

    // Source FBO priority — mirror DepthHazePass S4b pattern.
    const bgfx::FrameBufferHandle sourceFbo = PostProcessPass::selectSourceFbo(ctx);
    if (!BGFXAdapter::isValid(sourceFbo)) {
        return 0;
    }
    const bgfx::TextureHandle fboColor = adapter.getFboAttachment(sourceFbo, 0);
    if (!BGFXAdapter::isValid(fboColor)) {
        return 0;
    }

    // Deferred: GBuffer RT2 (gbufferMotionRt) holds RGBA16F
    // worldPos. Forward / no-gbuffer case is already filtered
    // out by render() central ssaoPassEnabled (gbufferPassPtr
    // null ⇒ gate false ⇒ resolve invalid ⇒ execute returns 0).
    if (ctx.gbufferPass == nullptr) {
        return 0;
    }
    const bgfx::TextureHandle worldPosRt = ctx.gbufferPass->gbufferMotionRt();
    if (!BGFXAdapter::isValid(worldPosRt)) {
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
        && _uProjection     != ayt::shader::InvalidBinding
        && _tSceneColor     != ayt::shader::InvalidBinding
        && _tWorldPosition  != ayt::shader::InvalidBinding
        && _tNoise          != ayt::shader::InvalidBinding;
    if (!programReady) {
        return 0;
    }

    ensureNoise(adapter);
    if (!BGFXAdapter::isValid(_noiseTex) || !_noiseUploaded) {
        return 0;
    }

    // Bound the SSAOTexture FBO as the draw target.
    constexpr uint8_t viewId = kSsaoViewId;
    const ayt::math::Float4x4 identity = ayt::math::Float4x4::identity();

    adapter.setViewFrameBuffer(viewId, target);
    adapter.setViewRect(viewId, 0, 0, viewportWidth, viewportHeight);
    adapter.setViewTransform(viewId, identity, identity);
    adapter.setViewClearRaw(viewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);

    const ayt::shader::TextureHandle sceneTexHandle =
        ayt::render::detail::toShaderTexture(fboColor);
    const ayt::shader::TextureHandle worldPosHandle =
        ayt::render::detail::toShaderTexture(worldPosRt);
    const ayt::shader::TextureHandle noiseHandle =
        ayt::render::detail::toShaderTexture(_noiseTex);

    // bgfx Vec4 slots — pad scalars into .x (lessons §3.1).
    const float strengthPad[4] = {frame.ssaoStrength, 0.0f, 0.0f, 0.0f};
    const float radiusPad[4]   = {frame.ssaoRadius,   0.0f, 0.0f, 0.0f};
    const float biasPad[4]     = {frame.ssaoBias,     0.0f, 0.0f, 0.0f};
    const float projPad[4]     = {0.0f, 0.0f, 0.0f, 0.0f};
    const float vpTexelPad[4]  = {
        1.0f / static_cast<float>(viewportWidth),
        1.0f / static_cast<float>(viewportHeight),
        0.0f, 0.0f};

    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    adapter.setIndexBuffer(_fullscreenIB, 0, 3);
    {
        const uint8_t stageColor = _program.getTextureStage(_tSceneColor);
        const uint8_t stageWorld = _program.getTextureStage(_tWorldPosition);
        const uint8_t stageNoise = _program.getTextureStage(_tNoise);
        _program.setTexture(stageColor, _tSceneColor,    sceneTexHandle);
        _program.setTexture(stageWorld, _tWorldPosition, worldPosHandle);
        _program.setTexture(stageNoise, _tNoise,         noiseHandle);
    }
    (void)projPad;   // reserved for a future cut; current FS uses viewProjectionMatrix builtin.
    _program.setUniform(_uSSAOStrength, strengthPad, sizeof(strengthPad));
    _program.setUniform(_uSSAORadius,   radiusPad,   sizeof(radiusPad));
    _program.setUniform(_uSSAOBias,     biasPad,     sizeof(biasPad));
    _program.setUniform(_uProjection,   vpTexelPad,  sizeof(vpTexelPad));

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
    _uProjection       = _program.getUniformBinding("projection");
    _tSceneColor       = _program.getTextureBinding("sceneColor");
    _tWorldPosition    = _program.getTextureBinding("worldPosition");
    _tNoise            = _program.getTextureBinding("noiseTexture");
}

void SSAOPass::ensureNoise(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_noiseTex) && _noiseUploaded) {
        return;
    }
    if (!adapter.isInitialized() || adapter.isNoopBackend()) {
        return;
    }
    // createTexture2D(width, height, rgba8Data, flags) — the
    // overload requires RGBA8 layout. Mirror SkyboxPass's noise
    // upload; the Phoskia FS binds this as a tile lookup.
    _noiseTex = adapter.createTexture2D(4, 4,
                                        kSSAONoiseData,
                                        BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE);
    if (BGFXAdapter::isValid(_noiseTex)) {
        _noiseUploaded = true;
    }
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
    _uSSAOStrength   = ayt::shader::InvalidBinding;
    _uSSAORadius     = ayt::shader::InvalidBinding;
    _uSSAOBias       = ayt::shader::InvalidBinding;
    _uProjection     = ayt::shader::InvalidBinding;
    _tSceneColor     = ayt::shader::InvalidBinding;
    _tWorldPosition  = ayt::shader::InvalidBinding;
    _tNoise          = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
}

} // namespace ayt::render::detail
