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

#include "AYRenderer/RenderTypes.h"
#include "detail/RenderTargetPool.h"

#include <cstdint>
#include <memory>
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
    // Full-resolution scene-linear color after opaque depth haze. HazeHalf is
    // retained as a source-compatible alias for the stable numeric resource
    // id; new code must use HazeColor because the target is no longer half-res.
    HazeColor    = 4,
    HazeHalf     = HazeColor,
    // §A1 SSAO MVP (2026-07-24, mid-term plan `frame-graph-mvp.md`
    // §S2 / cutsheet SSAO MVP Gate) — append-only ABI value 5.
    // SSAOPass writes the full-resolution RGBA8 occlusion RT
    // (R = aoOcclusion; A = geometry coverage). Resolve stays invalid
    // unless the complete enabled GBuffer -> SSAO -> Lighting chain and a
    // non-zero sanitized strength/radius are present. Never reorder or repurpose
    // existing values. Test pin:
    //   static_cast<uint8_t>(FgResourceId::SSAOTexture) == 5
    SSAOTexture  = 5,
    // Display-referred RGBA8 output of PostProcess. PresentPass consumes this
    // current-frame result; FXAA / grading nodes may read and replace
    // the presentation source without writing the backbuffer directly.
    FinalLdrColor = 6,
    // Display-referred RGBA8 output of FXAAPass. The pass promotes
    // PresentSource to this resource only after a successful current-frame
    // submit; otherwise presentation remains on FinalLdrColor.
    FxaaColor     = 7,
    // Display-referred RGBA8 output of ColorGradingPass. The pass samples a
    // 2D strip LUT and promotes PresentSource transactionally after submit.
    ColorGradedColor = 8,
    // SMAA 1x private intermediates and final display-referred output. Edge
    // and blend targets use linear/clamp sampling because endpoint-pattern
    // decoding intentionally samples at quarter-pixel offsets. SmaaColor
    // transactionally replaces PresentSource after all three stages submit.
    SmaaEdges        = 9,
    SmaaBlendWeights = 10,
    SmaaColor        = 11,
    // Persistent display-referred TAA history/output. The physical FBO is
    // imported from TAAPass because it must survive beginFrame/compile.
    TaaColor         = 12,
    // External deferred inputs used by SSAO/TAA. They remain pass-owned, but
    // declaring them here makes temporal dependencies visible to the graph
    // instead of hiding them behind PassExecContext producer pointers.
    GBufferNormal        = 13,
    GBufferWorldPosition = 14,
    GBufferSurface       = 15,
    GBufferDepth         = 16,
    MotionVectors        = 17,
    TaaHistory           = 18,
    // Sentinel ── 测试和实现都靠它做数组大小 / 上界判断。
    Count        = 19,
};

// 纹理缩放 ── full / half / quarter。MVP 只用 full + half。
enum class FgTextureScale : uint8_t {
    Full    = 0,
    Half    = 1,
    Quarter = 2,
};

// Scene-referred color stays HDR until the final post-process pass performs
// tone mapping and gamma conversion. Lighting, bloom, and haze must share this
// format so intermediate writes cannot clamp values above 1.0.
inline constexpr bgfx::TextureFormat::Enum kHdrSceneColorFormat =
    bgfx::TextureFormat::RGBA16F;

// 资源声明 ── 描述一个 logical 资源(format / 实际尺寸 / 是否 FG own
// / 是否带 depth attachment)。`transient == false` 表示该资源由外部
// import 而非 FG 创建；FG-owned transient targets 由 Renderer 级共享池
// 分配，独立 FrameGraph 测试则使用内部池。
struct FgTextureDesc {
    bgfx::TextureFormat::Enum format   = bgfx::TextureFormat::RGBA8;
    FgTextureScale            scale    = FgTextureScale::Full;
    bool                      transient = true;
    bool                      withDepth = false;
    // Edge/mask resources must not interpolate adjacent classifications.
    bool                      pointSampled = false;
};

// Pass 声明 ── 描述一个 logical pass 读哪些资源、写哪些资源、
// 是否启用。`enabled == false` ⇒ 该 pass 的私有 write 不进 live
// set ⇒ 不分配物理 FBO(cutsheet §7 第 3 条兑现)。
struct FgPassDesc {
    const char*                       name    = nullptr;
    std::vector<FgResourceId>         reads;
    std::vector<FgResourceId>         writes;
    bool                              enabled = true;
    // A side-effect pass is an execution root even when it has no logical
    // output consumed by another graph pass. Passes with an empty writes list
    // are also treated as roots for backward compatibility (Present/Consumer).
    bool                              sideEffect = false;
    // Optional bridge to the concrete RenderPipeline slot. A negative value
    // keeps standalone/tests source-compatible and leaves execution solely to
    // RenderPipeline. Product graph nodes set this so compile-time liveness
    // becomes an actual dispatch decision rather than diagnostics only.
    int16_t                           executionSlot = -1;
};

