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

// §F6.1 (2026-07-24 hotfix) — MVP alias policy = **never share**.
// Pre-hotfix F6 auto-aliased any same-shape pair except BloomBlur
// A↔B, which incorrectly grouped BloomBright + A + B into one
// group. resolve() also ignored aliasGroup, so the bug was latent
// until someone "fixed" sharing. Conservative ship: each owned
// live resource gets a unique aliasGroup; aliasHits stays 0.
// Future cutsheets may add an explicit whitelist once interval
// analysis exists.

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

    // 2) Logical resources 统计 + owned live 物理尺寸 / 独立
    //    aliasGroup（F6.1：永不共享）。
    int16_t nextAliasGroup = 0;
    for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
        ResourceEntry& r = _resources[i];
        if (r.live || r.isExternal) {
            ++_stats.logicalResources;
        }
        if (r.isExternal || !r.live) {
            continue;
        }
        r.physicalW = scaledDim(_viewportW, r.desc.scale);
        r.physicalH = scaledDim(_viewportH, r.desc.scale);
        // Unique group per owned live RT — no auto-alias (F6.1).
        r.aliasGroup = nextAliasGroup;
        ++nextAliasGroup;
        ++_stats.physicalTargets;
    }
    // aliasHits stays 0 under the conservative policy.

    // 3) Semantic 只锁 logical 映射。physical 不在 compile 缓存
    //    （F6.1 hotfix）：owned RT 是 resolve() lazy create 的，
    //    compile 时拷贝 r.physical 会让 Final 首帧采到 invalid。
    //    resolveSemantic() 改走 resolve(logical)。
    for (size_t i = 0; i < static_cast<size_t>(FgSemantic::Count); ++i) {
        _semantics[i].physical =
            bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
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
    if (_adapter == nullptr) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    // External borrow first — SceneColor may be imported for
    // FinalColorSource without any enabled effect Pass reading it,
    // so `live` can still be false. Never create/destroy externals.
    if (r.isExternal) {
        return r.physical;
    }
    if (!r.live) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    // owned transient: lazy create-on-first-resolve.
    // Noop / 未初始化 / 零 viewport ⇒ 不创建。
    if (!_adapter->isInitialized() || _adapter->isNoopBackend()) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    if (_viewportW == 0 || _viewportH == 0) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    uint16_t w = r.physicalW;
    uint16_t h = r.physicalH;
    if (w == 0 || h == 0) {
        w = scaledDim(_viewportW, r.desc.scale);
        h = scaledDim(_viewportH, r.desc.scale);
        r.physicalW = w;
        r.physicalH = h;
    }
    // F6.1: each owned live RT has a unique aliasGroup — create
    // per logical entry. If a future whitelist shares groups,
    // scan for an existing valid handle in the same group first.
    if (!BGFXAdapter::isValid(r.physical) && r.aliasGroup >= 0) {
        for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
            const ResourceEntry& other = _resources[i];
            if (other.aliasGroup == r.aliasGroup
                && BGFXAdapter::isValid(other.physical)) {
                r.physical = other.physical;
                break;
            }
        }
    }
    if (!BGFXAdapter::isValid(r.physical)) {
        r.physical = _adapter->createFrameBuffer(
            w, h, r.desc.format, r.desc.withDepth);
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
    // F6.1 hotfix — never return compile-time cached physical.
    // Owned Bloom/Haze RTs are created in Pass::execute via
    // resolve(); Final must see those same handles same-frame.
    if (!s.hasLogical) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    return resolve(s.logical);
}

// ─── 生命周期 ─────────────────────────────────────────────────

void FrameGraph::resize(uint16_t width, uint16_t height)
{
    _viewportW = width;
    _viewportH = height;
    // §F6 / F6.1 ── 集中 resize: destroy owned RT once per unique
    // handle (defensive if a future alias whitelist shares).
    // External 不动。下一帧 compile + resolve lazy recreate。
    if (_adapter != nullptr && _adapter->isInitialized()) {
        for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
            ResourceEntry& r = _resources[i];
            if (r.isExternal || !BGFXAdapter::isValid(r.physical)) {
                continue;
            }
            const bgfx::FrameBufferHandle handle = r.physical;
            _adapter->destroy(handle);
            for (size_t j = 0; j < static_cast<size_t>(FgResourceId::Count); ++j) {
                ResourceEntry& other = _resources[j];
                if (!other.isExternal
                    && BGFXAdapter::isValid(other.physical)
                    && other.physical.idx == handle.idx) {
                    other.physical  = BGFX_INVALID_HANDLE;
                    other.physicalW = 0;
                    other.physicalH = 0;
                }
            }
        }
    }
}

void FrameGraph::shutdown()
{
    // §F6 / F6.1 ── 释放所有 FG owned RT（unique handle once）;
    // external 不动。
    if (_adapter != nullptr && _adapter->isInitialized()) {
        for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
            ResourceEntry& r = _resources[i];
            if (r.isExternal || !BGFXAdapter::isValid(r.physical)) {
                continue;
            }
            const bgfx::FrameBufferHandle handle = r.physical;
            _adapter->destroy(handle);
            for (size_t j = 0; j < static_cast<size_t>(FgResourceId::Count); ++j) {
                ResourceEntry& other = _resources[j];
                if (!other.isExternal
                    && BGFXAdapter::isValid(other.physical)
                    && other.physical.idx == handle.idx) {
                    other.physical  = BGFX_INVALID_HANDLE;
                    other.physicalW = 0;
                    other.physicalH = 0;
                }
            }
        }
    }
    for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
        ResourceEntry& r = _resources[i];
        r.physical   = BGFX_INVALID_HANDLE;
        r.physicalW  = 0;
        r.physicalH  = 0;
        r.declared   = false;
        r.live       = false;
        r.isExternal = false;
        r.aliasGroup = -1;
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
