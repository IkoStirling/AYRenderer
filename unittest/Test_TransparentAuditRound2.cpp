#include "AYTest.h"

#include "AYRenderer/PbrShaderSources.h"
#include "AYRenderer/RenderScene.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/Phoskia.h"

#include "detail/FrameContext.h"
#include "detail/RenderViewOrder.h"
#include "detail/SceneLighting.h"
#include "detail/TransparentPass.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

TEST_SUITE(AYRenderer_TransparentAuditRound2)

TEST_CASE(route_requires_a_complete_fresh_deferred_target)
{
    using ayt::render::detail::TransparentRoute;
    using ayt::render::detail::selectTransparentRoute;

    CHECK(selectTransparentRoute(false, false, false, false, false)
          == TransparentRoute::Forward);
    CHECK(selectTransparentRoute(true, false, true, false, false)
          == TransparentRoute::Skip);
    CHECK(selectTransparentRoute(true, true, false, true, true)
          == TransparentRoute::Skip);
    CHECK(selectTransparentRoute(true, true, true, false, true)
          == TransparentRoute::Skip);
    CHECK(selectTransparentRoute(true, true, true, true, false)
          == TransparentRoute::Skip);
    CHECK(selectTransparentRoute(true, true, true, true, true)
          == TransparentRoute::Deferred);
}

TEST_CASE(default_sort_is_back_to_front_and_sort_key_stays_primary)
{
    using ayt::render::DrawItem;
    using ayt::render::detail::TransparentSortEntry;
    using ayt::render::detail::transparentDistanceSquared;
    using ayt::render::detail::transparentSortBefore;

    DrawItem nearItem;
    DrawItem farItem;
    DrawItem overrideItem;
    nearItem.world = ayt::math::Float4x4::translation({0.0f, 0.0f, 2.0f});
    farItem.world = ayt::math::Float4x4::translation({0.0f, 0.0f, 9.0f});
    overrideItem.world = ayt::math::Float4x4::translation({0.0f, 0.0f, 1.0f});
    overrideItem.sortKey = 7;

    const ayt::math::FVector3 camera(0.0f, 0.0f, 0.0f);
    std::vector<TransparentSortEntry> entries = {
        {&nearItem, transparentDistanceSquared(nearItem, camera)},
        {&farItem, transparentDistanceSquared(farItem, camera)},
        {&overrideItem, transparentDistanceSquared(overrideItem, camera)},
    };
    std::stable_sort(entries.begin(), entries.end(), transparentSortBefore);

    CHECK(entries[0].item == &overrideItem);
    CHECK(entries[1].item == &farItem);
    CHECK(entries[2].item == &nearItem);
    CHECK(entries[1].distanceSquared == 81.0f);
}

