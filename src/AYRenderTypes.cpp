#include "AYRenderer/RenderTypes.h"

#include <cstdio>

namespace ayt::render
{

namespace {

// §P1 L4 (2026-08-24) — silent 0 hides malformed layouts. add() and
// isValid() callers see "invalid" but never learn why. Now the
// rejection path logs componentCount/componentType/normalized once
// per shape so the developer can pinpoint the bad attribute.
uint32_t elementSizeBytes(const VertexElement& el, bool& outOk)
{
    outOk = true;
    if (el.componentCount == 0 || el.componentCount > 4) {
        std::fprintf(stderr,
                     "[AYRenderTypes] elementSizeBytes: invalid componentCount=%u\n",
                     static_cast<unsigned>(el.componentCount));
        outOk = false;
        return 0;
    }
    switch (el.componentType) {
    case VertexComponentType::Float:
        return static_cast<uint32_t>(el.componentCount) * sizeof(float);
    case VertexComponentType::Uint8:
        return static_cast<uint32_t>(el.componentCount) * sizeof(uint8_t);
    }
    std::fprintf(stderr,
                 "[AYRenderTypes] elementSizeBytes: unknown componentType=%u\n",
                 static_cast<unsigned>(el.componentType));
    outOk = false;
    return 0;
}

// Wrapper for callers that just want the size and don't care about
// the ok flag (keeps the internal helper signature private).
uint32_t elementSizeBytes(const VertexElement& el)
{
    bool ok = true;
    return elementSizeBytes(el, ok);
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
    // BoneIndices are integer-valued u8 slots carried through a vec4 shader
    // input. They must NOT be normalized: shaders use int(a_indices.*), so
    // UNORM would collapse every local palette index below 255 to slot 0.
    layout.add(VertexElement{
        VertexAttribute::BoneIndices, 4, VertexComponentType::Uint8, false});
    // BoneWeights: 4 x f32.
    layout.add(VertexElement{
        VertexAttribute::BoneWeights, 4, VertexComponentType::Float, false});
    return layout;
}

} // namespace ayt::render
