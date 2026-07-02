#include "AYRenderTypes.h"
#include "detail/VertexLayoutBridge.h"

#include "AYTest.h"

#include <bgfx/bgfx.h>

using namespace ayt::render;

TEST_SUITE(VertexLayoutTests)

TEST_CASE(vertex_layout_desc_stride_and_presets)
{
    const VertexLayoutDesc pos3 = VertexLayoutDesc::position3();
    CHECK(pos3.isValid());
    CHECK(pos3.elementCount == 1u);
    CHECK(pos3.strideBytes() == 12u);

    const VertexLayoutDesc posNorm = VertexLayoutDesc::position3Normal3();
    CHECK(posNorm.isValid());
    CHECK(posNorm.strideBytes() == 24u);

    const VertexLayoutDesc posUv = VertexLayoutDesc::position3TexCoord2();
    CHECK(posUv.isValid());
    CHECK(posUv.strideBytes() == 20u);
}

TEST_CASE(build_bgfx_vertex_layout_matches_stride)
{
    const VertexLayoutDesc desc = VertexLayoutDesc::position3();
    bgfx::VertexLayout bgfxLayout;
    CHECK(ayt::render::detail::buildBgfxVertexLayout(desc, bgfxLayout));
    CHECK(bgfxLayout.getStride() == desc.strideBytes());
}

TEST_SUITE_END
