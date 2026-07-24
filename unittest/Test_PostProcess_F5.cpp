// F5 — PostProcess 改用 FG.resolveSemantic (2026-07-24, mid-term
// `docs/frame-graph-mvp.md` F5 sub-cut)。
//
// 这一刀的物理行为预期(主人拍板):
//   - host `bloomStrength == 0` ⇒ BloomSource semantic 不可解析
//     ⇒ PostProcessPass 绑 sceneColor fallback (FS branchless collapse)
//   - host `hazeEnabled == false` ⇒ HazeSource semantic 不可解析
//     ⇒ PostProcessPass 绑 sceneColor fallback
//   - FinalColorSource 永远 = full-res SceneColor (Deferred ⇒
//     LightingOutput;Forward ⇒ sceneFbo;两者皆无 ⇒ invalid ⇒
//     Final PP 0 draw 路径)
//   - selectSourceFbo() 签名保留(cutsheet §P5 B6 接缝);内部
//     改为查 FG.resolveSemantic(FinalColorSource)
//
// 测试要点:
//   1) FinalColorSource 在 SceneColor external borrow 时 resolve
//      返 borrow handle
//   2) BloomSource 仅当 BloomBlurB live 时返 physical;否则 invalid
//   3) HazeSource 仅当 HazeHalf live 时返 physical;否则 invalid
//   4) 三 semantic 互不影响 — FinalColorSource 是 base color,
//      Bloom/Haze 是旁路 sampler
//   5) selectSourceFbo() 通过 FG.resolveSemantic 返 deferred
//      LightingOutputFbo(模拟)/sceneFbo(invalid 走 B6 fallback)
//      /全无 时返 invalid
//   6) RenderPassSlot::PostProcess ABI 不变 (= 11, append-only)

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYRenderTypes.h"

#include "detail/BGFXAdapter.h"
#include "detail/FgResource.h"
#include "detail/FrameContext.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"
#include "detail/RenderPipeline.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <unordered_map>

using ayt::render::Backend;
using ayt::render::RenderPipelineDesc;
using ayt::render::RenderPassSlot;
using ayt::render::RenderPath;
using ayt::render::detail::BGFXAdapter;
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
using ayt::render::detail::RenderPipeline;

namespace {

bgfx::FrameBufferHandle makeFakeHandle(uint16_t idx)
{
    bgfx::FrameBufferHandle h;
    h.idx = idx;
    return h;
}

} // namespace

TEST_SUITE(AYRenderer_PostProcessPass_F5)

// ─── A. selectSourceFbo() 通过 FG.resolveSemantic(FinalColorSource) ─

TEST_CASE(f5_select_source_fbo_reads_from_fg_finalcolorsource) {
    // 验证 selectSourceFbo() 内部改走 FG.resolveSemantic,当 FrameGraph
    // 配 FinalColorSource → SceneColor(external borrow)时,返
    // borrow handle 的 idx(模拟 Deferred LightingOutputFbo / Forward
    // sceneFbo 都通过这个 external borrow 入站)。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0xAA));
    fg.addPass({"Consumer", {FgResourceId::SceneColor}, {}, true});
    fg.setResolvedSemantic(FgSemantic::FinalColorSource, FgResourceId::SceneColor);
    fg.compile();

    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    ayt::shader::ShaderResourcePool pool;
    ayt::render::RenderScene scene;
    FrameContext frame{};
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0,
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE},
        nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr,
        nullptr,           // depthHaze
        &fg                // frameGraph wired
    };

    // FG 解析 FinalColorSource → SceneColor ⇒ selectSourceFbo 应该
    // 返 borrow handle 的 idx (= 0xAA)。
    const bgfx::FrameBufferHandle sourceFbo =
        PostProcessPass::selectSourceFbo(ctx);
    CHECK(BGFXAdapter::isValid(sourceFbo));
    CHECK(sourceFbo.idx == 0xAA);
}

TEST_CASE(f5_select_source_fbo_no_fg_falls_through_to_b6_priority) {
    // 没有 wire FrameGraph(ctx.frameGraph=nullptr)走 B6 legacy
    // fallback ── 没 LightingPass ⇒ ctx.sceneFbo 当 source。
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    ayt::render::RenderScene scene;
    FrameContext frame{};
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0,
        /*sceneFbo=*/makeFakeHandle(0xBB),
        nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr,
        nullptr,           // depthHaze
        nullptr            // frameGraph NOT wired
    };
    const bgfx::FrameBufferHandle sourceFbo =
        PostProcessPass::selectSourceFbo(ctx);
    CHECK(BGFXAdapter::isValid(sourceFbo));
    CHECK(sourceFbo.idx == 0xBB);
}

TEST_CASE(f5_select_source_fbo_returns_invalid_when_nothing_available) {
    // 既无 FG wire 也无 ctx.sceneFbo 也没 LightingPass ⇒ invalid
    // → caller (execute) early-return 0。
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    ayt::render::RenderScene scene;
    FrameContext frame{};
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0,
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE},  // sceneFbo invalid
        nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr,
        nullptr,
        nullptr            // frameGraph not wired
    };
    const bgfx::FrameBufferHandle sourceFbo =
        PostProcessPass::selectSourceFbo(ctx);
    CHECK(!BGFXAdapter::isValid(sourceFbo));
}