// Compile diagnostics are deliberately structural rather than textual so
// tests and future tooling do not have to parse log strings. passIndex uses
// kFgNoPass when the error belongs to a semantic rather than a pass.
enum class FgCompileErrorCode : uint8_t {
    InvalidResourceId = 0,
    UndeclaredRead,
    UndeclaredWrite,
    ReadBeforeWrite,
    MultipleWriters,
    UndeclaredSemanticResource,
    MissingSemanticProducer,
};

inline constexpr uint16_t kFgNoPass = 0xffffu;

struct FgCompileError {
    FgCompileErrorCode code = FgCompileErrorCode::InvalidResourceId;
    uint16_t           passIndex = kFgNoPass;
    FgResourceId       resource = FgResourceId::SceneColor;
};

const char* fgCompileErrorName(FgCompileErrorCode code) noexcept;

// Semantic ── F5 接入;Final PostProcess 想知道 base color 从哪取、
// Bloom/Haze 旁路 sampler 从哪取。`Invalid` 表示该 semantic 无
// 物理资源可读(由 selectSceneColorSourceFbo / FS fallback
// 处理)。
enum class FgSemantic : uint8_t {
    FinalColorSource = 0,
    BloomSource      = 1,
    HazeSource       = 2,
    // §A1 SSAO MVP (2026-07-24) — append-only ABI value 3.
    // LightingPass reads `resolveSemantic(FgSemantic::SSAOSource)` and folds
    // AO into the ambient term only after SSAOPass reports current-frame
    // production. When the pass is absent or fails, Lighting binds a safe
    // GBuffer fallback and uploads zero strength.
    // Never reorder or repurpose existing values.
    SSAOSource       = 3,
    // Current display-referred source consumed by PresentPass. Initially
    // FinalLdrColor; later post-tonemap nodes may promote this semantic.
    PresentSource    = 4,
    Count            = 5,
};

// `physicalTargets` 是 compile 期预计的 owned 物理 FBO 数
// (F6.1：无 auto-alias ⇒ 等于 owned live 数；若将来 whitelist
// alias 则 physicalTargets ≤ logical owned)。`aliasHits` 在
// F6.1 保守策略下恒为 0。
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
    FrameGraph(BGFXAdapter& adapter, RenderTargetPool& targetPool) noexcept;
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

    // 声明一个 logical pass。`enabled==false` 的 pass 不进 live set；
    // terminal/side-effect pass 与 semantic 输出作为反向活跃性根。
    void addPass(const FgPassDesc& desc);

    // Registers the runtime eligibility of a concrete pipeline slot. Compile
    // intersects this state with graph liveness; RenderPipeline then queries
    // shouldExecute() before dispatch. This separates "mounted" (pipeline),
    // "enabled" (eligibility), and "live this frame" (compiled graph).
    void setExecutionEligibility(RenderPassSlot slot, bool enabled) noexcept;
    bool hasExecutionDecision(RenderPassSlot slot) const noexcept;
    bool shouldExecute(RenderPassSlot slot) const noexcept;

    // F5 ── 设置 semantic 指向哪个 logical 资源。FG compile 时
    // 解析成物理 handle;若该 logical 不 live ⇒ resolveSemantic 返
    // invalid。
    void setResolvedSemantic(FgSemantic sem, FgResourceId logicalId);

    // ─── compile / resolve ───────────────────────────────────────
    // 校验所有 enabled pass 的资源声明、生产者顺序和单写者契约，
    // 再从 terminal/side-effect pass 与 semantic 输出反向形成 live set。
    // 无执行终点的独立旧图保持“全部 enabled pass live”兼容行为。
    // 返回 false 时 compileErrors() 给出结构化错误，owned target 不 live。
    bool compile();

    // 拿 logical 资源的物理 handle。**首次调用触发物理 FBO 创建**
    // (lazy);后续返已缓存 handle。compile 后该资源若不在 live
    // set ⇒ 返 invalid。external 即使未 live 也可 resolve（供
    // FinalColorSource 在无效果 Pass 时仍能读 SceneColor）。
    bgfx::FrameBufferHandle resolve(FgResourceId id) const;

    // F3 ── 拿 ping-pong 对。两块资源都必须 live;否则返 {invalid,
    // invalid}。F6.1：A/B 永不 alias（各有独立物理 RT）。
    FgPingPong resolvePingPong(FgResourceId a, FgResourceId b) const;

    // F5 / F6.1 ── 拿 semantic 对应 logical 的**即时** resolve()
    // 结果（不读 compile 缓存）。BloomSource / HazeSource 在
    // Pass 已 resolve 创建 RT 后，同帧 Final 才能采到有效 handle。
    bgfx::FrameBufferHandle resolveSemantic(FgSemantic sem) const;

    // Current-frame production latch shared by every effect node. Physical
    // handles persist across frames for reuse, so consumers must not treat a
    // valid handle as proof that its producer submitted this frame.
    void markProduced(FgResourceId id) noexcept;
    bool producedThisFrame(FgResourceId id) const noexcept;
    bool semanticProducedThisFrame(FgSemantic sem) const noexcept;

    // ─── 生命周期 / 统计 ───────────────────────────────────────
    // 释放 FG-owned pool leases(external 不动)。尺寸变化时 owned RT
    // 在下次 resolve 时按新 key 获取；logical 资源声明保留。
    void resize(uint16_t width, uint16_t height);

    // 释放全部 FG-owned pool leases + 清空所有 logical 声明。重复调安全。
    void shutdown();

    const FgCompileStats& stats() const noexcept { return _stats; }
    const std::vector<FgCompileError>& compileErrors() const noexcept {
        return _compileErrors;
    }

    // 诊断 ── 测试可见,owner 不可依赖。
    bool hasAdapter() const noexcept { return _adapter != nullptr; }

