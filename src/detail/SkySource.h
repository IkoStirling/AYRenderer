#pragma once

// §Skybox0 (2026-07-23) — SkyboxPass / LightingPass borrowed-ptr
// type alias. The actual `SkySource` POD lives in `AYRenderer/RenderScene.h`
// so hosts can build it without pulling detail headers (mirror
// `SceneLights` shape — public POD in include/, borrowed via
// PassExecContext). This detail-header exists for symmetry with
// `PerLightShadowDS.h` (declaration of a "DS kind" even though the
// actual POD is public — gives SkyboxPass.cpp a `detail::` import
// to follow without a wider change).

#include "AYRenderer/RenderScene.h"

namespace ayt::render::detail
{

// Lifetime contract: must outlive Renderer::render(). Easiest
// lifetime = host member, populated once at startup (e.g.
// `static const SkySource kSkybox{ TextureHandle{...} };` for
// unit tests) or per scene change in Editor.
//
// Pointer lives in PassExecContext::skySource (borrowed, non-owning).
// Renderer never copies or destroys; it just reads kind + equirect
// handle per frame and (when Equirect) binds the equirect texture
// into the SkyboxPass material.
using SkyDataSource = ayt::render::SkySource;

} // namespace ayt::render::detail
