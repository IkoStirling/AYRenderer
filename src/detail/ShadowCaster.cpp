#include "detail/ShadowCaster.h"

#include "AYRenderer/ShadowConfig.h"
#include "AYRenderer/ShadowDiagnostics.h"
#include "AYRenderer/ShadowShaderSources.h"
#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <AYIO/Env.h>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace ayt::render::detail
{

using ayt::render::castsShadow;
using ayt::render::kShadowCasterCacheKey;
using ayt::render::kShadowCasterFragmentSc;
using ayt::render::kShadowCasterPhoskiaSource;
using ayt::render::kShadowCasterVaryingSc;
using ayt::render::kShadowCasterVertexSc;
using ayt::render::kShadowMaskCasterCacheKey;
using ayt::render::kShadowMaskCasterFragmentSc;
using ayt::render::kShadowMaskCasterVaryingSc;
using ayt::render::kShadowMaskCasterVertexSc;

namespace {

// §P4 M12 (2026-08-24) — hoist the hard-coded "albedo-like" slot
// name allow-list into named constants. The previous inline
// `slot.name != "albedoMap" && ... && slot.name != "albedo"`
// chain was a maintenance hazard: adding a new importer that
// wrote "baseColor" instead of "baseColorTexture" silently
// bypassed shadow caster colorization. Hoisted as `constexpr
// std::string_view` (cheap to compare against `slot.name`).
constexpr std::string_view kAlbedoSlotAlbedoMap       = "albedoMap";
constexpr std::string_view kAlbedoSlotBaseColorTexture = "baseColorTexture";
constexpr std::string_view kAlbedoSlotDiffuse          = "diffuse";
constexpr std::string_view kAlbedoSlotMainTexture      = "mainTexture";
constexpr std::string_view kAlbedoSlotAlbedo           = "albedo";
constexpr std::string_view kOpacitySlotName            = "opacityTexture";

bool isAlbedoSlotName(std::string_view name) noexcept
{
    return name == kAlbedoSlotAlbedoMap
        || name == kAlbedoSlotBaseColorTexture
        || name == kAlbedoSlotDiffuse
        || name == kAlbedoSlotMainTexture
        || name == kAlbedoSlotAlbedo;
}

bool envForceScCaster()
{
    // §P4 M2 (2026-08-24) — call `env::get` once into a local; the
    // previous shape did two `env::get` calls per dispatch and
    // returned early on the first non-empty branch, leaking the
    // legacy AY_SHADOW_USE_PHOSKIA read onto the hot path even
    // when the modern env var was set. Now the modern key is
    // checked once (any non-"0" value ⇒ force .sc); the legacy key
    // is only consulted when the modern key is absent.
    const std::string sc = ayt::io::env::get("AY_SHADOW_USE_SC").value_or("");
    if (!sc.empty() && sc[0] != '0') {
        return true;
    }
    if (!sc.empty()) {
        // Modern key explicitly disabled .sc — don't fall through
        // to the legacy AY_SHADOW_USE_PHOSKIA escape hatch.
        return false;
    }
    // Legacy: AY_SHADOW_USE_PHOSKIA=0 forces .sc.
    const std::string v = ayt::io::env::get("AY_SHADOW_USE_PHOSKIA").value_or("");
    return v == "0";
}

} // namespace

void ShadowCaster::destroy(BGFXAdapter& /*adapter*/)
{
    _program.reset();
    _maskProgram.reset();
    _skeletonBinding   = ayt::shader::InvalidBinding;
    _castSkinnedBinding = ayt::shader::InvalidBinding;
    _solidBinding      = ayt::shader::InvalidBinding;
    _maskSkeletonBinding = ayt::shader::InvalidBinding;
    _maskCastSkinnedBinding = ayt::shader::InvalidBinding;
    _maskCutoffBinding = ayt::shader::InvalidBinding;
    _maskBaseColorBinding = ayt::shader::InvalidBinding;
    _maskOpacityBinding = ayt::shader::InvalidBinding;
    _maskOpacitySourceBinding = ayt::shader::InvalidBinding;
    _acquireFailed     = false;
    _maskAcquireFailed = false;
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
        // §P4 M3 (2026-08-24) — rate-limited diagnostic on the
        // `_acquireFailed` short-circuit. The previous shape
        // returned silently; an editor session that hit an
        // acquire failure would see zero log output on subsequent
        // frames, making the "shadow disabled forever" symptom
        // hard to triage. Mirror the Part 3 rate-limited pattern:
        // first frame logs full reason, subsequent frames log
        // once every N calls so the console stays usable during
        // long headless captures.
        if (_acquireFailed) {
            static uint32_t s_failSkipLog = 0;
            if (s_failSkipLog < 4) {
                std::fprintf(stderr,
                             "[ShadowCaster] acquire previously failed; "
                             "skipping retry (s_failSkipLog=%u)\n",
                             s_failSkipLog);
                ++s_failSkipLog;
            }
        }
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

    if (!_maskProgram.isValid() && !_maskAcquireFailed) {
        ayt::shader::ShaderResource mask = pool.acquireFromBgfxSc(
            kShadowMaskCasterVertexSc,
            kShadowMaskCasterFragmentSc,
            kShadowMaskCasterVaryingSc,
            kShadowMaskCasterCacheKey);
        if (!mask.isValid()) {
            _maskAcquireFailed = true;
            std::fprintf(stderr,
                         "[ShadowCaster] alpha-mask caster acquire failed; "
                         "Mask materials will skip shadow submission\n");
            for (const std::string& err : pool.lastCompileErrors()) {
                std::fprintf(stderr, "[ShadowCaster]   %s\n", err.c_str());
            }
        } else {
            _maskProgram = mask;
            _maskSkeletonBinding = _maskProgram.getUniformBlockBinding("Skeleton");
            _maskCastSkinnedBinding = _maskProgram.getUniformBinding("castSkinned");
            _maskCutoffBinding = _maskProgram.getUniformBinding("alphaCutoff");
            _maskBaseColorBinding = _maskProgram.getUniformBinding("baseColor");
            _maskOpacityBinding = _maskProgram.getUniformBinding("opacity");
            _maskOpacitySourceBinding =
                _maskProgram.getUniformBinding("opacitySource");
        }
    }
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
    const std::unordered_map<uint64_t, GpuMesh>& meshes,
    const std::unordered_map<uint64_t, GpuTexture>& textures,
    const std::unordered_map<uint64_t, GpuMaterial>& materials)
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

        const auto materialIt = materials.find(item.material.id);
        if (materialIt == materials.end()) {
            continue;
        }
        const GpuMaterial& material = materialIt->second;
        // Transparent surfaces do not cast by default. This avoids opaque
        // rectangular cards in the shadow map; an explicit translucent
        // shadow model can be introduced as a separate material feature.
        if (material.blendMode == ayt::render::BlendMode::Alpha) {
            continue;
        }
        const bool alphaMask = material.alphaCutout;
        if (alphaMask && !_maskProgram.isValid()) {
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

        const DrawIndexRange drawRange = resolveDrawIndexRange(item, mesh);
        if (drawRange.indexCount == 0) {
            continue;
        }

        adapter.setTransform(item.world);
        adapter.setVertexBuffer(mesh.vertexBuffer, 0, UINT32_MAX);
        adapter.setIndexBuffer(mesh.indexBuffer, drawRange.firstIndex,
                               drawRange.indexCount);

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
                             static_cast<unsigned>(drawRange.indexCount),
                             static_cast<unsigned>(item.shadowFlags));
                ++s_castLog;
            }
        }

        ayt::shader::ShaderResource& drawProgram =
            alphaMask ? _maskProgram : _program;
        if (casterReady) {
            const uint8_t castSkinnedValue =
                (item.boneMatrices != nullptr && item.jointCount > 0) ? 1u : 0u;

            tryUploadBonePalette(drawProgram,
                                 alphaMask ? _maskSkeletonBinding : _skeletonBinding,
                                 alphaMask ? _maskCastSkinnedBinding : _castSkinnedBinding,
                                 castSkinnedValue,
                                 item);

            if (alphaMask) {
                if (_maskCutoffBinding != ayt::shader::InvalidBinding) {
                    const float cutoff[4] = {
                        material.alphaCutoff, 0.0f, 0.0f, 0.0f
                    };
                    drawProgram.setUniform(_maskCutoffBinding, cutoff, sizeof(cutoff));
                }
                if (_maskBaseColorBinding != ayt::shader::InvalidBinding) {
                    // §P4 M11 (2026-08-24) — drop the four
                    // `hasColorOverride ? c.x : 1.0f` ternaries.
                    // The GpuMaterial contract is that
                    // `colorOverride` is always
                    // `(1,1,1,1)` when `hasColorOverride==false`,
                    // so the ternary was an identity on the
                    // `hasColorOverride==true` branch and a
                    // hard-coded `(1,1,1,1)` on the other — i.e.
                    // always equal to `material.colorOverride`.
                    // Shadow caster uses the value for matte tint,
                    // not for the receiver PBR pass, so this
                    // simplification is safe here.
                    const float baseColor[4] = {
                        material.colorOverride.x,
                        material.colorOverride.y,
                        material.colorOverride.z,
                        material.colorOverride.w,
                    };
                    drawProgram.setUniform(_maskBaseColorBinding,
                                           baseColor, sizeof(baseColor));
                }
                if (_maskOpacitySourceBinding != ayt::shader::InvalidBinding) {
                    const float source[4] = {
                        materialUniformScalar(material, "opacitySource", 0.0f),
                        0.0f, 0.0f, 0.0f
                    };
                    drawProgram.setUniform(_maskOpacitySourceBinding,
                                           source, sizeof(source));
                }
                if (_maskOpacityBinding != ayt::shader::InvalidBinding) {
                    const float opacityValue[4] = {
                        materialUniformScalar(material, "opacity", 1.0f),
                        0.0f, 0.0f, 0.0f
                    };
                    drawProgram.setUniform(_maskOpacityBinding,
                                           opacityValue, sizeof(opacityValue));
                }

                bool albedoBound = false;
                bool opacityBound = false;
                const ayt::shader::BindingId albedoBinding =
                    drawProgram.getTextureBinding("albedoMap");
                const ayt::shader::BindingId opacityBinding =
                    drawProgram.getTextureBinding("opacityMap");
                for (const GpuMaterial::TextureSlot& slot : material.textures) {
                    if (!slot.texture.isValid()) continue;
                    const bool isOpacity = slot.name == kOpacitySlotName;
                    // §P4 M12 (2026-08-24) — collapsed the
                    // 5-string inline allow-list into
                    // `isAlbedoSlotName`. The helper is
                    // declared at the top of this TU (next to
                    // the kAlbedoSlot* constants).
                    if (!isOpacity && !isAlbedoSlotName(slot.name)) {
                        continue;
                    }
                    const auto textureIt = textures.find(slot.texture.id);
                    if (textureIt == textures.end()
                        || !BGFXAdapter::isValid(textureIt->second.handle)) {
                        continue;
                    }
                    const ayt::shader::BindingId binding =
                        isOpacity ? opacityBinding : albedoBinding;
                    if (binding == ayt::shader::InvalidBinding) continue;
                    drawProgram.setTexture(
                        drawProgram.getTextureStage(binding),
                        binding,
                        toShaderTexture(textureIt->second.handle));
                    if (isOpacity) opacityBound = true;
                    else albedoBound = true;
                }
                tryBindWhiteTexture(drawProgram, adapter, "albedoMap", albedoBound);
                tryBindWhiteTexture(drawProgram, adapter, "opacityMap", opacityBound);
            }

            if (!alphaMask && _solidBinding != ayt::shader::InvalidBinding) {
                // bgfx Vec4 slot (Phoskia float property also lands as Vec4).
                const float solidVal[4] = {
                    ayt::render::ShadowDiagnostics::casterSolidEnabled() ? 1.0f : 0.0f,
                    0.0f, 0.0f, 0.0f};
                drawProgram.setUniform(_solidBinding, solidVal, sizeof(solidVal));
            }

            ayt::shader::DrawCallContext sub;
            sub.viewId = viewId;
            sub.state  = material.doubleSided
                ? (casterState & ~BGFX_STATE_CULL_MASK)
                : casterState;
            drawProgram.submit(sub);
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
