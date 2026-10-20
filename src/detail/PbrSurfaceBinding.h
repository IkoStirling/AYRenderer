#pragma once
#include "AYRenderer/PbrSurfaceSettings.h"
namespace ayt::shader
{
class ShaderResource;
}
namespace ayt::render::detail
{
struct GpuMaterial;
PbrSurfaceSettings resolvePbrSurface(const GpuMaterial &material) noexcept;
// Always supplies identity defaults as programs are shared across materials.
// Specialized passes submit their own program, not the material program.
void uploadPbrSurface(const GpuMaterial &material, shader::ShaderResource &program);
} // namespace ayt::render::detail
