#pragma once

#include <bgfx/bgfx.h>

namespace ayt::render::detail
{

struct PassExecContext;

// Resolve the current frame's scene-linear color producer. This routing is
// shared by haze, transparent, bloom, and the final post-process pass; no
// consumer owns the policy.
bgfx::FrameBufferHandle selectSceneColorSourceFbo(
    const PassExecContext& ctx) noexcept;

} // namespace ayt::render::detail
