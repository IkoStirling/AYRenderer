// Test_PublicHeaderSurface.cpp — public headers must not pull in bgfx

#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYRenderTypes.h"

#include "AYTest.h"

TEST_SUITE(RendererPublicHeaderTests)

TEST_CASE(public_headers_compile_without_bgfx)
{
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    ayt::render::Renderer renderer;
    CHECK(!renderer.isInitialized());
    (void)desc;
}

TEST_SUITE_END
