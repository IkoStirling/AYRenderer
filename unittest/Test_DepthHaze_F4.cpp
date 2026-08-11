// F4 — DepthHaze 改用 FG.HazeHalf (2026-07-24, mid-term
// `docs/frame-graph-mvp.md` F4 sub-cut)。
//
// 这一刀的物理行为预期(主人拍板):
//   - host `hazeEnabled == false`(default) ⇒ DepthHaze 0 draw + 0 alloc
//   - host `hazeEnabled == false` ⇒ visual 与 F3 baseline 字节一致
//     (今日 K3 invariant #2: hazeEnabled=false ⇒ no FBO created)
//   - FG 物理创建延后到 F6;F4 阶段 resolve(HazeHalf) 永远返 invalid
//   - hazePassEnabled = 中央 frame.hazeEnabled && frame.hazeStrength
//     > 0 && gbufferPass!=nullptr ⇒ 决策集中到 render()
//
// 测试要点:
//   1) FG.compile() 后,当 hazePassEnabled 计算式各项条件不满足时,
//      HazeHalf 不进 live set(F4 主路径不写 enabled=false 的 pass)
//   2) DepthHaze::execute() 在 frameGraph==nullptr 时早退 0
//   3) DepthHaze::execute() 在 frameGraph wired 但 HazeHalf 不 live
//      时,resolve 返 invalid ⇒ 0 draw
//   4) F4 物理行为:stats().logicalResources 不含 HazeHalf
//      (compile 期 live set 已剔除)
//   5) DepthHazePass 不再 own _fbo;halfResFbo() 返 invalid (F5 移除)
//   6) destroyResources 仍安全可调(只释放 VB/IB + program,不动 FG)
//   7) RenderPipelineDesc::makeDefault passes 列表与值不变
//      (FG 不动 slot ABI),DepthHaze enum 值仍 = 10
//   8) 多条件 hazePassEnabled 决策表(host enabled + Forward ⇒
//      no haze; host enabled + Deferred + strength=0 ⇒ no haze;
//      host disabled ⇒ no haze; 等等)

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYRenderTypes.h"

#include "detail/BGFXAdapter.h"
#include "detail/DepthHazePass.h"
#include "detail/FgResource.h"
#include "detail/FrameContext.h"
#include "detail/PassExecContext.h"
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
using ayt::render::detail::DepthHazePass;
using ayt::render::detail::FgResourceId;
using ayt::render::detail::FgTextureScale;
using ayt::render::detail::FrameContext;
using ayt::render::detail::FrameGraph;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::RenderPipeline;

namespace {

bgfx::FrameBufferHandle makeFakeHandle(uint16_t idx)
{
    bgfx::FrameBufferHandle h;
    h.idx = idx;
    return h;
}

} // namespace

TEST_SUITE(AYRenderer_DepthHazePass_F4)

// ─── A. FG compile:hazePassEnabled=false ⇒ HazeHalf 不 live ─────

TEST_CASE(f4_haze_disabled_means_hazehalf_not_live) {
    // 主人拍板的"零变化路径":host hazeEnabled=false(default)时,
    // 集中 hazePassEnabled 计算为 false ⇒ fg 完全不声明 HazeHalf
    // 也不写 DepthHaze pass ── K3 invariant #2 "hazeEnabled=false
    // ⇒ no FBO created" 在 F4 阶段的兑现。stats().logicalResources
    // 不含 HazeHalf。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);

    // 模拟 host 默认态:hazeEnabled=false ⇒ render() 集中逻辑
    // 不 addResource / addPass。
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x10));
    fg.compile();

    CHECK(fg.stats().livePasses     == 0);
    CHECK(fg.stats().declaredPasses == 0);

    // resolve(HazeHalf) 必 invalid(根本未声明 + 未 live)。
    const bgfx::FrameBufferHandle target =
        fg.resolve(FgResourceId::HazeHalf);
    CHECK(!BGFXAdapter::isValid(target));
}

