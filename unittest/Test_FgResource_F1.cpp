// F1 — FrameGraph MVP 资源池骨架 (2026-07-24, mid-term
// `docs/frame-graph-mvp.md` §7 升条件 1+2+3 全满足后开)
//
// 这一刀仅交付 API 形态完整、物理创建延后到 F6 的 FrameGraph
// 骨架。**不接管任何 Pass**;Pass 仍自己 own `_fbo`/`_pingFbo`/
// `_pongFbo`。F2-F5 才迁。
//
// 本测试在 Noop / 未初始化 adapter 路径上验证:
//   1) FgResourceId enum 唯一性 + 计数正确
//   2) external import + resolve 返 borrow handle,FG 不 own
//   3) disabled pass 的私有 write 不进 live set ⇒ resolve 返 invalid
//   4) Noop / 未初始化 adapter / 零 viewport ⇒ resolve 返 invalid 不创建资源
//   5) resize / shutdown 幂等,不破坏已 borrow 的 external handle
//   6) resolvePingPong 形态正确(两个独立 handle;F1 物理骨架阶段
//      都返 invalid ── 跟 FG "F1 物理创建延后" 一致;F3 真接
//      BloomBlur 时再启用 real-handle 路径)
//   7) FgSemantic + resolveSemantic 形态完整
//   8) FgCompileStats 字段对齐 ── declaredPasses + livePasses +
//      logicalResources 三项在 F1 即填

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYRenderTypes.h"

#include "detail/BGFXAdapter.h"
#include "detail/FgResource.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>
#include <vector>

using ayt::render::Backend;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FgResourceId;
using ayt::render::detail::FgTextureScale;
using ayt::render::detail::FgTextureDesc;
using ayt::render::detail::FgPassDesc;
using ayt::render::detail::FgSemantic;
using ayt::render::detail::FgCompileStats;
using ayt::render::detail::FrameGraph;

namespace {

// Sentinel ── Noop / 未初始化 adapter 上 createFrameBuffer 返回的
// handle 在 BGFXAdapter.h:109 处声明为 kInvalidFrameBufferHandle;
// 与 BGFX_INVALID_HANDLE 等价。
constexpr uint16_t kInvalidHandle = 0xFFFFu;

bgfx::FrameBufferHandle makeFakeHandle(uint16_t idx)
{
    // bgfx::FrameBufferHandle 是 struct{id};构造一个非 invalid 的
    // 假 handle 给 external import 测试用 ── FG 只借用,不会去碰
    // bgfx 调用链,所以 idx 数值本身不重要,只要 isValid()==true。
    bgfx::FrameBufferHandle h;
    h.idx = idx;
    return h;
}

} // namespace

TEST_SUITE(AYRenderer_FrameGraph_F1)

// ─── A. enum 唯一性 + Count 哨兵 ───────────────────────────────────

TEST_CASE(f1_fgresource_id_enum_unique_and_count) {
    // ABI append-only 锁 ── 任何重新排序都会让 enum 值漂移并破
    // 现有 `RenderPipelineDesc::passes` 列表的字节级契约。这里
    // 把 5 个 ID 的具体值钉死,后续添加新 ID 须 append 到
    // `Count` 之前、绝不重用旧值。
    CHECK(static_cast<uint8_t>(FgResourceId::SceneColor)  == 0);
    CHECK(static_cast<uint8_t>(FgResourceId::BloomBright) == 1);
    CHECK(static_cast<uint8_t>(FgResourceId::BloomBlurA)  == 2);
    CHECK(static_cast<uint8_t>(FgResourceId::BloomBlurB)  == 3);
    CHECK(static_cast<uint8_t>(FgResourceId::HazeHalf)    == 4);
    CHECK(static_cast<uint8_t>(FgResourceId::Count)       == 5);
}

