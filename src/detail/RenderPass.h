#pragma once

#include "AYRenderer/RenderScene.h"
#include "AYShader/ShaderResourcePool.h"
#include "detail/BGFXAdapter.h"
#include "detail/BgfxMatrix.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/WireframeGeometry.h"

#include <cstdint>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unordered_map>

// Forward declaration — tryBindShadowSampler (below) only needs a
// pointer; ShadowPass.h pulls in <bgfx/bgfx.h> and the full class
// definition which we don't want leaking into every TU that
// already includes RenderPass.h for the trySet helpers.
namespace ayt::render::detail { class ShadowPass; }

namespace ayt::render::detail
{

struct DrawIndexRange {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
};

// Resolve DrawItem's optional submesh slice against the uploaded mesh.
// A zero item.indexCount means "draw the complete mesh" for compatibility.
// Invalid/out-of-bounds slices resolve to an empty range and are skipped.
inline DrawIndexRange resolveDrawIndexRange(const DrawItem& item,
                                            const GpuMesh& mesh) noexcept
{
    if (mesh.indexCount == 0) {
        return {};
    }
    if (item.indexCount == 0) {
        return {0, mesh.indexCount};
    }
    if (item.firstIndex >= mesh.indexCount) {
        return {};
    }
    const uint32_t remaining = mesh.indexCount - item.firstIndex;
    return {item.firstIndex,
            item.indexCount < remaining ? item.indexCount : remaining};
}

inline bool bindDrawIndexBuffer(BGFXAdapter& adapter, const GpuMesh& mesh,
                                const DrawIndexRange& range,
                                bool wireframe) noexcept
{
    if (wireframe && BGFXAdapter::isValid(mesh.wireframeIndexBuffer)) {
        const WireframeIndexRange wireRange = resolveWireframeIndexRange(
            range.firstIndex, range.indexCount, mesh.wireframeIndexCount);
        if (wireRange.valid) {
            adapter.setIndexBuffer(mesh.wireframeIndexBuffer,
                                   wireRange.firstIndex,
                                   wireRange.indexCount);
            return true;
        }
    }
    adapter.setIndexBuffer(mesh.indexBuffer, range.firstIndex,
                           range.indexCount);
    return false;
}

// U1.5 — shared per-material uniform-upload helpers. ForwardOpaquePass
// and TransparentPass both need to upload MVP / cameraPos / lightDir /
// lightColor with the same lazy-resolve + fallback semantics. Keeping
// the bodies byte-for-byte identical in one header is cheaper than
// risking drift between two anonymous-namespace copies. Inline in the
// header so neither Pass owns the definition; the body is small (no
// cost to inline). Caller passes a ShaderResource by reference, so the
// lazy-resolve writes back into ShaderResource's internal binding
// cache (consistent with ForwardOpaquePass behavior since Phase 1).
//
// Why free fns (not static methods): anonymous-namespace free fns in
// the original ForwardOpaquePass.cpp would need to be exposed at
// namespace scope for TransparentPass to call them. Inline header
// free fns in `detail::` get us that visibility without forcing a
// non-inline definition in a .cpp.
inline void trySetUniformVec3(shader::ShaderResource& shader, const char* name, const float* values)
{
    const shader::BindingId binding = shader.getUniformBinding(name);
    if (binding == shader::InvalidBinding || values == nullptr) {
        return;
    }
    const float padded[4] = {values[0], values[1], values[2], 0.0f};
    shader.setUniform(binding, padded, sizeof(padded));
}

// §P3 M2 (2026-08-24) — explicit Vec4 helper. Mirrors trySetUniformVec3
// (no Vec4 padding when the shader already declares Vec4). Used by the
// per-light cameraPos / lightDirection / lightColor / viewportTexel
// upload blocks in LightingPass + SSAOPass + PostProcessPass — those
// wrote byte-for-byte identical `if (binding != Invalid) { padded[4] =
// {...}; setUniform(...) }` blocks four times in a row. Hoisted so a
// future scalar/vec2 helper or uniform-format change lands in one
// place instead of N.
inline void trySetUniformVec4(shader::ShaderResource& shader, const char* name, const float* values)
{
    const shader::BindingId binding = shader.getUniformBinding(name);
    if (binding == shader::InvalidBinding || values == nullptr) {
        return;
    }
    shader.setUniform(binding, values, sizeof(float) * 4);
}

// Fullscreen passes cache one immutable VB/IB pair. Creation must be
// transactional: if either allocation fails, retaining or overwriting the
// other handle leaks a GPU resource on every retry. Clear any partial cached
// pair first, build into locals, and publish only after both allocations
// succeed.
inline bool ensureFullscreenTriangleBuffers(
    BGFXAdapter& adapter,
    bgfx::VertexBufferHandle& vertexBuffer,
    bgfx::IndexBufferHandle& indexBuffer,
    const void* vertexData,
    uint32_t vertexDataSize,
    const bgfx::VertexLayout& layout,
    const void* indexData,
    uint32_t indexDataSize)
{
    if (BGFXAdapter::isValid(vertexBuffer)
        && BGFXAdapter::isValid(indexBuffer)) {
        return true;
    }

    if (BGFXAdapter::isValid(vertexBuffer)) {
        adapter.destroy(vertexBuffer);
        vertexBuffer = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(indexBuffer)) {
        adapter.destroy(indexBuffer);
        indexBuffer = BGFX_INVALID_HANDLE;
    }

    const bgfx::VertexBufferHandle newVertexBuffer =
        adapter.createVertexBuffer(vertexData, vertexDataSize, layout,
                                   BGFX_BUFFER_NONE);
    if (!BGFXAdapter::isValid(newVertexBuffer)) {
        return false;
    }

    const bgfx::IndexBufferHandle newIndexBuffer =
        adapter.createIndexBuffer(indexData, indexDataSize, BGFX_BUFFER_NONE);
    if (!BGFXAdapter::isValid(newIndexBuffer)) {
        adapter.destroy(newVertexBuffer);
        return false;
    }

    vertexBuffer = newVertexBuffer;
    indexBuffer = newIndexBuffer;
    return true;
}

inline void tryBindWhiteTexture(shader::ShaderResource& shader,
                                BGFXAdapter& adapter,
                                const char* name,
                                bool alreadyBound)
{
    if (alreadyBound || name == nullptr) {
        return;
    }
    const shader::BindingId binding = shader.getTextureBinding(name);
    if (binding == shader::InvalidBinding) {
        return;
    }
    const bgfx::TextureHandle white = adapter.getWhiteFallbackTexture();
    if (!BGFXAdapter::isValid(white)) {
        return;
    }
    shader.setTexture(shader.getTextureStage(binding), binding,
                      toShaderTexture(white));
}

inline void tryBindFlatNormalTexture(shader::ShaderResource& shader,
                                     BGFXAdapter& adapter,
                                     const char* name,
                                     bool alreadyBound)
{
    if (alreadyBound || name == nullptr) {
        return;
    }
    const shader::BindingId binding = shader.getTextureBinding(name);
    if (binding == shader::InvalidBinding) {
        return;
    }
    const bgfx::TextureHandle flat = adapter.getFlatNormalFallbackTexture();
    if (!BGFXAdapter::isValid(flat)) {
        return;
    }
    shader.setTexture(shader.getTextureStage(binding), binding,
                      toShaderTexture(flat));
}

// Read one scalar from the renderer's padded Vec4 material-uniform cache.
// Pass-specific shaders (GBuffer/Shadow) do not submit the material's own
// ShaderResource, so semantic properties must be bridged explicitly.
inline float materialUniformScalar(const GpuMaterial& material,
                                   std::string_view name,
                                   float fallback) noexcept
{
    for (const GpuMaterial::UniformSlot& slot : material.uniformSlots) {
        if (slot.name == name && slot.size >= sizeof(float)) {
            float value = fallback;
            std::memcpy(&value, slot.data, sizeof(value));
            return value;
        }
    }
    return fallback;
}

inline void trySetUniformMat4(shader::ShaderResource& shader, const char* primaryName,
                              const char* fallbackName, const ayt::math::Float4x4& matrix)
{
    shader::BindingId binding = shader.getUniformBinding(primaryName);
    if (binding == shader::InvalidBinding && fallbackName != nullptr) {
        binding = shader.getUniformBinding(fallbackName);
    }
    if (binding == shader::InvalidBinding) {
        return;
    }
    float colMajor[16];
    toBgfxColumnMajor(matrix, colMajor);
    shader.setUniform(binding, colMajor, sizeof(colMajor));
}

// PR-F2 / Phase 5 — bind shadowMap + upload light-space VP for receivers.
// See AYRenderer/ShadowReceiverContract.h for the material contract.
//
// `flags` gates Receive: Cast-only / None items get the lit fallback so
// meshes without Receive stay fully lit even if the shader declares shadowMap.
//
// `shadowPass == nullptr` or missing FBO/sample → lit fallback (safe).
// Materials without `shadowMap` → no-op (pre-shadow shaders unchanged).
//
// P4.2 (2026-07-22) — `bias` parameter replaces the previous
// hard-coded Contract::kDefaultBias. Pass the FrameContext::shadowBias
// from the call site so a host Renderer::setShadowBias() call takes
// effect for every receiver. Default 0.003f matches the Phoskia
// receiver property default (AYRenderer/ShadowShaderSources.h:81 + ShadowSettings
// ::kBiasDefault). Existing call sites pass frame.shadowBias
// explicitly.
void tryBindShadowSampler(shader::ShaderResource& shader,
                          BGFXAdapter& adapter,
                          const ShadowPass* shadowPass,
                          ShadowFlags flags = kShadowCastAndReceive,
                          float bias = 0.003f);

// PR-F3 (2026-07-21) — bone-palette upload shared by
// ForwardOpaquePass's draw loop AND ShadowPass's caster draw loop.
//
// Lifted from ForwardOpaquePass::execute's inline block (now byte-
// for-byte identical to that block's CPU behavior — only call site
// moved). The pass-shader may be the SkinnedLit (forward) or the
// depth-only Caster — both link a `Skeleton` UBO so the same bytes
// work.
//
// `castSkinned` arg is the uniform toggle for programs that
// distinguish static / skinned VS segments (the depth caster does
// via `property castSkinned`). ForwardOpaquePass passes the value
// it inherits from the GpuMaterial's lazily-resolved uniform (kept
// = 0 there — SkinnedLit's VS is unconditional); ShadowPass passes
// `item.boneMatrices != nullptr ? 1 : 0` so the caster program
// selects the right VS segment.
//
// `skeletonBinding` may be `shader::InvalidBinding` for non-
// skeletal programs — the helper no-ops cleanly (matching the
// pre-F3 "missing binding → log 3x, skip upload" behavior). When
// `castSkinned` arg selects is also nullified when
// `castSkinned == 0` to avoid a redundant uniform write.
//
// Stack-path for <= 16 draw-local joints. Larger palettes use a heap
// scratch buffer up to the current renderer capability. The complete
// skeleton may be larger and is addressed through DrawItem::boneRemap.
//
// Pure: no other state touched beyond `shader.setUniformBlock` /
// `shader.setUniform`.
void tryUploadBonePalette(shader::ShaderResource& shader,
                          shader::BindingId skeletonBinding,
                          shader::BindingId castSkinnedBinding,
                          uint8_t castSkinnedValue,
                          const DrawItem& item);

// §P2 supplemental (2026-08-24) — upload the per-frame light / camera
// uniforms (cameraPos, lightDir, lightDirection, lightColor) that both
// ForwardOpaquePass::flushMaterial and TransparentPass::submitItem were
// writing byte-for-byte. Hoisted to keep the two passes from drifting.
// All four writes are best-effort — the shader may not declare any of
// the names, in which case trySetUniformVec3 no-ops.
inline void tryUploadLightUniforms(shader::ShaderResource& shader,
                                   const FrameContext& frame)
{
    trySetUniformVec3(shader, "cameraPos", frame.cameraPosition.ptr());
    const ayt::math::FVector3 toLight(
        -frame.lightDirection.x, -frame.lightDirection.y, -frame.lightDirection.z);
    const ayt::math::FVector3 toLightDir = toLight.normalize();
    trySetUniformVec3(shader, "lightDir", toLightDir.ptr());
    trySetUniformVec3(shader, "lightDirection", toLightDir.ptr());
    trySetUniformVec3(shader, "lightColor", frame.lightColor.ptr());
}

// §P3 L11 (2026-08-24) — rate-limited early-return diagnostic.
// Replaces the previous pattern of "loud log on every frame when
// a path is unready" that drowned the host's stderr on long
// captures. Uses a per-(pass,reason) counter; the first frame is
// always loud, subsequent frames are suppressed until 256 have
// elapsed (matches the console-noise ceiling of 1 line / ~4s at
// 60fps). The pass name + reason are baked into the message so
// the host can grep / pinpoint without consulting a code map.
class RateLimitedLogTable {
public:
    bool shouldEmit(const char* passName,
                    const char* reason,
                    uint32_t interval = 256u) noexcept
    {
        const uint64_t key = makeKey(passName, reason);
        Entry* freeEntry = nullptr;
        for (Entry& entry : _entries) {
            if (entry.used && entry.key == key) {
                const bool emit = entry.count == 0u
                    || (interval != 0u && (entry.count % interval) == 0u);
                ++entry.count;
                return emit;
            }
            if (!entry.used && freeEntry == nullptr) {
                freeEntry = &entry;
            }
        }

        if (freeEntry != nullptr) {
            freeEntry->used = true;
            freeEntry->key = key;
            freeEntry->count = 1u;
            return true;
        }

        // The renderer currently has fewer than 64 static diagnostic keys.
        // If that invariant is exceeded, stay loud instead of hiding a new
        // failure behind an exhausted diagnostics table.
        return true;
    }

private:
    struct Entry {
        uint64_t key = 0;
        uint32_t count = 0;
        bool used = false;
    };

