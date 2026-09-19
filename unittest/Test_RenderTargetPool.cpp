#include "AYTest.h"

#include "detail/BGFXAdapter.h"
#include "detail/RenderTargetPool.h"

using ayt::render::Backend;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::BGFXInitParams;
using ayt::render::detail::RenderTargetKey;
using ayt::render::detail::RenderTargetPool;
using ayt::render::detail::RenderTargetPoolCategory;

TEST_SUITE(AYRenderer_RenderTargetPool)

TEST_CASE(RenderTargetKey_PointSamplingParticipatesInExactReuseKey)
{
    RenderTargetKey linear;
    linear.width = 64;
    linear.height = 64;
    RenderTargetKey point = linear;
    point.pointSampled = true;
    CHECK_FALSE(linear == point);
    point.pointSampled = false;
    CHECK_TRUE(linear == point);
}

TEST_CASE(RenderTargetPool_QuarantinesReusesAndEvictsExactKeyTargets)
{
    BGFXAdapter adapter;
    BGFXInitParams init;
    init.backend = Backend::Noop;
    init.width = 320;
    init.height = 180;
    init.vsync = false;
    init.msaa = 0;
    CHECK_TRUE(adapter.initialize(init));
    if (!adapter.isInitialized()) return;

    RenderTargetPool pool(adapter);
    pool.setDeferredFrames(2);

    RenderTargetKey key;
    key.width = 64;
    key.height = 32;
    key.colorFormat = bgfx::TextureFormat::RGBA8;
    key.withDepth = false;

    const auto first = pool.acquire(key);
    CHECK_TRUE(first.isValid());
    CHECK_TRUE(pool.isValid(first));
    CHECK_TRUE(pool.matches(first, key));
    CHECK(BGFXAdapter::isValid(pool.framebuffer(first)));
    CHECK(BGFXAdapter::isValid(pool.texture(first)));
    CHECK(pool.stats().allocations == 1u);
    CHECK(pool.stats().liveLeases == 1u);
    CHECK(pool.stats().allocatedBytes == 64u * 32u * 4u);

    pool.release(first);
    CHECK_FALSE(pool.isValid(first));
    CHECK(pool.stats().liveLeases == 0u);
    CHECK(pool.stats().idleTargets == 1u);

    // The just-released target is still referenced by bgfx's queued frame,
    // so an immediate request receives a different physical allocation.
    const auto second = pool.acquire(key);
    CHECK_TRUE(second.isValid());
    CHECK(second.slot != first.slot);
    CHECK(pool.stats().allocations == 2u);
    CHECK(pool.stats().reuses == 0u);
    pool.release(second);

    pool.beginFrame();
    CHECK(pool.stats().idleTargets == 2u);
    pool.beginFrame();
    const auto reused = pool.acquire(key);
    CHECK_TRUE(reused.isValid());
    CHECK(reused.slot == first.slot || reused.slot == second.slot);
    CHECK(reused.generation != first.generation);
    CHECK(pool.stats().allocations == 2u);
    CHECK(pool.stats().reuses == 1u);

    pool.release(reused);
    pool.setBudgetBytes(0);
    CHECK(pool.stats().evictions == 1u);
    CHECK(pool.stats().allocatedBytes == 64u * 32u * 4u);
    pool.beginFrame();
    CHECK(pool.stats().allocatedBytes == 64u * 32u * 4u);
    pool.beginFrame();
    CHECK(pool.stats().evictions == 2u);
    CHECK(pool.stats().allocatedBytes == 0u);

    const auto afterEviction = pool.acquire(key);
    CHECK_TRUE(afterEviction.isValid());
    CHECK(pool.stats().allocations == 3u);
    pool.reset();
    CHECK_FALSE(pool.isValid(afterEviction));
    CHECK(pool.stats().allocatedBytes == 0u);

    pool.shutdown();
    adapter.shutdown();
}

TEST_CASE(RenderTargetPool_RejectsUnsupportedDescriptorsWithoutAllocating)
{
    BGFXAdapter adapter;
    RenderTargetPool pool(adapter);

    RenderTargetKey key;
    key.width = 0;
    key.height = 64;
    CHECK_FALSE(pool.acquire(key).isValid());

    key.width = 64;
    key.sampleCount = 4;
    CHECK_FALSE(pool.acquire(key).isValid());
    CHECK(pool.stats().allocations == 0u);
    CHECK(pool.stats().allocatedBytes == 0u);
}

