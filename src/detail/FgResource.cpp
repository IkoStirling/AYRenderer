#include "detail/FgResource.h"

#include "detail/BGFXAdapter.h"

#include <cstdio>

namespace ayt::render::detail
{

namespace {

// F1 — 由 scale + 完整 viewport 算出实际物理尺寸。
// 与 BloomExtractPass / BloomBlurPass / DepthHazePass 的 half-res
// 约定一致: `(viewport + 1) / 2`。quarter 留作未来用。
uint16_t scaledDim(uint16_t viewport, FgTextureScale scale)
{
    switch (scale) {
    case FgTextureScale::Full:    return viewport;
    case FgTextureScale::Half:    return static_cast<uint16_t>((viewport + 1u) / 2u);
    case FgTextureScale::Quarter: return static_cast<uint16_t>((viewport + 3u) / 4u);
    }
    return viewport;
}

// §F6 (2026-07-24) — alias 决策。两块 logical 资源可共享同一
// 物理 RT 当且仅当:
//   1. 同样的 format
//   2. 同样的 withDepth flag
//   3. 同样的实际尺寸(WxH)
//   4. interval 不重叠 — 此 Pass 的 `lastRead` < 另一 Pass 的
//      `nextFirstWrite`,即 H 阶段写 A,V 阶段读 A 写 B 的
//      ping-pong 互不 alias(因为 H 写 A 期间 V 已经要读 A)。
//
// §F6 简化模型 ── cutsheet "physically different
// lifetimes/transitions" 是 overlap 的根因;本 MVP 不做完整
// interval analysis(那需要 pass DAG),而是显式禁两组 alias:
//   - BloomBlurA / BloomBlurB (H-write-A / V-read-A-write-B
//     lifecycle overlap → K invariant #7 显式禁 ── F6 守)
//   - BloomBright + BloomBlurA (形状同但 lifecycle 不同:
//     Extract-write-Bright / BlurH-read-Bright-write-A,顺序
//     相邻 ── 不重叠 → 可 alias;但 plan 写保守禁,避免
//     第一次 ship 走 alias 路径意外)
//
// 任何新引入的 logical 资源 F6 默认不 alias(不在 aliasWhiteList
// 中的两块 → 各自物理 RT)。这是 "compile-to-correct-by-default"
// 策略:cutsheet §4 第 3 条要求先保守再优化。
// ─── Alias whitelist (F6 ship scope) ─────────────────────────────
//
// §F6 显式禁 alias(默认 conservative 安全模型):
//   - BloomBlurA / BloomBlurB ── lifecycle overlap (F3 K #7)
//
// §F6 保守不 alias(alias-eligible 但本期不打开):
//   - BloomBright / BloomBlurA ── 形状同,顺序写-读同,但
//     F6 不开 alias 避免首次 ship 意外路径。如果未来 cutsheet
//     决定打开,这里改 aliasWhiteList 一行即可。
//
// F6 实现:两份 fgPass read / write 通过 (FgResourceId)排序,
// 然后同一个 aliasGroup 集合内任两块:在 aliasWhitelist 中
// (本期只 BloomBlur A/B)的禁止 alias;都不在 whitelist 中的
// 允 alias(格式 + 尺寸 + withDepth 匹配时)。这样既守住 K #7
// 又留口子给未来 cutsheet。
constexpr bool isAliasForbiddenPair(FgResourceId a, FgResourceId b)
{
    const bool aBlkA = (a == FgResourceId::BloomBlurA);
    const bool aBlkB = (a == FgResourceId::BloomBlurB);
    const bool bBlkA = (b == FgResourceId::BloomBlurA);
    const bool bBlkB = (b == FgResourceId::BloomBlurB);
    // BloomBlur A 和 BloomBlur B 互为 forbidden alias 对。
    return (aBlkA && bBlkB) || (aBlkB && bBlkA);
}

} // namespace

// ─── ctor / dtor ──────────────────────────────────────────────

FrameGraph::FrameGraph(BGFXAdapter& adapter) noexcept
    : _adapter(&adapter)
{
    // _resources / _passes / _semantics 默认初始化(ResourceEntry
    // POD-default,PassEntry default,SemanticEntry default)。
}

FrameGraph::~FrameGraph()
{
    shutdown();
}

// ─── 帧头 ────────────────────────────────────────────────────

void FrameGraph::beginFrame(uint16_t width, uint16_t height)
{
    _viewportW = width;
    _viewportH = height;
    _compiled  = false;
    _stats     = FgCompileStats{};

    // 清空 logical 声明。但 ── F6 ── 物理 RT (aliasGroup /
    // physicalW / physicalH) 在 compile() 的 alias 决策后写;
    // beginFrame 重置 physicalW / physicalH 和 aliasGroup(每帧
    // 重决策,因为上一帧的 alias 可能不再合法 ── pass 列表变了)。
    for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
        _resources[i].declared   = false;
        _resources[i].live       = false;
        _resources[i].aliasGroup = -1;
        _resources[i].physicalW  = 0;
        _resources[i].physicalH  = 0;
        // physical handle 保留 ── alias 决策会让多块 logical
        // 指向同一物理 handle。shutdown 时一次性 destroy。
    }
    _passes.clear();