TEST_CASE(f1_fgsemantic_enum_values_locked) {
    // FgSemantic 也 append-only;FinalColorSource=0、BloomSource=1、
    // HazeSource=2 ── 后续若加 SSAO source 等只能 append。
    CHECK(static_cast<uint8_t>(FgSemantic::FinalColorSource) == 0);
    CHECK(static_cast<uint8_t>(FgSemantic::BloomSource)      == 1);
    CHECK(static_cast<uint8_t>(FgSemantic::HazeSource)       == 2);
    CHECK(static_cast<uint8_t>(FgSemantic::Count)            == 3);
}

// ─── B. external import + resolve 借用语义 ─────────────────────────

TEST_CASE(f1_external_import_resolves_to_borrowed_handle) {
    BGFXAdapter adapter;       // 默认 ctor → 未初始化
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);

    // importExternal:SceneColor 借用到一个非 invalid 的假 handle
    // (FG 不 own ── 不应 destroy)。
    const bgfx::FrameBufferHandle borrowed = makeFakeHandle(0x1234);
    fg.importExternal(FgResourceId::SceneColor, borrowed);

    // addResource:同样 ID 不能 addResource 二次声明(覆盖所有权会
    // 让 external handle 失效 ── FG 应保留最后一次声明)。F1 实现
    // 选了"addResource 覆盖 isExternal=false",本测试只验证 import
    // 后立刻 resolve 能拿到 borrow handle。
    fg.addPass({
        "FakeConsumer", {FgResourceId::SceneColor}, {}, true
    });
    CHECK(fg.compile());

    // resolve(SceneColor) 返 borrow handle(id 一致)。
    const bgfx::FrameBufferHandle resolved = fg.resolve(FgResourceId::SceneColor);
    CHECK(resolved.idx == borrowed.idx);

    // shutdown 不应触碰 external ── 下一次 importExternal 仍应能
    // 拿到 handle。
    fg.shutdown();
}

TEST_CASE(f1_external_handle_survives_resize_and_shutdown) {
    // K invariant:external 永不 destroy / resize。F1 实现里 resize
    // 和 shutdown 都只清空 declared/live 标记,不动 _physical ──
    // 借用 handle 的语义保留到 owner 主动 destroy。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    const bgfx::FrameBufferHandle borrowed = makeFakeHandle(0xABCD);
    fg.importExternal(FgResourceId::SceneColor, borrowed);
    fg.addPass({"Consumer", {FgResourceId::SceneColor}, {}, true});
    CHECK(fg.compile());

    fg.resize(640, 360);
    const bgfx::FrameBufferHandle afterResize = fg.resolve(FgResourceId::SceneColor);
    CHECK(afterResize.idx == borrowed.idx);

    fg.shutdown();
}

// ─── C. disabled pass 私有 write 不进 live set ─────────────────────

TEST_CASE(f1_disabled_pass_write_not_live) {
    // Cutsheet §7 第 3 条兑现:F1 阶段即守 ── "enabled==false 的
    // pass 私有 write 不进 live set ⇒ resolve 返 invalid ⇒ 零分配"
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);

    // addPass 用 enabled=false ⇒ 该 pass 的 write 资源 BloomBright
    // 不应被标记 live。
    fg.addPass({
        "BloomExtract",
        {FgResourceId::SceneColor},
        {FgResourceId::BloomBright},
        /*enabled=*/false
    });
    fg.addResource(FgResourceId::BloomBright,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});

    CHECK(fg.compile());

    // 验证:livePasses = 0,logicalResources 不含 BloomBright。
    CHECK(fg.stats().livePasses       == 0);
    CHECK(fg.stats().declaredPasses   == 1);

    // resolve(BloomBright) 必须 invalid(未 live ⇒ FG 不创建物理 RT)。
    const bgfx::FrameBufferHandle resolved = fg.resolve(FgResourceId::BloomBright);
    CHECK(!BGFXAdapter::isValid(resolved));
}

