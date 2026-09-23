#include "detail/FgResource.h"

#include "detail/BGFXAdapter.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>
#include <limits>

namespace ayt::render::detail
{

namespace {

// F1 — 由 scale + 完整 viewport 算出实际物理尺寸。
// Half scale uses Bloom's `(viewport + 1) / 2` convention. DepthHaze and
// SSAO request Full explicitly; quarter remains reserved for future effects.
uint16_t scaledDim(uint16_t viewport, FgTextureScale scale)
{
    switch (scale) {
    case FgTextureScale::Full:    return viewport;
    case FgTextureScale::Half:    return static_cast<uint16_t>((viewport + 1u) / 2u);
    case FgTextureScale::Quarter: return static_cast<uint16_t>((viewport + 3u) / 4u);
    case FgTextureScale::Eighth:  return static_cast<uint16_t>((viewport + 7u) / 8u);
    case FgTextureScale::Sixteenth:
        return static_cast<uint16_t>((viewport + 15u) / 16u);
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

const char* fgCompileErrorName(FgCompileErrorCode code) noexcept
{
    switch (code) {
    case FgCompileErrorCode::InvalidResourceId: return "invalid resource id";
    case FgCompileErrorCode::UndeclaredRead: return "read of undeclared resource";
    case FgCompileErrorCode::UndeclaredWrite: return "write of undeclared resource";
    case FgCompileErrorCode::ReadBeforeWrite: return "read before producer";
    case FgCompileErrorCode::MultipleWriters: return "multiple writers";
    case FgCompileErrorCode::UndeclaredSemanticResource:
        return "semantic references undeclared resource";
    case FgCompileErrorCode::MissingSemanticProducer:
        return "semantic resource has no producer";
    }
    return "unknown frame-graph compile error";
}

// ─── ctor / dtor ──────────────────────────────────────────────

FrameGraph::FrameGraph(BGFXAdapter& adapter) noexcept
    : _adapter(&adapter),
      _ownedTargetPool(std::make_unique<RenderTargetPool>(adapter)),
      _targetPool(_ownedTargetPool.get())
{
    // _resources / _passes / _semantics 默认初始化(ResourceEntry
    // POD-default,PassEntry default,SemanticEntry default)。
}

FrameGraph::FrameGraph(BGFXAdapter& adapter, RenderTargetPool& targetPool) noexcept
    : _adapter(&adapter), _targetPool(&targetPool)
{
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
    _releasedIdleTargets = 0;
    _compileErrors.clear();

    // 清空 logical 声明。但 ── F6 ── 物理 RT (aliasGroup /
    // physicalW / physicalH) 在 compile() 的 alias 决策后写;
    // beginFrame 重置 physicalW / physicalH 和 aliasGroup(每帧
    // 重决策,因为上一帧的 alias 可能不再合法 ── pass 列表变了)。
    for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
        _resources[i].declared   = false;
        _resources[i].live       = false;
        _resources[i].producedThisFrame = false;
        _resources[i].aliasGroup = -1;
        _resources[i].physicalW  = 0;
        _resources[i].physicalH  = 0;
        // physical handle 保留 ── alias 决策会让多块 logical
        // 指向同一物理 handle。shutdown 时一次性 destroy。
    }
    _passes.clear();
    std::fill(std::begin(_executionTracked), std::end(_executionTracked), false);
    std::fill(std::begin(_executionEligible), std::end(_executionEligible), false);
    std::fill(std::begin(_executionLive), std::end(_executionLive), false);

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
    if (_targetPool != nullptr && r.pooled.isValid()) {
        _targetPool->release(r.pooled);
        r.pooled = {};
    }
    r.retentionTracked = false;
    r.inactiveFrames = 0;
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
    if (r.isExternal) {
        r.physical = BGFX_INVALID_HANDLE;
    }
    r.declared   = true;
    r.isExternal = false;  // 即使前一次是 external,这次声明覆盖所有权
    r.desc       = desc;
    r.retentionTracked = true;
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

void FrameGraph::setExecutionEligibility(RenderPassSlot slot,
                                         bool enabled) noexcept
{
    const size_t index = static_cast<size_t>(static_cast<uint8_t>(slot));
    _executionTracked[index] = true;
    _executionEligible[index] = enabled;
}

bool FrameGraph::hasExecutionDecision(RenderPassSlot slot) const noexcept
{
    const size_t index = static_cast<size_t>(static_cast<uint8_t>(slot));
    return _executionTracked[index];
}

bool FrameGraph::shouldExecute(RenderPassSlot slot) const noexcept
{
    const size_t index = static_cast<size_t>(static_cast<uint8_t>(slot));
    return _executionTracked[index] && _compiled && _executionLive[index];
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
    _compileErrors.clear();
    _compiled = false;
    std::fill(std::begin(_executionLive), std::end(_executionLive), false);

    constexpr size_t kResourceCount =
        static_cast<size_t>(FgResourceId::Count);
    constexpr int16_t kNoWriter = -1;
    std::array<int16_t, kResourceCount> latestWriter{};
    latestWriter.fill(kNoWriter);

    for (ResourceEntry& resource : _resources) {
        resource.live = false;
        resource.aliasGroup = -1;
    }
    for (PassEntry& pass : _passes) {
        pass.live = false;
    }

    auto addError = [this](FgCompileErrorCode code, size_t passIndex,
                           FgResourceId resource) {
        FgCompileError error{};
        error.code = code;
        error.passIndex = passIndex > std::numeric_limits<uint16_t>::max()
            ? kFgNoPass
            : static_cast<uint16_t>(passIndex);
        error.resource = resource;
        _compileErrors.push_back(error);
    };
    auto resourceIndex = [](FgResourceId id) {
        return static_cast<size_t>(id);
    };

    // 1) Validate declaration and producer order in registration order.
    // External imports are valid read sources and may also be written (TAA's
    // imported persistent history target). Owned resources require an earlier
    // enabled producer before they can be read.
    for (size_t passIndex = 0; passIndex < _passes.size(); ++passIndex) {
        const PassEntry& pass = _passes[passIndex];
        if (!pass.desc.enabled) {
            continue;
        }
        for (FgResourceId id : pass.desc.reads) {
            const size_t index = resourceIndex(id);
            if (index >= kResourceCount) {
                addError(FgCompileErrorCode::InvalidResourceId, passIndex, id);
                continue;
            }
            const ResourceEntry& resource = _resources[index];
            if (!resource.declared) {
                addError(FgCompileErrorCode::UndeclaredRead, passIndex, id);
            } else if (!resource.isExternal && latestWriter[index] == kNoWriter) {
                addError(FgCompileErrorCode::ReadBeforeWrite, passIndex, id);
            }
        }
        for (FgResourceId id : pass.desc.writes) {
            const size_t index = resourceIndex(id);
            if (index >= kResourceCount) {
                addError(FgCompileErrorCode::InvalidResourceId, passIndex, id);
                continue;
            }
            if (!_resources[index].declared) {
                addError(FgCompileErrorCode::UndeclaredWrite, passIndex, id);
                continue;
            }
            if (latestWriter[index] != kNoWriter) {
                addError(FgCompileErrorCode::MultipleWriters, passIndex, id);
            }
            latestWriter[index] = static_cast<int16_t>(passIndex);
        }
    }

    for (size_t semanticIndex = 0;
         semanticIndex < static_cast<size_t>(FgSemantic::Count);
         ++semanticIndex) {
        const SemanticEntry& semantic = _semantics[semanticIndex];
        if (!semantic.hasLogical) {
            continue;
        }
        const size_t index = resourceIndex(semantic.logical);
        if (index >= kResourceCount || !_resources[index].declared) {
            addError(FgCompileErrorCode::UndeclaredSemanticResource,
                     kFgNoPass, semantic.logical);
        } else if (!_resources[index].isExternal
                   && latestWriter[index] == kNoWriter) {
            addError(FgCompileErrorCode::MissingSemanticProducer,
                     kFgNoPass, semantic.logical);
        }
    }

    if (!_compileErrors.empty()) {
        updateIdleRetention();
        return false;
    }

    // 2) Backward liveness from terminal/side-effect passes. Semantic outputs
    // are additional roots because some resources (SSAO) are consumed by a
    // pass outside this MVP graph. Graphs without an execution sink retain the
    // legacy behavior and keep every enabled pass live; this preserves small
    // standalone resource tests while product graphs gain real dead-branch
    // culling through Present.
    std::vector<size_t> worklist;
    bool hasExecutionSink = false;
    auto markPassLive = [this, &worklist](size_t passIndex) {
        PassEntry& pass = _passes[passIndex];
        if (!pass.live) {
            pass.live = true;
            worklist.push_back(passIndex);
        }
    };

    for (size_t passIndex = 0; passIndex < _passes.size(); ++passIndex) {
        const PassEntry& pass = _passes[passIndex];
        if (!pass.desc.enabled) {
            continue;
        }
        if (pass.desc.sideEffect || pass.desc.writes.empty()) {
            hasExecutionSink = true;
            markPassLive(passIndex);
        }
    }

    if (hasExecutionSink) {
        for (const SemanticEntry& semantic : _semantics) {
            if (!semantic.hasLogical) {
                continue;
            }
            const int16_t writer = latestWriter[resourceIndex(semantic.logical)];
            if (writer != kNoWriter) {
                markPassLive(static_cast<size_t>(writer));
            }
        }
        while (!worklist.empty()) {
            const size_t consumerIndex = worklist.back();
            worklist.pop_back();
            const PassEntry& consumer = _passes[consumerIndex];
            for (FgResourceId id : consumer.desc.reads) {
                const size_t index = resourceIndex(id);
                bool foundCurrentFrameProducer = false;
                for (size_t candidate = consumerIndex; candidate > 0; --candidate) {
                    const size_t producerIndex = candidate - 1;
                    const PassEntry& producer = _passes[producerIndex];
                    if (!producer.desc.enabled) {
                        continue;
                    }
                    if (std::find(producer.desc.writes.begin(),
                                  producer.desc.writes.end(), id)
                        != producer.desc.writes.end()) {
                        markPassLive(producerIndex);
                        foundCurrentFrameProducer = true;
                        break;
                    }
                }
                // Imported resources are valid prior-frame/upstream inputs,
                // but only when no earlier pass overwrites them this frame.
                // TAA history is imported and then written before Present.
                if (!foundCurrentFrameProducer && _resources[index].isExternal) {
                    continue;
                }
            }
        }
    } else {
        for (size_t passIndex = 0; passIndex < _passes.size(); ++passIndex) {
            if (_passes[passIndex].desc.enabled) {
                _passes[passIndex].live = true;
            }
        }
    }

    for (PassEntry& pass : _passes) {
        if (!pass.live) {
            continue;
        }
        if (pass.desc.executionSlot >= 0
            && pass.desc.executionSlot
                < static_cast<int16_t>(kExecutionSlotCount)) {
            const size_t slot = static_cast<size_t>(pass.desc.executionSlot);
            if (_executionTracked[slot] && _executionEligible[slot]) {
                _executionLive[slot] = true;
            }
        }
        ++_stats.livePasses;
        for (FgResourceId id : pass.desc.reads) {
            _resources[resourceIndex(id)].live = true;
        }
        for (FgResourceId id : pass.desc.writes) {
            _resources[resourceIndex(id)].live = true;
        }
    }

    // 3) Logical resources 统计 + owned live 物理尺寸 / 独立
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

    // 4) Semantic 只锁 logical 映射。physical 不在 compile 缓存
    //    （F6.1 hotfix）：owned RT 是 resolve() lazy create 的，
    //    compile 时拷贝 r.physical 会让 Final 首帧采到 invalid。
    //    resolveSemantic() 改走 resolve(logical)。
    for (size_t i = 0; i < static_cast<size_t>(FgSemantic::Count); ++i) {
        _semantics[i].physical =
            bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }

    _compiled = true;
    updateIdleRetention();
    return true;
}

void FrameGraph::updateIdleRetention() noexcept
{
    if (_targetPool == nullptr) {
        return;
    }
    for (ResourceEntry& resource : _resources) {
        if (resource.isExternal || !resource.retentionTracked) {
            continue;
        }
        if (resource.live) {
            resource.inactiveFrames = 0;
            continue;
        }
        if (resource.inactiveFrames < std::numeric_limits<uint16_t>::max()) {
            ++resource.inactiveFrames;
        }
        if (resource.inactiveFrames < _idleReleaseFrames) {
            continue;
        }
        if (resource.pooled.isValid()) {
            _targetPool->release(resource.pooled);
            ++_releasedIdleTargets;
        }
        resource.pooled = {};
        resource.physical = BGFX_INVALID_HANDLE;
        resource.physicalW = 0;
        resource.physicalH = 0;
        resource.retentionTracked = false;
        resource.inactiveFrames = 0;
    }
}

FgRetentionStats FrameGraph::retentionStats() const noexcept
{
    FgRetentionStats result{};
    result.releasedTargets = _releasedIdleTargets;
    result.releaseAfterFrames = _idleReleaseFrames;
    for (const ResourceEntry& resource : _resources) {
        if (resource.isExternal || !resource.retentionTracked) {
            continue;
        }
        ++result.retainedResources;
        if (resource.pooled.isValid()) {
            ++result.leasedTargets;
        }
        if (!resource.live && resource.inactiveFrames != 0) {
            ++result.idleResources;
        }
    }
    return result;
}

// ─── resolve ─────────────────────────────────────────────────

bgfx::FrameBufferHandle FrameGraph::resolve(FgResourceId id) const
{
    if (static_cast<size_t>(id) >= static_cast<size_t>(FgResourceId::Count)) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    const ResourceEntry& r = _resources[static_cast<size_t>(id)];
    if (_adapter == nullptr || _targetPool == nullptr) {
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
    const RenderTargetKey key{w, h, r.desc.format, r.desc.withDepth, 1,
                              r.desc.pointSampled};
    if (r.pooled.isValid() && !_targetPool->matches(r.pooled, key)) {
        _targetPool->release(r.pooled);
        r.pooled = {};
        r.physical = BGFX_INVALID_HANDLE;
    }
    if (!r.pooled.isValid() || !_targetPool->isValid(r.pooled)) {
        r.pooled = _targetPool->acquire(key);
    }
    r.physical = _targetPool->framebuffer(r.pooled);
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

void FrameGraph::markProduced(FgResourceId id) noexcept
{
    if (static_cast<size_t>(id) >= static_cast<size_t>(FgResourceId::Count)) {
        return;
    }
    ResourceEntry& resource = _resources[static_cast<size_t>(id)];
    resource.producedThisFrame = resource.declared && resource.live;
}

bool FrameGraph::producedThisFrame(FgResourceId id) const noexcept
{
    if (static_cast<size_t>(id) >= static_cast<size_t>(FgResourceId::Count)) {
        return false;
    }
    return _resources[static_cast<size_t>(id)].producedThisFrame;
}

bool FrameGraph::semanticProducedThisFrame(FgSemantic sem) const noexcept
{
    if (static_cast<size_t>(sem) >= static_cast<size_t>(FgSemantic::Count)) {
        return false;
    }
    const SemanticEntry& semantic = _semantics[static_cast<size_t>(sem)];
    return semantic.hasLogical && producedThisFrame(semantic.logical);
}

// ─── 生命周期 ─────────────────────────────────────────────────

void FrameGraph::resize(uint16_t width, uint16_t height)
{
    _viewportW = width;
    _viewportH = height;
    // Shared pools keep released targets quarantined for later exact-key
    // reuse. Standalone FrameGraph tests own their pool, so reset preserves
    // the historical immediate-destruction lifecycle.
    for (ResourceEntry& r : _resources) {
        if (!r.isExternal && _targetPool != nullptr && r.pooled.isValid()) {
            _targetPool->release(r.pooled);
        }
        if (!r.isExternal) {
            r.pooled = {};
            r.physical = BGFX_INVALID_HANDLE;
            r.physicalW = 0;
            r.physicalH = 0;
            r.retentionTracked = false;
            r.inactiveFrames = 0;
        }
    }
    if (_ownedTargetPool != nullptr) {
        _ownedTargetPool->reset();
    }
}

void FrameGraph::shutdown()
{
    // Release leases; external handles remain borrowed and untouched.
    for (ResourceEntry& r : _resources) {
        if (!r.isExternal && _targetPool != nullptr && r.pooled.isValid()) {
            _targetPool->release(r.pooled);
        }
    }
    for (size_t i = 0; i < static_cast<size_t>(FgResourceId::Count); ++i) {
        ResourceEntry& r = _resources[i];
        r.physical   = BGFX_INVALID_HANDLE;
        r.pooled     = {};
        r.physicalW  = 0;
        r.physicalH  = 0;
        r.declared   = false;
        r.live       = false;
        r.producedThisFrame = false;
        r.retentionTracked = false;
        r.inactiveFrames = 0;
        r.isExternal = false;
        r.aliasGroup = -1;
    }
    _passes.clear();
    std::fill(std::begin(_executionTracked), std::end(_executionTracked), false);
    std::fill(std::begin(_executionEligible), std::end(_executionEligible), false);
    std::fill(std::begin(_executionLive), std::end(_executionLive), false);
    for (size_t i = 0; i < static_cast<size_t>(FgSemantic::Count); ++i) {
        _semantics[i].hasLogical = false;
        _semantics[i].physical   = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    _viewportW = 0;
    _viewportH = 0;
    _compiled  = false;
    _stats     = FgCompileStats{};
    _releasedIdleTargets = 0;
    _compileErrors.clear();
    if (_ownedTargetPool != nullptr) {
        _ownedTargetPool->shutdown();
    }
}

} // namespace ayt::render::detail
