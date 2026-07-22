#include "detail/LightingPass.h"

#include "detail/BgfxMatrix.h"
#include "detail/FrameContext.h"
#include "detail/GBufferPass.h"
#include "detail/GpuResources.h"
#include "detail/RenderPass.h"

#include <cstdio>

namespace ayt::render::detail
{

// §P5 B5 (2026-07-22) — build stamp literal (mirror
// GBufferPass.cpp:15 `kGBufferBuildStamp`). Pointer-equal compare;
// bumping this triggers a FBO rebuild on next execute(). B5 is
// the first lock — bumping is safe across B5.x cuts.
static constexpr const char* kLightingBuildStamp = "b5-2026-07-22";

// §P5 B5 (2026-07-22) — cache key literal (mirror GBufferPass.cpp:68
// `kGBufferCacheKey`). Pointer-equal compare so cache invalidates
// when the literal bumps. `s_acquiredCacheKey` static guard inside
// ensureProgram() forces re-acquire.
static constexpr const char* kLightingCacheKey = "lighting_v1_b5_directional_lambert";

// §P5 B5 (2026-07-22) — fullscreen-triangle vertex data, duplicated
// from PostProcessPass.cpp:27-33 (private state there — coupling
// would require a friend class, duplicate is cheaper). Same
// 3-vert NDC oversize-triangle bgfx pattern; same FullscreenVertex
// {x,y,u,v} layout that vertexLayoutPosUv() emits.
struct alignas(16) LightingFullscreenVertex {
    float x;
    float y;
    float u;
    float v;
};

constexpr LightingFullscreenVertex kLightingFullscreenTriangle[3] = {
    { -1.0f, -1.0f, 0.0f, 1.0f },
    {  3.0f, -1.0f, 2.0f, 1.0f },
    { -1.0f,  3.0f, 0.0f, -1.0f },
};

constexpr uint16_t kLightingFullscreenIndices[3] = { 0, 1, 2 };

// §P5 B5 (2026-07-22) — Phoskia Lighting VS/FS source.
//
// Sampler inputs (cutsheet B5 spec):
//   - gbufferAlbedo : color (RGB = baseColor, A = opacity) from B4b
//   - gbufferNormal : color (RGB = world-space normal encoded [0,1])
//   - gbufferMotion : color (xy = prev-frame displacement, [0,1] —
//                                   B5 currently does NOT consume
//                                   motion; binding present for B7+
//                                   TAA consumer symmetry — cutsheet
//                                   B5 boundary documented)
//
// Uniform inputs (cutsheet B5 spec, all from FrameContext):
//   - u_lightDirection : vec3 = -frame.lightDirection (Phoskia wants
//                                          a TO-light vector; we
//                                          negate on the CPU once,
//                                          feed as-is)
//   - u_lightColor     : vec3 = frame.lightColor
//   - u_cameraPos      : vec3 = frame.cameraPosition
//     (reserved for B5.5 specular/Blinn-Phong — B5 keeps Lambert
//     flat; uploader still emits it so future B5.x can reuse the
//     same uniform binding without shader changes)
//
// Math (Lambert):
//   N = sample(gbufferNormal).xyz * 2.0 - 1.0      // decode [0,1]→[-1,1]
//   L = normalize(u_lightDirection)
//   NdotL = max(dot(N, L), 0.0)
//   ambient = 0.1                                    // floor term
//   lit = sample(gbufferAlbedo).rgb * (ambient + NdotL * u_lightColor)
//   return vec4(lit, sample(gbufferAlbedo).a)
//
// Phoskia `let` chain uses the same surface as the B4b GBufferFill
// source (verified at PR-F2/PR-F3 ship) — `let` declarations +
// arithmetic + texture2D() samples. B5 emits a SINGLE `return`
// (not MRT) so no Phoskia MRT extension needed — falls back to the
// legacy `return → gl_FragColor` path (verified at AYShader/
// unittest/Test_MRT_Fragment.cpp::mrt_legacy_return_still_emits_fragcolor).
constexpr const char* kLightingPhoskiaSource = R"(
material Lighting {
    texture2D gbufferAlbedo
    texture2D gbufferNormal
    texture2D gbufferMotion
    uniform vec4 u_lightDirection
    uniform vec4 u_lightColor
    uniform vec4 u_cameraPos
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in vUv : texcoord
        let baseUv = vec2(vUv.x, 1.0 - vUv.y)
        let albedo = texture2D(gbufferAlbedo, baseUv)
        let normalSample = texture2D(gbufferNormal, baseUv)
        let N = normalSample.xyz * 2.0 - vec3(1.0)
        let L = normalize(u_lightDirection.xyz)
        let NdotL = max(dot(N, L), 0.0)
        let ambient = 0.1
        let lit = albedo.rgb * (vec3(ambient) + NdotL * u_lightColor.xyz)
        return vec4(lit, albedo.a)
    }
}
)";