    static uint64_t hashText(const char* text) noexcept
    {
        constexpr uint64_t kOffset = 1469598103934665603ull;
        constexpr uint64_t kPrime = 1099511628211ull;
        uint64_t hash = kOffset;
        if (text == nullptr) {
            return hash;
        }
        while (*text != '\0') {
            hash ^= static_cast<unsigned char>(*text++);
            hash *= kPrime;
        }
        return hash;
    }

    static uint64_t makeKey(const char* passName, const char* reason) noexcept
    {
        const uint64_t passHash = hashText(passName);
        const uint64_t reasonHash = hashText(reason);
        return passHash ^ (reasonHash + 0x9e3779b97f4a7c15ull
                           + (passHash << 6u) + (passHash >> 2u));
    }

    std::array<Entry, 64> _entries{};
};

inline void rateLimitedEarlyReturn(const char* passName, const char* reason)
{
    static thread_local RateLimitedLogTable s_table;
    if (s_table.shouldEmit(passName, reason)) {
        std::fprintf(stderr,
                     "[%s] early-return (rate-limited) reason=%s\n",
                     passName != nullptr ? passName : "<null>",
                     reason != nullptr ? reason : "<null>");
    }
}

// U0 (Phase 2 Pass scaffold) — abstract base for one rendering pass.
// One subclass = one logical draw on one bgfx view. The pipeline
// (implemented in U1+ at detail/RenderPipeline.{h,cpp}) calls
// execute() in registration order; today the single call site is
// Renderer::render via RenderPipeline::executeAll.
//
// Design ref: `design.md:431-471` (RenderPass class + RenderPipeline
// + kFullPipelineOrder default table).
//
// Why a base class NOW: design.md promises polymorphic passes but
// live code has zero of them — ForwardOpaquePass is a free-standing
// concrete class with a single execute() called directly by
// Renderer::render. Adding Transparent / PostProcess / Shadow / UIPass
// today means duplicating dispatch wiring in Renderer::render for each
// one. A 55-line abstract base eliminates that; every subclass then
// fits into one registry vector (deferred to RenderPipeline in U1+).
//
// Non-goals in U0:
//   - No RenderPipeline / PassManager / FrameGraph (U1+).
//   - No PipelineState / RenderTargetHandle abstraction (bgfx owns RTs;
//     public headers must not include <bgfx/bgfx.h>; see Test_PublicHeaderSurface).
//   - Subclasses own their viewId + viewportRect internally via the
//     last two execute() args — no automatic sub-rect assignment.
//   - No `addToRegistry()` API on the base; that lives in U1+.
//
// Threading: execute() is called from the render thread (currently
// main thread; multiplayer deferred to Phase 5).
class RenderPass {
public:
    virtual ~RenderPass() = default;

