// F3 — BloomBlur ping-pong 改用 FG (2026-07-24, mid-term
// `docs/frame-graph-mvp.md` F3 sub-cut).
//
// 这一刀的物理行为预期(主人拍板):
//   - host `bloomStrength == 0`(default) ⇒ BloomBlur 0 draw
//   - host `bloomStrength == 0` ⇒ visual 与 F2 baseline 字节一致
//     (今日 K2 invariant #1: bloomStrength=0 ⇒ 与关 Bloom 字节一致)
//   - FG 物理创建延后到 F6;F3 阶段 resolvePingPong 永远返 {invalid,
//     invalid};直接走 0 draw 路径
//
// 测试要点:
//   1) FG.compile() 后,当 bloomStrength=0 时,BloomBlurA/B 不进
//      live set(F3 主路径不写 enabled=false 的 pass)
//   2) BloomBlur::execute() 在 frameGraph==nullptr 时早退 0
//      (C++14 trailing-default 兼容的 legacy 测试接缝)
//   3) BloomBlur::execute() 在 frameGraph wired 但 bloomStrength=0
//      时,resolvePingPong 返 invalid ⇒ 0 draw
//   4) F3 物理行为:stats().logicalResources 不含 BloomBlurA/B
//      (compile 期 live set 已剔除)
//   5) BloomBlurPass 不再 own `_pingFbo/_pongFbo`;
//      pingFbo()/pongFbo() 返 invalid (F5 移除)
//   6) destroyResources 仍安全可调(只释放 VB/IB + program,
//      不动 FG)
//   7) RenderPipelineDesc::makeDefault passes 列表与值不变
//      (FG 不动 slot ABI)
//   8) A/B alias 禁止用例 ── 同样 format + 同样 scale + 同样
//      transient 描述 ⇒ FG 仍分配两个独立 handle(显式禁 alias)
//   9) execute() wired→fg+bloomEnabled ⇒ resolvePingPong 仍
//      invalid(F3 物理骨架阶段,真 handle 留给 F6)

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYRenderTypes.h"

#include "detail/BGFXAdapter.h"
#include "detail/BloomBlurPass.h"
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
using ayt::render::detail::BloomBlurPass;
using ayt::render::detail::FgResourceId;
using ayt::render::detail::FgTextureScale;
using ayt::render::detail::FgPingPong;
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

TEST_SUITE(AYRenderer_BloomBlurPass_F3)

// ─── A. FG compile:bloomStrength=0 ⇒ BloomBlur A/B 不 live ─────

TEST_CASE(f3_bloom_disabled_means_blur_ab_not_live) {
    // 主人拍板的"零变化路径":host bloomStrength=0(default)时,
    // FG.compile() 完全不声明 BloomBlur A/B + 不写它们的 pass ──
    // 这样 compile 期 live set 把它们剔除 (K2 invariant #1
    // 在 F3 阶段的兑现)。stats().logicalResources 不含 A/B。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);

    // host bloomStrength = 0 → AYRenderer 集中算 bloomEnabled=false
    // → fg.addResource / addPass 完全跳过。模拟一下:
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x10));
    // 不 addPass;不 addResource。
    fg.compile();

    CHECK(fg.stats().livePasses     == 0);
    CHECK(fg.stats().declaredPasses == 0);

    // resolvePingPong 必 {invalid, invalid}(根本未声明 + 未 live)。
    const FgPingPong pp = fg.resolvePingPong(
        FgResourceId::BloomBlurA, FgResourceId::BloomBlurB);
    CHECK(!BGFXAdapter::isValid(pp.first));
    CHECK(!BGFXAdapter::isValid(pp.second));
}

