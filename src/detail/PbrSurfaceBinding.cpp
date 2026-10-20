#include "detail/PbrSurfaceBinding.h"
#include "detail/RenderPass.h"
#include <cstring>
namespace ayt::render::detail
{
PbrSurfaceSettings resolvePbrSurface(const GpuMaterial &material) noexcept
{
    PbrSurfaceSettings value;
    for (const auto &slot : material.uniformSlots)
    {
        if (slot.name == "uvTransform" && slot.size >= 4 * sizeof(float))
        {
            float uv[4];
            std::memcpy(uv, slot.data, sizeof(uv));
            value.uvScaleX = uv[0];
            value.uvScaleY = uv[1];
            value.uvOffsetX = uv[2];
            value.uvOffsetY = uv[3];
        }
    }
    value.normalStrength = materialUniformScalar(material, "normalStrength", 1);
    value.roughnessBias = materialUniformScalar(material, "roughnessBias", 0);
    return sanitizePbrSurface(value);
}
void uploadPbrSurface(const GpuMaterial &material, shader::ShaderResource &program)
{
    const auto value = resolvePbrSurface(material);
    const float uv[4] = {value.uvScaleX, value.uvScaleY, value.uvOffsetX, value.uvOffsetY};
    const float normal[4] = {value.normalStrength, 0, 0, 0}, roughness[4] = {value.roughnessBias, 0, 0, 0};
    trySetUniformVec4(program, "uvTransform", uv);
    trySetUniformVec4(program, "normalStrength", normal);
    trySetUniformVec4(program, "roughnessBias", roughness);
}
} // namespace ayt::render::detail
