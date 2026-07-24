#pragma once

// F1 — FrameGraph MVP 资源池骨架 (2026-07-24, mid-term cutsheet
// `docs/frame-graph-mvp.md` §7 升条件 1+2+3 全满足后开)
//
// 范围(抄 cutsheet §2 红线):
//   只迁 Lighting 之后 → Final 这段(BloomExtract / BloomBlur /
//   DepthHaze / Final PostProcess)。Shadow / Skybox / GBuffer /
//   Lighting / ForwardOpaque / Transparent / UI 不动。
//
// 这一刀(F1)只交付:
//   - FgResourceId enum(cutsheet §3 资源 ID = enum,不裸字符串)
//   - FgTextureDesc POD(format / scale / transient / withDepth)
//   - FgPassDesc POD(name / reads / writes / enabled)
//   - FgCompileStats POD(用于 F6 compile 摘要)
//   - FgSemantic enum(F5 接入 Final source 解析)
//   - FrameGraph 类骨架:
//       beginFrame / importExternal / addResource / addPass /
//       setResolvedSemantic / compile / resolve / resolvePingPong /
//       resolveSemantic / resize / shutdown / stats
//
// 这一刀**不接管任何 Pass** ── FG 与现有 RenderPipeline / PassExecContext
// 并存,RenderPipeline.executeAll 仍然按 slot 顺序 dispatch,Pass
// 仍然自己 own `_fbo`/`_pingFbo`/`_pongFbo`(F2-F5 才迁)。这保证 F1
// 是纯增量、3-run stable、最小风险。
//
// 建图 + compile 唯一地点 = `AYRenderer::render()`(F2 起生效);
// F1 不接线任何 render(),只让 FrameGraph 类可独立编译/测试。
//
// K 不变量(F1 即可在测试中守):
//   - Noop / adapter 未初始化 / 零 viewport ⇒ resolve() 返 invalid 不创建
//   - disabled pass 的私有 write 不进 live set ⇒ resolve() 返 invalid
//   - external 永不 destroy / resize / shutdown
//   - 物理 FBO create-on-first-resolve(F6 才加 alias 决策,F1 仅 lazy)

#include <bgfx/bgfx.h>

#include <cstdint>
#include <vector>

namespace ayt::render::detail
{

// Forward declaration — FrameGraph 引用 BGFXAdapter 但头文件不该
// 强 include(若 TU 仅持有 FrameGraph 指针就用得上 forward decl)。
// 实际实现文件 FgResource.cpp 才 include BGFXAdapter.h。
class BGFXAdapter;

// 资源 ID ── cutsheet §3 "命名资源"。enum 不是字符串,防拼写
// 错误、零运行时查表、所有权清晰。append-only,后续若需新增 ID
// 加到 `Count` 之前(ABI 锁)。
enum class FgResourceId : uint8_t {
    SceneColor   = 0,
    BloomBright  = 1,
    BloomBlurA   = 2,
    BloomBlurB   = 3,
    HazeHalf     = 4,
    // Sentinel ── 测试和实现都靠它做数组大小 / 上界判断。
    Count        = 5,
};

// 纹理缩放 ── full / half / quarter。MVP 只用 full + half。
enum class FgTextureScale : uint8_t {
    Full    = 0,
    Half    = 1,
    Quarter = 2,
};

// 资源声明 ── 描述一个 logical 资源(format / 实际尺寸 / 是否 FG own
// / 是否带 depth attachment)。`transient == false` 表示该资源由外部
// import 而非 FG 创建(MVP 不会用 ── 留口子给未来 RT pool 共享)。
struct FgTextureDesc {
    bgfx::TextureFormat::Enum format   = bgfx::TextureFormat::RGBA8;
    FgTextureScale            scale    = FgTextureScale::Full;
    bool                      transient = true;
    bool                      withDepth = false;
};

// Pass 声明 ── 描述一个 logical pass 读哪些资源、写哪些资源、
// 是否启用。`enabled == false` ⇒ 该 pass 的私有 write 不进 live
// set ⇒ 不分配物理 FBO(cutsheet §7 第 3 条兑现)。
struct FgPassDesc {
    const char*                       name    = nullptr;
    std::vector<FgResourceId>         reads;
    std::vector<FgResourceId>         writes;
    bool                              enabled = true;
};

// Semantic ── F5 接入;Final PostProcess 想知道 base color 从哪取、
// Bloom/Haze 旁路 sampler 从哪取。`Invalid` 表示该 semantic 无
// 物理资源可读(由 PostProcessPass::selectSourceFbo / FS fallback
// 处理)。
enum class FgSemantic : uint8_t {
    FinalColorSource = 0,
    BloomSource      = 1,
    HazeSource       = 2,
    Count            = 3,
};

// Compile 摘要 ── F6 接入;F1 字段已定但统计只填 0。`declaredPasses`
// 是声明数,`livePasses` 是 enabled + 被消费链命中的数,`physicalTargets`
// 是 FG 实际创建的物理 FBO 数(若 alias 命中则 < logicalResources)。
struct FgCompileStats {
    uint16_t declaredPasses   = 0;
    uint16_t livePasses       = 0;
    uint16_t logicalResources = 0;
    uint16_t physicalTargets  = 0;
    uint16_t aliasHits        = 0;
};

// Ping-pong pair ── F3 接入;BloomBlur H/V 两段共享资源对。
struct FgPingPong {
    bgfx::FrameBufferHandle first;
    bgfx::FrameBufferHandle second;
};

// FrameGraph 类 ── FG MVP 核心。F1 仅 API 形态完整,物理创建 + alias
// 决策延后到 F6。本类不是线程安全的(render thread 调用即可,与
// RenderPipeline 约定一致)。
//
// 生命周期:
//   1. ctor(BGFXAdapter&) ── 一次性
//   2. 每帧 render():
//      fg.beginFrame(w, h);
//      fg.importExternal(SceneColor, ...);
//      fg.addResource(...);  // 视 enabled 决定是否调用
//      fg.addPass(...);
//      fg.setResolvedSemantic(...);
//      fg.compile();
//      pipeline.executeAll(ctx);   // Pass 内 fg.resolve / resolvePingPong
//   3. resize() ── 集中调 fg.resize(w, h)
//   4. shutdown() ── 释放所有 FG owned RT
class FrameGraph final {
public:
    explicit FrameGraph(BGFXAdapter& adapter) noexcept;
    ~FrameGraph();