    // Human-readable name for logs / debug overlay. Empty allowed.
    // Implementations return a static-lifetime string literal (no alloc).
    virtual std::string_view name() const = 0;

    // Render this pass into the bgfx view + viewport described by
    // `ctx`. Returns draw-call count recorded by the implementation
    // (used to update RenderFrameStats).
    //
    // The 12-arg signature collapsed into `detail::PassExecContext` in
    // PR-C (P1, 2026-07-20). See PassExecContext.h for what each field
    // is and why. `frame` stays const per docs/execution-plan.md §5.3.
    //
    // `materials` inside the context is non-const because some passes
    // (ForwardOpaquePass) lazily resolve BindingIds on first use and
    // cache them in GpuMaterial::colorBinding/etc. (See
    // ForwardOpaquePass::flushMaterial.)
    virtual uint32_t execute(PassExecContext& ctx) = 0;

    // Per-pass enable / disable (used by debug overlay toggle /
    // pipeline hot-reload in U1+). Default = enabled. Subclasses
    // inherit this state and never need to override.
    void setEnabled(bool enabled) { _enabled = enabled; }
    bool isEnabled() const { return _enabled; }

protected:
    // U1++ — shared color-uniform upload step. ForwardOpaquePass +
    // TransparentPass both need to (1) lazily resolve colorBinding
    // (baseColor -> color fallback), (2) re-validate the cached
    // binding each frame (hot-reload safety net), and (3) write
    // the override value or the neutral white default. Identity =
    // byte-for-byte between the two passes; lifted here so they
    // cannot diverge. Pure helper, no I/O beyond setUniform. Caller
    // must guard `material.shader.isValid()` first (both existing
    // call sites do).
    static void resolveAndApplyColorUniforms(GpuMaterial& material);

    bool _enabled = true;
};

} // namespace ayt::render::detail