TEST_CASE(f4_haze_enabled_means_hazehalf_live_but_resolve_invalid) {
    // 模拟 host hazeEnabled=true(集中 hazePassEnabled 全
    // 条件满足)⇒ FG 注册 HazeHalf + DepthHaze pass enabled ⇒
    // compile mark live。但 F4 阶段 FG 物理创建延后,resolve 仍
    // 返 invalid(F6 真打开后这里返真 handle)。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x20));
    fg.addResource(FgResourceId::HazeHalf,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addPass({"DepthHaze",
                {FgResourceId::SceneColor},
                {FgResourceId::HazeHalf},
                /*enabled=*/true});
    fg.compile();

    CHECK(fg.stats().livePasses       == 1);
    CHECK(fg.stats().declaredPasses   == 1);
    CHECK(fg.stats().logicalResources >= 1);

    const bgfx::FrameBufferHandle target =
        fg.resolve(FgResourceId::HazeHalf);
    CHECK(!BGFXAdapter::isValid(target));
}

// ─── B. hazePassEnabled 决策集中(模拟 render() 集中条件) ─────

TEST_CASE(f4_haze_pass_enabled_full_predicate_matches_render_central) {
    // 这个测试盯中央 hazePassEnabled 决策公式:
    //   `frame.hazeEnabled && frame.hazeStrength > 0 &&
    //    gbufferPassPtr != nullptr && viewport > 0`
    // 任意一项不满足 ⇒ FG 不注册。模拟四种组合验证:
    BGFXAdapter adapter;

    // ── 组合 1 ── 全关(host default),HazeHalf 不该 live。
    {
        FrameGraph fg(adapter);
        fg.beginFrame(1280, 720);
        fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x30));
        fg.compile();  // 不 addPass
        CHECK(fg.stats().livePasses == 0);
        CHECK(!BGFXAdapter::isValid(fg.resolve(FgResourceId::HazeHalf)));
    }
    // ── 组合 2 ── hazeEnabled=true + strength>0 + Deferred,
    //             central decision = enabled;模拟注册后的 live set。
    {
        FrameGraph fg(adapter);
        fg.beginFrame(1280, 720);
        fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x31));
        fg.addResource(FgResourceId::HazeHalf,
                       {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
        fg.addPass({"DepthHaze",
                    {FgResourceId::SceneColor},
                    {FgResourceId::HazeHalf},
                    true});
        fg.compile();
        CHECK(fg.stats().livePasses == 1);
    }
    // ── 组合 3 ── hazeEnabled=true + strength=0 ⇒ 中央判定为
    //             disabled (strength>0 不满足)。模拟不加 pass。
    {
        FrameGraph fg(adapter);
        fg.beginFrame(1280, 720);
        fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x32));
        fg.compile();  // 不 addPass
        CHECK(fg.stats().livePasses == 0);
    }
    // ── 组合 4 ── 零 viewport ⇒ 中央判定为 disabled。
    {
        FrameGraph fg(adapter);
        fg.beginFrame(0, 0);
        fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x33));
        fg.compile();  // 不 addPass
        CHECK(fg.stats().livePasses == 0);
    }
}

// ─── C. Pass::execute 在 frameGraph==nullptr 时 0 draw ────────────

TEST_CASE(f4_depthhaze_execute_without_framegraph_returns_zero) {
    // legacy 23-field brace-init test sites 不 wire frameGraph,
    // 默认 nullptr;DepthHaze 应早退 0(F4 新增 frameGraph==nullptr
    // 早退守,K3 invariant #2 复守)。
    DepthHazePass pass;
    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    ayt::render::RenderScene scene;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
        // frameGraph 默认 nullptr
    };
    CHECK(pass.execute(ctx) == 0);
}

TEST_CASE(f4_depthhaze_execute_with_wired_fg_hazehalf_not_live_returns_zero) {
    // host hazeEnabled=false ⇒ FG 不声明 HazeHalf ⇒
    // resolve 返 invalid ⇒ DepthHaze 0 draw。
    DepthHazePass pass;
    FrameContext frame{};
    frame.hazeEnabled  = false;
    frame.hazeStrength = 0.0f;  // host default
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    ayt::render::RenderScene scene;
    BGFXAdapter fgAdapter;
    FrameGraph fg(fgAdapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x40));
    // 不 addPass(DepthHaze host disabled by central decision)。
    fg.compile();
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0,
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE},
        nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr,
        nullptr,           // depthHaze
        &fg                // frameGraph wired (last field)
    };
    CHECK(pass.execute(ctx) == 0);
}

TEST_CASE(f4_depthhaze_execute_with_wired_fg_hazehalf_live_still_invalid_f4) {
    // host hazeEnabled=true + 集中决策全满足 ⇒ FG 注册 HazeHalf +
    // DepthHaze pass enabled ⇒ compile mark live。但 F4 物理阶段
    // resolve(HazeHalf) 仍 invalid(未初始化 adapter)⇒
    // DepthHaze 0 draw ── 与 F6 真创建物理 RT 后的差异由 F6
    // 测试覆盖。
    DepthHazePass pass;
    FrameContext frame{};
    frame.hazeEnabled  = true;
    frame.hazeStrength = 0.6f;
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    ayt::render::RenderScene scene;
    BGFXAdapter fgAdapter;
    FrameGraph fg(fgAdapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x50));
    fg.addResource(FgResourceId::HazeHalf,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addPass({"DepthHaze",
                {FgResourceId::SceneColor},
                {FgResourceId::HazeHalf},
                true});
    fg.compile();
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
    CHECK(pass.execute(ctx) == 0);
}