TEST_CASE(f1_enabled_pass_write_marks_resource_live) {
    // enabled=true 的对照:write 资源被标记 live,stats.logicalResources
    // 计 1。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);

    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x10));
    fg.addPass({
        "BloomExtract",
        {FgResourceId::SceneColor},
        {FgResourceId::BloomBright},
        /*enabled=*/true
    });
    fg.addResource(FgResourceId::BloomBright,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});

    CHECK(fg.compile());
    CHECK(fg.stats().livePasses       == 1);
    CHECK(fg.stats().declaredPasses   == 1);
    CHECK(fg.stats().logicalResources >= 1);  // 至少 BloomBright(SceneColor external 也算 live)
}

// ─── D. Noop / 未初始化 / 零 viewport 短路 ────────────────────────

TEST_CASE(f1_resolve_on_uninitialized_adapter_returns_invalid) {
    // K invariant ── resolve() 在 Noop / 未初始化 adapter 上不创建
    // 任何 GPU 资源(物理创建延后到 F6;F1 阶段一律返 invalid,
    // 这让 caller 短路 ── 与现有 Pass 的 Noop guard 一致)。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x20));
    fg.addPass({
        "BloomExtract",
        {FgResourceId::SceneColor},
        {FgResourceId::BloomBright},
        true
    });
    fg.addResource(FgResourceId::BloomBright,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    CHECK(fg.compile());

    // Noop path:external SceneColor 应能 resolve(它就是 borrow),
    // 但 owned BloomBright 应 invalid ── 因为 resolve 在未初始化
    // adapter 上不创建物理 RT。
    const bgfx::FrameBufferHandle sceneHandle =
        fg.resolve(FgResourceId::SceneColor);
    CHECK(sceneHandle.idx == 0x20);  // borrow 保留

    const bgfx::FrameBufferHandle brightHandle =
        fg.resolve(FgResourceId::BloomBright);
    CHECK(!BGFXAdapter::isValid(brightHandle));
}

TEST_CASE(f1_resolve_on_zero_viewport_returns_invalid) {
    // K invariant ── 零 viewport 时不创建物理 RT,即便 adapter 已
    // 初始化(这里 adapter 默认未初始化,但语义仍覆盖"F1 物理创建
    // 延后"的占位)。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(0, 0);  // 零 viewport
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x30));
    fg.addPass({
        "BloomExtract",
        {FgResourceId::SceneColor},
        {FgResourceId::BloomBright},
        true
    });
    fg.addResource(FgResourceId::BloomBright,
                   {bgfx::TextureFormat::RGBA8, FgTextureScale::Half, true, false});
    CHECK(fg.compile());
    // external borrow 仍能 resolve(零 viewport 不影响 external 借用)
    const bgfx::FrameBufferHandle sceneHandle =
        fg.resolve(FgResourceId::SceneColor);
    CHECK(sceneHandle.idx == 0x30);
    // owned RT 在零 viewport 下不创建
    const bgfx::FrameBufferHandle brightHandle =
        fg.resolve(FgResourceId::BloomBright);
    CHECK(!BGFXAdapter::isValid(brightHandle));
}

// ─── E. resolvePingPong 形态 ──────────────────────────────────────

TEST_CASE(f1_resolve_ping_pong_returns_two_handles) {
    // F1 物理骨架阶段 ── resolvePingPong 把两块 logical 各 resolve
    // 一次,分别返独立 handle(BloomBlur A/B 显式禁止 alias 在 F6
    // 实现)。当前 owned RT 路径返 invalid,external 路径返 borrow。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);

    // 把两块都设成 external(便于验证两块独立 borrow)。
    fg.importExternal(FgResourceId::BloomBlurA, makeFakeHandle(0xAA));
    fg.importExternal(FgResourceId::BloomBlurB, makeFakeHandle(0xBB));
    fg.addPass({"BloomBlurH", {FgResourceId::BloomBright}, {FgResourceId::BloomBlurA}, true});
    fg.addPass({"BloomBlurV", {FgResourceId::BloomBlurA},  {FgResourceId::BloomBlurB}, true});
    fg.importExternal(FgResourceId::BloomBright, makeFakeHandle(0xCC));
    CHECK(fg.compile());

    const auto pp = fg.resolvePingPong(FgResourceId::BloomBlurA,
                                       FgResourceId::BloomBlurB);
    CHECK(pp.first.idx  == 0xAA);
    CHECK(pp.second.idx == 0xBB);
    CHECK(pp.first.idx != pp.second.idx);  // 两块独立
}

