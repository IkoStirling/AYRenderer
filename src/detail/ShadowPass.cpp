#include "detail/ShadowPass.h"

#include "detail/ShadowLightMatrix.h"

#include "AYShaderResource.h"  // ShaderResource::getUniformBlockBinding / setUniform / etc.

#include <bgfx/bgfx.h>

#include <cstdio>

namespace ayt::render::detail
{

namespace {

// PR-F3 (2026-07-21) — depth-only caster program with a conditional
// `castSkinned` vertex segment. The unconditional static-path is the
// hot path (most items in a scene aren't skinned). Skinned items pay
// the `if (castSkinned == 1)` branch cost once per vertex; the
// alternative (two programs, two VBs) doubles the bgfx program state.
//
// Why inline (mirror of PostProcessPass's postprocess.phoskia and
// forward-sampling's simple_lit_shadow.phoskia):
//   (a) ShaderResourcePool::acquire(src, cacheKey) takes the source
//       string directly — no "find asset at run-time" lookup that
//       would need a mount path in headless tests.
//   (b) The only asset that compiles this source is ShadowPass, so
//       co-locating prevents double-maintenance.
constexpr const char* kShadowCasterPhoskiaSource = R"(
material ShadowCaster {
    uniformblock Skeleton {
        mat4 bones[128]
    }
    property castSkinned = 0

    vertex {
        in pos    : position
        in boneId : boneindices
        in boneWt : boneweights

        if (castSkinned == 1) {
            let skinned = skinningMatrix(boneId, boneWt, Skeleton.bones, vec4(pos, 1.0))
            return modelViewProjection * skinned
        } else {
            return modelViewProjection * vec4(pos, 1.0)
        }
    }
    fragment {
        return vec4(0.0, 0.0, 0.0, 1.0)
    }
}
)";

constexpr const char* kShadowCasterCacheKey = "shadow_caster_f3";

} // namespace

ShadowPass::~ShadowPass()
{
    // dtor intentionally does not touch bgfx handles — same pattern as
    // PostProcessPass. Call destroyResources() before mid-lifetime teardown.
}

uint32_t ShadowPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    ayt::shader::ShaderResourcePool& pool = ctx.pool;
    const uint8_t viewId = ctx.viewId;
    const auto& meshes    = ctx.meshes;
    const RenderScene& scene = ctx.scene;

    if (!adapter.isInitialized() || adapter.isNoopBackend()) {
        return 0;
    }

    if (_requestedSize == 0) {
        _requestedSize = kDefaultShadowMapSize;
    }
    ensureShadowFbo(adapter, _requestedSize);
    if (!BGFXAdapter::isValid(_shadowFbo)) {
        std::fprintf(stderr,
                     "[ShadowPass] shadow FBO create failed at %ux%u; "
                     "shadow disabled for this frame\n",
                     _requestedSize, _requestedSize);
        return 0;
    }

    // PR-F3 (2026-07-21) — acquire the depth-only caster program
    // lazily on the first real-backend frame. The cache key matches
    // a single program asset across all ShadowPass instances; the
    // ShaderResourcePool owns the lifetime, this pass just holds a
    // non-owning reference (same contract as PostProcessPass). On
    // acquire failure (no shaderc, parse error, etc.) we log the
    // surface-error and fall through to the ORIGINAL F2 fallback
    // path — BGFXAdapter::submit with an invalid program records the
    // draw but writes nothing to depth. Backwards compatibility for
    // hosts that have the cache warm but the asset missing.
    ensureCasterProgram(pool);
    const bool casterReady = _caster.isValid()
        && _casterSkeletonBinding != ayt::shader::InvalidBinding;

    const bool homogeneousDepth =
        bgfx::getCaps() != nullptr && bgfx::getCaps()->homogeneousDepth;
    buildDirectionalShadowMatrices(
        ctx.frame.lightDirection,
        _lightView,
        _lightProj,
        ayt::math::FVector3(0.0f, 0.0f, 0.0f),
        kDefaultFrustumRadius,
        homogeneousDepth);
    _lightViewProj = _lightProj * _lightView;

    adapter.setViewFrameBuffer(viewId, _shadowFbo);
    adapter.setViewRect(viewId, 0, 0, _requestedSize, _requestedSize);
    adapter.setViewTransform(viewId, _lightView.ptr(), _lightProj.ptr());
    adapter.setViewClearDepthOnly(viewId, /*depth=*/1.0f);

    const uint64_t depthOnlyState = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                                  | BGFX_STATE_CULL_CW;
    adapter.setState(depthOnlyState);

    uint32_t drawCount = 0;
    for (const DrawItem& item : scene.items()) {
        if (!item.mesh.isValid()) {
            continue;
        }
        const auto meshIt = meshes.find(item.mesh.id);
        if (meshIt == meshes.end()) {
            continue;
        }
        const GpuMesh& mesh = meshIt->second;
        if (!BGFXAdapter::isValid(mesh.vertexBuffer)
            || !BGFXAdapter::isValid(mesh.indexBuffer)) {
            continue;
        }

        adapter.setTransform(item.world);
        adapter.setVertexBuffer(mesh.vertexBuffer, 0, UINT32_MAX);
        adapter.setIndexBuffer(mesh.indexBuffer, 0, mesh.indexCount);

        if (casterReady) {
            // PR-F3 — toggle the skinned branch off by default
            // (most items are static). When item.boneMatrices is
            // non-null + castSkinned binding exists, flip the
            // property so the VS swizzles into the Skeleton UBO
            // path. Per-draw; matches how ForwardOpaquePass's
            // upload happens immediately before submit.
            const uint8_t castSkinnedValue =
                (item.boneMatrices != nullptr && item.jointCount > 0) ? 1u : 0u;

            // ForwardOpaquePass owns the Skeleton UBO upload for
            // the FO pass; here on the depth pass we re-upload with
            // THIS program's binding (Phoskia's uniform blocks
            // are per-program, not shared — bones live twice in
            // program state but only once per frame in CPU
            // memory).
            tryUploadBonePalette(_caster,
                                 _casterSkeletonBinding,
                                 _casterCastSkinned,
                                 castSkinnedValue,
                                 item);

            ayt::shader::DrawCallContext sub;
            sub.viewId = viewId;
            sub.state  = depthOnlyState;
            _caster.submit(sub);
        } else {
            // F2 fallback (PR-F2's no-op path) — preserves hosts
            // that can't compile the caster material (e.g. disabled
            // shaderc on CI). Records the draw but writes nothing
            // to depth; the shadow map stays clear, consumers see
            // "shadowed = 0.0" everywhere.
            adapter.submit(viewId,
                           bgfx::ProgramHandle{BGFX_INVALID_HANDLE},
                           /*depth=*/0,
                           BGFX_DISCARD_ALL);
        }
        ++drawCount;
    }

    adapter.setViewFrameBuffer(viewId, BGFX_INVALID_HANDLE);
    return drawCount;
}

