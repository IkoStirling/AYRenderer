#include "detail/GBufferPass.h"

#include "detail/BgfxMatrix.h"
#include "detail/FrameContext.h"
#include "detail/RenderPass.h"

#include <cstdio>

namespace ayt::render::detail
{

// §P5 B4a (2026-07-22) — build stamp literal (mirror
// ShadowMapResources.h:55). Pointer-equal comparison; callers MUST
// pass this exact literal. Bumping requires a string change here AND
// a re-ensure (next execute() will rebuild the FBO).
// Stamp bump forces GBuffer FBO rebuild after RT2 format changes
// (RGBA8 motion → RGBA16F worldPos). Pointer-equal compare in ensure().
static constexpr const char* kGBufferBuildStamp = "b5p5-2026-07-23-rgba16f-rt2";

// §P5 B5.5 / deferred-shadow contract (2026-07-23):
//   RT0 albedo RGBA8 / RT1 normal RGBA8 / RT2 worldPos RGBA16F /
//   depth D24S8. Slot is still named `gbufferMotion` (MRT name lock)
//   but stores raw worldPos xyz for LightingPass key-only PCF.
//   Motion NDC encoding is deferred until a dedicated RT exists —
//   writing motion into RGBA8 RT2 previously broke shadows (Lighting
//   projected [0,1] junk as worldPos).
//
// `u_prevViewProj` stays declared + uploaded so B7+ TAA can reclaim
// the slot without another host wire; FS does not write motion today.
//
// Three `out color` slots map to gl_FragData[0..2]:
//   gl_FragData[0] = gbufferAlbedo
//   gl_FragData[1] = gbufferNormal
//   gl_FragData[2] = gbufferMotion  (xyz = worldPos, w = 1)
constexpr const char* kGBufferPhoskiaSource = R"(
material GBufferFill {
    texture2d albedoMap
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0)
    uniform mat4 u_prevViewProj

    vertex {
        in pos : position
        in nrm : normal
        in uv  : texcoord
        out worldNormal : normal   = (modelMatrix * vec4(nrm, 0.0)).xyz
        out worldPos    : position = (modelMatrix * vec4(pos, 1.0)).xyz
        out vUv         : texcoord = uv
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in worldNormal : normal
        in worldPos    : position
        in vUv         : texcoord
        out gbufferAlbedo : color = vec4(0.0, 0.0, 0.0, 0.0)
        out gbufferNormal : color = vec4(0.0, 0.0, 0.0, 0.0)
        out gbufferMotion : color = vec4(0.0, 0.0, 0.0, 0.0)
        let n = normalize(worldNormal)
        let albedo = sample(albedoMap, vUv) * baseColor
        gbufferAlbedo = vec4(albedo.rgb, albedo.a)
        gbufferNormal = vec4(n * 0.5 + vec3(0.5, 0.5, 0.5), 1.0)
        gbufferMotion = vec4(worldPos, 1.0)
    }
}
)";

// Cache key: worldPos in RT2 (RGBA16F FBO) for deferred shadow PCF.
static constexpr const char* kGBufferCacheKey = "gbuffer_fill_v7_worldpos_rgba16f";
const char* const kGBufferCacheKeyCStr = kGBufferCacheKey;
const char* const kGBufferBuildStampCStr = kGBufferBuildStamp;

GBufferPass::~GBufferPass() = default;

void GBufferPass::ensureProgram(ayt::shader::ShaderResourcePool& pool)
{
    // Mirror ShadowCaster::ensureProgram (ShadowCaster.cpp:48-101).
    // Issue 1 fix (2026-07-21) — `const char*` (constexpr pointer)
    // not std::string. Pointer-equal compare; bumping the literal
    // forces re-acquire.
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != kGBufferCacheKey) {
        _program.reset();
        _acquireFailed = false;
        s_acquiredCacheKey = kGBufferCacheKey;
    }

    if (_program.isValid() || _acquireFailed) {
        return;
    }

    ayt::shader::ShaderResource acquired =
        pool.acquire(kGBufferPhoskiaSource, kGBufferCacheKey);
    if (!acquired.isValid()) {
        _acquireFailed = true;
        std::fprintf(stderr,
                     "[GBufferPass] acquire failed; GBuffer fill pass will "
                     "run as no-op (no GPU draw). Errors:\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[GBufferPass]   %s\n", err.c_str());
        }
        return;
    }

    std::fprintf(stderr,
                 "[GBufferPass] program ready via Phoskia (cacheKey=%s)\n",
                 kGBufferCacheKey);

    _program = acquired;
}

bool GBufferPass::isProgramReady() const noexcept
{
    return _program.isValid();
}

