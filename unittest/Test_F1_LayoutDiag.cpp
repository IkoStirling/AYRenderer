// F1 layout / ODR diagnosis — docs/f1-sigsegv-repro.md
//
// Purpose: distinguish "EventBus bug" vs "RendererSubSystem sizeof mismatch
// across TUs (incremental build / ODR)" vs "heap corruption from diag flags".
//
// §5.5 cleanup (2026-07-22): the F1-diag compile flags
// (AY_F1_DIAG_LIGHT, AY_F1_DIAG_FRAME_SHADOW) are now permanently 0 —
// the diagnostic code paths they gated are gone (RenderScene::Light,
// FrameContext shadow writeback, lastFrameShadowFbo cache). This suite
// is now a plain "ABI / ODR / sticky-Noop" anchor with no diagnostic
// toggles left to flip. The companion doc f1-sigsegv-repro.md is kept
// as a historical record of the F1 SIGSEGV bisect that motivated these
// guards in the first place.

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
    // §5.5: diag-flag printout replaced with a "retired flags" stamp.
    // The sizeof checks below remain because they are still the primary
    // ODR / layout-drift guard for this TU pair (any future change to
    // RenderScene / RendererSubSystem / FrameContext would re-introduce
    // the same class of EventBus _Orphan_all bug if lib and test TUs
    // drifted).
    std::fprintf(stderr,
                 "[F1 DIAG] flags retired (AY_F1_DIAG_LIGHT=0, "
                 "AY_F1_DIAG_FRAME_SHADOW=0) — §5.5 cleanup 2026-07-22\n"
                 "[F1 DIAG] sizeof RenderScene     test=%zu lib=%zu\n"
                 "[F1 DIAG] sizeof SubSystem       test=%zu lib=%zu\n"
                 "[F1 DIAG] sizeof FrameContext    test=%zu lib=%zu\n",
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

    // §5.5: the diagFlagLight/diagFlagFrameShadow runtime checks are
    // gone (the compile flags are unconditionally 0 now). We pin the
    // "always-0" contract at compile time instead so a future
    // contributor who tries to re-introduce one of the flags sees a
    // build break here instead of a silent ABI mismatch.
    static_assert(AY_F1_DIAG_LIGHT == 0,
                  "AY_F1_DIAG_LIGHT is retired (§5.5 cleanup 2026-07-22); "
                  "do not re-introduce the diagnostic compile flag.");
    static_assert(AY_F1_DIAG_FRAME_SHADOW == 0,
                  "AY_F1_DIAG_FRAME_SHADOW is retired (§5.5 cleanup 2026-07-22); "
                  "do not re-introduce the diagnostic compile flag.");
    CHECK(true);  // placate some test runners that need an executable CHECK.
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
        // §5.5: scene.addLight(Light{}) removed (Light struct retired).
        // The render() call now exercises the post-cleanup scene
        // shape — RenderScene has no _lights vector and the FO/Trans
        // passes no longer reference one. This case catches a
        // regression where a host TU still holds a stale sizeof
        // expectation for RenderScene.
        ayt::render::RenderScene scene;
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