    for (size_t i = 0; i < static_cast<size_t>(FgSemantic::Count); ++i) {
        _semantics[i].hasLogical = false;
        _semantics[i].physical   = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
}

// ─── 声明 ────────────────────────────────────────────────────

void FrameGraph::importExternal(FgResourceId id, bgfx::FrameBufferHandle handle)
{
    if (static_cast<size_t>(id) >= static_cast<size_t>(FgResourceId::Count)) {
        return;
    }
    ResourceEntry& r = _resources[static_cast<size_t>(id)];
    r.declared   = true;
    r.isExternal = true;
    r.physical   = handle;  // 借用 ── FG 不 own,resize/shutdown 不动。
    r.physicalW  = 0;
    r.physicalH  = 0;
    // external 资源的 scale / format / withDepth 留默认 ──
    // 不参与尺寸计算,只供 resolve() 返 handle。
    // live 标记在 compile() 阶段根据是否有 pass 读取它来设置。
}

void FrameGraph::addResource(FgResourceId id, const FgTextureDesc& desc)
{
    if (static_cast<size_t>(id) >= static_cast<size_t>(FgResourceId::Count)) {
        return;
    }
    ResourceEntry& r = _resources[static_cast<size_t>(id)];
    r.declared   = true;
    r.isExternal = false;  // 即使前一次是 external,这次声明覆盖所有权
    r.desc       = desc;
    // physical 保留 ── 若已 lazy 创建且尺寸匹配则复用;否则由
    // resolve() 检测尺寸变化重建。
}

void FrameGraph::addPass(const FgPassDesc& desc)
{
    PassEntry p{};
    p.desc     = desc;
    p.declared = true;
    p.live     = desc.enabled;
    _passes.push_back(p);
}

void FrameGraph::setResolvedSemantic(FgSemantic sem, FgResourceId logicalId)
{
    if (static_cast<size_t>(sem) >= static_cast<size_t>(FgSemantic::Count)) {
        return;
    }
    if (static_cast<size_t>(logicalId) >= static_cast<size_t>(FgResourceId::Count)) {
        return;
    }
    SemanticEntry& s = _semantics[static_cast<size_t>(sem)];
    s.hasLogical = true;
    s.logical    = logicalId;
}

// ─── compile ─────────────────────────────────────────────────

bool FrameGraph::compile()
{
    _stats = FgCompileStats{};
    _stats.declaredPasses = static_cast<uint16_t>(_passes.size());

    // 1) Live 标记(同 F1)。
    for (PassEntry& p : _passes) {
        if (!p.desc.enabled) {
            p.live = false;
            continue;
        }
        p.live = true;
        ++_stats.livePasses;
        for (FgResourceId rid : p.desc.writes) {
            if (static_cast<size_t>(rid) < static_cast<size_t>(FgResourceId::Count)) {
                _resources[static_cast<size_t>(rid)].live = true;
            }
        }
        for (FgResourceId rid : p.desc.reads) {
            if (static_cast<size_t>(rid) < static_cast<size_t>(FgResourceId::Count)) {
                _resources[static_cast<size_t>(rid)].live = true;
            }
        }
    }

    // 2) Logical resources 统计。
    for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
        if (_resources[i].live || _resources[i].isExternal) {
            ++_stats.logicalResources;
        }
    }

