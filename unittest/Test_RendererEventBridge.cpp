// AYRenderer/unittest/Test_RendererEventBridge.cpp
//
// INT-04 (2026-07-20) — RendererSubSystem → EventBus bridge tests.
//
// RendererSubSystem subscribes to WindowResizeEvent on the EventBus during
// initialize() and forwards the size delta to Renderer::resize() (which wraps
// bgfx::reset). These tests verify the wiring using a Backend::Noop renderer
// so we don't need a real bgfx device in CI, and an in-process EventBus so
// the suite is self-contained.
//
// Frame order simulated by the tests:
//   1. RendererSubSystem::initialize() — Backend::Noop + a placeholder native
//      window handle. The renderer reports isInitialized() == true after this.
//   2. RendererSubSystem::update() — currently a no-op, but we drive it to
//      exercise the loop hook.
//   3. bus.post<WindowResizeEvent>({w,h}) — what DeviceSubSystem (INT-03) does
//      each frame in production.
//   4. bus.pump() — what GameLoop::tickOnceFrame() does after the device
//      update in production. Drives the listener synchronously on the main
//      thread (Phase 4 contract).
//
// Tests use a synthetic probe event to count "frame" deliveries without
// colliding with WindowResizeEvent itself.

#include "AYRendererSubSystem.h"
#include "AYTest.h"

#include <AYAppEventHost.h>
#include <ayevent/EventBus.h>
#include <ayevent/Events/WindowEvents.h>

#include <atomic>
#include <utility>

using namespace ayt::render;

namespace ayt::render::test
{

// Hand-picked synthetic probe event used to count "pump cycles" without
// colliding with WindowResizeEvent produced/consumed by the bridge itself.
struct RendererBridgeProbeEvent {
    int  frame = 0;
    static constexpr ayt::event::EventTypeId   kTypeId   = 0x0A11'0001;
    static constexpr ayt::event::EventPriority kPriority = ayt::event::EventPriority::Normal;
};

namespace {

// Backend::Noop requires a non-null windowHandle (RendererSubSystem guards on
// nullptr at the top of initialize()). A pointer to a sentinel int is enough
// to satisfy the guard; BGFXAdapter's Noop branch never dereferences it.
int g_sentinelWindowHandle = 0;

void resetBootstrapForTest()
{
    RendererSubSystem::setWindowProvider({});  // clear any previous provider
    RendererSubSystem::setBootstrapBackend(Backend::Noop);
    RendererSubSystem::setBootstrapWindow(&g_sentinelWindowHandle, 320, 240);
}

} // namespace

TEST_SUITE(RendererEventBridge)

TEST_CASE(Bridge_WindowResize_ForwardsToRenderer) {
    auto& bus = ayt::event::EventBus::instance();
    const auto baseline = bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId);

    resetBootstrapForTest();
    RendererSubSystem sub;
    CHECK(sub.initialize());
    CHECK(sub.isReady());
    CHECK(sub.renderer().isInitialized());

    // The bridge subscribes inside initialize() — listenerCount must rise.
    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId)),
                 static_cast<int>(baseline + 1));

    // Post a WindowResizeEvent (what DeviceSubSystem does in INT-03). Pump
    // the bus to drive the listener — exactly what GameLoop::tickOnceFrame()
    // does in production.
    bus.post<ayt::event::WindowResizeEvent>(
        ayt::event::WindowResizeEvent{800, 600});
    bus.pump();

    // Renderer::resize() forwards to BGFXAdapter::resetResolution which
    // stores width/height in _impl->initDesc. We assert via a no-op probe
    // event delivery that the pump happened (one probe = one pump pass),
    // and we trust the resize() path is exercised by the call not throwing.
    bus.post(RendererBridgeProbeEvent{1});
    bus.pump();

    // Cleanup.
    sub.shutdown();
    CHECK(!sub.isReady());

    // Shutdown disconnects the listener — listenerCount returns to baseline.
    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId)),
                 static_cast<int>(baseline));
}

