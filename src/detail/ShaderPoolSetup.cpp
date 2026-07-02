#include "detail/ShaderPoolSetup.h"

#include "AYShadercDriver.h"

#include <sys/stat.h>

#include <string>
#include <vector>

namespace ayt::render::detail
{

namespace {

bool fileExists(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

#ifndef AY_SHADER_SHADERC_HINT
#  ifdef _WIN32
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc.exe"
#  else
#    define AY_SHADER_SHADERC_HINT "thirdParty/bgfx-install/debug/bin/shaderc"
#  endif
#endif

#ifndef AY_SHADER_BGFX_COMMON_HINT
#  define AY_SHADER_BGFX_COMMON_HINT "../../../thirdparty/bgfx/examples/common"
#endif

#ifndef AY_SHADER_BGFX_SRC_HINT
#  define AY_SHADER_BGFX_SRC_HINT "../../../thirdparty/bgfx/src"
#endif

std::vector<std::string> shadercIncludeDirs()
{
    std::vector<std::string> dirs;
    if (AY_SHADER_BGFX_COMMON_HINT[0] && fileExists(AY_SHADER_BGFX_COMMON_HINT)) {
        dirs.push_back(AY_SHADER_BGFX_COMMON_HINT);
    }
    if (AY_SHADER_BGFX_SRC_HINT[0] && fileExists(AY_SHADER_BGFX_SRC_HINT)) {
        dirs.push_back(AY_SHADER_BGFX_SRC_HINT);
    }
    return dirs;
}

} // namespace

bool configureShaderPool(shader::ShaderResourcePool& pool)
{
    const std::string shadercPath = AY_SHADER_SHADERC_HINT;
    if (!fileExists(shadercPath)) {
        return false;
    }

    try {
        shader::AYShadercDriver probe(shadercPath);
        if (probe.shadercPath().empty()) {
            return false;
        }
        shader::AYShadercDriver::setDefaultExecutable(shadercPath);
    } catch (...) {
        return false;
    }

    pool.setShadercExecutable(shadercPath);
    pool.setBgfxIncludeDirs(shadercIncludeDirs());
    // Resolve platform/profile from bgfx::getCaps() on first acquire (e.g. D3D11 → s_5_0).
    pool.setAutoProbeFromRendererType(true);
    return true;
}

} // namespace ayt::render::detail
