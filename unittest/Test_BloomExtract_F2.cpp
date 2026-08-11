// F2 — BloomExtract 改用 FG.BloomBright (2026-07-24, mid-term
// `docs/frame-graph-mvp.md` F2 sub-cut)
//
// 这一刀的物理行为预期(主人拍板):
//   - host `bloomStrength == 0`(default) ⇒ BloomExtract 0 draw
//   - host `bloomStrength == 0` ⇒ visual 与 F1 baseline 字节一致
//     (今日 K1 invariant #1:bloomStrength=0 ⇒ 与关 Bloom 字节一致)
//   - FG 物理创建延后到 F6;F2 阶段 FG 永远 resolve() 返 invalid
//
// 测试要点:
//   1) FG.compile() 后,当 bloomStrength=0 时,BloomBright 不进
//      live set(F2 主路径不写 enabled=false 的 pass)
//   2) BloomExtract::execute() 在 frameGraph==nullptr 时早退 0
//      (C++14 trailing-default 兼容的 legacy 测试接缝)
//   3) BloomExtract::execute() 在 frameGraph wired 但 bloomStrength
//      =0(Bright 不 live)时,resolve 返 invalid ⇒ 0 draw
//   4) F2 物理行为:stats().logicalResources 不含 BloomBright
//      (compile 期 live set 已剔除)
//   5) BloomExtractPass 不再 own _fbo;halfResFbo() 返 invalid
//   6) destroyResources 仍安全可调(只释放 program/VB/IB,不动 FG)
//   7) RenderPipelineDesc::makeDefault 的 passes 列表与值不变
//      (FG 不动 slot ABI)

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYRenderTypes.h"

#include "detail/BGFXAdapter.h"
#include "detail/BloomExtractPass.h"
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
using ayt::render::detail::BloomExtractPass;
using ayt::render::detail::FgResourceId;
using ayt::render::detail::FgSemantic;
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

TEST_SUITE(AYRenderer_BloomExtractPass_F2)

// ─── A. FG compile:bloomStrength=0 ⇒ BloomBright 不 live ─────────

TEST_CASE(f2_bloom_disabled_means_bright_not_live) {
    // 主人拍板的"零变化路径":host bloomStrength=0(default)时,
    // FG.compile() 应把 BloomExtract pass 标 disabled,BloomBright
    // 不进 live set ── 这是 K3 invariant #2(cutsheet §7 row 3)
    // 在 F2 阶段的兑现。stats().logicalResources 不含 BloomBright。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);

    // host bloomStrength = 0 → AYRenderer 集中算 bloomEnabled=false
    // → fg.addResource / addPass 完全跳过(F2 主路径)。模拟一下:
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x10));
    // 不 addPass;不 addResource(F2 集中逻辑的真值是:enabled=false
    // ⇒ 上面两个调用根本不被发出)。
    fg.compile();

    // livePasses = 0(没注册任何 pass)。
    CHECK(fg.stats().livePasses     == 0);
    CHECK(fg.stats().declaredPasses == 0);

    // resolve(BloomBright) 必 invalid(根本未声明 + 未 live)。
    const bgfx::FrameBufferHandle bright = fg.resolve(FgResourceId::BloomBright);
    CHECK(!BGFXAdapter::isValid(bright));
}

