#include "detail/ShadowCaster.h"

#include "AYShadowConfig.h"
#include "AYShadowDiagnostics.h"
#include "AYShadowShaderSources.h"
#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <ayio/Env.h>
#include <cstdio>
#include <cmath>
#include <cstdlib>

namespace ayt::render::detail
{

using ayt::render::castsShadow;
using ayt::render::kShadowCasterCacheKey;
using ayt::render::kShadowCasterFragmentSc;
using ayt::render::kShadowCasterPhoskiaSource;
using ayt::render::kShadowCasterVaryingSc;
using ayt::render::kShadowCasterVertexSc;

namespace {

bool envForceScCaster()
{
    const std::string sc = ayt::io::env::get("AY_SHADOW_USE_SC").value_or("");
    if (!sc.empty() && sc[0] != '\0' && sc[0] != '0') {
        return true;
    }
    // Legacy: AY_SHADOW_USE_PHOSKIA=0 forces .sc.
    const std::string v = ayt::io::env::get("AY_SHADOW_USE_PHOSKIA").value_or("");
    return v == "0";
}

} // namespace

void ShadowCaster::destroy(BGFXAdapter& /*adapter*/)
{
    _program.reset();
    _skeletonBinding   = ayt::shader::InvalidBinding;
    _castSkinnedBinding = ayt::shader::InvalidBinding;
    _solidBinding      = ayt::shader::InvalidBinding;
    _acquireFailed     = false;
}

void ShadowCaster::ensureProgram(ayt::shader::ShaderResourcePool& pool)
{
    // Issue 1 fix (2026-07-21) — use `const char*` (constexpr pointer)
    // not std::string. `std::string` vs `const char*` via `operator!=`
    // resolves to pointer comparison, not string content comparison.
    // That means every process startup would always see a pointer
    // mismatch (since std::string data ptr != constexpr-pointer-literal
    // ptr), forcing the program to re-acquire every frame and never
    // reaching the pool's cache. With both sides now `const char*` the
    // comparison is pointer-equal only when the constexpr string is
    // byte-identical to the previously-recorded literal — exactly the
    // intent of "reset on cache key bump".
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != kShadowCasterCacheKey) {
        _program.reset();
        _acquireFailed = false;
        s_acquiredCacheKey = kShadowCasterCacheKey;
    }

    if (_program.isValid() || _acquireFailed) {
        return;
    }

    ayt::shader::ShaderResource acquired;
    if (envForceScCaster()) {
        acquired = pool.acquireFromBgfxSc(kShadowCasterVertexSc,
                                          kShadowCasterFragmentSc,
                                          kShadowCasterVaryingSc,
                                          kShadowCasterCacheKey);
    } else {
        acquired = pool.acquire(kShadowCasterPhoskiaSource, kShadowCasterCacheKey);
    }
    if (!acquired.isValid()) {
        _acquireFailed = true;
        std::fprintf(stderr,
                     "[ShadowCaster] acquire failed (%s); "
                     "shadow depth pass will run as F2 no-op fallback\n",
                     envForceScCaster() ? "bgfx .sc" : "Phoskia");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[ShadowCaster]   %s\n", err.c_str());
        }
        return;
    }

    std::fprintf(stderr,
                 "[ShadowCaster] program ready via %s (cacheKey=%s)\n",
                 envForceScCaster() ? "bgfx .sc" : "Phoskia",
                 kShadowCasterCacheKey);

    _program = acquired;
    _skeletonBinding    = _program.getUniformBlockBinding("Skeleton");
    _castSkinnedBinding = _program.getUniformBinding("castSkinned");
    _solidBinding       = _program.getUniformBinding("casterSolidTest");
}

bool ShadowCaster::isProgramReady() const noexcept
{
    return _program.isValid();
}

uint32_t ShadowCaster::drawCasters(
    BGFXAdapter& adapter,
    const uint8_t viewId,
    const uint64_t casterState,
    const RenderScene& scene,
    const std::unordered_map<uint64_t, GpuMesh>& meshes)
{
    const bool casterReady = _program.isValid();
    uint32_t drawCount = 0;

    for (const DrawItem& item : scene.items()) {
        if (!castsShadow(item.shadowFlags)) {
            continue;
        }
        // CM-1 (2026-08-11) — 2D lane items never cast: ortho z=0
        // quads in the light's view-proj are meaningless + waste the
        // shadow map. The payload pointer is the lane discriminator
        // (ForwardOpaquePass mirror).
        if (item.payload != nullptr) {
            continue;
        }
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

        if (ayt::render::ShadowDiagnostics::enabled(ayt::render::ShadowLogLevel::L4_Verbose)) {
            static uint32_t s_castLog = 0;
            if (s_castLog < ayt::render::ShadowDiagnostics::kVerboseLogLimit) {
                const float* m = item.world.ptr();
                std::fprintf(stderr,
                             "[ShadowDbg] cast draw#%u worldT=(%.2f,%.2f,%.2f) "
                             "sx=%.2f sy=%.2f sz=%.2f idx=%u flags=0x%02x\n",
                             s_castLog,
                             m[3], m[7], m[11],
                             std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]),
                             std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]),
                             std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]),
                             static_cast<unsigned>(mesh.indexCount),
                             static_cast<unsigned>(item.shadowFlags));
                ++s_castLog;
            }
        }

        if (casterReady) {
            const uint8_t castSkinnedValue =
                (item.boneMatrices != nullptr && item.jointCount > 0) ? 1u : 0u;

            tryUploadBonePalette(_program,
                                 _skeletonBinding,
                                 _castSkinnedBinding,
                                 castSkinnedValue,
                                 item);

            if (_solidBinding != ayt::shader::InvalidBinding) {
                // bgfx Vec4 slot (Phoskia float property also lands as Vec4).
                const float solidVal[4] = {
                    ayt::render::ShadowDiagnostics::casterSolidEnabled() ? 1.0f : 0.0f,
                    0.0f, 0.0f, 0.0f};
                _program.setUniform(_solidBinding, solidVal, sizeof(solidVal));
            }

            ayt::shader::DrawCallContext sub;
            sub.viewId = viewId;
            sub.state  = casterState;
            _program.submit(sub);
        } else {
            adapter.submit(viewId,
                           bgfx::ProgramHandle{BGFX_INVALID_HANDLE},
                           /*depth=*/0,
                           BGFX_DISCARD_ALL);
        }
        ++drawCount;
    }

    return drawCount;
}

} // namespace ayt::render::detail
