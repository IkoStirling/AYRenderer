#pragma once

#include <AYGameLoop/SubSystemModule.h>

#include <string_view>

namespace ayt::render
{

inline constexpr std::string_view kRendererRuntimeModuleId =
    "AYRenderer.Runtime";

class RendererRuntimeModule final : public ayt::game::SubSystemModule
{
public:
    RendererRuntimeModule();
};

} // namespace ayt::render
