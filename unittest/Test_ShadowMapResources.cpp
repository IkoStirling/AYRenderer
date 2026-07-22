#include "detail/ShadowMapResources.h"

#include "AYTest.h"

#include <bgfx/bgfx.h>

using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::ShadowMapResources;

TEST_SUITE(AYShadowMapResources)

TEST_CASE(noop_adapter_resources_stay_invalid)
{
    BGFXAdapter adapter;
    ShadowMapResources resources;

    resources.ensure(adapter, 1024, "test-stamp");
    CHECK(resources.isValid() == false);
    CHECK(resources.hasSampleableShadow() == false);
    CHECK(resources.lastBlitOk() == false);
    CHECK(resources.allocatedSize() == 0);

    resources.destroy(adapter);
}

TEST_CASE(destroy_on_empty_is_noop)
{
    BGFXAdapter adapter;
    ShadowMapResources resources;
    resources.destroy(adapter);
    CHECK(resources.isValid() == false);
}

TEST_CASE(caster_draw_state_uses_less_and_write_z)
{
    const uint64_t state = ShadowMapResources::casterDrawState();
    CHECK((state & BGFX_STATE_WRITE_RGB) != 0);
    CHECK((state & BGFX_STATE_WRITE_A) != 0);
    CHECK((state & BGFX_STATE_WRITE_Z) != 0);
    CHECK((state & BGFX_STATE_DEPTH_TEST_LESS) != 0);
    CHECK((state & BGFX_STATE_DEPTH_TEST_ALWAYS) == 0);
}

TEST_CASE(resolve_without_fbo_fails_safely)
{
    BGFXAdapter adapter;
    ShadowMapResources resources;
    CHECK(resources.resolveForSampling(adapter, 1) == false);
    CHECK(resources.lastBlitOk() == false);
}

TEST_SUITE_END
