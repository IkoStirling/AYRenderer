#include "detail/ForwardOpaquePass.h"

#include "detail/FrameContext.h"
#include "detail/ShadowPass.h"

#include <cstdio>

namespace ayt::render::detail
{

void ForwardOpaquePass::flushMaterial(GpuMaterial& material,
                                      const std::unordered_map<uint64_t, GpuTexture>& textures,
                                      const FrameContext& frame,
                                      const ayt::math::Float4x4& world,
                                      BGFXAdapter& adapter,
                                      const ShadowPass* shadowPass,
                                      ShadowFlags shadowFlags)
{
    (void)world;
    if (!material.shader.isValid()) {
        return;
    }

    // Phase 1 RD-04: lazy-resolve the `Skeleton` UBO binding for
    // skinned materials. ForwardOpaquePass::execute uses this to
    // upload bone matrices per-draw.
    if (material.boneBlockBinding == shader::InvalidBinding) {
        material.boneBlockBinding =
            material.shader.getUniformBlockBinding("Skeleton");
    }

    // Do NOT upload u_modelViewProj manually — Phoskia maps
    // modelViewProjection → bgfx builtin filled by setViewTransform+setTransform.
    // A second upload was a regression risk (bad MVP → silhouette-only / black).
    trySetUniformVec3(material.shader, "cameraPos", frame.cameraPosition.ptr());

    const ayt::math::FVector3 toLight(
        -frame.lightDirection.x, -frame.lightDirection.y, -frame.lightDirection.z);
    const ayt::math::FVector3 toLightDir = toLight.normalize();
    trySetUniformVec3(material.shader, "lightDir", toLightDir.ptr());
    trySetUniformVec3(material.shader, "lightDirection", toLightDir.ptr());
    trySetUniformVec3(material.shader, "lightColor", frame.lightColor.ptr());
    {
        static uint32_t s_lightLog = 0;
        if (s_lightLog < 2) {
            std::fprintf(stderr,
                         "[FOLight] dir=(%.2f,%.2f,%.2f) color=(%.2f,%.2f,%.2f)\n",
                         toLightDir.x, toLightDir.y, toLightDir.z,
                         frame.lightColor.x, frame.lightColor.y, frame.lightColor.z);
            ++s_lightLog;
        }
    }
    // Bind material albedo FIRST, then shadow — and log stages once so a
    // unit collision (both on 0 ⇒ R32F depth as "albedo" ⇒ gray) is obvious.
    uint32_t albedoBinds = 0;
    for (const GpuMaterial::TextureSlot& slot : material.textures) {
        if (slot.name.empty() || !slot.texture.isValid()) {
            continue;
        }
        if (slot.name == "shadowMap") {
            continue;
        }
        const shader::BindingId binding = material.shader.getTextureBinding(slot.name);
        if (binding == shader::InvalidBinding) {
            continue;
        }
        const auto texIt = textures.find(slot.texture.id);
        if (texIt == textures.end() || !BGFXAdapter::isValid(texIt->second.handle)) {
            continue;
        }
        const uint8_t stage = material.shader.getTextureStage(binding);
        material.shader.setTexture(stage, binding, toShaderTexture(texIt->second.handle));
        ++albedoBinds;
        static uint32_t s_albedoLog = 0;
        if (s_albedoLog < 4) {
            std::fprintf(stderr,
                         "[AlbedoBind] name=%s stage=%u tex.idx=%u "
                         "colorOverride=%d base=(%.2f,%.2f,%.2f)\n",
                         slot.name.c_str(),
                         static_cast<unsigned>(stage),
                         static_cast<unsigned>(texIt->second.handle.idx),
                         material.hasColorOverride ? 1 : 0,
                         material.colorOverride.x,
                         material.colorOverride.y,
                         material.colorOverride.z);
            ++s_albedoLog;
        }
    }
    {
        const shader::BindingId shadowBinding =
            material.shader.getTextureBinding("shadowMap");
        static uint32_t s_stageLog = 0;
        if (s_stageLog < 2 && shadowBinding != shader::InvalidBinding) {
            std::fprintf(stderr,
                         "[TexStages] albedoBinds=%u albedoStage=%u shadowStage=%u\n",
                         albedoBinds,
                         static_cast<unsigned>(
                             material.shader.getTextureStage(
                                 material.shader.getTextureBinding("albedoMap"))),
                         static_cast<unsigned>(
                             material.shader.getTextureStage(shadowBinding)));
            ++s_stageLog;
        }
    }
    tryBindShadowSampler(material.shader, adapter, shadowPass,
                          shadowFlags, frame.shadowBias);

    // U1++ — color-uniform upload lifted to RenderPass helper; see
    // RenderPass.cpp::resolveAndApplyColorUniforms. Identical bytes
    // to the prior inline body; TransparentPass uses the same helper.
    RenderPass::resolveAndApplyColorUniforms(material);
    if (material.mat4Binding != shader::InvalidBinding && material.hasMat4Override) {
        if (material.shader.hasUniformBinding(material.mat4Binding)) {
            float colMajor[16];
            toBgfxColumnMajor(material.mat4Override, colMajor);
            material.shader.setUniform(material.mat4Binding, colMajor, sizeof(colMajor));
        } else {
            material.mat4Binding = shader::InvalidBinding;
        }
    }

    for (const GpuMaterial::UniformSlot& slot : material.uniformSlots) {
        if (slot.name.empty() || slot.size == 0) {
            continue;
        }
        const shader::BindingId binding = material.shader.getUniformBinding(slot.name);
        if (binding == shader::InvalidBinding) {
            continue;
        }
        material.shader.setUniform(binding, slot.data, slot.size);
    }
}

uint32_t ForwardOpaquePass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    const FrameContext& frame = ctx.frame;
    const uint8_t viewId = ctx.viewId;
    const auto& meshes    = ctx.meshes;
    const auto& textures  = ctx.textures;
    auto& materials       = ctx.materials;
    const uint16_t viewportX      = ctx.viewportX;
    const uint16_t viewportY      = ctx.viewportY;
    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    const RenderScene& scene = ctx.scene;