// ─── B. BloomSource / HazeSource 仅在对应 live 时返 physical ─────

TEST_CASE(f5_bloomsource_live_when_bloomenabled) {
    // 模拟 render() 集中 bloomEnabled=true(BloomBright + BloomBlurA
    // + BloomBlurB + 3 个 enabled pass);setResolvedSemantic 把
    // BloomSource 指向 BloomBlurB。compile 后 resolveSemantic 返
    // BloomBlurB 的物理 ── external 时返 borrow;owned (F6) 时返
    // 真 handle。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x10));
    fg.importExternal(FgResourceId::BloomBright,  makeFakeHandle(0x11));
    fg.importExternal(FgResourceId::BloomBlurB,  makeFakeHandle(0x12));
    fg.addPass({"BloomExtract",
                {FgResourceId::SceneColor},
                {FgResourceId::BloomBright}, true});
    fg.addPass({"BloomBlurV",
                {FgResourceId::BloomBright},
                {FgResourceId::BloomBlurB}, true});
    fg.setResolvedSemantic(FgSemantic::BloomSource, FgResourceId::BloomBlurB);
    fg.compile();

    const bgfx::FrameBufferHandle bloomSrc =
        fg.resolveSemantic(FgSemantic::BloomSource);
    CHECK(BGFXAdapter::isValid(bloomSrc));
    CHECK(bloomSrc.idx == 0x12);  // bloomExternal borrow
}

TEST_CASE(f5_bloomsource_invalid_when_bloomdisabled) {
    // 模拟 render() 集中 bloomEnabled=false ── BloomSource semantic
    // 根本不被 setResolvedSemantic;resolve 返 invalid;consumer
    // 走 fallback(sceneColor)+ FS branchless strength gate(K3 #1)。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x20));
    // 不 addPass Bbloom → BloomBright/BloomBlurA/BloomBlurB 不 live
    // // 不 setResolvedSemantic(BloomSource)
    fg.setResolvedSemantic(FgSemantic::FinalColorSource,
                           FgResourceId::SceneColor);
    fg.compile();

    const bgfx::FrameBufferHandle bloomSrc =
        fg.resolveSemantic(FgSemantic::BloomSource);
    CHECK(!BGFXAdapter::isValid(bloomSrc));
}

TEST_CASE(f5_hazesource_live_when_hazepassenabled) {
    // 模拟 render() 集中 hazePassEnabled=true;setResolvedSemantic
    // HazeSource → HazeHalf。compile 后 resolve 返 HazeHalf 物理。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x30));
    fg.importExternal(FgResourceId::HazeHalf, makeFakeHandle(0x31));
    fg.addPass({"DepthHaze",
                {FgResourceId::SceneColor},
                {FgResourceId::HazeHalf}, true});
    fg.setResolvedSemantic(FgSemantic::HazeSource, FgResourceId::HazeHalf);
    fg.compile();

    const bgfx::FrameBufferHandle hazeSrc =
        fg.resolveSemantic(FgSemantic::HazeSource);
    CHECK(BGFXAdapter::isValid(hazeSrc));
    CHECK(hazeSrc.idx == 0x31);  // hazeExternal borrow
}

TEST_CASE(f5_hazesource_invalid_when_hazedisabled) {
    // 模拟 render() 集中 hazePassEnabled=false;HazeSource 不被
    // setResolvedSemantic ⇒ resolveSemantic 返 invalid ⇒ consumer
    // fallback + FS branchless collapse(K3 #3)。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x40));
    fg.setResolvedSemantic(FgSemantic::FinalColorSource,
                           FgResourceId::SceneColor);
    fg.compile();

    const bgfx::FrameBufferHandle hazeSrc =
        fg.resolveSemantic(FgSemantic::HazeSource);
    CHECK(!BGFXAdapter::isValid(hazeSrc));
}

// ─── C. 三 semantic 互不影响 ─────────────────────────────────────

