#include "detail/PostProcessPass.h"

namespace ayt::render::detail
{

uint32_t PostProcessPass::execute(
    BGFXAdapter& /*adapter*/,
    shader::ShaderResourcePool& /*pool*/,
    const RenderScene& /*scene*/,
    const std::unordered_map<uint64_t, GpuMesh>& /*meshes*/,
    const std::unordered_map<uint64_t, GpuTexture>& /*textures*/,
    std::unordered_map<uint64_t, GpuMaterial>& /*materials*/,
    uint16_t /*viewportX*/, uint16_t /*viewportY*/,
    uint16_t /*viewportWidth*/, uint16_t /*viewportHeight*/,
    const FrameContext& /*frame*/,
    uint8_t /*viewId*/)
{
    // P0 — intentional no-op. The pass IS dispatched (RenderPipeline
    // visits it in registration order between Transparent and UI),
    // and isEnabled() honors the host toggle, but we don't draw.
    //
    // Rationale: see PostProcessPass.h P0 doc comment. R5+ deferred
    // work to materialize here:
    //
    //   1) Acquire/create the R5+ FBO on BGFXAdapter (currently no
    //      framebuffer abstraction exists). The FBO size must track
    //      viewportWidth/Height (or the screen resolution when no
    //      viewport panel is set — same rule UIPass uses).
    //   2) Set the R5+ FBO as the draw target (bgfx::setViewFrameBuffer
    //      on viewId). Submit the ForwardOpaquePass depth as input
    //      via a sampler on the fullscreen triangle's Phoskia
    //      fragment. Today's ForwardOpaque writes only to the default
    //      backbuffer so the readback requires capturing the scene
    //      color into a BGFX_TEXTURE_BLIT_DST attachment first.
    //   3) Upload per-frame uniforms from FrameContext (extend
    //      FrameContext with `timeSeconds`, `bloomStrength`,
    //      `exposure` before R5+ lands; today only the 5 fields
    //      below exist).
    //   4) setTransform(identity) + setVertexBuffer(fullscreenVB) +
    //      setIndexBuffer(fullscreenIB) + shader.submit(ctx).
    //   5) Blit the FBO back to the default backbuffer (or to UI
    //      pass's input if UIPass wants scene-color as a texture
    //      too — that would let chrome composite over a post-bloomed
    //      background).
    //
    // Returning 0 keeps RenderFrameStats.drawCalls honest about
    // today (no draws from this slot) while the dispatch order
    // already shows the slot to anyone walking pipeline.passes().
    return 0;
}

} // namespace ayt::render::detail