TEST_CASE(RenderTargetPool_StrictBudgetRejectsPressureUntilQuarantineExpires)
{
    BGFXAdapter adapter;
    BGFXInitParams init;
    init.backend = Backend::Noop;
    init.width = 320;
    init.height = 180;
    init.vsync = false;
    CHECK_TRUE(adapter.initialize(init));
    if (!adapter.isInitialized()) return;

    RenderTargetPool pool(adapter);
    pool.setDeferredFrames(2);
    constexpr size_t kTargetBytes = 64u * 64u * 4u;
    pool.setBudgetBytes(kTargetBytes);

    RenderTargetKey firstKey;
    firstKey.width = 64;
    firstKey.height = 64;
    firstKey.withDepth = false;
    RenderTargetKey secondKey = firstKey;
    secondKey.width = 32;
    secondKey.height = 128;

    const auto first = pool.acquire(firstKey, false);
    CHECK_TRUE(first.isValid());
    CHECK_FALSE(pool.acquire(secondKey, false).isValid());
    pool.release(first);
    CHECK_FALSE(pool.acquire(secondKey, false).isValid());
    CHECK(pool.stats().budgetMisses == 2u);

    pool.beginFrame();
    CHECK_FALSE(pool.acquire(secondKey, false).isValid());
    pool.beginFrame();
    const auto second = pool.acquire(secondKey, false);
    CHECK_TRUE(second.isValid());
    CHECK(pool.stats().evictions == 1u);
    CHECK(pool.stats().allocatedBytes == kTargetBytes);
    CHECK(pool.stats().peakAllocatedBytes <= kTargetBytes);

    pool.release(second);
    pool.shutdown();
    adapter.shutdown();
}

TEST_CASE(RenderTargetPool_SoftBudgetRemainsAvailableForFrameGraph)
{
    BGFXAdapter adapter;
    BGFXInitParams init;
    init.backend = Backend::Noop;
    init.width = 64;
    init.height = 64;
    init.vsync = false;
    CHECK_TRUE(adapter.initialize(init));
    if (!adapter.isInitialized()) return;

    RenderTargetPool pool(adapter);
    pool.setBudgetBytes(0u);
    RenderTargetKey key;
    key.width = 16;
    key.height = 16;
    key.withDepth = false;
    const auto lease = pool.acquire(key);
    CHECK_TRUE(lease.isValid());
    CHECK(pool.stats().allocatedBytes == 16u * 16u * 4u);
    CHECK(pool.stats().budgetMisses == 0u);

    pool.release(lease);
    pool.shutdown();
    adapter.shutdown();
}

TEST_CASE(RenderTargetPool_CategoryBudgetIsolatesRetainedUiFromFrameGraph)
{
    BGFXAdapter adapter;
    BGFXInitParams init;
    init.backend = Backend::Noop;
    init.width = 64;
    init.height = 64;
    init.vsync = false;
    CHECK_TRUE(adapter.initialize(init));
    if (!adapter.isInitialized()) return;

    constexpr size_t kTargetBytes = 32u * 32u * 4u;
    RenderTargetPool pool(adapter);
    pool.setBudgetBytes(kTargetBytes * 3u);
    pool.setCategoryBudgetBytes(
        RenderTargetPoolCategory::RetainedUi, kTargetBytes);

    RenderTargetKey key;
    key.width = 32;
    key.height = 32;
    key.withDepth = false;
    const auto ui = pool.acquire(
        key, false, RenderTargetPoolCategory::RetainedUi);
    CHECK_TRUE(ui.isValid());

    RenderTargetKey otherUi = key;
    otherUi.width = 16;
    otherUi.height = 64;
    CHECK_FALSE(pool.acquire(
        otherUi, false, RenderTargetPoolCategory::RetainedUi).isValid());

    // A UI budget miss must not consume or evict the FrameGraph partition.
    const auto frameGraph = pool.acquire(
        otherUi, true, RenderTargetPoolCategory::FrameGraph);
    CHECK_TRUE(frameGraph.isValid());
    const auto stats = pool.stats();
    const auto& uiStats = stats.categories[static_cast<size_t>(
        RenderTargetPoolCategory::RetainedUi)];
    const auto& frameStats = stats.categories[static_cast<size_t>(
        RenderTargetPoolCategory::FrameGraph)];
    CHECK(uiStats.allocatedBytes == kTargetBytes);
    CHECK(uiStats.budgetBytes == kTargetBytes);
    CHECK(uiStats.budgetMisses == 1u);
    CHECK(uiStats.liveLeases == 1u);
    CHECK(frameStats.allocatedBytes == kTargetBytes);
    CHECK(frameStats.liveLeases == 1u);

    pool.release(ui);
    pool.release(frameGraph);
    pool.shutdown();
    adapter.shutdown();
}

TEST_SUITE_END
