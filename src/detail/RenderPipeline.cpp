#include "detail/RenderPipeline.h"

namespace ayt::render::detail
{

void RenderPipeline::addPass(std::unique_ptr<RenderPass> pass)
{
    _passes.push_back(std::move(pass));
}

uint32_t RenderPipeline::executeAll(PassExecContext& ctx)
{
    uint32_t total = 0;
    for (auto& pass : _passes) {
        if (pass && pass->isEnabled()) {
            total += pass->execute(ctx);
        }
    }
    return total;
}

} // namespace ayt::render::detail