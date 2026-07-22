// F1 layout / ODR diagnosis — docs/f1-sigsegv-repro.md
//
// Purpose: distinguish "EventBus bug" vs "RendererSubSystem sizeof mismatch
// across TUs (incremental build / ODR)" vs "heap corruption from diag flags".

#include "AYF1DiagFlags.h"
#include "AYRenderer.h"
#include "AYRendererSubSystem.h"
#include "AYTest.h"

#include "detail/FrameContext.h"

#include <AYAppEventHost.h>
#include <ayevent/EventBus.h>
#include <ayevent/Events/WindowEvents.h>

#include <cstdio>
#include <iostream>

using ayt::render::RendererSubSystem;

namespace {

int g_sentinelWindowHandle = 0;

void printDiagBanner()
{
    std::fprintf(stderr,
                 "[F1 DIAG] flags light=%d frameShadow=%d "
                 "(AY_F1_DIAG_DEFAULT_SHADOW retired in E4 §5.4 2026-07-22)\n"
                 "[F1 DIAG] sizeof RenderScene     test=%zu lib=%zu\n"
                 "[F1 DIAG] sizeof SubSystem       test=%zu lib=%zu\n"
                 "[F1 DIAG] sizeof FrameContext    test=%zu lib=%zu\n",
                 AY_F1_DIAG_LIGHT,
                 AY_F1_DIAG_FRAME_SHADOW,
                 sizeof(ayt::render::RenderScene),
                 RendererSubSystem::diagSizeofRenderScene(),
                 sizeof(RendererSubSystem),
                 RendererSubSystem::diagSizeofRendererSubSystem(),
                 sizeof(ayt::render::detail::FrameContext),
                 RendererSubSystem::diagSizeofFrameContext());
}

} // namespace

TEST_SUITE(AYRenderer_F1_LayoutDiag)

TEST_CASE(f1_diag_sizeof_matches_between_test_tu_and_lib)
{
    printDiagBanner();

    // If these fail, you have an incremental-build / ODR layout split.
    // Clean rebuild AYRenderer + AYRenderer_Test with identical CMake flags.
    CHECK(sizeof(ayt::render::RenderScene) ==
          RendererSubSystem::diagSizeofRenderScene());
    CHECK(sizeof(RendererSubSystem) ==
          RendererSubSystem::diagSizeofRendererSubSystem());
    CHECK(sizeof(ayt::render::detail::FrameContext) ==
          RendererSubSystem::diagSizeofFrameContext());

    CHECK_INT_EQ(RendererSubSystem::diagFlagLight(), AY_F1_DIAG_LIGHT);
    CHECK_INT_EQ(RendererSubSystem::diagFlagFrameShadow(), AY_F1_DIAG_FRAME_SHADOW);
    // diagFlagDefaultShadow() retired in E4 (§5.4, 2026-07-22).
    // The remaining two flags + sizeof checks still cover the ODR /
    // layout-drift concerns that motivated the original suite.
}

TEST_CASE(f1_diag_eventbridge_subscribe_after_noop_init)
{
    printDiagBanner();

    auto& bus = ayt::event::EventBus::instance();
    const auto baseline = bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId);

    RendererSubSystem::setWindowProvider({});
    RendererSubSystem::setBootstrapBackend(ayt::render::Backend::Noop);
    RendererSubSystem::setBootstrapWindow(&g_sentinelWindowHandle, 320, 240);

    std::cerr << "[F1 DIAG] constructing RendererSubSystem...\n";
    RendererSubSystem sub;
    std::cerr << "[F1 DIAG] initialize() — crash here ⇒ layout/heap at subscribe\n";
    CHECK(sub.initialize());
    CHECK(sub.isReady());

    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId)),
                 static_cast<int>(baseline + 1));

    sub.shutdown();
    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId)),
                 static_cast<int>(baseline));
    std::cerr << "[F1 DIAG] eventbridge path OK\n";
}

TEST_CASE(f1_diag_dual_renderer_then_eventbridge)
{
    // Stress: two Noop renderers (sticky bgfx life) then EventBridge.
    // If EventBridge alone is green but this fails ⇒ heap rot from
    // multi-init, not SubSystem layout.
    printDiagBanner();

    for (int i = 0; i < 2; ++i) {
        ayt::render::Renderer r;
        ayt::render::InitDesc d;
        d.backend = ayt::render::Backend::Noop;
        d.width = 64;
        d.height = 64;
        CHECK(r.initialize(d));
        ayt::render::RenderScene scene;
#if AY_F1_DIAG_LIGHT
        scene.addLight(ayt::render::Light{});
#endif
        r.beginFrame({});
        r.render(scene);
        r.endFrame();
        r.shutdown();
    }

    RendererSubSystem::setWindowProvider({});
    RendererSubSystem::setBootstrapBackend(ayt::render::Backend::Noop);
    RendererSubSystem::setBootstrapWindow(&g_sentinelWindowHandle, 320, 240);
    RendererSubSystem sub;
    CHECK(sub.initialize());
    sub.shutdown();
    std::cerr << "[F1 DIAG] dual-renderer then eventbridge OK\n";
}

TEST_SUITE_END