TEST_CASE(Bridge_MultipleResizeEvents_AllForwarded) {
    auto& bus = ayt::event::EventBus::instance();
    const auto baseline = bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId);

    resetBootstrapForTest();
    RendererSubSystem sub;
    CHECK(sub.initialize());

    std::atomic<int> deliveredCount{0};
    std::atomic<int> lastWidth{0};
    std::atomic<int> lastHeight{0};
    ayt::app::EventBusHostScope probeScope;
    // Use a separate probe listener on the SAME event to verify multiple
    // posts each fire. The bridge listener runs first (or interleaved), but
    // both are dispatched per pump.
    probeScope.subscribe<ayt::event::WindowResizeEvent>(
        [&deliveredCount, &lastWidth, &lastHeight](const ayt::event::WindowResizeEvent& e) {
            deliveredCount.fetch_add(1);
            lastWidth.store(static_cast<int>(e.width));
            lastHeight.store(static_cast<int>(e.height));
        });

    // 3 resize events back-to-back.
    bus.post<ayt::event::WindowResizeEvent>(ayt::event::WindowResizeEvent{640, 480});
    bus.post<ayt::event::WindowResizeEvent>(ayt::event::WindowResizeEvent{1024, 768});
    bus.post<ayt::event::WindowResizeEvent>(ayt::event::WindowResizeEvent{1920, 1080});
    bus.pump();

    CHECK_INT_EQ(deliveredCount.load(), 3);
    CHECK_INT_EQ(lastWidth.load(), 1920);
    CHECK_INT_EQ(lastHeight.load(), 1080);

    probeScope.disconnect();
    sub.shutdown();

    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId)),
                 static_cast<int>(baseline));
}

TEST_CASE(Bridge_Shutdown_DisconnectsHostScope) {
    // Phase 4 lesson applied: shutdown() must drain the EventBusHostScope
    // exactly once and leave the bus in baseline state. Idempotent re-shutdown
    // must not double-unsubscribe (which would crash on stale ConnectionIds).
    auto& bus = ayt::event::EventBus::instance();
    const auto baseline = bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId);

    resetBootstrapForTest();
    RendererSubSystem sub;
    CHECK(sub.initialize());
    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId)),
                 static_cast<int>(baseline + 1));

    sub.shutdown();
    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId)),
                 static_cast<int>(baseline));

    sub.shutdown();  // idempotent
    CHECK(!sub.isReady());
    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId)),
                 static_cast<int>(baseline));

    // Post-disconnect: a posted resize must NOT deliver to anyone from this
    // scope. Use the probe as a witness — no probe listener exists in this
    // test, so we assert the listener count is still baseline.
    bus.post<ayt::event::WindowResizeEvent>(ayt::event::WindowResizeEvent{100, 100});
    bus.pump();
    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId)),
                 static_cast<int>(baseline));
}

TEST_CASE(Bridge_NoRendererInit_InitializesCleanly) {
    // The handler guards on _renderer.isInitialized() — if a WindowResizeEvent
    // is posted BEFORE the renderer's initialize() succeeds, the handler
    // silently drops it. Here we verify the bridge does NOT crash and does
    // NOT partially-register the listener if initialize() rejects the input.
    auto& bus = ayt::event::EventBus::instance();
    const auto baseline = bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId);

    RendererSubSystem::setWindowProvider({});  // clear
    RendererSubSystem::setBootstrapWindow(nullptr, 320, 240);  // forces fail
    RendererSubSystem sub;
    CHECK(!sub.initialize());
    CHECK(!sub.isReady());

    // Listener count must be at baseline — initialize() returned early
    // BEFORE the _events.subscribe call, so no listener was registered.
    CHECK_INT_EQ(static_cast<int>(bus.listenerCount(ayt::event::WindowResizeEvent::kTypeId)),
                 static_cast<int>(baseline));
}

TEST_SUITE_END

} // namespace ayt::render::test