TEST_CASE(f2_bloom_enabled_means_bright_live_but_resolve_invalid) {
    // host bloomStrength > 0 ⇒ FG 注册 BloomBright + BloomExtract;
    // compile 后 BloomBright 进 live set。但 F2 阶段 FG 物理创建
    // 延后,resolve() 仍返 invalid(BloomExtract 0 draw,与 F6 真
    // 打开物理 RT 后的行为差异由 F6 测试覆盖)。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x20));
    fg.addResource(FgResourceId::BloomBright,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    fg.addPass({"BloomExtract",
                {FgResourceId::SceneColor},
                {FgResourceId::BloomBright},
                /*enabled=*/true});
    fg.compile();

    // livePasses = 1,logicalResources ≥ 1。
    CHECK(fg.stats().livePasses     == 1);
    CHECK(fg.stats().declaredPasses == 1);
    CHECK(fg.stats().logicalResources >= 1);

    // resolve 在未初始化 adapter 上返 invalid(F2 物理骨架阶段;
    // F6 真打开后这里返真 handle)。
    const bgfx::FrameBufferHandle bright = fg.resolve(FgResourceId::BloomBright);
    CHECK(!BGFXAdapter::isValid(bright));
}

// ─── B. Pass::execute 在 frameGraph==nullptr 时 0 draw ────────────

TEST_CASE(f2_bloomextract_execute_without_framegraph_returns_zero) {
    // 旧测试接缝兼容 ── legacy 22-field brace-init test sites 不
    // wire frameGraph,默认 nullptr;BloomExtract 应早退 0(F2
    // 字 段 加 入 PassExecContext 后,C++14 trailing-default 让旧
    // 测试零修改编译通过)。
    BloomExtractPass pass;
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

TEST_CASE(f2_bloomextract_execute_with_wired_fg_zero_bloom_returns_zero) {
    // host bloomStrength=0 ⇒ FG 不注册 BloomBright ⇒ resolve 返
    // invalid ⇒ BloomExtract 0 draw。
    BloomExtractPass pass;
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
    // 不 addPass(BloomExtract disabled by host)。
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

TEST_CASE(f2_bloomextract_execute_with_wired_fg_nonzero_bloom_still_invalid_f2) {
    // host bloomStrength>0 ⇒ FG 注册 BloomBright + BloomExtract
    // enabled ⇒ compile mark live。但 F2 物理阶段 resolve 仍
    // invalid(未初始化 adapter)⇒ BloomExtract 0 draw ── 与 F6
    // 真创建物理 RT 后的差异由 F6 测试覆盖。
    BloomExtractPass pass;
    FrameContext frame{};
    frame.bloomStrength = 0.4f;  // host 启用 bloom
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
    fg.addPass({"BloomExtract",
                {FgResourceId::SceneColor},
                {FgResourceId::BloomBright},
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
    // F2 物理骨架:即便 live 也返 invalid ⇒ 0 draw。
    CHECK(pass.execute(ctx) == 0);
}

// ─── C. BloomExtractPass 不再 own _fbo ──────────────────────────

TEST_CASE(f2_bloomextract_halfresfbo_returns_invalid_f2) {
    // F2 ── halfResFbo() getter 保留为"stateless 占位"(返 invalid),
    // 因为 consumer(BloomBlurPass)在 F3 之前仍按旧约定调它;F3
    // 会改 BloomBlurPass 走 ctx.frameGraph->resolvePingPong 并删除
    // 此 getter。本测试钉死 F2 阶段它返 invalid ── 任何把它恢复
    // 到返回 own handle 的改动都会被它抓住。
    BloomExtractPass pass;
    CHECK(!BGFXAdapter::isValid(pass.halfResFbo()));
    CHECK(pass.halfWidth()  == 0);
    CHECK(pass.halfHeight() == 0);
    // isReady() 也恒 false(F2 物理未开;F6 真打开后会再评估)。
    CHECK(pass.isReady() == false);
}

// ─── D. destroyResources 安全可调(只释放 program/VB/IB) ────────

TEST_CASE(f2_bloomextract_destroy_resources_safe) {
    // F2 ── BloomExtract 不再 own _fbo,destroyResources 不动
    // _fbo;只释放 program / VB / IB。在未初始化 adapter 上安全
    // 可调(idempotent),这让 RenderPipeline teardown 路径仍能照
    // 旧契约调它。
    BloomExtractPass pass;
    BGFXAdapter adapter;
    pass.destroyResources(adapter);  // 第一次调用
    pass.destroyResources(adapter);  // 第二次幂等
    CHECK(true);  // 没崩就过
}

// ─── E. Slot ABI 不变 ───────────────────────────────────────────

TEST_CASE(f2_render_pipeline_slot_abi_lock) {
    // F2 不动 slot ABI ── BloomExtract 仍是 8。
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.path == RenderPath::Forward);
    // S4b 后是 9 slot (CM-1 2026-08-11: +1 Forward2DOpaque),顺序
    // Shadow, FO, 2DOpaque, Trans, BloomExtract, BloomBlur,
    // DepthHaze, PostProcess, UI。
    CHECK(desc.passes.size() == 9);
    CHECK(desc.passes[4] == RenderPassSlot::BloomExtract);
    CHECK(desc.contains(RenderPassSlot::BloomExtract));
    // BloomExtract enum 值仍 = 8(append-only 锁)。
    CHECK(static_cast<uint8_t>(RenderPassSlot::BloomExtract) == 8);
}

TEST_SUITE_END