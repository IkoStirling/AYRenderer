#include "detail/FullscreenPassGeometry.h"

#include "detail/BGFXAdapter.h"
#include "detail/RenderPass.h"

#include "AYMath/MathUtils.h"

namespace ayt::render::detail
{

namespace {

struct alignas(16) FullscreenVertex {
    float x;
    float y;
    float u;
    float v;
};

constexpr FullscreenVertex kFullscreenTriangle[3] = {
    {-1.0f, -1.0f, 0.0f,  1.0f},
    { 3.0f, -1.0f, 2.0f,  1.0f},
    {-1.0f,  3.0f, 0.0f, -1.0f},
};

constexpr uint16_t kFullscreenIndices[3] = {0, 1, 2};

} // namespace

bool FullscreenPassGeometry::ensure(BGFXAdapter& adapter)
{
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();
    return ensureFullscreenTriangleBuffers(
        adapter,
        _vertexBuffer,
        _indexBuffer,
        kFullscreenTriangle,
        sizeof(kFullscreenTriangle),
        layout,
        kFullscreenIndices,
        sizeof(kFullscreenIndices));
}

bool FullscreenPassGeometry::isReady() const noexcept
{
    return BGFXAdapter::isValid(_vertexBuffer)
        && BGFXAdapter::isValid(_indexBuffer);
}

void FullscreenPassGeometry::bind(BGFXAdapter& adapter) const
{
    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_vertexBuffer, 0, UINT32_MAX);
    adapter.setIndexBuffer(_indexBuffer, 0, 3);
}

void FullscreenPassGeometry::destroy(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_vertexBuffer)) {
        adapter.destroy(_vertexBuffer);
        _vertexBuffer = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_indexBuffer)) {
        adapter.destroy(_indexBuffer);
        _indexBuffer = BGFX_INVALID_HANDLE;
    }
}

void configureFullscreenPassView(BGFXAdapter& adapter,
                                 uint8_t viewId,
                                 bgfx::FrameBufferHandle target,
                                 uint16_t x,
                                 uint16_t y,
                                 uint16_t width,
                                 uint16_t height)
{
    const ayt::math::Float4x4 identity = ayt::math::Float4x4::identity();
    adapter.setViewFrameBuffer(viewId, target);
    adapter.setViewRect(viewId, x, y, width, height);
    adapter.setViewTransform(viewId, identity, identity);
    adapter.setViewClearRaw(viewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);
}

} // namespace ayt::render::detail
