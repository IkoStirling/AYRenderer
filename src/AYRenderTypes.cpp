#include "AYRenderer/RenderTypes.h"

namespace ayt::render
{

namespace {

uint32_t elementSizeBytes(const VertexElement& el)
{
    if (el.componentCount == 0 || el.componentCount > 4) {
        return 0;
    }
    switch (el.componentType) {
    case VertexComponentType::Float:
        return static_cast<uint32_t>(el.componentCount) * sizeof(float);
    case VertexComponentType::Uint8:
        return static_cast<uint32_t>(el.componentCount) * sizeof(uint8_t);
    }
    return 0;
}

} // namespace

bool VertexLayoutDesc::add(const VertexElement& element)
{
    if (elementCount >= kMaxElements || elementSizeBytes(element) == 0) {
        return false;
    }
    elements[elementCount++] = element;
    return true;
}

bool VertexLayoutDesc::isValid() const noexcept
{
    if (elementCount == 0 || elementCount > kMaxElements) {
        return false;
    }
    uint32_t stride = 0;
    for (uint8_t i = 0; i < elementCount; ++i) {
        const uint32_t size = elementSizeBytes(elements[i]);
        if (size == 0) {
            return false;
        }
        stride += size;
    }
    return stride > 0;
}

uint32_t VertexLayoutDesc::strideBytes() const noexcept
{
    if (!isValid()) {
        return 0;
    }
    uint32_t stride = 0;
    for (uint8_t i = 0; i < elementCount; ++i) {
        stride += elementSizeBytes(elements[i]);
    }
    return stride;
}

VertexLayoutDesc VertexLayoutDesc::position3()
{
    VertexLayoutDesc layout;
    layout.add(VertexElement{
        VertexAttribute::Position, 3, VertexComponentType::Float, false});
    return layout;
}

VertexLayoutDesc VertexLayoutDesc::position3Normal3()
{
    VertexLayoutDesc layout = position3();
    layout.add(VertexElement{
        VertexAttribute::Normal, 3, VertexComponentType::Float, false});
    return layout;
}

VertexLayoutDesc VertexLayoutDesc::position3TexCoord2()
{
    VertexLayoutDesc layout = position3();
    layout.add(VertexElement{
        VertexAttribute::TexCoord0, 2, VertexComponentType::Float, false});
    return layout;
}

VertexLayoutDesc VertexLayoutDesc::position3Normal3TexCoord2()
{
    VertexLayoutDesc layout = position3Normal3();
    layout.add(VertexElement{
        VertexAttribute::TexCoord0, 2, VertexComponentType::Float, false});
    return layout;
}

VertexLayoutDesc VertexLayoutDesc::skinnedAddon()
{
    VertexLayoutDesc layout;
    // BoneIndices: 4 x u8, normalized to [0,1] so bgfx exposes them as float in the shader.
    layout.add(VertexElement{
        VertexAttribute::BoneIndices, 4, VertexComponentType::Uint8, true});
    // BoneWeights: 4 x f32.
    layout.add(VertexElement{
        VertexAttribute::BoneWeights, 4, VertexComponentType::Float, false});
    return layout;
}

} // namespace ayt::render
