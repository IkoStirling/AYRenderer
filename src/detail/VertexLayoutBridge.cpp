#include "detail/VertexLayoutBridge.h"

namespace ayt::render::detail
{

namespace {

bool mapAttribute(VertexAttribute attr, bgfx::Attrib::Enum& out)
{
    switch (attr) {
    case VertexAttribute::Position:  out = bgfx::Attrib::Position;  return true;
    case VertexAttribute::Normal:    out = bgfx::Attrib::Normal;    return true;
    case VertexAttribute::TexCoord0: out = bgfx::Attrib::TexCoord0; return true;
    case VertexAttribute::Tangent:   out = bgfx::Attrib::Tangent;   return true;
    case VertexAttribute::Color0:   out = bgfx::Attrib::Color0;   return true;
    }
    return false;
}

bool mapComponentType(VertexComponentType type, bgfx::AttribType::Enum& out)
{
    switch (type) {
    case VertexComponentType::Float: out = bgfx::AttribType::Float; return true;
    case VertexComponentType::Uint8: out = bgfx::AttribType::Uint8; return true;
    }
    return false;
}

} // namespace

bool buildBgfxVertexLayout(const VertexLayoutDesc& desc, bgfx::VertexLayout& out)
{
    if (!desc.isValid()) {
        return false;
    }

    out = bgfx::VertexLayout{};
    out.begin();
    for (uint8_t i = 0; i < desc.elementCount; ++i) {
        const VertexElement& el = desc.elements[i];
        if (el.componentCount == 0 || el.componentCount > 4) {
            return false;
        }

        bgfx::Attrib::Enum bgfxAttr{};
        bgfx::AttribType::Enum bgfxType{};
        if (!mapAttribute(el.attribute, bgfxAttr) || !mapComponentType(el.componentType, bgfxType)) {
            return false;
        }

        out.add(bgfxAttr, el.componentCount, bgfxType, el.normalized);
    }
    out.end();
    return out.getStride() == desc.strideBytes();
}

} // namespace ayt::render::detail