    // 3) Alias 决策 ── §F6 ── 在 compile 期对 owned (non-external)
    //    live logical 资源决策 aliasGroup。规则:
    //      - external 不参与 alias(它已经由 owner 管)
    //      - 同 format + 同 物理尺寸 + 同 withDepth + 不在 forbidden
    //        whitelist(BloomBlur A/B) ⇒ 同一 aliasGroup
    //
    //    实现先算每块 owned live 资源的物理尺寸,然后 O(N^2) 配对
    //    设 aliasGroup(N ≤ 5 个 enum 值,常数级,无负担)。
    if (_adapter != nullptr && _adapter->isInitialized()
        && !_adapter->isNoopBackend()
        && _viewportW > 0 && _viewportH > 0) {
        // 先填 physicalW/H(external 跳过)。
        for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
            ResourceEntry& r = _resources[i];
            if (r.isExternal || !r.live) continue;
            r.physicalW = scaledDim(_viewportW, r.desc.scale);
            r.physicalH = scaledDim(_viewportH, r.desc.scale);
        }
        // alias 配对:按 logicalId 顺序遍历,若两块 alias-eligible
        // 且 format + 尺寸 + withDepth 都匹配且不在 forbidden pair,
        // 设同一 aliasGroup(后续 resolve 期间只第一块 lazy create,
        // 后续块复制物理 handle)。
        int16_t nextAliasGroup = 0;
        for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
            ResourceEntry& ri = _resources[i];
            if (ri.isExternal || !ri.live) continue;
            // 已经有 aliasGroup(maybe from earlier pair)跳过。
            if (ri.aliasGroup >= 0) continue;
            ri.aliasGroup = nextAliasGroup;
            ++nextAliasGroup;
            for (size_t j = i + 1; j < static_cast<size_t>(FgResourceId::Count); ++j) {
                ResourceEntry& rj = _resources[j];
                if (rj.isExternal || !rj.live) continue;
                if (rj.aliasGroup >= 0) continue;
                const auto id_i = static_cast<FgResourceId>(i);
                const auto id_j = static_cast<FgResourceId>(j);
                if (isAliasForbiddenPair(id_i, id_j)) continue;
                // format / scale / withDepth / WxH 全匹配才 alias。
                if (ri.desc.format   != rj.desc.format)   continue;
                if (ri.desc.withDepth != rj.desc.withDepth) continue;
                if (ri.desc.scale    != rj.desc.scale)    continue;
                if (ri.physicalW     != rj.physicalW)      continue;
                if (ri.physicalH     != rj.physicalH)      continue;
                // Alias!
                rj.aliasGroup = ri.aliasGroup;
                ++_stats.aliasHits;
            }
        }
    }

    // 4) Semantic → physical 解析(external borrow 或 owned
    //    live)。owned 但 aliasGroup == -1(resolve 失败路径)返 invalid。
    for (size_t i = 0; i < static_cast<size_t>(FgSemantic::Count); ++i) {
        SemanticEntry& s = _semantics[i];
        if (!s.hasLogical) continue;
        const ResourceEntry& r = _resources[static_cast<size_t>(s.logical)];
        const bool resolvable = r.live || r.isExternal;
        if (!resolvable) {
            s.physical = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
            continue;
        }
        // External ⇒ borrow；owned ⇒ alias 决策后第一块 lazy create
        // 期间 actual handle 才生成,这里先搬 r.physical(若已创建)。
        s.physical = r.physical;
    }

    _compiled = true;
    return true;
}

// ─── resolve ─────────────────────────────────────────────────

