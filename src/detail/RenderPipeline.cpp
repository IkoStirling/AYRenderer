#include "detail/RenderPipeline.h"

#include <chrono>

namespace ayt::render::detail
{

void RenderPipeline::addPass(std::unique_ptr<RenderPass> pass)
{
    _passes.push_back(std::move(pass));
}

void RenderPipeline::clear()
{
    _passes.clear();
    _lastPassStats.clear();
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
    for (auto& pass : _passes) {
        if (pass && pass->isEnabled()) {
            const auto begin = Clock::now();
            const uint32_t draws = pass->execute(ctx);
            const auto end = Clock::now();

            RenderPassFrameStats stats;
            stats.name.assign(pass->name().data(), pass->name().size());
            stats.drawCalls = draws;
            stats.cpuTimeMs = std::chrono::duration<float, std::milli>(
                end - begin).count();
            _lastPassStats.push_back(std::move(stats));
            total += draws;
        }
    }
    return total;
}

} // namespace ayt::render::detail