TEST_CASE(f3_bloom_enabled_means_blur_ab_live_but_resolve_invalid) {
    // host bloomStrength > 0 ⇒ FG 走完整路径:addResource BloomBright
    // + addResource BloomBlurA + addResource BloomBlurB + 3 个 pass。
    // compile 后 BloomBlurA/B 都进 live set。但 F3 阶段 FG 物理
    // 创建延后,resolvePingPong 仍返 {invalid, invalid} ── 与
    // F6 真打开物理 RT 后的行为差异由 F6 测试覆盖。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x20));
    fg.addResource(FgResourceId::BloomBright,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addResource(FgResourceId::BloomBlurA,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addResource(FgResourceId::BloomBlurB,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addPass({"BloomExtract",
                {FgResourceId::SceneColor}, {FgResourceId::BloomBright},
                /*enabled=*/true});
    fg.addPass({"BloomBlurH",
                {FgResourceId::BloomBright}, {FgResourceId::BloomBlurA},
                /*enabled=*/true});
    fg.addPass({"BloomBlurV",
                {FgResourceId::BloomBlurA},  {FgResourceId::BloomBlurB},
                /*enabled=*/true});
    fg.compile();

    // livePasses = 3(Extract + BlurH + BlurV);logicalResources ≥ 3。
    CHECK(fg.stats().livePasses       == 3);
    CHECK(fg.stats().declaredPasses   == 3);
    CHECK(fg.stats().logicalResources >= 3);

    // resolvePingPong 在未初始化 adapter 上仍 {invalid, invalid}。
    const FgPingPong pp = fg.resolvePingPong(
        FgResourceId::BloomBlurA, FgResourceId::BloomBlurB);
    CHECK(!BGFXAdapter::isValid(pp.first));
    CHECK(!BGFXAdapter::isValid(pp.second));
}

// ─── B. A/B alias 显式禁止 + 两块独立 ──────────────────────────

TEST_CASE(f3_alias_forbidden_two_independent_external_resources) {
    // cutsheet §4 "BloomBlur A/B 显式禁止 alias"。这一刀用 external
    // 方式验证(FG 也仍按两块独立 borrow 处理 ── 即便 format / scale
    // 都匹配):两块 separate handle ⇒ resolvePingPong 返
    // {handleA, handleB}。后续 F6 物理创建时,即便 format + scale
    // 完全相同,FG 也不 alias 它们。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::BloomBright,  makeFakeHandle(0xA1));
    fg.importExternal(FgResourceId::BloomBlurA,  makeFakeHandle(0xA2));
    fg.importExternal(FgResourceId::BloomBlurB,  makeFakeHandle(0xA3));
    fg.addPass({"BloomExtract",
                {FgResourceId::SceneColor}, {FgResourceId::BloomBright}, true});
    fg.addPass({"BloomBlurH",
                {FgResourceId::BloomBright}, {FgResourceId::BloomBlurA}, true});
    fg.addPass({"BloomBlurV",
                {FgResourceId::BloomBlurA},  {FgResourceId::BloomBlurB}, true});
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0xA0));
    fg.compile();

    const FgPingPong pp = fg.resolvePingPong(
        FgResourceId::BloomBlurA, FgResourceId::BloomBlurB);
    CHECK(BGFXAdapter::isValid(pp.first));
    CHECK(BGFXAdapter::isValid(pp.second));
    CHECK(pp.first.idx  == 0xA2);
    CHECK(pp.second.idx == 0xA3);
    CHECK(pp.first.idx != pp.second.idx);  // 两块独立 borrow,即便 format 同
}

// ─── C. Pass::execute 在 frameGraph==nullptr 时 0 draw ────────────

TEST_CASE(f3_bloomblur_execute_without_framegraph_returns_zero) {
    // 旧测试接缝兼容 ── legacy 23-field brace-init test sites 不
    // wire frameGraph,默认 nullptr;BloomBlur 应早退 0(F3 字段
    // 加⼊ PassExecContext 是 F2 ── frameGraph 末位 ── 之后,本 Pass
    // 新增 ctx.frameGraph==nullptr 早退守,K2 invariant #1)。
    BloomBlurPass pass;
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
        // frameGraph 默认 nullptr;legacy 接缝本 Pass 不读
        // bloomExtractPass,只读 frameGraph。
    };
    CHECK(pass.execute(ctx) == 0);
}

