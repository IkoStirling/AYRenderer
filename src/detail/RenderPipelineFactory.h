#pragma once
#include "AYRenderer/RenderTypes.h"
#include <memory>

namespace ayt::render::detail {
class RenderPass;
// Owns a fresh pass instance; GPU preparation remains explicit in Renderer.
// Unknown slots return nullptr. Include RenderPass.h where the owner is destroyed.
std::unique_ptr<RenderPass> makeRenderPass(RenderPassSlot slot);
} // namespace ayt::render::detail
