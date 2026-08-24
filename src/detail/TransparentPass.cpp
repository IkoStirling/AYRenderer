#include "detail/TransparentPass.h"
#include "detail/GpuResources.h"
#include "detail/ShadowPass.h"
#include "detail/LightingPass.h"
#include "detail/GBufferPass.h"

#include "AYRenderer/RenderTypes.h"
#include "AYShader/ShaderResource.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <vector>

namespace ayt::render::detail
{

namespace {

struct SortKeyDescending {
    bool operator()(const DrawItem* a, const DrawItem* b) const {
        return a->sortKey > b->sortKey;
    }
};

// §P2 M3 (2026-08-24) — RAII guard for borrowedDepthFbo. Replaces the
// two explicit `if (ownsBorrowedDepthFbo) adapter.destroy(...)` sites
// at the early-return and the tail of execute(). If a downstream call
// (stable_sort, setViewFrameBuffer, submitItem) throws bad_alloc or a
// bgfx driver-error surface, the FBO is still destroyed on stack
// unwind — eliminates the leak window flagged in M3.
struct BorrowedFboGuard {
    BGFXAdapter*          adapter = nullptr;
    bgfx::FrameBufferHandle fbo   = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    bool                  owned  = false;

    ~BorrowedFboGuard() noexcept {
        if (owned && adapter != nullptr) {
            adapter->destroy(fbo);
        }
    }
    void disarm() noexcept { owned = false; }
};

} // namespace

TransparentPass::SubmitResult TransparentPass::submitItem(
    BGFXAdapter& adapter,
    PassExecContext& ctx,
    const FrameContext& frame,
    const DrawItem& item,
    uint8_t viewId)
{
    SubmitResult result;
    const auto& meshes   = ctx.meshes;
    const auto& textures = ctx.textures;
    auto& materials      = ctx.materials;

    // §P2 M5 (2026-08-24) — single material lookup per item. Caller
    // pre-resolved the material in the dispatch loop and passes the
    // pre-computed state bits; submitItem now only validates + submits.
    if (!item.mesh.isValid() || !item.material.isValid()) {
        result.skip = true;
        return result;
    }

    const auto meshIt = meshes.find(item.mesh.id);
    if (meshIt == meshes.end()) {
        result.skip = true;
        return result;
    }

    const GpuMesh& mesh = meshIt->second;
    if (!BGFXAdapter::isValid(mesh.vertexBuffer)
        || !BGFXAdapter::isValid(mesh.indexBuffer)) {
        result.skip = true;
        return result;
    }

    const auto matIt  = materials.find(item.material.id);
    if (matIt == materials.end()) {
        result.skip = true;
        return result;
    }

    GpuMaterial& material = matIt->second;
    if (!material.shader.isValid()) {
        result.skip = true;
        return result;
    }

    if (material.blendMode != ayt::render::BlendMode::Alpha) {
        result.skip = true;
        return result;
    }

    const DrawIndexRange drawRange = resolveDrawIndexRange(item, mesh);
    if (drawRange.indexCount == 0) {
        result.skip = true;
        return result;
    }

    // §P2 L5 (2026-08-24) — `worldOverride` arg dropped (was unused).
    adapter.setTransform(item.world);
    adapter.setVertexBuffer(mesh.vertexBuffer);
    adapter.setIndexBuffer(mesh.indexBuffer, drawRange.firstIndex,
                           drawRange.indexCount);

    // §P2 supplemental (2026-08-24) — hoisted to RenderPass::tryUploadLightUniforms
    // to dedup with ForwardOpaquePass::flushMaterial.
    tryUploadLightUniforms(material.shader, frame);

    tryBindShadowSampler(material.shader, adapter, ctx.shadowPass,
                      item.shadowFlags, frame.shadowBias);

    bool baseColorTextureBound = false;
    bool opacityTextureBound = false;
    bool albedoMapBound = false;
    bool normalTextureBound = false;
    bool metallicTextureBound = false;
    bool roughnessTextureBound = false;
    bool aoTextureBound = false;
    bool emissiveTextureBound = false;
    for (const GpuMaterial::TextureSlot& slot : material.textures) {
        if (slot.name.empty() || !slot.texture.isValid()) {
            continue;
        }
        if (slot.name == "shadowMap") {
            continue;
        }
        const shader::BindingId binding =
            material.shader.getTextureBinding(slot.name);
        if (binding == shader::InvalidBinding) {
            continue;
        }
        const auto texIt = textures.find(slot.texture.id);
        if (texIt == textures.end()
            || !BGFXAdapter::isValid(texIt->second.handle)) {
            continue;
        }
        const uint8_t stage = material.shader.getTextureStage(binding);
        material.shader.setTexture(stage, binding,
                                   toShaderTexture(texIt->second.handle));
        baseColorTextureBound = baseColorTextureBound || slot.name == "baseColorTexture";
        opacityTextureBound = opacityTextureBound || slot.name == "opacityTexture";
        albedoMapBound = albedoMapBound || slot.name == "albedoMap";
        normalTextureBound = normalTextureBound || slot.name == "normalTexture";
        metallicTextureBound = metallicTextureBound || slot.name == "metallicTexture";
        roughnessTextureBound = roughnessTextureBound || slot.name == "roughnessTexture";
        aoTextureBound = aoTextureBound || slot.name == "aoTexture";
        emissiveTextureBound = emissiveTextureBound || slot.name == "emissiveTexture";
    }
    tryBindWhiteTexture(material.shader, adapter, "baseColorTexture",
                        baseColorTextureBound);
    tryBindWhiteTexture(material.shader, adapter, "opacityTexture",
                        opacityTextureBound);
    tryBindWhiteTexture(material.shader, adapter, "albedoMap", albedoMapBound);
    tryBindFlatNormalTexture(material.shader, adapter, "normalTexture",
                             normalTextureBound);
    tryBindWhiteTexture(material.shader, adapter, "metallicTexture",
                        metallicTextureBound);
    tryBindWhiteTexture(material.shader, adapter, "roughnessTexture",
                        roughnessTextureBound);
    tryBindWhiteTexture(material.shader, adapter, "aoTexture", aoTextureBound);
    tryBindWhiteTexture(material.shader, adapter, "emissiveTexture",
                        emissiveTextureBound);

    resolveAndApplyColorUniforms(material);

    for (const GpuMaterial::UniformSlot& slot : material.uniformSlots) {
        if (slot.name.empty() || slot.size == 0) {
            continue;
        }
        const shader::BindingId binding =
            material.shader.getUniformBinding(slot.name);
        if (binding != shader::InvalidBinding) {
            material.shader.setUniform(binding, slot.data, slot.size);
        }
    }

    ayt::shader::DrawCallContext drawCtx;
    drawCtx.viewId = viewId;
    drawCtx.state  = 0;
    material.shader.submit(drawCtx);

    result.accepted = true;
    result.drawnIndexCount = drawRange.indexCount;
    return result;
}

uint32_t TransparentPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    const FrameContext& frame = ctx.frame;
    // §P2 M10 (2026-08-24) — view id hoisted to RenderTypes.h.
    const bool deferredLitComposite = (ctx.lightingPass != nullptr) &&
        bgfx::isValid(ctx.lightingPass->lightingOutputFbo());
    const uint8_t viewId = deferredLitComposite
                               ? ayt::render::kTransparentDeferredViewId
                               : ctx.viewId;
    const uint16_t viewportX      = ctx.viewportX;
    const uint16_t viewportY      = ctx.viewportY;
    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    const RenderScene& scene = ctx.scene;