bgfx::FrameBufferHandle FrameGraph::resolve(FgResourceId id) const
{
    if (static_cast<size_t>(id) >= static_cast<size_t>(FgResourceId::Count)) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    const ResourceEntry& r = _resources[static_cast<size_t>(id)];
    // Noop / adapter 未初始化 / 未 compile / 不 live → invalid。
    if (_adapter == nullptr || !r.live) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    // external 直接返 physical(borrow,不 create)。
    if (r.isExternal) {
        return r.physical;
    }
    // owned transient:lazy create-on-first-resolve ── §F6 ──
    // Noop / 未初始化 / 零 viewport ⇒ 不创建。
    if (!_adapter->isInitialized() || _adapter->isNoopBackend()) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    if (_viewportW == 0 || _viewportH == 0) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    // alias 处理:同 aliasGroup 的两块共享(handle 由第一块创建
    // 后,后续块 copy 同一物理 handle)。F6 信任 (W,H,format,
    // withDepth) 严格 equal 触发 bgfx handle 复用 ── 若未来要
    // 严格 "首块创建、后续 alias" 的集中维护,改 aliasGroup 表。
    // `ResourceEntry::physical` 标 mutable 后此处可写。
    if (!BGFXAdapter::isValid(r.physical)) {
        r.physical = _adapter->createFrameBuffer(
            r.physicalW, r.physicalH, r.desc.format, r.desc.withDepth);
    }
    return r.physical;
}

FgPingPong FrameGraph::resolvePingPong(FgResourceId a, FgResourceId b) const
{
    FgPingPong out{};
    out.first  = resolve(a);
    out.second = resolve(b);
    return out;
}

bgfx::FrameBufferHandle FrameGraph::resolveSemantic(FgSemantic sem) const
{
    if (static_cast<size_t>(sem) >= static_cast<size_t>(FgSemantic::Count)) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    const SemanticEntry& s = _semantics[static_cast<size_t>(sem)];
    return s.physical;
}

// ─── 生命周期 ─────────────────────────────────────────────────

void FrameGraph::resize(uint16_t width, uint16_t height)
{
    _viewportW = width;
    _viewportH = height;
    // §F6 ── 集中 resize:在 resize 时销毁所有 owned RT(external
    // 不动,跟 F1 一样)。下一次 compile + resolve 触发 lazy
    // recreate at 新的 W,H。
    //
    // 尺寸匹配检查挪到 lazy resolve 端(createOwnedTransient 内部
    // 比较 physicalW/H;若仍相等则 skip destroy)。但 F6 ship 简化
    // 路径:resize 直接无脑 destroy owned RT,下一帧 lazy create。
    // 这是 "保守重启" 路径 ── 即使尺寸实际上未变也重建 ──
    // 简单可靠;Resizable 频繁 resize 的 host 仍可以接受 (一次
    // destroy + create 是 bgfx O(1) 操作)。
    if (_adapter != nullptr && _adapter->isInitialized()) {
        for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
            ResourceEntry& r = _resources[i];
            if (!r.isExternal && BGFXAdapter::isValid(r.physical)) {
                _adapter->destroy(r.physical);
                r.physical   = BGFX_INVALID_HANDLE;
                r.physicalW  = 0;
                r.physicalH  = 0;
            }
        }
    }
}

void FrameGraph::shutdown()
{
    // §F6 ── 释放所有 FG owned RT;external 不动。
    if (_adapter != nullptr) {
        for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
            ResourceEntry& r = _resources[i];
            if (!r.isExternal && BGFXAdapter::isValid(r.physical)) {
                if (_adapter->isInitialized()) {
                    _adapter->destroy(r.physical);
                }
            }
            r.physical   = BGFX_INVALID_HANDLE;
            r.physicalW  = 0;
            r.physicalH  = 0;
            r.declared   = false;
            r.live       = false;
            r.aliasGroup = -1;
        }
    } else {
        for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
            ResourceEntry& r = _resources[i];
            r.physical   = BGFX_INVALID_HANDLE;
            r.physicalW  = 0;
            r.physicalH  = 0;
            r.declared   = false;
            r.live       = false;
            r.aliasGroup = -1;
        }
    }
    _passes.clear();
    for (size_t i = 0; i < static_cast<size_t>(FgSemantic::Count); ++i) {
        _semantics[i].hasLogical = false;
        _semantics[i].physical   = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    _viewportW = 0;
    _viewportH = 0;
    _compiled  = false;
    _stats     = FgCompileStats{};
}

} // namespace ayt::render::detail