    // §5.4 (2026-07-22) — `isInitialized()` guard fixes the
    // pre-existing landmine where FO went straight into
    // `adapter.setViewTransform(viewId, frame.view, frame.projection)`
    // on an uninitialized adapter — bgfx::setViewTransform is a raw
    // C-API surface that dereferences internal bgfx state, UB on
    // uninit on most backends. Mirror ShadowPass.cpp:24-26,
    // GBufferPass.cpp:127, LightingPass.cpp:24.
    //
    // NOTE: We deliberately do NOT add an `isNoopBackend()` check
    // here. The Noop backend already short-circuits internally in
    // `BGFXAdapter::setViewTransform` (and other draw commands) —
    // it's safe to reach the scene-items loop on Noop, the loop's
    // `++drawCount` is the correct behavior tested by 7+ existing
    // cases (`debug_overlay_reports_draw_count`, `forward_opaque_draw_one_frame`,
    // `opaque_alpha_split_counts`, etc.) which count "logical draw
    // submissions" regardless of GPU outcome. Those tests were
    // implicitly relying on Noop NOT short-circuiting at the Pass
    // level. Adding the gate would silently break them. The
    // `isInitialized()` check alone fixes the actual UB without
    // disturbing the test semantics.
    if (!adapter.isInitialized()) {
        return 0;
    }

    adapter.setViewTransform(viewId, frame.view, frame.projection);

    // P2 (PR-D, 2026-07-20) — bind the shared scene FBO so this pass's
    // depth+color output is captured for PostProcessPass to sample
    // (and for any future GBufferPass / LightingPass that reads the
    // offscreen depth). BGFX_INVALID_HANDLE ⇒ fall back to the
    // default backbuffer (matches pre-PR-D behavior on headless
    // test paths / SceneRT-off hosts).
    //
    // View rect: the scene FBO is sized exactly to (viewportW ×
    // viewportH). When bound, use origin (0,0). The editor panel
    // offset (viewportX, viewportY) only applies when drawing into
    // the full-window backbuffer hole — using the offset against an
    // FBO of panel size clips the scene to black / shifts it.
    adapter.setViewFrameBuffer(viewId, ctx.sceneFbo);
    if (BGFXAdapter::isValid(ctx.sceneFbo)) {
        adapter.setViewRect(viewId, 0, 0, viewportWidth, viewportHeight);
        // Scene FBO is offscreen — clear it each frame (backbuffer clear
        // on view 0 does not touch this target).
        adapter.setViewClearRaw(viewId,
                                BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                                /*rgba=*/0x191a1cff,
                                /*depth=*/1.0f,
                                /*stencil=*/0);
    } else {
        // Composite Game View hole: clear this view's rect so the panel
        // never inherits a stale/white backbuffer after UI hides the
        // placeholder Image. Matches RendererSubSystem composite clear.
        adapter.setViewRect(viewId, viewportX, viewportY, viewportWidth, viewportHeight);
        adapter.setViewClearRaw(viewId,
                                BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                                /*rgba=*/0x191a1cff,
                                /*depth=*/1.0f,
                                /*stencil=*/0);
    }

    // P6.5 (2026-07-22) — preset state combination replaces the
    // pre-P6.5 inline `BGFX_STATE_WRITE_RGB | WRITE_A | WRITE_Z |
    // DEPTH_TEST_LESS | CULL_CW`. Bit combination identical; called
    // once per execute() because BGFXAdapter::setStateOpaque resets
    // the cached state on each pass.
    adapter.setStateOpaque();

