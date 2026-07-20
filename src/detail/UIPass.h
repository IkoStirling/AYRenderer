#pragma once

#include "detail/RenderPass.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace ayt::render
{
class UIRenderBackend;
} // namespace ayt::render

namespace ayt::render::detail
{

// U0.5 — second concrete RenderPass subclass. Bridges AYUI's chrome
// rendering (driven externally by UIManager::render → IRenderBackend
// callbacks) into the RenderPass dispatch taxonomy.
//
// Lifetime: UIPass is non-owning. Its backend pointer is set via
// setBackend() and must outlive the UIPass (typically owned by the
// host — AYEditor's EditorApp or a unit-test fixture). If null,
// execute() is a no-op (returns 0 draw calls) so a not-yet-set backend
// doesn't crash.
//
// Execute semantics (locked):
//   1) backend->setFramebufferSize(viewportWidth, viewportHeight) —
//      so its NDC projection uses this pass's declared sub-rect.
//   2) backend->flushBatches() — push the batched geometry accumulated
//      by AYUI's prior drawRect/drawText calls into bgfx view 2.
//   3) Return backend->getDrawCallCount().
//
// What execute() does NOT do:
//   - It does NOT call backend->beginFrame() / endFrame() — those are
//     IRenderBackend lifecycle hooks driven by UIManager::render(),
//     which runs at host-side render time (after Renderer::render in
//     current call order — see AYEditor/design.md:109 for the
//     composite-mode flow diagram). RenderPass dispatch does not own
//     the IRenderBackend lifecycle.
//   - It does NOT call adapter.setViewRect / setViewTransform — the
//     bgfx UiGpuContext owns those internally during flushBatches →
//     flushPendingText → bgfx::submit.
//   - It does NOT consult execute()'s viewId arg — UIRenderBackend
//     hard-codes kViewId = 2 today. Future UIPass subclasses (e.g.
//     menu-only UI) that need different views override name() + use
//     viewId.
//
// U0.5 scope: this class is constructed and held by Renderer::Impl
// but is NOT YET DISPATCHED by Renderer::render — that lands in U1+
// when RenderPipeline takes over dispatch. The class exists today
// only to prove RenderPass polymorphism with a second concrete
// subclass and to keep `Impl` ready for the U1+ swap.
class UIPass : public RenderPass {
public:
    UIPass() = default;
    explicit UIPass(ayt::render::UIRenderBackend* backend) : _backend(backend) {}

    void setBackend(ayt::render::UIRenderBackend* backend) { _backend = backend; }
    ayt::render::UIRenderBackend* backend() const { return _backend; }

    std::string_view name() const override { return "UI"; }

    uint32_t execute(
        BGFXAdapter& adapter,
        shader::ShaderResourcePool& pool,
        const RenderScene& scene,
        const std::unordered_map<uint64_t, GpuMesh>& meshes,
        const std::unordered_map<uint64_t, GpuTexture>& textures,
        std::unordered_map<uint64_t, GpuMaterial>& materials,
        uint16_t viewportX, uint16_t viewportY,
        uint16_t viewportWidth, uint16_t viewportHeight,
        const FrameContext& frame,
        uint8_t viewId) override;

private:
    ayt::render::UIRenderBackend* _backend = nullptr;
};

} // namespace ayt::render::detail
