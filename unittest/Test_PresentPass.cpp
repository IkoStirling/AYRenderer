#include "AYTest.h"

#include "AYRenderer.h"
#include "AYRenderer/RenderScene.h"
#include "AYRenderer/RenderTypes.h"
#include "AYRenderer/UIRenderBackend.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/Phoskia.h"
#include "AYShader/ShaderResourcePool.h"

#include "detail/BGFXAdapter.h"
#include "detail/EditorOverlayPass.h"
#include "detail/FgResource.h"
#include "detail/FrameContext.h"
#include "detail/GBufferDebugPass.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/PresentPass.h"
#include "detail/ShadowPass.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

using ayt::render::RenderPassSlot;
using ayt::render::RenderPipelineDesc;
using ayt::render::RenderScene;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::EditorOverlayPass;
using ayt::render::detail::FgResourceId;
using ayt::render::detail::FgSemantic;
using ayt::render::detail::FgTextureScale;
using ayt::render::detail::FrameContext;
using ayt::render::detail::FrameGraph;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::PostProcessPass;
using ayt::render::detail::PresentPass;

namespace {

std::size_t passIndex(const RenderPipelineDesc& desc, RenderPassSlot slot)
{
    const auto it = std::find(desc.passes.begin(), desc.passes.end(), slot);
    return it == desc.passes.end()
        ? desc.passes.size()
        : static_cast<std::size_t>(it - desc.passes.begin());
}

bool hasTexture(const ayt::shader::CompiledShaderProgram& program,
                const char* name)
{
    return std::any_of(program.textures.begin(), program.textures.end(),
        [name](const ayt::shader::BGFXTexture& texture) {
            return texture.name == name;
        });
}

} // namespace

TEST_SUITE(AYRenderer_PresentPass)

TEST_CASE(present_append_only_abi_values_are_locked)
{
    CHECK(static_cast<uint8_t>(RenderPassSlot::Present) == 15u);
    CHECK(static_cast<uint8_t>(FgResourceId::FinalLdrColor) == 6u);
    CHECK(static_cast<uint8_t>(FgResourceId::Count) == 7u);
    CHECK(static_cast<uint8_t>(FgSemantic::PresentSource) == 4u);
    CHECK(static_cast<uint8_t>(FgSemantic::Count) == 5u);
}

TEST_CASE(present_view_map_has_no_shadow_ui_or_debug_collision)
{
    CHECK(PostProcessPass::kBlitViewId == 15u);
    CHECK(PresentPass::kPresentViewId == 16u);
    CHECK(ayt::render::detail::ShadowPass::kShadowAtlasFirstViewId == 18u);
    CHECK(ayt::render::UIRenderBackend::kFirstLayerViewId == 26u);
    CHECK(ayt::render::UIRenderBackend::kLastLayerViewId == 249u);
    CHECK(ayt::render::detail::GBufferDebugPass::kGBufferDebugViewId == 250u);
    CHECK(EditorOverlayPass::kAxisViewId == 251u);
    CHECK(EditorOverlayPass::kBlitViewId == 252u);
    CHECK(ayt::render::UIRenderBackend::kViewId == 255u);
}

TEST_CASE(present_is_mandatory_between_postprocess_and_ui)
{
    for (const RenderPipelineDesc desc : {
             RenderPipelineDesc::makeDefault(),
             RenderPipelineDesc::makeDeferred()}) {
        const std::size_t post = passIndex(desc, RenderPassSlot::PostProcess);
        const std::size_t present = passIndex(desc, RenderPassSlot::Present);
        const std::size_t ui = passIndex(desc, RenderPassSlot::UI);
        CHECK(post < present);
        CHECK(present < ui);
        CHECK(present == post + 1u);
    }
}

TEST_CASE(editor_overlay_is_after_present_and_before_ui)
{
    for (const RenderPipelineDesc desc : {
             RenderPipelineDesc::makeEditorForward(),
             RenderPipelineDesc::makeEditorDeferred()}) {
        const std::size_t present = passIndex(desc, RenderPassSlot::Present);
        const std::size_t overlay = passIndex(desc, RenderPassSlot::EditorOverlay);
        const std::size_t ui = passIndex(desc, RenderPassSlot::UI);
        CHECK(present < overlay);
        CHECK(overlay < ui);
        CHECK(overlay == present + 1u);
    }
}

TEST_CASE(final_ldr_current_frame_latch_resets_at_begin_frame)
{
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(640, 360);
    fg.addResource(FgResourceId::FinalLdrColor,
                   {bgfx::TextureFormat::RGBA8,
                    FgTextureScale::Full,
                    true,
                    false});
    fg.addPass({"PostProcess", {}, {FgResourceId::FinalLdrColor}, true});
    fg.setResolvedSemantic(FgSemantic::PresentSource,
                           FgResourceId::FinalLdrColor);
    CHECK(fg.compile());
    CHECK_FALSE(fg.producedThisFrame(FgResourceId::FinalLdrColor));
    CHECK_FALSE(fg.semanticProducedThisFrame(FgSemantic::PresentSource));
    fg.markProduced(FgResourceId::FinalLdrColor);
    CHECK(fg.producedThisFrame(FgResourceId::FinalLdrColor));
    CHECK(fg.semanticProducedThisFrame(FgSemantic::PresentSource));

    fg.beginFrame(640, 360);
    CHECK_FALSE(fg.producedThisFrame(FgResourceId::FinalLdrColor));
    CHECK_FALSE(fg.semanticProducedThisFrame(FgSemantic::PresentSource));
}

TEST_CASE(present_noop_backend_returns_zero)
{
    PresentPass pass;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    FrameContext frame{};
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 640, 360, frame, 0
    };
    CHECK(pass.execute(ctx) == 0u);
    pass.destroyResources(adapter);
    pass.destroyResources(adapter);
}

TEST_CASE(present_runtime_shader_compiles_and_reflects_final_color)
{
    ayt::shader::phoskia::Compiler compiler;
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
    compiler.compileToProgram(
        ayt::render::detail::presentPhoskiaSourceForTests(),
        frontend,
        backend,
        program);
    if (!program.success) {
        for (const std::string& error : program.errors) {
            std::cerr << "[PresentPass test] " << error << '\n';
        }
    }
    CHECK(program.success);
    CHECK(hasTexture(program, "finalColor"));
    CHECK(std::string(ayt::render::detail::kPresentCacheKeyCStr)
          == "present_final_ldr_blit_v1");
}

TEST_SUITE_END