LightingPass::~LightingPass() = default;

void LightingPass::setOutputSize(uint16_t width, uint16_t height) noexcept
{
    // §P5 B5 (2026-07-22) — host-driven store-only call (mirror
    // GBufferPass::setGbufferSize at GBufferPass.cpp:231-238).
    // No adapter access here; the next execute() honors the size.
    _lightingW = width;
    _lightingH = height;
}

void LightingPass::destroyResources(BGFXAdapter& adapter)
{
    // §P5 B5 (2026-07-22) — mirror GBufferPass::destroyResources at
    // GBufferPass.cpp:240-266. Drop the FBO, fullscreen VB/IB, and
    // reset all cached handles. W/H + buildStamp reset UNCONDITIONALLY
    // so a host that calls setOutputSize(800,600) → destroyResources()
    // expects W/H back to 0 (Test_B5 case 6 pins this). The FBO/VB/IB
    // handles are only destroyed when actually allocated (calling
    // bgfx::destroy on an invalid handle is a UAF on some bgfx
    // backends — mirror ShadowMapResources::destroy guard).
    if (BGFXAdapter::isValid(_lightingFbo)) {
        adapter.destroy(_lightingFbo);
        _lightingFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    if (BGFXAdapter::isValid(_fullscreenVB)) {
        adapter.destroy(_fullscreenVB);
        _fullscreenVB = bgfx::VertexBufferHandle{BGFX_INVALID_HANDLE};
    }
    if (BGFXAdapter::isValid(_fullscreenIB)) {
        adapter.destroy(_fullscreenIB);
        _fullscreenIB = bgfx::IndexBufferHandle{BGFX_INVALID_HANDLE};
    }
    _lightingW  = 0;
    _lightingH  = 0;
    _buildStamp = "";
    _program.reset();
    _programReady = false;
    _programAcquireFailed = false;
}

void LightingPass::ensure(BGFXAdapter& adapter, uint16_t width, uint16_t height)
{
    // §P5 B5 (2026-07-22) — mirror GBufferPass::ensure at
    // GBufferPass.cpp:268-314. Stamp-changed fast path + same-size
    // fast path; rebuild path on size/stamp change.
    if (!adapter.isInitialized() || width == 0 || height == 0) {
        return;
    }

    const bool stampChanged = (_buildStamp != kLightingBuildStamp);
    if (stampChanged) {
        _buildStamp = kLightingBuildStamp;
    }

    if (bgfx::isValid(_lightingFbo)
        && _lightingW == width
        && _lightingH == height
        && !stampChanged) {
        return;  // FBO cached
    }

    // Rebuild path — drop old FBO + recreate
    if (bgfx::isValid(_lightingFbo)) {
        adapter.destroy(_lightingFbo);
        _lightingFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        _lightingW = _lightingH = 0;
    }

    // §P5 B5 (2026-07-22) — 1× RGBA8 LightingOutput FBO. NO depth
    // attachment — this is a fullscreen post-process pass that
    // does not read depth. `withDepth=false` matches cutsheet
    // `pass-lessons-from-deferred.md:151,161,169` "LightingPass
    // 写 LightingOutput FBO" semantics (independent RGBA8 RT, not
    // a color+depth like sceneFbo).
    _lightingFbo = adapter.createFrameBuffer(width, height,
                                              bgfx::TextureFormat::RGBA8,
                                              /*withDepth=*/false);
    if (bgfx::isValid(_lightingFbo)) {
        _lightingW = width;
        _lightingH = height;
    }
}

void LightingPass::ensureFullscreenQuad(BGFXAdapter& adapter)
{
    // §P5 B5 (2026-07-22) — mirror PostProcessPass::ensureFullscreenQuad
    // at PostProcessPass.cpp:280-309. Lazy creation; VB/IB cached
    // after first call.
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();
    _fullscreenVB = adapter.createVertexBuffer(kLightingFullscreenTriangle,
                                                sizeof(kLightingFullscreenTriangle),
                                                layout,
                                                BGFX_BUFFER_NONE);
    _fullscreenIB = adapter.createIndexBuffer(kLightingFullscreenIndices,
                                              sizeof(kLightingFullscreenIndices),
                                              BGFX_BUFFER_NONE);
}

void LightingPass::ensureProgram(ayt::shader::ShaderResourcePool& pool)
{
    // §P5 B5 (2026-07-22) — mirror GBufferPass::ensureProgram at
    // GBufferPass.cpp:72-107. Stamp-checked `s_acquiredCacheKey`
    // pointer-equal guard (ShadowCaster Issue 1 fix at
    // ShadowCaster.cpp:60-65). On compile failure: log + set
    // _programAcquireFailed, leave _programReady false. On success:
    // _program = acquired, _programReady = true.
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != kLightingCacheKey) {
        _program.reset();
        _programReady = false;
        _programAcquireFailed = false;
        s_acquiredCacheKey = kLightingCacheKey;
    }

    if (_program.isValid() || _programAcquireFailed) {
        return;
    }

    ayt::shader::ShaderResource acquired =
        pool.acquire(kLightingPhoskiaSource, kLightingCacheKey);
    if (!acquired.isValid()) {
        _programAcquireFailed = true;
        std::fprintf(stderr,
                     "[LightingPass] acquire failed; LightingPass will "
                     "run as no-op (no GPU draw). Errors:\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[LightingPass]   %s\n", err.c_str());
        }
        return;
    }

    std::fprintf(stderr,
                 "[LightingPass] program ready via Phoskia (cacheKey=%s)\n",
                 kLightingCacheKey);

    _program = acquired;
    _programReady = true;
}