void GBufferPass::setPrevViewProj(const ayt::math::Float4x4& view,
                                  const ayt::math::Float4x4& projection) noexcept
{
    // §P5 B4c (2026-07-22) — host pushes prev view/projection. Stored
    // as-is; execute() builds prevViewProj = proj * view per
    // pass-lessons-from-shadow.md §3.1 P×V ordering.
    //
    // Idempotent: Renderer::render() calls this every frame inside
    // the GBuffer slot block. Re-pushing the same matrices is free
    // (4×4 copy = 64 bytes, negligible).
    //
    // No GPU work here. The actual upload happens inside execute()
    // once the program is ready + we have a valid scene to draw.
    _prevView       = view;
    _prevProjection = projection;
}

uint32_t GBufferPass::execute(PassExecContext& ctx)
{
    // §P5 B4a (2026-07-22) — ensure 4-attach MRT FBO + cache attachments.
    // §P5 B4b (2026-07-22) — first real GPU draw dispatch in deferred path:
    //   - Phoskia GBuffer VS/FS acquires via pool.acquire
    //   - bind view 7 to the 4-attach MRT FBO
    //   - clear color + depth (must clear color because each attachment
    //     is an offscreen RT, backbuffer clears don't touch them)
    //   - iterate RenderScene.items() and submit draw calls (mirror
    //     ForwardOpaquePass shape, but view 7 + 3-output FS)
    //
    // B5 LightingPass will consume gbufferFbo() + the 3 RT attachments
    // as its scene-color/normal/motion inputs.
    if (!ctx.adapter.isInitialized() || ctx.adapter.isNoopBackend()) {
        return 0;
    }

    // Disable signal: host called setGbufferSize(0, 0) (or never
    // called setGbufferSize). Mirror ShadowMapResources::ensure
    // early-return on size == 0.
    if (_gbufferW == 0 || _gbufferH == 0) {
        return 0;
    }

    ensure(ctx.adapter, _gbufferW, _gbufferH);
    if (!bgfx::isValid(_gbufferFbo)) {
        return 0;
    }

    // B4b: Phoskia VS/FS must succeed before we touch the GPU.
    // ensureProgram records _acquireFailed on compile error so we
    // skip dispatch silently (cutsheet §1.7 "no FBO/work" signal).
    ensureProgram(ctx.pool);
    if (!_program.isValid()) {
        return 0;
    }

    const FrameContext& frame = ctx.frame;

    // View 7 wiring: bind MRT FBO, set rect to GBuffer size (full
    // viewport), clear all 4 attachments, draw scene items. Mirrors
    // ForwardOpaquePass shape but targets an offscreen RT instead of
    // the backbuffer / sceneFbo.
    const uint8_t viewId = kGBufferViewId;
    ctx.adapter.setViewTransform(viewId, frame.view, frame.projection);
    ctx.adapter.setViewFrameBuffer(viewId, _gbufferFbo);
    ctx.adapter.setViewRect(viewId, 0, 0, _gbufferW, _gbufferH);
    // Cutsheet §5.2: 3× RGBA8 + 1× D24S8. Clears every attachment.
    // depth=1.0 / stencil=0 is the conventional far-plane / cleared
    // stencil (matches ForwardOpaquePass's `0x191a1cff` scene-FBO
    // clear — same uniform-clear semantics on a different FBO).
    ctx.adapter.setViewClearRaw(viewId,
                                BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                                /*rgba=*/0x000000ff,
                                /*depth=*/1.0f,
                                /*stencil=*/0);

    // GBufferPass draw state: depth-write + no-blend (same as
    // ForwardOpaquePass opaque defaults — cutsheet §1.4 forward
    // fill semantics). The cached Adapter state is overwritten by
    // setStateOpaque() each pass (see ForwardOpaquePass::execute at
    // AYRenderer/src/detail/ForwardOpaquePass.cpp:189 — same line).
    ctx.adapter.setStateOpaque();

    uint32_t drawCount = 0;
    for (const DrawItem& item : ctx.scene.items()) {
        if (item.outlineHull) {
            continue;
        }
        if (!item.mesh.isValid() || !item.material.isValid()) {
            continue;
        }
        const auto meshIt = ctx.meshes.find(item.mesh.id);
        const auto matIt  = ctx.materials.find(item.material.id);
        if (meshIt == ctx.meshes.end() || matIt == ctx.materials.end()) {
            continue;
        }
        const GpuMesh& mesh = meshIt->second;
        if (!BGFXAdapter::isValid(mesh.vertexBuffer)
            || !BGFXAdapter::isValid(mesh.indexBuffer)) {
            continue;
        }
        const GpuMaterial& material = matIt->second;
        if (!material.shader.isValid()) {
            continue;
        }
        // Cutsheet deferred-pass.md §2: GBuffer receives Opaque only.
        // Alpha glass must not write albedo/depth here — otherwise it
        // shows as solid cyan and steals depth from real opaques.
        // TransparentPass composites Alpha after Lighting.
        if (material.blendMode == ayt::render::BlendMode::Alpha) {
            continue;
        }

        // §P5 B4c (2026-07-22) — PREV-FRAME VP UPLOAD. Build
        // prevViewProj = prevProj * prevView (P×V same-order as
        // `setViewTransform` + `viewProjectionMatrix` builtin —
        // mirror pass-lessons-from-shadow.md §3.1 warning). On real
        // backend this triggers `bgfx::setUniform(handle, ptr, 64)`
        // for `u_prevViewProj`; on Noop the upload is a no-op
        // (ShaderResource::setUniform short-circuits).
        //
        // First frame: prevViewProj = identity (default) ⇒
        // currClip - 0/1 = currClip ⇒ motion = currNDC*0.5+0.5 —
        // B7+ TAA consumer tolerates this single-frame noise.
        {
            const shader::BindingId prevVpBinding =
                _program.getUniformBinding("u_prevViewProj");
            if (prevVpBinding != shader::InvalidBinding) {
                const ayt::math::Float4x4 prevViewProj =
                    _prevProjection * _prevView;
                float prevVpCol[16];
                toBgfxColumnMajor(prevViewProj, prevVpCol);
                _program.setUniform(prevVpBinding, prevVpCol, sizeof(prevVpCol));
            }
        }

        // baseColor upload (mirror ForwardOpaquePass baseColor slot,
        // AYRenderer/src/detail/ForwardOpaquePass.cpp:42, applied via
        // the Phoskia `property baseColor` declared on GBufferFill).
        // Resolution policy identical to FO: prefer host override if
        // set, else use a neutral white tint so the GBuffer survives
        // a missing material baseColor.
        //
        // §P5 B4c (2026-07-22) — FIXED B4b bug: baseColor binding is
        // now read from `_program` (the GBufferFill Phoskia program
        // that owns the `baseColor` property uniform), NOT from
        // `material.shader` (the host material's own program). On
        // a real backend the B4b code path was uploading uniforms
        // into a different program than the one actually drawing,
        // silently dropping baseColor overrides from the GBuffer
        // fill. Read from `_program` so the binding matches the
        // submitting program below.
        const shader::BindingId baseColorBinding =
            _program.getUniformBinding("baseColor");
        if (baseColorBinding != shader::InvalidBinding) {
            const float base[4] = {
                material.hasColorOverride ? material.colorOverride.x : 1.0f,
                material.hasColorOverride ? material.colorOverride.y : 1.0f,
                material.hasColorOverride ? material.colorOverride.z : 1.0f,
                material.hasColorOverride ? material.colorOverride.w : 1.0f,
            };
            _program.setUniform(baseColorBinding, base, sizeof(base));
        }

        // Forward parity: sample(albedoMap)*baseColor. Bind host
        // material texture slots onto GBufferFill's albedoMap stage
        // (skip shadowMap — GBuffer does not consume it).
        // Imported .aymat uses baseColorTexture / diffuse; GBufferFill
        // only declares albedoMap — alias those names so characters
        // are not forced to solid white.
        for (const GpuMaterial::TextureSlot& slot : material.textures) {
            if (slot.name.empty() || !slot.texture.isValid()) {
                continue;
            }
            if (slot.name == "shadowMap") {
                continue;
            }
            shader::BindingId binding = _program.getTextureBinding(slot.name);
            if (binding == shader::InvalidBinding
                && (slot.name == "baseColorTexture"
                    || slot.name == "diffuse"
                    || slot.name == "mainTexture"
                    || slot.name == "albedo")) {
                binding = _program.getTextureBinding("albedoMap");
            }
            if (binding == shader::InvalidBinding) {
                continue;
            }
            const auto texIt = ctx.textures.find(slot.texture.id);
            if (texIt == ctx.textures.end()
                || !BGFXAdapter::isValid(texIt->second.handle)) {
                continue;
            }
            const uint8_t stage = _program.getTextureStage(binding);
            _program.setTexture(stage, binding,
                                toShaderTexture(texIt->second.handle));
        }

        ctx.adapter.setTransform(item.world);
        ctx.adapter.setVertexBuffer(mesh.vertexBuffer);
        ctx.adapter.setIndexBuffer(mesh.indexBuffer, 0, mesh.indexCount);

        shader::DrawCallContext submitCtx;
        submitCtx.viewId = viewId;
        submitCtx.state  = 0;  // state owned by Adapter; shader.submit
                                // only writes viewId (matches FO shape).
        // §P5 B4c (2026-07-22) — FIXED B4b bug: submit uses `_program`
        // (the Phoskia GBufferFill program), NOT `material.shader`
        // (the host material's program). On a real backend the B4b
        // path was submitting the host material's draws into view
        // 7, which is wrong: the GBufferFill VS/FS (which writes
        // albedo/normal/motion to gl_FragData[0..2]) is the program
        // that MUST bind view 7, otherwise the MRT attachments stay
        // untouched. Fix: bind `_program` (GBufferFill), use the
        // host material only for per-draw state (VB/IB/world) and
        // for colorOverride fallback above.
        _program.submit(submitCtx);
        ++drawCount;
    }

    return drawCount;
}

