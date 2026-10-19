#pragma once
#include <bgfx/bgfx.h>
#include "detail/FgResource.h"
namespace ayt::render::detail {
struct PassExecContext;
struct PresentationStageTargets {
    bgfx::FrameBufferHandle target=BGFX_INVALID_HANDLE;
    bgfx::TextureHandle input=BGFX_INVALID_HANDLE;
};
// Common safety boundary only; each stage retains its shader, algorithm,
// settings and resource ownership. No target is resolved on Noop/stale input.
bool resolvePresentationStage(PassExecContext& context,FgResourceId output,PresentationStageTargets& targets);
void publishPresentationStage(PassExecContext& context,FgResourceId output);
} // namespace ayt::render::detail
