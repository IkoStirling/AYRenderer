#pragma once
#include "AYShader/ShaderResourcePool.h"
#include <filesystem>
namespace ayt::render::detail {
std::filesystem::path defaultShaderPackageRoot();
bool loadProductionShaderPackages(shader::ShaderResourcePool& pool,
    const std::filesystem::path& root, bool required, std::vector<std::string>& errors);
}
