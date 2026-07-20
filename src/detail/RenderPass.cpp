// RenderPass.cpp — U1++ lifted helper. Color-uniform upload used to
// be duplicated byte-for-byte in ForwardOpaquePass::flushMaterial and
// TransparentPass::execute (now both call this single source of truth).

#include "detail/RenderPass.h"
#include "detail/GpuResources.h"

#include "AYShaderResource.h"  // ShaderResource::getUniformBinding / hasUniformBinding / setUniform

namespace ayt::render::detail
{

void RenderPass::resolveAndApplyColorUniforms(GpuMaterial& material)
{
    // Lazily resolve colorBinding on first use (hot-reload friendly:
    // RenderResourceManager::setMaterialColor pre-populates this too,
    // so on a normal path the cached binding is non-invalid before
    // we get here — this block is the safety net for materials that
    // are dispatched without going through setMaterialColor first).
    if (material.colorBinding == ayt::shader::InvalidBinding) {
        material.colorBinding = material.shader.getUniformBinding("baseColor");
        if (material.colorBinding == ayt::shader::InvalidBinding) {
            material.colorBinding = material.shader.getUniformBinding("color");
        }
    }
    // Re-validate the cached binding each frame in case the shader
    // was hot-reloaded out from under us and the cached BindingId no
    // longer maps to a live uniform slot. Cheaper than re-resolving
    // and keeps the invariant "colorBinding == InvalidBinding  ⇔
    // shader has no baseColor/color uniform".
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
            // Phoskia property defaults are not guaranteed on D3D;
            // use a neutral white tint as the no-override baseline.
            const float defaultBaseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            material.shader.setUniform(material.colorBinding,
                                       defaultBaseColor,
                                       sizeof(defaultBaseColor));
        }
    }
}

} // namespace ayt::render::detail