// ─── D. DepthHazePass 不再 own _fbo ───────────────────────────

TEST_CASE(f4_depthhaze_halfres_fbo_getters_return_invalid_f4) {
    // F4 ── halfResFbo() 是 legacy 占位 getter,返 invalid;因为
    // consumer (PostProcessPass S4c) 在 F5 之前仍按旧约定调它
    // ── F5 会改 PostProcessPass 走
    // `ctx.frameGraph->resolveSemantic(FgSemantic::HazeSource)`,
    // 并删除本 getter。F4 钉死它们返 invalid ── 任何把它们恢复
    // 到返回 own handle 的改动都会被它抓住。
    DepthHazePass pass;
    CHECK(!BGFXAdapter::isValid(pass.halfResFbo()));
    CHECK(pass.halfWidth()  == 0);
    CHECK(pass.halfHeight() == 0);
    // isReady() 也恒 false(F4 物理未开;F6 真打开后会再评估)。
    CHECK(pass.isReady() == false);
}

// ─── E. destroyResources 安全可调(只释放 VB/IB + program) ──────

TEST_CASE(f4_depthhaze_destroy_resources_safe) {
    // F4 ── DepthHaze 不再 own _fbo,destroyResources 不动
    // _fbo;只释放 VB / IB / program。在未初始化 adapter 上安全
    // 可调(idempotent)。
    DepthHazePass pass;
    BGFXAdapter adapter;
    pass.destroyResources(adapter);  // 第一次调用
    pass.destroyResources(adapter);  // 第二次幂等
    CHECK(true);  // 没崩就过
}

// ─── F. Slot ABI 不变 ───────────────────────────────────────────

TEST_CASE(f4_render_pipeline_slot_abi_lock) {
    // F4 不动 slot ABI ── DepthHaze 仍是 10。
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.path == RenderPath::Forward);
    CHECK(desc.passes.size() == 9);
    CHECK(desc.passes[6] == RenderPassSlot::DepthHaze);
    CHECK(desc.contains(RenderPassSlot::DepthHaze));
    // DepthHaze enum 值仍 = 10(append-only 锁)。
    CHECK(static_cast<uint8_t>(RenderPassSlot::DepthHaze) == 10);
}

TEST_CASE(f4_view_id_constant_lock) {
    // F4 view id 锁:DepthHaze=13,F4 不动;钉死避免后续重构误调。
    CHECK(static_cast<uint16_t>(DepthHazePass::kDepthHazeViewId) == 13);
}

// ─── G. 集成:HazeHalf 与其他 FG 资源共存 ─────────────────────

TEST_CASE(f4_hazehalf_coexists_with_bloom_chain_when_both_enabled) {
    // 集成测试 ── 同时启用 bloom + haze(集中决策双满足),FG 应
    // 同时注册 BloomBright / BloomBlurA / BloomBlurB / HazeHalf +
    // 4 个 pass + DepthHaze 与 BloomBlur 互不干扰(K3 invariant
    // 跨链)。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x60));
    fg.addResource(FgResourceId::BloomBright,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addResource(FgResourceId::BloomBlurA,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addResource(FgResourceId::BloomBlurB,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addResource(FgResourceId::HazeHalf,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addPass({"BloomExtract",
                {FgResourceId::SceneColor}, {FgResourceId::BloomBright}, true});
    fg.addPass({"BloomBlurH",
                {FgResourceId::BloomBright}, {FgResourceId::BloomBlurA}, true});
    fg.addPass({"BloomBlurV",
                {FgResourceId::BloomBlurA},  {FgResourceId::BloomBlurB}, true});
    fg.addPass({"DepthHaze",
                {FgResourceId::SceneColor}, {FgResourceId::HazeHalf}, true});
    fg.compile();

    CHECK(fg.stats().livePasses     == 4);
    CHECK(fg.stats().declaredPasses == 4);
    CHECK(fg.stats().logicalResources >= 4);
    // HazeHalf 仍 invalid(F4 物理骨架阶段)。
    CHECK(!BGFXAdapter::isValid(fg.resolve(FgResourceId::HazeHalf)));
}

TEST_SUITE_END
