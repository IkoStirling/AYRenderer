#include "detail/RenderPipeline.h"
#include "detail/FgResource.h"

#include <chrono>
#include <cstdio>
#include <utility>

namespace ayt::render::detail
{

void RenderPipeline::addPass(std::unique_ptr<RenderPass> pass)
{
    if (pass) {
        _passesByType.try_emplace(std::type_index(typeid(*pass)), pass.get());
    }
    _passes.push_back(std::move(pass));
    _passSlots.push_back(-1);
}

void RenderPipeline::addPass(RenderPassSlot slot,
                             std::unique_ptr<RenderPass> pass)
{
    if (pass) {
        _passesByType.try_emplace(std::type_index(typeid(*pass)), pass.get());
    }
    _passes.push_back(std::move(pass));
    _passSlots.push_back(
        static_cast<int16_t>(static_cast<uint8_t>(slot)));
}

void RenderPipeline::clear()
{
    _passesByType.clear();
    _passes.clear();
    _passSlots.clear();
    _lastPassStats.clear();
    _lastPassOutcomes.clear();
}

RenderPass* RenderPipeline::findPass(std::string_view name) noexcept
{
    for (auto& pass : _passes) {
        if (pass && pass->name() == name) {
            return pass.get();
        }
    }
    return nullptr;
}

const RenderPass* RenderPipeline::findPass(std::string_view name) const noexcept
{
    for (const auto& pass : _passes) {
        if (pass && pass->name() == name) {
            return pass.get();
        }
    }
    return nullptr;
}

uint32_t RenderPipeline::executeAll(PassExecContext& ctx)
{
    using Clock = std::chrono::steady_clock;

    uint32_t total = 0;
    _lastPassStats.clear();
    _lastPassStats.reserve(_passes.size());
    _lastPassOutcomes.clear();
    _lastPassOutcomes.reserve(_passes.size());
    for (size_t passIndex = 0; passIndex < _passes.size(); ++passIndex) {
        auto& pass = _passes[passIndex];
        if (!pass) {
            continue;
        }
        const std::string_view passName = pass->name();
        auto recordOutcome = [this, passName](PassExecutionState state,
                                               uint32_t draws,
                                               std::string detail = {}) {
            PassExecutionOutcome outcome;
            outcome.name.assign(passName.data(), passName.size());
            outcome.state = state;
            outcome.drawCalls = draws;
            outcome.detail = std::move(detail);
            _lastPassOutcomes.push_back(std::move(outcome));
        };
        if (!pass->isEnabled()) {
            recordOutcome(PassExecutionState::Disabled, 0,
                          "RenderPass::isEnabled=false");
            continue;
        }
        const int16_t slot = passIndex < _passSlots.size()
            ? _passSlots[passIndex] : -1;
        if (slot >= 0 && ctx.frameGraph != nullptr) {
            const RenderPassSlot passSlot = static_cast<RenderPassSlot>(slot);
            if (ctx.frameGraph->hasExecutionDecision(passSlot)
                && !ctx.frameGraph->shouldExecute(passSlot)) {
                recordOutcome(PassExecutionState::GraphCulled, 0,
                              "compiled FrameGraph branch is not live");
                continue;
            }
        }
        const auto begin = Clock::now();
        uint32_t draws = 0;
        bool failed = false;
        std::string failureDetail;
        // §P2 M15 (2026-08-24) — wrap each pass dispatch in try/catch so
        // a single failing pass (bad_alloc on uniform upload, bgfx driver
        // error surface) doesn't abort the frame. The remaining passes
        // still run; the failed one's stats entry records 0 draws and
        // carries the CPU time spent up to the throw. Hosts can detect
        // via RenderFrameStats.drawCalls < expected.
        try {
            draws = pass->execute(ctx);
        } catch (const std::exception& ex) {
            std::fprintf(stderr,
                         "[RenderPipeline] pass '%.*s' threw std::exception: %s\n",
                         static_cast<int>(passName.size()), passName.data(),
                         ex.what());
            draws = 0;
            failed = true;
            failureDetail = ex.what();
        } catch (...) {
            std::fprintf(stderr,
                         "[RenderPipeline] pass '%.*s' threw non-std exception\n",
                         static_cast<int>(passName.size()), passName.data());
            draws = 0;
            failed = true;
            failureDetail = "non-std exception";
        }
        const auto end = Clock::now();

        RenderPassFrameStats stats;
        stats.name.assign(passName.data(), passName.size());
        stats.drawCalls = draws;
        stats.cpuTimeMs = std::chrono::duration<float, std::milli>(
            end - begin).count();
        _lastPassStats.push_back(std::move(stats));
        recordOutcome(failed
                          ? PassExecutionState::Failed
                          : (draws != 0
                                 ? PassExecutionState::Submitted
                                 : PassExecutionState::CompletedNoDraws),
                      draws, std::move(failureDetail));
        total += draws;
    }
    return total;
}

} // namespace ayt::render::detail
