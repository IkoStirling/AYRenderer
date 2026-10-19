#pragma once
#include <bgfx/bgfx.h>
#include <AYShader/ShaderResource.h>
namespace ayt::render::detail {
struct PassExecContext;
bool bindVolumetricDensity(PassExecContext& ctx,shader::ShaderResource& program,bgfx::TextureHandle fallback);
}
