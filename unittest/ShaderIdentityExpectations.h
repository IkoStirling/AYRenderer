#pragma once

#include <string_view>

namespace renderer_test {

// Independent reviewed snapshots, deliberately not imported from production.
// An intentional built-in shader identity change updates this one fixture
// instead of unrelated historical integration suites' literal copies.
inline constexpr char kLightingSourceKey[]="lighting_v36_material_soft_diffuse";
inline constexpr auto kLightingVersionMarker=std::string_view(kLightingSourceKey).substr(9);
inline constexpr char kGBufferDebugSourceKey[]="gbufferdebug_v7_four_material_models";
inline constexpr char kPostProcessSourceKey[]="postprocess_tonemap_v14_lens_effects";

} // namespace renderer_test
