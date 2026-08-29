#include "AYTest.h"

#include "detail/BGFXAdapter.h"
#include "detail/RenderTargetPool.h"

using ayt::render::Backend;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::BGFXInitParams;
using ayt::render::detail::RenderTargetKey;
using ayt::render::detail::RenderTargetPool;

TEST_SUITE(AYRenderer_RenderTargetPool)

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

TEST_SUITE_END
