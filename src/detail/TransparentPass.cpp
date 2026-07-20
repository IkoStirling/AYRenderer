#include "detail/TransparentPass.h"
#include "detail/GpuResources.h"

#include "AYShaderResource.h"  // for ShaderResource::setUniform

#include <bgfx/bgfx.h>

namespace ayt::render::detail
{

namespace {

// BGFX state for transparent geometry. Differences from ForwardOpaquePass:
//   * STATE_BLEND_ALPHA added (standard non-premultiplied "over" —
//     assumes shader output is vec4(rgb, alpha), which is Phoskia default)
//   * STATE_WRITE_Z removed — depth-write from transparent draws causes
//     them to occlude each other when the scene isn't pre-sorted (sort
//     is deferred to U1+). Depth TEST remains so we composite against
//     the opaque z-buffer that ForwardOpaquePass populated first.
const uint64_t kTransparentState = BGFX_STATE_WRITE_RGB
                                 | BGFX_STATE_WRITE_A
                                 | BGFX_STATE_BLEND_ALPHA
                                 | BGFX_STATE_DEPTH_TEST_LESS
                                 | BGFX_STATE_CULL_CW;

} // namespace

uint32_t TransparentPass::execute(
    BGFXAdapter& adapter,
    shader::ShaderResourcePool& /*pool*/,
    const RenderScene& scene,
    const std::unordered_map<uint64_t, GpuMesh>& meshes,
    const std::unordered_map<uint64_t, GpuTexture>& /*textures*/,
    std::unordered_map<uint64_t, GpuMaterial>& materials,
    uint16_t viewportX, uint16_t viewportY,
    uint16_t viewportWidth, uint16_t viewportHeight,
    const FrameContext& frame,
    uint8_t viewId)
{
    adapter.setViewRect(viewId, viewportX, viewportY, viewportWidth, viewportHeight);
    adapter.setViewTransform(viewId, frame.view.ptr(), frame.projection.ptr());

    uint32_t drawCount = 0;

    for (const DrawItem& item : scene.items()) {
        if (!item.mesh.isValid() || !item.material.isValid()) {
            continue;
        }

        const auto meshIt = meshes.find(item.mesh.id);
        const auto matIt  = materials.find(item.material.id);
        if (meshIt == meshes.end() || matIt == materials.end()) {
            continue;
        }

        const GpuMesh& mesh = meshIt->second;
        if (!bgfx::isValid(mesh.vertexBuffer) || !bgfx::isValid(mesh.indexBuffer)) {
            continue;
        }

        GpuMaterial& material = matIt->second;
        if (!material.shader.isValid()) {
            continue;
        }

        // U1 tag check — only Alpha materials enter this pass.
        // ForwardOpaquePass draws the Opaque ones (default for all
        // pre-existing materials).
        if (material.blendMode != ayt::render::BlendMode::Alpha) {
            continue;
        }

        // Hand-rolled 5-line submission mirroring ForwardOpaquePass's
        // flushMaterial body (which is private static). We
        // deliberately duplicate rather than lift flushMaterial to
        // the base to keep U1's surface-area small — both pass
        // implementations will move in lockstep when U1+ adds more
        // shader-uniform coverage.
        adapter.setTransform(item.world);
        adapter.setVertexBuffer(mesh.vertexBuffer);
        adapter.setIndexBuffer(mesh.indexBuffer, 0, mesh.indexCount);

        // Mirror ForwardOpaquePass::flushMaterial lines 69-89 with no
        // skinning branch — U1 transparent draws are non-skinned.
        // Lazily resolve colorBinding once, then re-validate via
        // hasUniformBinding each frame (cheaper than re-resolving).
        if (material.colorBinding == ayt::shader::InvalidBinding) {
            material.colorBinding = material.shader.getUniformBinding("baseColor");
            if (material.colorBinding == ayt::shader::InvalidBinding) {
                material.colorBinding = material.shader.getUniformBinding("color");
            }
        }
        if (material.colorBinding != ayt::shader::InvalidBinding) {
            if (!material.shader.hasUniformBinding(material.colorBinding)) {
                material.colorBinding = ayt::shader::InvalidBinding;
            }
        }
        if (material.colorBinding != ayt::shader::InvalidBinding) {
            if (material.hasColorOverride) {
                material.shader.setUniform(material.colorBinding,
                                           material.colorOverride.ptr(),
                                           sizeof(float) * 4);
            } else {
                const float defaultBaseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                material.shader.setUniform(material.colorBinding,
                                           defaultBaseColor,
                                           sizeof(defaultBaseColor));
            }
        }

        ayt::shader::DrawCallContext ctx;
        ctx.viewId = viewId;
        ctx.state  = kTransparentState;
        material.shader.submit(ctx);
        ++drawCount;
    }

    return drawCount;
}

} // namespace ayt::render::detail