TEST_CASE(f5_three_semantics_independent_when_all_enabled) {
    // 全开 ── FinalColorSource = SceneColor;BloomSource = BloomBlurB;
    // HazeSource = HazeHalf。三 semantic 各自返对应 borrow,互不干扰
    // (半分辨率 HazeHalf 不参与 FinalColorSource ── base color)。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x50));
    fg.importExternal(FgResourceId::BloomBright, makeFakeHandle(0x51));
    fg.importExternal(FgResourceId::BloomBlurA,  makeFakeHandle(0x52));
    fg.importExternal(FgResourceId::BloomBlurB,  makeFakeHandle(0x53));
    fg.importExternal(FgResourceId::HazeHalf,    makeFakeHandle(0x54));
    fg.addPass({"BloomExtract",
                {FgResourceId::SceneColor}, {FgResourceId::BloomBright}, true});
    fg.addPass({"BloomBlurH",
                {FgResourceId::BloomBright}, {FgResourceId::BloomBlurA}, true});
    fg.addPass({"BloomBlurV",
                {FgResourceId::BloomBlurA},  {FgResourceId::BloomBlurB}, true});
    fg.addPass({"DepthHaze",
                {FgResourceId::SceneColor}, {FgResourceId::HazeHalf}, true});
    fg.setResolvedSemantic(FgSemantic::FinalColorSource, FgResourceId::SceneColor);
    fg.setResolvedSemantic(FgSemantic::BloomSource,      FgResourceId::BloomBlurB);
    fg.setResolvedSemantic(FgSemantic::HazeSource,       FgResourceId::HazeHalf);
    fg.compile();

    const bgfx::FrameBufferHandle finalSrc =
        fg.resolveSemantic(FgSemantic::FinalColorSource);
    const bgfx::FrameBufferHandle bloomSrc =
        fg.resolveSemantic(FgSemantic::BloomSource);
    const bgfx::FrameBufferHandle hazeSrc =
        fg.resolveSemantic(FgSemantic::HazeSource);

    CHECK(finalSrc.idx  == 0x50);
    CHECK(bloomSrc.idx  == 0x53);
    CHECK(hazeSrc.idx   == 0x54);
    // 三 handle 互不相同 ── 不混淆。FinalColorSource 是 full-res,
    // BloomSource 是 half-res BloomBlurB,HazeSource 是 half-res
    // HazeHalf。
    CHECK(finalSrc.idx  != bloomSrc.idx);
    CHECK(bloomSrc.idx  != hazeSrc.idx);
    CHECK(finalSrc.idx  != hazeSrc.idx);
}

TEST_CASE(f5_finalcolorsource_only_no_bloom_no_haze) {
    // Default host state:bloomStrength=0,hazeEnabled=false。
    // 仅 FinalColorSource 走通 ── Bloom / Haze 旁路 sampler 都是
    // invalid => PostProcessPass FS branchless strength gate 折叠
    // 到场景基色(K3 #1 + #3)。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x60));
    fg.setResolvedSemantic(FgSemantic::FinalColorSource,
                           FgResourceId::SceneColor);
    // 不 setResolvedSemantic(BloomSource/HazeSource)
    fg.compile();

    const bgfx::FrameBufferHandle finalSrc =
        fg.resolveSemantic(FgSemantic::FinalColorSource);
    CHECK(finalSrc.idx == 0x60);
    CHECK(!BGFXAdapter::isValid(
        fg.resolveSemantic(FgSemantic::BloomSource)));
    CHECK(!BGFXAdapter::isValid(
        fg.resolveSemantic(FgSemantic::HazeSource)));
}

// ─── D. Slot ABI 不变 ───────────────────────────────────────────

TEST_CASE(f5_render_pipeline_slot_abi_lock) {
    // F5 不动 slot ABI ── PostProcess 仍是 4(与 E5 + §Skybox0 + §S1
    // 之后稳定:Shadow=0 / Skybox=1 / FO=2 / Trans=3 / PostProcess=4 /
    // UI=5 / GBuffer=6 / Lighting=7 / BloomExtract=8 / BloomBlur=9 /
    // DepthHaze=10)。
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.path == RenderPath::Forward);
    CHECK(desc.passes.size() == 8);
    CHECK(desc.passes[6] == RenderPassSlot::PostProcess);
    CHECK(desc.contains(RenderPassSlot::PostProcess));
    CHECK(static_cast<uint8_t>(RenderPassSlot::PostProcess) == 4);
}

TEST_CASE(f5_view_id_constant_lock) {
    // F5 view id 锁:PostProcess = kBlitViewId = 14,F5 不动。
    //
    // §A2 SSAO MVP (2026-07-24, mid-term FG MVP SSAO Gate) — single-
    // point view-id bump 14→15 so PostProcess runs AFTER the SSAO
    // pass (view 14) and BEFORE UI (view 255). The lock is bumped
    // here to match the new kBlitViewId. No F5 test contract
    // changes.
    CHECK(static_cast<uint16_t>(PostProcessPass::kBlitViewId) == 15);
}

// ─── E. setResolvedSemantic + compile 顺序 ──────────────────────

TEST_CASE(f5_set_resolved_semantic_called_before_compile) {
    // cutsheet §1:compile 期 FG 已记录 semantic mapping,compile
    // 后再调 setResolvedSemantic 不影响本帧已 compile 的 state。
    // 本测试守住 "先 wire 再 compile" 的契约 ── 后续 F5 实施若
    // 改成"compile 后允许 resetResolvedSemantic"不影响该契约。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x70));
    fg.addPass({"Consumer", {FgResourceId::SceneColor}, {}, true});
    // compile 之前 setResolvedSemantic
    fg.setResolvedSemantic(FgSemantic::FinalColorSource,
                           FgResourceId::SceneColor);
    fg.compile();
    const bgfx::FrameBufferHandle src =
        fg.resolveSemantic(FgSemantic::FinalColorSource);
    CHECK(src.idx == 0x70);
}

TEST_SUITE_END