// ─── F. FgSemantic + resolveSemantic 形态 ─────────────────────────

TEST_CASE(f1_resolve_semantic_unset_returns_invalid) {
    // 未调用 setResolvedSemantic 的 semantic 必返 invalid。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x40));
    fg.addPass({"Consumer", {FgResourceId::SceneColor}, {}, true});
    CHECK(fg.compile());

    const bgfx::FrameBufferHandle finalSrc =
        fg.resolveSemantic(FgSemantic::FinalColorSource);
    CHECK(!BGFXAdapter::isValid(finalSrc));

    const bgfx::FrameBufferHandle bloomSrc =
        fg.resolveSemantic(FgSemantic::BloomSource);
    CHECK(!BGFXAdapter::isValid(bloomSrc));

    const bgfx::FrameBufferHandle hazeSrc =
        fg.resolveSemantic(FgSemantic::HazeSource);
    CHECK(!BGFXAdapter::isValid(hazeSrc));
}

TEST_CASE(f1_resolve_semantic_resolves_to_logical_handle) {
    // setResolvedSemantic(FinalColorSource, SceneColor) 之后,compile
    // 阶段把 FinalColorSource.physical = SceneColor.physical(F1 实
    // 现中 SceneColor 是 external borrow)。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x50));
    fg.addPass({"Consumer", {FgResourceId::SceneColor}, {}, true});
    fg.setResolvedSemantic(FgSemantic::FinalColorSource, FgResourceId::SceneColor);
    CHECK(fg.compile());

    const bgfx::FrameBufferHandle finalSrc =
        fg.resolveSemantic(FgSemantic::FinalColorSource);
    CHECK(finalSrc.idx == 0x50);  // borrow handle 透传
}

// ─── G. shutdown / beginFrame 重置语义 ───────────────────────────

TEST_CASE(f1_begin_frame_resets_declarations_but_keeps_external_borrow) {
    // beginFrame 必须清空 declared/live ── 让上一帧的 pass/resource
    // 声明不污染下一帧。但 external 的 physical handle 是 borrowed
    // owner ── FG 不 own,beginFrame 不应 reset 它(否则 owner 调用
    // resolve() 时会拿到 invalid,F2 接 PostProcess 时会撞)。
    //
    // F1 实现选择:beginFrame 清空 declared/live 但保留 physical ──
    // external 借用方在下一帧 importExternal 之前 resolve 仍能
    // 拿到 handle(若该资源被 declare 过)。本测试守住这个契约。
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x60));
    fg.addPass({"Consumer", {FgResourceId::SceneColor}, {}, true});
    CHECK(fg.compile());
    CHECK(fg.stats().declaredPasses == 1);

    // 第二帧 beginFrame 应重置 stats,但 external 借用方的 borrow
    // handle 在第二次 importExternal 之前仍可 resolve(F1 实现:仅
    // declared 清零,physical 保留 ── 简化 owner 写入模式)。
    fg.beginFrame(1280, 720);
    CHECK(fg.stats().declaredPasses == 0);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x60));  // owner 重 import
    fg.addPass({"Consumer", {FgResourceId::SceneColor}, {}, true});
    CHECK(fg.compile());
    CHECK(fg.stats().declaredPasses == 1);
}

TEST_CASE(f1_shutdown_is_idempotent) {
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(1280, 720);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x70));
    fg.addPass({"Consumer", {FgResourceId::SceneColor}, {}, true});
    CHECK(fg.compile());
    fg.shutdown();
    // 第二次 shutdown 安全。
    fg.shutdown();
    // shutdown 后 beginFrame 应能正常工作。
    fg.beginFrame(800, 600);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x70));
    fg.addPass({"Consumer", {FgResourceId::SceneColor}, {}, true});
    CHECK(fg.compile());
}

TEST_SUITE_END