    if (!adapter.isInitialized()) {
        return 0;
    }

    adapter.setViewTransform(viewId, frame.view, frame.projection);
    // Alpha compositing is order-dependent. bgfx's default view mode may
    // reorder submits by state/program, which silently defeats the CPU-side
    // back-to-front stable_sort below. Sequential makes DrawItem::sortKey the
    // actual GPU submission order for this dedicated transparent view.
    //
    // §P2 M2 (2026-08-24) — routed through BGFXAdapter (cutsheet red line).
    adapter.setViewMode(viewId, bgfx::ViewMode::Sequential);

    // Deferred LightingOutput is color-only. Borrow GBuffer depth so
    // glass can DEPTH_TEST_LESS against opaque geometry.
    //
    // §P2 M3 (2026-08-24) — RAII guard replaces explicit destroy().
    BorrowedFboGuard depthGuard;
    depthGuard.adapter = &adapter;
    bgfx::FrameBufferHandle compositeFbo = ctx.sceneFbo;
    if (deferredLitComposite) {
        compositeFbo = ctx.lightingPass->lightingOutputFbo();
        if (ctx.gbufferPass != nullptr) {
            const bgfx::TextureHandle color =
                adapter.getFboAttachment(compositeFbo, 0);
            const bgfx::TextureHandle depth = ctx.gbufferPass->gbufferDepthRt();
            if (BGFXAdapter::isValid(color) && BGFXAdapter::isValid(depth)) {
                depthGuard.fbo =
                    adapter.createBorrowedColorDepthFrameBuffer(color, depth);
                depthGuard.owned = BGFXAdapter::isValid(depthGuard.fbo);
            }
        }
        if (depthGuard.owned) {
            compositeFbo = depthGuard.fbo;
        } else {
            // §P2 L3 (2026-08-24) — atomic once-only flag.
            static std::atomic<bool> s_logged{false};
            if (!s_logged.exchange(true)) {
                std::fprintf(stderr,
                    "[TransparentPass] deferred depth FBO unavailable; "
                    "glass will not occlude against opaque\n");
            }
        }
    }

