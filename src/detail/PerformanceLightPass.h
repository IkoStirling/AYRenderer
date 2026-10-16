#pragma once
#include <AYRenderer/PerformanceLighting.h>
#include <AYShader/ShaderResource.h>
#include "detail/FullscreenPassGeometry.h"
namespace ayt::render::detail {
struct PassExecContext;
class PerformanceLightPass {
public:
    uint32_t execute(PassExecContext& context);
    void destroy(BGFXAdapter& adapter) { _program.reset(); _geometry.destroy(adapter); }
private:
    shader::ShaderResource _program;
    FullscreenPassGeometry _geometry;
};
} // namespace ayt::render::detail