private:
    BGFXAdapter*    _adapter     = nullptr;
    std::unique_ptr<RenderTargetPool> _ownedTargetPool;
    RenderTargetPool* _targetPool = nullptr;
    uint16_t        _viewportW   = 0;
    uint16_t        _viewportH   = 0;
    bool            _compiled    = false;

    FgCompileStats  _stats{};
    std::vector<FgCompileError> _compileErrors;

    // F1 仅声明形态,真正逻辑到物理映射延后到 F6。`isExternal`
    // 区分 importExternal vs addResource;`_physical` 缓存 lazy
    // 创建的 bgfx handle。
    struct ResourceEntry {
        FgTextureDesc             desc{};
        bool                      declared    = false;
        bool                      isExternal  = false;
        bool                      live        = false;
        bool                      producedThisFrame = false;
        // `mutable` 因为 resolve() 是 const 成员,但 lazy
        // create-on-first-resolve 路径必须能写 physical。
        mutable bgfx::FrameBufferHandle physical =
            bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        mutable PooledRenderTargetHandle pooled{};
        // F6 才填 ── 物理尺寸 / alias 索引
        // `mutable`: resolve() is const but may fill size on first
        // lazy create when compile left zeros (defensive).
        mutable uint16_t              physicalW   = 0;
        mutable uint16_t              physicalH   = 0;
        // F6.1 ── unique per owned live RT (no auto-alias). Shared
        // groups reserved for a future explicit whitelist.
        int16_t                       aliasGroup  = -1;
    };
    ResourceEntry _resources[static_cast<size_t>(FgResourceId::Count)];

    struct PassEntry {
        FgPassDesc                  desc{};
        bool                        declared = false;
        bool                        live     = false;
    };
    std::vector<PassEntry> _passes;

    static constexpr size_t kExecutionSlotCount = 256;
    bool _executionTracked[kExecutionSlotCount]{};
    bool _executionEligible[kExecutionSlotCount]{};
    bool _executionLive[kExecutionSlotCount]{};

    // F5 ── 每个 semantic 指向哪个 logical。physical 字段保留但
    // F6.1 起不再由 compile 缓存 —— resolveSemantic() 直接
    // resolve(logical)，避免 owned RT lazy-create 后 Final 读到
    // 过期 invalid handle。
    struct SemanticEntry {
        bool                       hasLogical = false;
        FgResourceId               logical    = FgResourceId::SceneColor;
        bgfx::FrameBufferHandle    physical   = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    };
    SemanticEntry _semantics[static_cast<size_t>(FgSemantic::Count)];
};

} // namespace ayt::render::detail
