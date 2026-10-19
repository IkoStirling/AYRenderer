#pragma once
#include <AYShader/ShaderResource.h>
#include <cstddef>
namespace ayt::render::detail {
struct PassExecContext;
/// Shared full optical light binding for opaque GBuffer and transparent PBR.
bool bindPerformanceSurfaceLight(PassExecContext& context,shader::ShaderResource& program,size_t index,bool receiveShadows=true);
} // namespace ayt::render::detail
