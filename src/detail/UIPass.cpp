#include "detail/UIPass.h"

#include "AYRenderer/UIRenderBackend.h"

namespace ayt::render::detail
{

uint32_t UIPass::execute(PassExecContext& ctx)
{
    (void)ctx;

    if (_backend == nullptr) {
        return 0;
    }
    if (!_backend->isInitialized()) {
        return 0;
    }
    // AI-1: RenderPipeline owns the UI flush boundary. The host
    // populates batches at full-window size BEFORE 3D. Do NOT resize
    // the backend to the Game View panel here — that remaps chrome /
    // overlay menu coords into the panel hole and makes dropdowns look
    // clipped by the 3D blit. Keep whatever setFramebufferSize the
    // host already applied (full client size).
    _backend->flushBatches();
    return static_cast<uint32_t>(_backend->getDrawCallCount());
}

} // namespace ayt::render::detail
