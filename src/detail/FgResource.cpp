#include "detail/FgResource.h"

#include "detail/BGFXAdapter.h"

#include <cstdio>

namespace ayt::render::detail
{

namespace {

// F1 ── 由 scale + 完整 viewport 算出实际物理尺寸。
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

    // 清空 logical 声明。owned RT 保留(物理尺寸在 resize() 时
    // 处理,这里不动 GPU handle ── lazy create-on-resolve)。
    for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
        _resources[i].declared = false;
        _resources[i].live     = false;
        // _resources[i].physical 保留 ── owned RT 不每帧 destroy。
        // external 的 physical 在 importExternal 时被覆盖。
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
    p.live     = desc.enabled;  // 预占位;compile 时可能根据
                                  // read-producer 是否 live 反向
                                  // 再核一次(目前 cutsheet 简单
                                  // 模型:enabled 决定)。
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
    // physical 在 compile 阶段根据 logical.physical 填。
}

// ─── compile ─────────────────────────────────────────────────

bool FrameGraph::compile()
{
    _stats = FgCompileStats{};
    _stats.declaredPasses = static_cast<uint16_t>(_passes.size());

    // F1 简单裁剪:enabled==false 的 pass 直接不进 live set;其
    // 私有 write 资源也不被消费链标记为 live(即便有别的 pass 写
    // ── 因为 cutsheet MVP 不做拓扑排序,假定顺序由 addPass 顺序
    // 决定 ── 也只把 enabled pass 的 write 资源视作"产生者")。
    //
    // 真正 live 标记:enabled pass 的 write 资源 + 被 enabled pass
    // 读取的资源(external / 其他 pass 写)。MVP 不做反向 propagation
    // (orphan-write 裁剪),保留所有 enabled pass 涉及的 resources
    // ── 这是 cutsheet §6 红线外最简单的近似,够 F1 验收。

    for (PassEntry& p : _passes) {
        if (!p.desc.enabled) {
            p.live = false;
            continue;
        }
        p.live = true;
        ++_stats.livePasses;

        // 标记该 pass 涉及的资源为 live。external 即使未 addResource
        // 也允许(后续 importExternal 把它指向具体 handle)。
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

    // 统计 logical resources ── 包含 external + transient(被任何
    // enabled pass 引用即算 live logical)。
    for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
        if (_resources[i].live) {
            ++_stats.logicalResources;
        }
    }

    // Resolve semantic → physical (FG owned / external 借用,都不
    // 重新创建)。F6 才做"physical not live ⇒ fallback" 决策。
    for (size_t i = 0; i < static_cast<size_t>(FgSemantic::Count); ++i) {
        SemanticEntry& s = _semantics[i];
        if (!s.hasLogical) continue;
        const ResourceEntry& r = _resources[static_cast<size_t>(s.logical)];
        // semantic 引用的 logical 必须 live;否则该 semantic 不可解析。
        // 物理 handle = r.physical(可能是 invalid ── 由 resolveSemantic
        // 调用方短路)。
        s.physical = r.live ? r.physical
                             : bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }

    // F1 不做物理 FBO 创建(create-on-resolve 延后) ── 也不做 alias
    // 决策。`physicalTargets` / `aliasHits` 留 0。
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
    // owned transient:lazy create-on-first-resolve(F1 骨架 ── 真
    // 正的 BGFXAdapter::createFrameBuffer 调用留到 F6 + BGFXAdapter
    // ready 守)。F1 直接返 invalid 占位,让 compile/execute 路径
    // 守 K invariant 而不实际申请 GPU 资源 ── 这让 F1 测试零 GPU
    // 副作用,易 3-run stable。
    //
    // 守:Noop / 未初始化 / 零 viewport ⇒ 不创建。
    if (!_adapter->isInitialized() || _adapter->isNoopBackend()) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    if (_viewportW == 0 || _viewportH == 0) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    // F1 骨架 ── 物理创建延后到 F6;现在返 invalid 作为 placeholder
    // 不会破坏 K invariant(resolveSemantic 走外部 fallback)。
    return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
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
    // F1 ── owned RT 物理重建延后到 F6。现在只更新尺寸缓存 + 让
    // 下一帧 compile 重算 stats;现有物理 handle 保持不变(若尺寸
    // 变化会在 resolve() 时检测,这是 F6 的事)。
    //
    // external 不动 ── 由其 owner 管(场景 RT / LightingPass FBO)。
}

void FrameGraph::shutdown()
{
    // F1 ── 物理 owned RT 释放延后到 F6。这里只清空声明。
    for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
        _resources[i].physical = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        _resources[i].declared = false;
        _resources[i].live     = false;
        _resources[i].physicalW = 0;
        _resources[i].physicalH = 0;
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