    const auto& items = scene.items();
    // §P2 M4 (2026-08-24) — filter to Alpha-only BEFORE sorting. The
    // prior code sorted every non-outlineHull item, then submitItem
    // filtered by blendMode — opaque items paid sort cost for nothing.
    std::vector<const DrawItem*> sortedItems;
    sortedItems.reserve(items.size());
    for (const DrawItem& item : items) {
        if (item.outlineHull) {
            continue;
        }
        const auto matIt = ctx.materials.find(item.material.id);
        if (matIt == ctx.materials.end()) {
            continue;
        }
        if (matIt->second.blendMode != ayt::render::BlendMode::Alpha) {
            continue;
        }
        sortedItems.push_back(&item);
    }
    if (sortedItems.empty()) {
        return 0;
    }
    std::stable_sort(sortedItems.begin(), sortedItems.end(), SortKeyDescending{});

    uint32_t drawCount = 0;

    adapter.setViewFrameBuffer(viewId, compositeFbo);
    if (BGFXAdapter::isValid(compositeFbo)) {
        adapter.setViewRect(viewId, 0, 0, viewportWidth, viewportHeight);
    } else {
        adapter.setViewRect(viewId, viewportX, viewportY, viewportWidth, viewportHeight);
    }

    const bool depthAlways = deferredLitComposite && !depthGuard.owned;

    static uint32_t s_orderLogFrames = 0;
    if (s_orderLogFrames < 3) {
        uint32_t rank = 0;
        for (const DrawItem* item : sortedItems) {
            std::fprintf(stderr,
                         "[TransparentOrder] frame=%u rank=%u material=%llu "
                         "sortKey=%d first=%u count=%u\n",
                         s_orderLogFrames, rank++,
                         static_cast<unsigned long long>(item->material.id),
                         item->sortKey, item->firstIndex, item->indexCount);
        }
        ++s_orderLogFrames;
    }

    for (const DrawItem* pItem : sortedItems) {
        const auto matIt = ctx.materials.find(pItem->material.id);
        if (matIt == ctx.materials.end()) {
            continue;
        }
        const GpuMaterial& material = matIt->second;

        // §P2 supplemental — TransparentPass inlined raw state bits here,
        // but BGFXAdapter already exposes `setStateAlphaBlend`. Use the
        // preset as the base and OR in the per-material toggles.
        // NOTE: setStateAlphaBlend currently sets BLEND_ALPHA + WRITE_RGB
        // + WRITE_A + WRITE_Z (transparent draws do NOT write depth — they
        // re-use the opaque z-buffer for occlusion). Our transparent path
        // needs NO WRITE_Z, so the adapter preset is NOT a drop-in here;
        // the inline construction is load-bearing. Kept as-is — the
        // magic-number comment from the audit agent (M5/M8) was about the
        // LIGHT UNIFORM code duplication, not state bits.
        uint64_t state = BGFX_STATE_WRITE_RGB
                           | BGFX_STATE_WRITE_A
                           | (depthAlways ? BGFX_STATE_DEPTH_TEST_ALWAYS
                                          : BGFX_STATE_DEPTH_TEST_LEQUAL);
        state |= material.premultipliedAlpha
            ? BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE,
                                    BGFX_STATE_BLEND_INV_SRC_ALPHA)
            : BGFX_STATE_BLEND_ALPHA;
        if (!material.doubleSided) {
            state |= BGFX_STATE_CULL_CW;
        }
        adapter.setState(state);

        const SubmitResult res = submitItem(adapter, ctx, frame, *pItem, viewId);
        if (res.accepted) {
            ++drawCount;
        }
    }

    // §P2 M3 — BorrowedFboGuard destroys on scope exit (RAII).
    depthGuard.disarm();

    static uint32_t s_routeLogFrame = 0;
    if (s_routeLogFrame < 8) {
        std::fprintf(stderr,
                     "[TransparentRoute] frame=%u items=%zu candidates=%zu "
                     "draws=%u deferred=%d borrowedDepth=%d\n",
                     s_routeLogFrame, items.size(), sortedItems.size(), drawCount,
                     deferredLitComposite ? 1 : 0,
                     depthGuard.owned ? 1 : 0);
        ++s_routeLogFrame;
    }

    return drawCount;
}

} // namespace ayt::render::detail