void ShadowPass::ensureShadowFbo(BGFXAdapter& adapter, uint16_t size)
{
    if (BGFXAdapter::isValid(_shadowFbo) && _shadowSize == size) {
        return;
    }
    if (BGFXAdapter::isValid(_shadowFbo)) {
        adapter.destroy(_shadowFbo);
        _shadowFbo  = BGFX_INVALID_HANDLE;
        _shadowSize = 0;
    }
    _shadowFbo = adapter.createDepthOnlyFrameBuffer(size, size);
    if (BGFXAdapter::isValid(_shadowFbo)) {
        _shadowSize = size;
    }
}

void ShadowPass::destroyResources(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_shadowFbo)) {
        adapter.destroy(_shadowFbo);
        _shadowFbo  = BGFX_INVALID_HANDLE;
        _shadowSize = 0;
    }

    // PR-F3 (2026-07-21) — release the ShaderResource reference.
    // The pool (held by Renderer::Impl) owns the underlying GPU
    // program; this just drops our local handle. Same pattern as
    // PostProcessPass::destroyResources.
    _caster.reset();
    _casterSkeletonBinding = ayt::shader::InvalidBinding;
    _casterCastSkinned     = ayt::shader::InvalidBinding;
}

// PR-F3 (2026-07-21) — lazy-acquire the depth-only caster program.
// Mirrors PostProcessPass::ensureProgram one-for-one so a future
// "refactor these to share a base" is the obvious extraction. On
// acquire failure the cached program state stays default-init and
// execute() falls back to the BGFXAdapter INVALID-program submit
// (records the draw, writes nothing — same shape as the original
// F2 fallback).
void ShadowPass::ensureCasterProgram(ayt::shader::ShaderResourcePool& pool)
{
    if (_caster.isValid()) {
        return;
    }

    ayt::shader::ShaderResource acquired =
        pool.acquire(kShadowCasterPhoskiaSource, kShadowCasterCacheKey);
    if (!acquired.isValid()) {
        std::fprintf(stderr,
                     "[ShadowPass] Phoskia acquire failed; "
                     "shadow depth pass will run as F2 no-op fallback\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[ShadowPass]   %s\n", err.c_str());
        }
        return;
    }
    _caster = acquired;
    _casterSkeletonBinding = _caster.getUniformBlockBinding("Skeleton");
    _casterCastSkinned     = _caster.getUniformBinding("castSkinned");

    // Skeleton UBO must exist for skinned items to land in the
    // correct clip. Without it, accept the castSkinned=0 path
    // only — skinned items will render at the world-space
    // transform (T-pose, but at least not crash). The Phoskia
    // binding lookup failure is rare; we log at most 3 frames to
    // avoid stderr spam.
    if (_casterSkeletonBinding == ayt::shader::InvalidBinding) {
        static uint32_t s_missingLog = 0;
        if (s_missingLog < 3) {
            std::fprintf(stderr,
                         "[ShadowPass] caster program compiled without "
                         "`Skeleton` UBO; skinned casts rendered as static "
                         "(T-pose) depth\n");
            ++s_missingLog;
        }
    }
}

} // namespace ayt::render::detail
