#pragma once
#include "AYRenderer/RenderTypes.h"

namespace ayt::render::detail {
// Pure product policy: resolve an empty descriptor and the legacy missing
// Present boundary. Does not validate, construct passes or touch GPU state.
RenderPipelineDesc resolveRenderPipelineDesc(const RenderPipelineDesc& requested);
} // namespace ayt::render::detail