TEST_CASE(alpha_state_reads_depth_never_writes_it_and_uses_correct_over_alpha)
{
    using ayt::render::BlendMode;
    using ayt::render::detail::transparentDrawState;

    const uint64_t state = transparentDrawState(
        BlendMode::Alpha, false, true, false);
    const uint64_t expected = BGFX_STATE_WRITE_RGB
        | BGFX_STATE_WRITE_A
        | BGFX_STATE_DEPTH_TEST_LEQUAL
        | BGFX_STATE_BLEND_FUNC_SEPARATE(
            BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA,
            BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
    CHECK(state == expected);
    CHECK((state & BGFX_STATE_WRITE_Z) == 0u);
    CHECK((state & BGFX_STATE_DEPTH_TEST_ALWAYS) == 0u);
}

TEST_CASE(selection_mask_uses_full_silhouette_and_screen_space_composite)
{
    using ayt::render::detail::selectionMaskState;
    using ayt::render::detail::selectionVisibleMaskState;
    using ayt::render::detail::selectionCompositeState;
    using ayt::render::detail::selectionOutlinePhoskiaSourceForTests;

    const uint64_t maskState = selectionMaskState(false, false);
    CHECK((maskState & BGFX_STATE_WRITE_RGB) == 0u);
    CHECK((maskState & BGFX_STATE_WRITE_A) != 0u);
    CHECK((maskState & BGFX_STATE_WRITE_Z) == 0u);
    CHECK((maskState & BGFX_STATE_DEPTH_TEST_ALWAYS) != 0u);
    CHECK((maskState & BGFX_STATE_BLEND_MASK) == 0u);

    const uint64_t visibleMaskState = selectionVisibleMaskState(false, false);
    CHECK((visibleMaskState & BGFX_STATE_WRITE_RGB) != 0u);
    CHECK((visibleMaskState & BGFX_STATE_WRITE_A) == 0u);
    CHECK((visibleMaskState & BGFX_STATE_WRITE_Z) == 0u);
    CHECK((visibleMaskState & BGFX_STATE_DEPTH_TEST_LEQUAL) != 0u);

    const uint64_t compositeState = selectionCompositeState();
    CHECK((compositeState & BGFX_STATE_WRITE_Z) == 0u);
    CHECK((compositeState & BGFX_STATE_DEPTH_TEST_ALWAYS) != 0u);
    CHECK((compositeState & BGFX_STATE_BLEND_MASK) != 0u);

    const std::string source(selectionOutlinePhoskiaSourceForTests());
    CHECK(source.find("selectionTexelSize.xy * 2.0") != std::string::npos);
    CHECK(source.find("let stableUv") != std::string::npos);
    CHECK(source.find("let visibleUv = stableUv + selectionTexelSize.zw")
          != std::string::npos);
    CHECK(source.find("sample(selectionMask, stableUv).w")
          != std::string::npos);
    CHECK(source.find("neighborSum") != std::string::npos);
    CHECK(source.find("outerCoverage") != std::string::npos);
    CHECK(source.find("visibleCoverage") != std::string::npos);
    CHECK(source.find("smoothstep(0.05, 0.95, center)")
          != std::string::npos);
    CHECK(source.find("vec4(1.0, 0.55, 0.12, edge)") != std::string::npos);

    ayt::shader::phoskia::Compiler compiler;
    ayt::shader::phoskia::CompileResult compileResult{};
    compiler.compile(source, compileResult);
    CHECK(compileResult.success);
    CHECK(compileResult.errors.empty());

    ayt::shader::phoskia::CompileOptions frontend;
    ayt::shader::BGFXCompileOptions backend;
    backend.shadercPath = AY_SHADER_SHADERC_HINT;
    backend.platform = "linux";
    backend.profile = "430";
#ifdef AY_SHADER_BGFX_COMMON_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
    ayt::shader::CompiledShaderProgram program;
    compiler.compileToProgram(source, frontend, backend, program);
    if (!program.success) {
        for (const std::string& error : program.errors) {
            std::cerr << "[SelectionOutline test] " << error << '\n';
        }
    }
    CHECK(program.success);
    CHECK(std::any_of(program.textures.begin(), program.textures.end(),
        [](const ayt::shader::BGFXTexture& texture) {
            return texture.name == "selectionMask";
        }));
    CHECK(std::any_of(program.uniforms.begin(), program.uniforms.end(),
        [](const ayt::shader::BGFXUniform& uniform) {
            return uniform.name == "selectionTexelSize";
        }));
}

TEST_CASE(selection_mask_precedes_postprocess_and_composite_follows_present)
{
    using ayt::render::detail::kRenderViewRemap;
    using ayt::render::detail::kRenderViewCapacity;
    using ayt::render::detail::TransparentPass;

    std::array<bool, kRenderViewCapacity> seen{};
    bool validPermutation = true;
    for (const bgfx::ViewId viewId : kRenderViewRemap) {
        const size_t index = static_cast<size_t>(viewId);
        if (index >= seen.size() || seen[index]) {
            validPermutation = false;
        } else {
            seen[index] = true;
        }
    }
    CHECK(validPermutation);
    CHECK(std::all_of(seen.begin(), seen.end(), [](bool value) {
        return value;
    }));

    const auto rankOf = [](bgfx::ViewId viewId) {
        return std::find(kRenderViewRemap.begin(), kRenderViewRemap.end(),
                         viewId) - kRenderViewRemap.begin();
    };
    CHECK(rankOf(ayt::render::kTransparentDeferredViewId)
          < rankOf(TransparentPass::kSelectionStableMaskViewId));
    CHECK(rankOf(TransparentPass::kSelectionStableMaskViewId)
          < rankOf(TransparentPass::kSelectionMaskViewId));
    CHECK(rankOf(TransparentPass::kSelectionMaskViewId)
          < rankOf(bgfx::ViewId{10}));
    CHECK(rankOf(bgfx::ViewId{16})
          < rankOf(TransparentPass::kSelectionCompositeViewId));
    CHECK(rankOf(TransparentPass::kSelectionCompositeViewId)
          < rankOf(bgfx::ViewId{251}));
}

TEST_CASE(scene_light_pack_matches_shadow_caster_first_order)
{
    using ayt::render::Light;
    using ayt::render::SceneLights;
    using ayt::render::detail::FrameContext;
    using ayt::render::detail::packSceneLighting;

    SceneLights lights;
    lights.add(Light::point({2.0f, 3.0f, 4.0f}, 8.0f, 5.0f,
                            {0.2f, 0.3f, 0.4f}));
    Light caster = Light::directional({1.0f, -2.0f, 3.0f},
                                      {0.8f, 0.7f, 0.6f});
    caster.withShadow(0.004f);
    lights.add(caster);

    const auto packed = packSceneLighting(&lights, FrameContext{});
    CHECK(packed.activeLightCount == 2u);
    CHECK(packed.dirs[0][3] == 0.0f);
    CHECK(packed.dirs[0][0] == -1.0f);
    CHECK(packed.colors[0][0] == 0.8f);
    CHECK(packed.dirs[1][3] == 1.0f);
    CHECK(packed.dirs[1][0] == 2.0f);
    CHECK(packed.params[1][0] == 8.0f);
    CHECK(packed.params[1][1] == 5.0f);
}

TEST_CASE(scene_light_pack_sanitizes_inactive_and_nonfinite_values)
{
    ayt::render::SceneLights lights;
    ayt::render::Light point = ayt::render::Light::point(
        {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f},
        std::numeric_limits<float>::infinity(), -2.0f,
        {1.0f, 1.0f, 1.0f});
    lights.add(point);

    const auto packed = ayt::render::detail::packSceneLighting(
        &lights, ayt::render::detail::FrameContext{});
    CHECK(std::isfinite(packed.dirs[0][0]));
    CHECK(std::isfinite(packed.params[0][0]));
    CHECK(packed.params[0][0] > 0.0f);
    CHECK(packed.params[0][1] == 0.0f);
    CHECK(std::isfinite(packed.params[7][0]));
    CHECK(std::isfinite(packed.spotDirs[7][1]));
}

TEST_CASE(runtime_pbr_consumes_shared_lighting_shadow_and_ibl_contract)
{
    const std::string source(ayt::render::kPbrPhoskiaSource);
    CHECK(source.find("uniformblock Lights") != std::string::npos);
    CHECK(source.find("vec4 spotDir[8]") != std::string::npos);
    CHECK(source.find("uniform vec4 shadowAtlasRects[8]")
          != std::string::npos);
    CHECK(source.find("let active7 = step(7.5, activeLightCount.x)")
          != std::string::npos);
    CHECK(source.find("let direct7 = (diffuse7 + specular7)")
          != std::string::npos);
    CHECK(source.find("let cubeAmbient = sample(envCube, N)")
          != std::string::npos);
}

TEST_SUITE_END