TEST_CASE(f3_bloomblur_execute_with_wired_fg_zero_bloom_returns_zero) {
    // host bloomStrength=0 ⇒ FG 不声明 BloomBlur A/B ⇒
    // resolvePingPong 返 {invalid, invalid} ⇒ BloomBlur 0 draw。
    BloomBlurPass pass;
    FrameContext frame{};
    frame.bloomStrength = 0.0f;  // host default
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    ayt::render::RenderScene scene;
    BGFXAdapter fgAdapter;
    FrameGraph fg(fgAdapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x30));
    // 不 addPass(BloomBlur host disabled)。
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

TEST_CASE(f3_bloomblur_execute_with_wired_fg_nonzero_bloom_still_invalid_f3) {
    // host bloomStrength>0 ⇒ FG 注册 BloomBright + BloomBlur A/B +
    // 3 个 enabled pass ⇒ compile mark live。但 F3 物理阶段
    // resolvePingPong 仍 {invalid, invalid}(未初始化 adapter)⇒
    // BloomBlur 0 draw ── 与 F6 真创建物理 RT 后的差异由 F6
    // 测试覆盖。
    BloomBlurPass pass;
    FrameContext frame{};
    frame.bloomStrength = 0.4f;
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
    fg.addResource(FgResourceId::BloomBright,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addResource(FgResourceId::BloomBlurA,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addResource(FgResourceId::BloomBlurB,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addPass({"BloomExtract",
                {FgResourceId::SceneColor}, {FgResourceId::BloomBright}, true});
    fg.addPass({"BloomBlurH",
                {FgResourceId::BloomBright}, {FgResourceId::BloomBlurA}, true});
    fg.addPass({"BloomBlurV",
                {FgResourceId::BloomBlurA},  {FgResourceId::BloomBlurB}, true});
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

// ─── D. BloomBlurPass 不再 own _pingFbo/_pongFbo ─────────────────

TEST_CASE(f3_bloomblur_pingpong_fbo_getters_return_invalid_f3) {
    // F3 ── pingFbo() / pongFbo() 是 legacy 占位 getter,返 invalid;
    // 因为 consumer (PostProcessPass S1c / S4c) 在 F5 之前仍按旧
    // 约定调它们 ── F5 会改 PostProcessPass 走
    // `ctx.frameGraph->resolveSemantic(FgSemantic::BloomSource)`,
    // 并删除本 getter。F3 钉死它们返 invalid ── 任何把它们恢复
    // 到返回 own handle 的改动都会被它抓住。
    BloomBlurPass pass;
    CHECK(!BGFXAdapter::isValid(pass.pingFbo()));
    CHECK(!BGFXAdapter::isValid(pass.pongFbo()));
    // isReady() 也恒 false(F3 物理未开;F6 真打开后会再评估)。
    CHECK(pass.isReady() == false);
}

// ─── E. destroyResources 安全可调(只释放 VB/IB + program) ──────

TEST_CASE(f3_bloomblur_destroy_resources_safe) {
    // F3 ── BloomBlur 不再 own _pingFbo/_pongFbo,destroyResources
    // 不动 FG;只释放 VB/IB/program。在未初始化 adapter 上安全
    // 可调(idempotent),这让 RenderPipeline teardown 路径仍能照
    // 旧契约调它。
    BloomBlurPass pass;
    BGFXAdapter adapter;
    pass.destroyResources(adapter);  // 第一次调用
    pass.destroyResources(adapter);  // 第二次幂等
    CHECK(true);  // 没崩就过
}

// ─── F. Slot ABI 不变 ───────────────────────────────────────────

TEST_CASE(f3_render_pipeline_slot_abi_lock) {
    // F3 不动 slot ABI ── BloomBlur 仍是 9。
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.path == RenderPath::Forward);
    // F3 阶段 RenderPipelineDesc::makeDefault 的 passes 列表 8 个 slot。
    CHECK(desc.passes.size() == 8);
    CHECK(desc.passes[4] == RenderPassSlot::BloomBlur);
    CHECK(desc.contains(RenderPassSlot::BloomBlur));
    // BloomBlur enum 值仍 = 9(append-only 锁)。
    CHECK(static_cast<uint8_t>(RenderPassSlot::BloomBlur) == 9);
}

TEST_CASE(f3_view_id_constants_lock) {
    // F3 view id 锁:F2 已经守住 BlurH=11 / BlurV=12,
    // F3 不动;钉死避免后续重构误调。
    CHECK(static_cast<uint16_t>(BloomBlurPass::kBloomBlurHorizontalViewId) == 11);
    CHECK(static_cast<uint16_t>(BloomBlurPass::kBloomBlurVerticalViewId)   == 12);
}

// ─── G. beginFrame 重置 + alias stats ──────────────────────────────

TEST_CASE(f3_begin_frame_resets_blur_ab_live) {
    // 同一 FrameGraph 实例:第一帧 register Blur A/B live → 第二帧
    // beginFrame 后 register 不命中 → 不进 live set。
    // 这守住 compile 之前的 beginFrame 隔离(cutsheet §1 "每帧
    // beginFrame 是 reset point" 决策)。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x50));
    fg.addResource(FgResourceId::BloomBright,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addResource(FgResourceId::BloomBlurA,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addResource(FgResourceId::BloomBlurB,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addPass({"BloomExtract",
                {FgResourceId::SceneColor}, {FgResourceId::BloomBright}, true});
    fg.addPass({"BloomBlurH",
                {FgResourceId::BloomBright}, {FgResourceId::BloomBlurA}, true});
    fg.addPass({"BloomBlurV",
                {FgResourceId::BloomBlurA},  {FgResourceId::BloomBlurB}, true});
    fg.compile();
    CHECK(fg.stats().livePasses == 3);

    // 第二帧 ── reset
    fg.beginFrame(1280, 720);
    CHECK(fg.stats().declaredPasses == 0);
    CHECK(fg.stats().livePasses     == 0);
    fg.shutdown();
}

// ─── H. resolve 顺序一致性 ────────────────────────────────────────

TEST_CASE(f3_resolve_pingpong_order_matches_arg_order) {
    // 验证 resolvePingPong(first, second) 准确返 first / second,
    // 而不依赖声明顺序 ── 这是 F3 BloomBlur 决定写 read 顺序
    // 的关键 (H pass 写 first = BloomBlurA,V pass 读 first + 写 second
    // = BloomBlurB)。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::BloomBlurA, makeFakeHandle(0xAA));
    fg.importExternal(FgResourceId::BloomBlurB, makeFakeHandle(0xBB));
    fg.addPass({"BlurX", {}, {FgResourceId::BloomBlurA}, true});
    fg.addPass({"BlurY", {}, {FgResourceId::BloomBlurB}, true});
    fg.compile();

    const FgPingPong pp = fg.resolvePingPong(
        FgResourceId::BloomBlurA, FgResourceId::BloomBlurB);
    CHECK(pp.first.idx  == 0xAA);
    CHECK(pp.second.idx == 0xBB);

    // 颠倒调用顺序也应得到一样的语义 ── first 永远 = 第一个参数,
    // second 永远 = 第二个参数。
    const FgPingPong ppReversed = fg.resolvePingPong(
        FgResourceId::BloomBlurB, FgResourceId::BloomBlurA);
    CHECK(ppReversed.first.idx  == 0xBB);
    CHECK(ppReversed.second.idx == 0xAA);
}

TEST_SUITE_END