    uint32_t drawCount = 0;

    for (const DrawItem& item : scene.items()) {
        if (item.outlineHull) {
            continue;
        }
        if (!item.mesh.isValid() || !item.material.isValid()) {
            continue;
        }

        const auto meshIt = meshes.find(item.mesh.id);
        const auto matIt  = materials.find(item.material.id);
        if (meshIt == meshes.end() || matIt == materials.end()) {
            continue;
        }

        const GpuMesh& mesh = meshIt->second;
        if (!BGFXAdapter::isValid(mesh.vertexBuffer)
            || !BGFXAdapter::isValid(mesh.indexBuffer)) {
            continue;
        }

        GpuMaterial& material = matIt->second;
        if (!material.shader.isValid()) {
            continue;
        }

        // P0.4 (2026-07-20) — skip BlendMode::Alpha. TransparentPass
        // already draws Alpha materials (it gates on
        // `material.blendMode == BlendMode::Alpha`). Without this
        // skip, Alpha items are submitted TWICE per frame:
        //   1) ForwardOpaquePass — WRITE_RGB|WRITE_A|WRITE_Z,
        //      so the alpha pixels are written to the depth buffer
        //      AND the color buffer. They overwrite anything the
        //      opaque pass laid down behind them and the alpha z is
        //      then tested by every later transparent draw, blocking
        //      legit back-to-front compositing on the GPU.
        //   2) TransparentPass — BLEND_ALPHA, no WRITE_Z, re-uses
        //      the opaque z-buffer for occlusion. The duplicate draw
        //      produces visible double-write (z-fighting + wrong color)
        //      on real GPU backends and pinches throughput on all.
        // ForwardOpaquePass owns Opaque only; the pass name is the
        // contract. See docs/execution-plan.md §1.2 + §P0.4.
        if (material.blendMode == ayt::render::BlendMode::Alpha) {
            continue;
        }

        adapter.setTransform(item.world);
        adapter.setVertexBuffer(mesh.vertexBuffer);
        adapter.setIndexBuffer(mesh.indexBuffer, 0, mesh.indexCount);

        // PR-F2 (2026-07-21) — ctx.shadowPass feeds flushMaterial. When
        // the active shadow producer has a ready FBO, the helper
        // uploads `u_lightViewProj` and binds `shadowMap`; otherwise
        // the shader binding misses are no-ops.
        flushMaterial(material, textures, frame, item.world, adapter,
                      ctx.shadowPass, item.shadowFlags);

        static uint32_t s_foDrawLog = 0;
        if (s_foDrawLog < 4) {
            const float* m = item.world.ptr();
            const shader::BindingId shadowBinding =
                material.shader.getTextureBinding("shadowMap");
            const shader::BindingId lvpBinding =
                material.shader.getUniformBinding("u_lightViewProj");
            std::fprintf(stderr,
                         "[ShadowDbg] FO draw#%u worldT=(%.2f,%.2f,%.2f) "
                         "shadowPass=%p hasShadowMap=%d hasLvp=%d flags=0x%02x\n",
                         s_foDrawLog,
                         m[3], m[7], m[11],
                         static_cast<const void*>(ctx.shadowPass),
                         shadowBinding != shader::InvalidBinding ? 1 : 0,
                         lvpBinding != shader::InvalidBinding ? 1 : 0,
                         static_cast<unsigned>(item.shadowFlags));
            ++s_foDrawLog;
        }

        // PR-F3 (2026-07-21) — bone-palette upload lifted to the
        // shared helper. The helper is byte-for-byte identical to
        // the prior inline block; the Skeleton UBO binding comes
        // from the cached `material.boneBlockBinding` (lazy-
        // resolved in flushMaterial). The SkinnedLit's
        // `castSkinned` binding is Invalid here (no such property
        // in skinned_lit.phoskia) so the helper silently skips the
        // uniform write.
        if (material.shader.isValid()) {
            tryUploadBonePalette(material.shader,
                                 material.boneBlockBinding,
                                 /*castSkinnedBinding=*/shader::InvalidBinding,
                                 /*castSkinnedValue=*/0u,
                                 item);
        }

        shader::DrawCallContext ctx;
        ctx.viewId = viewId;
        ctx.state  = 0;  // P6.5: per-draw state owned by Adapter; shader.submit
                          // only writes viewId (state==0 means "use Adapter-
                          // set state"). This matches the design.md §2.5
                          // 'Adapter sets state, submit only owns viewId'.
        material.shader.submit(ctx);
        ++drawCount;
    }

    return drawCount;
}

} // namespace ayt::render::detail