void GBufferPass::setGbufferSize(uint16_t width, uint16_t height) noexcept
{
    // B4a: still only stores request (mirror B2 behavior — do NOT
    // call ensure here; no adapter access). Host can call this BEFORE
    // initialize() and the next execute() will honor the size.
    _gbufferW = width;
    _gbufferH = height;
}

void GBufferPass::destroyResources(BGFXAdapter& adapter)
{
    // B4a: real cleanup (mirror ShadowMapResources::destroy
    // ShadowMapResources.cpp:128-143). All 4 attachments are owned
    // by _gbufferFbo (destroyTextures=true upstream), so resetting
    // them to BGFX_INVALID_HANDLE is enough — DO NOT call
    // bgfx::destroy on the cached attachments or you'll double-free.
    //
    // W/H + buildStamp are reset unconditionally (Test_B4_GBufferMRT
    // case 6 verifies gbufferWidth/Height return 0 after destroy even
    // when no FBO was ever allocated — a host that calls
    // setGbufferSize(800,600) → destroyResources() expects W/H back to
    // 0). The FBO handle itself is only destroyed when it was actually
    // allocated (calling bgfx::destroy on an invalid handle is a UAF
    // on some bgfx backends — see ShadowMapResources::destroy guard).
    _gbufferDepthRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferAlbedoRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferNormalRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferMotionRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferW = 0;
    _gbufferH = 0;
    _allocatedW = 0;
    _allocatedH = 0;
    _buildStamp = "";
    if (bgfx::isValid(_gbufferFbo)) {
        adapter.destroy(_gbufferFbo);
        _gbufferFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
}

void GBufferPass::ensure(BGFXAdapter& adapter, uint16_t width, uint16_t height)
{
    // Mirror ShadowMapResources::ensure (ShadowMapResources.cpp:93-126).
    //
    // Stamp-changed fast path: when `buildStamp` differs from
    // `_buildStamp`, force rebuild. Currently we always pass
    // kGBufferBuildStamp so stamp is stable across frames — keeps
    // the cache hot until a cutsheet-bump forces a rebuild.
    if (!adapter.isInitialized() || width == 0 || height == 0) {
        return;
    }

    const bool stampChanged = (_buildStamp != kGBufferBuildStamp);
    if (stampChanged) {
        _buildStamp = kGBufferBuildStamp;
    }

    // Fast path: allocated FBO size matches request (NOT _gbufferW —
    // setGbufferSize already wrote the request into those fields).
    if (bgfx::isValid(_gbufferFbo)
        && _allocatedW == width
        && _allocatedH == height
        && !stampChanged) {
        if (!bgfx::isValid(_gbufferAlbedoRt)) {
            cacheAttachments(adapter);
        }
        return;
    }

    // Rebuild path — destroy old FBO + reset cache + create new.
    if (bgfx::isValid(_gbufferFbo)) {
        adapter.destroy(_gbufferFbo);
        _gbufferFbo       = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        _gbufferAlbedoRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _gbufferNormalRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _gbufferMotionRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _gbufferDepthRt   = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _allocatedW = _allocatedH = 0;
    }

    _gbufferFbo = adapter.createGbufferFrameBuffer(width, height);
    if (bgfx::isValid(_gbufferFbo)) {
        _allocatedW = width;
        _allocatedH = height;
        _gbufferW = width;
        _gbufferH = height;
        cacheAttachments(adapter);
    }
}

void GBufferPass::cacheAttachments(BGFXAdapter& adapter)
{
    // Mirror ShadowMapResources::cacheColorAttachment
    // (ShadowMapResources.cpp:14-21). All 4 attachments are owned by
    // _gbufferFbo (destroyTextures=true), so we read them via
    // adapter.getFboAttachment and never call bgfx::destroy on them.
    _gbufferAlbedoRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferNormalRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferMotionRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferDepthRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    if (!bgfx::isValid(_gbufferFbo)) {
        return;
    }
    _gbufferAlbedoRt = adapter.getFboAttachment(_gbufferFbo, 0);
    _gbufferNormalRt = adapter.getFboAttachment(_gbufferFbo, 1);
    _gbufferMotionRt = adapter.getFboAttachment(_gbufferFbo, 2);
    _gbufferDepthRt  = adapter.getFboAttachment(_gbufferFbo, 3);
}

} // namespace ayt::render::detail