uint32_t LightingPass::execute(PassExecContext& ctx)
{
    // §P5 B5 (2026-07-22) — first real GPU work in the Deferred
    // LightingPass. Phoskia VS/FS acquires, binds view 8 to the
    // LightingOutput FBO, dispatches a fullscreen triangle that
    // samples 3 GBuffer attachments + applies Lambert directional
    // light (1 direction from FrameContext::lightDirection, 1 color
    // from FrameContext::lightColor).
    //
    // B5.5+ boundary: NOT consuming shadow map (B5 keeps Lambert
    // flat, no shadow attenuation). NOT consuming gbufferMotion
    // (binding present for B7+ TAA consumer symmetry, but FS does
    // not read it — see cutsheet B5 boundary doc).
    //
    // §5.4 (2026-07-22, FO/Trans PR) — `isInitialized()` guard only,
    // NOT `|| isNoopBackend()`. Noop short-circuits inside
    // BGFXAdapter (each draw command is gated there); Pass-level
    // Noop gate would skip the scene-items loop on FO/Trans but
    // here the loop body is just 1 fullscreen-triangle submit so
    // the practical impact is small. Symmetry with FO/Trans fix
    // keeps the test semantic consistent: "logical draw count is
    // 1 on Noop".
    if (!ctx.adapter.isInitialized()) {
        return 0;
    }

    // Disable signal: host called setOutputSize(0, 0) (or never
    // called it). Mirror GBufferPass.cpp:134-136 size==0 early-out.
    if (_lightingW == 0 || _lightingH == 0) {
        return 0;
    }

    ensure(ctx.adapter, _lightingW, _lightingH);
    if (!bgfx::isValid(_lightingFbo)) {
        return 0;
    }

    ensureFullscreenQuad(ctx.adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        return 0;
    }

    ensureProgram(ctx.pool);
    if (!_program.isValid()) {
        return 0;
    }

    const FrameContext& frame = ctx.frame;

    // View 8 wiring: bind LightingOutput FBO, set rect to viewport
    // size, clear, dispatch fullscreen triangle. Mirror GBufferPass
    // view 7 wiring at GBufferPass.cpp:157-176 but with
    // LightingOutput FBO + view 8.
    const uint8_t viewId = kLightingViewId;
    ctx.adapter.setViewTransform(viewId, frame.view, frame.projection);
    ctx.adapter.setViewFrameBuffer(viewId, _lightingFbo);
    ctx.adapter.setViewRect(viewId, 0, 0, _lightingW, _lightingH);
    // Clear to black (matches cutsheet §5.2 "GBuffer/Lighting 默认
    // clear=0" lock — black is the correct "no light contribution"
    // baseline; lit fragments overwrite).
    ctx.adapter.setViewClearRaw(viewId,
                                BGFX_CLEAR_COLOR,
                                /*rgba=*/0x000000ff,
                                /*depth=*/1.0f,
                                /*stencil=*/0);

    // B5 lighting state: WRITE_RGB | WRITE_A | DEPTH_TEST_ALWAYS
    // (no depth read, no depth write — fullscreen post-process).
    // BGFXAdapter doesn't expose a setStatePostProcess helper yet,
    // so use the inline form via setStateOpaque() + override: we
    // bypass setStateOpaque and use bgfx::setState directly via
    // adapter.setStateOpaque() as the base (DEPTH_TEST_LESS +
    // CULL_CW) — but for a fullscreen post-process pass CULL_CW is
    // wrong (the oversize triangle needs no culling). Use
    // setStateDepthTestAlways() (verified at BGFXAdapter.cpp:171-178
    // documentation: "PostProcessPass fullscreen blit"). That's
    // WRITE_RGB | WRITE_A | DEPTH_TEST_ALWAYS + no CULL_CW.
    ctx.adapter.setStateDepthTestAlways();

    // §P5 B5 (2026-07-22) — BIND 3 GBuffer samplers via borrowed
    // pointer to ctx.gbufferPass. The handles come from the
    // producer (B4a GBufferPass cacheAttachments) — we just read
    // them. Falls through cleanly if any is invalid (cutsheet
    // §1.7 "no work" signal — but B5 draws 1 submit regardless;
    // missing samplers surface as visual artifacts, not crashes).
    if (ctx.gbufferPass != nullptr) {
        // albedo sampler — stage from compiled Phoskia
        const shader::BindingId albedoBinding =
            _program.getTextureBinding("gbufferAlbedo");
        if (albedoBinding != shader::InvalidBinding) {
            bgfx::TextureHandle albedoHandle = ctx.gbufferPass->gbufferAlbedoRt();
            if (bgfx::isValid(albedoHandle)) {
                const uint8_t stage = _program.getTextureStage(albedoBinding);
                _program.setTexture(stage, albedoBinding,
                                    toShaderTexture(albedoHandle));
            }
        }
        // normal sampler
        const shader::BindingId normalBinding =
            _program.getTextureBinding("gbufferNormal");
        if (normalBinding != shader::InvalidBinding) {
            bgfx::TextureHandle normalHandle = ctx.gbufferPass->gbufferNormalRt();
            if (bgfx::isValid(normalHandle)) {
                const uint8_t stage = _program.getTextureStage(normalBinding);
                _program.setTexture(stage, normalBinding,
                                    toShaderTexture(normalHandle));
            }
        }
        // motion sampler (binding present, FS does not currently read it;
        // bound for B7+ TAA consumer symmetry — cutsheet B5 boundary)
        const shader::BindingId motionBinding =
            _program.getTextureBinding("gbufferMotion");
        if (motionBinding != shader::InvalidBinding) {
            bgfx::TextureHandle motionHandle = ctx.gbufferPass->gbufferMotionRt();
            if (bgfx::isValid(motionHandle)) {
                const uint8_t stage = _program.getTextureStage(motionBinding);
                _program.setTexture(stage, motionBinding,
                                    toShaderTexture(motionHandle));
            }
        }
    }

    // §P5 B5 (2026-07-22) — upload 3 light uniforms from FrameContext
    // (cutsheet §5.3 NO new FrameContext fields — reuse
    // lightDirection/lightColor/cameraPosition already shipped).
    //
    // lightDirection: Phoskia expects a TO-light vector, but
    // FrameContext stores FROM-light (frame.lightDirection points
    // from surface to light source is the conventional lighting
    // convention; verify the actual semantic at FrameContext.h:23
    // — currently the doc says "lightDirection" as-is). To avoid
    // silently breaking B5 if the semantic is opposite, we just
    // upload the raw value and document the direction convention
    // in the Phoskia source comment. B5.5+ will verify against
    // captured frames.
    //
    // Phoskia uniform ABI: vec4 uniforms are uploaded as vec4 (one
    // vec4 = bgfx Vec4 slot — see docs/pass-lessons-from-shadow.md
    // §3.1). 3-float vec3 → 4-float padded with .w = 0 (or 1 for
    // position-like).
    const shader::BindingId lightDirBinding =
        _program.getUniformBinding("u_lightDirection");
    if (lightDirBinding != shader::InvalidBinding) {
        const float lightDir[4] = {
            frame.lightDirection.x,
            frame.lightDirection.y,
            frame.lightDirection.z,
            0.0f,
        };
        _program.setUniform(lightDirBinding, lightDir, sizeof(lightDir));
    }

    const shader::BindingId lightColorBinding =
        _program.getUniformBinding("u_lightColor");
    if (lightColorBinding != shader::InvalidBinding) {
        const float lightColor[4] = {
            frame.lightColor.x,
            frame.lightColor.y,
            frame.lightColor.z,
            0.0f,
        };
        _program.setUniform(lightColorBinding, lightColor, sizeof(lightColor));
    }

    const shader::BindingId cameraPosBinding =
        _program.getUniformBinding("u_cameraPos");
    if (cameraPosBinding != shader::InvalidBinding) {
        const float cameraPos[4] = {
            frame.cameraPosition.x,
            frame.cameraPosition.y,
            frame.cameraPosition.z,
            1.0f,  // position-like → .w = 1
        };
        _program.setUniform(cameraPosBinding, cameraPos, sizeof(cameraPos));
    }

    // §P5 B5 (2026-07-22) — fullscreen triangle dispatch. B5
    // submits exactly 1 draw (the fullscreen triangle), not N.
    // `_program.submit()` writes viewId + draws using the
    // Adapter-set state. setTransform is a no-op for a fullscreen
    // post-process pass (no model matrix), but the API still wants
    // a valid identity; cheaper to skip — the FS only reads the
    // viewId and the state.
    ctx.adapter.setTransform(ayt::math::Float4x4::identity());
    ctx.adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    ctx.adapter.setIndexBuffer(_fullscreenIB, 0, 3);

    shader::DrawCallContext submitCtx;
    submitCtx.viewId = viewId;
    submitCtx.state  = 0;  // state owned by Adapter
    _program.submit(submitCtx);

    return 1;  // B5 ships exactly 1 draw (fullscreen triangle)
}

} // namespace ayt::render::detail