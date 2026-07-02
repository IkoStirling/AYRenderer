#pragma once

#include "AYRenderTypes.h"

#include <bgfx/bgfx.h>

namespace ayt::render::detail
{

bool buildBgfxVertexLayout(const VertexLayoutDesc& desc, bgfx::VertexLayout& out);

} // namespace ayt::render::detail
