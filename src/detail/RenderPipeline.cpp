#include "detail/RenderPipeline.h"

#include <chrono>
#include <cstdio>

namespace ayt::render::detail
{

void RenderPipeline::addPass(std::unique_ptr<RenderPass> pass)
{
    if (pass) {
        _passesByType.try_emplace(std::type_index(typeid(*pass)), pass.get());
    }
    _passes.push_back(std::move(pass));
}

void RenderPipeline::clear()
{
    _passesByType.clear();
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
        if (!pass || !pass->isEnabled()) {
            continue;
        }
        const std::string_view passName = pass->name();
        const auto begin = Clock::now();
        uint32_t draws = 0;
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
        } catch (...) {
            std::fprintf(stderr,
                         "[RenderPipeline] pass '%.*s' threw non-std exception\n",
                         static_cast<int>(passName.size()), passName.data());
            draws = 0;
        }
        const auto end = Clock::now();

        RenderPassFrameStats stats;
        stats.name.assign(passName.data(), passName.size());
        stats.drawCalls = draws;
        stats.cpuTimeMs = std::chrono::duration<float, std::milli>(
            end - begin).count();
        _lastPassStats.push_back(std::move(stats));
        total += draws;
    }
    return total;
}

} // namespace ayt::render::detail