    FrameGraph(const FrameGraph&) = delete;
    FrameGraph& operator=(const FrameGraph&) = delete;

    // ─── 帧头 / 帧尾 ─────────────────────────────────────────────
    // 每帧调用一次。清空上一帧的 resource / pass / semantic 声明;
    // 重置 availability / stats。物理 owned RT 保留到 resize 或
    // shutdown。
    void beginFrame(uint16_t width, uint16_t height);

    // ─── 声明 ──────────────────────────────────────────────────
    // 把一个外部 FBO 标记为某 logical 资源的物理来源。external RT
    // FG 不 own ── 不 destroy / 不 resize(由其 owner 管)。
    void importExternal(FgResourceId id, bgfx::FrameBufferHandle handle);

    // 声明一个 logical 资源。同一 ID 重复声明保留最后一次(用于
    // resize 后重新声明 format / size 变化)。`enabled==false` 的
    // pass 不会调它。
    void addResource(FgResourceId id, const FgTextureDesc& desc);

    // 声明一个 logical pass。`enabled==false` 的 pass 不进 live set。
    void addPass(const FgPassDesc& desc);

    // F5 ── 设置 semantic 指向哪个 logical 资源。FG compile 时
    // 解析成物理 handle;若该 logical 不 live ⇒ resolveSemantic 返
    // invalid。
    void setResolvedSemantic(FgSemantic sem, FgResourceId logicalId);

    // ─── compile / resolve ───────────────────────────────────────
    // 按依赖关系形成 live set;disabled pass 的 write 裁掉;F1 不
    // 做 alias(F6 才做)。返回 true 表示 compile 成功(false 仅当
    // addPass 后忘了 compile 或 logical ID 引用了未声明的资源)。
    bool compile();

    // 拿 logical 资源的物理 handle。**首次调用触发物理 FBO 创建**
    // (lazy);后续返已缓存 handle。compile 后该资源若不在 live
    // set ⇒ 返 invalid。
    bgfx::FrameBufferHandle resolve(FgResourceId id) const;

    // F3 ── 拿 ping-pong 对。两块资源都必须 live;否则返 {invalid,
    // invalid}。两块间 alias 由 F6 决定(MVP 默认不 alias ──
    // BloomBlur A/B 显式禁止)。
    FgPingPong resolvePingPong(FgResourceId a, FgResourceId b) const;

    // F5 ── 拿 semantic 解析后的物理 handle。`FinalColorSource` 永
    // 远 = full-res SceneColor(由 importExternal 提供);BloomSource /
    // HazeSource 旁路 sampler,引用对应 RT(若未 live 返 invalid)。
    bgfx::FrameBufferHandle resolveSemantic(FgSemantic sem) const;

    // ─── 生命周期 / 统计 ───────────────────────────────────────
    // 只 destroy FG owned RT(external 不动)。尺寸变化时 owned RT
    // 重建;logical 资源声明保留。
    void resize(uint16_t width, uint16_t height);

    // 释放全部 FG owned RT + 清空所有 logical 声明。重复调安全。
    void shutdown();

    const FgCompileStats& stats() const noexcept { return _stats; }

    // 诊断 ── 测试可见,owner 不可依赖。
    bool hasAdapter() const noexcept { return _adapter != nullptr; }

private:
    BGFXAdapter*    _adapter     = nullptr;
    uint16_t        _viewportW   = 0;
    uint16_t        _viewportH   = 0;
    bool            _compiled    = false;

    FgCompileStats  _stats{};

    // F1 仅声明形态,真正逻辑到物理映射延后到 F6。`isExternal`
    // 区分 importExternal vs addResource;`_physical` 缓存 lazy
    // 创建的 bgfx handle。
    struct ResourceEntry {
        FgTextureDesc             desc{};
        bool                      declared    = false;
        bool                      isExternal  = false;
        bool                      live        = false;
        // `mutable` 因为 resolve() 是 const 成员,但 lazy
        // create-on-first-resolve 路径必须能写 physical。
        mutable bgfx::FrameBufferHandle physical =
            bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        // F6 才填 ── 物理尺寸 / alias 索引
        uint16_t                  physicalW   = 0;
        uint16_t                  physicalH   = 0;
        // F6 才填 ── 与哪个 logical alias 到同一物理 RT
        int16_t                   aliasGroup  = -1;
    };
    ResourceEntry _resources[static_cast<size_t>(FgResourceId::Count)];

    struct PassEntry {
        FgPassDesc                  desc{};
        bool                        declared = false;
        bool                        live     = false;
    };
    std::vector<PassEntry> _passes;

    // F5 ── 每个 semantic 指向哪个 logical;hasLogical=false 表示
    // 该 semantic 无可解析源(resolveSemantic 返 invalid)。logical
    // 字段在 hasLogical=true 时才有意义。
    struct SemanticEntry {
        bool                       hasLogical = false;
        FgResourceId               logical    = FgResourceId::SceneColor;
        bgfx::FrameBufferHandle    physical   = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    };
    SemanticEntry _semantics[static_cast<size_t>(FgSemantic::Count)];
};

} // namespace ayt::render::detail