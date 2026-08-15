#include "detail/ShaderPoolSetup.h"

#include "AYShader/ShadercDriver.h"

#include <AYIO/Env.h>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <Windows.h>
#else
#  include <cerrno>
#  include <sys/stat.h>
#endif

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
#ifndef AY_BGFX_SHADER_INCLUDE_HINT
#  define AY_BGFX_SHADER_INCLUDE_HINT ""
#endif
    if (AY_BGFX_SHADER_INCLUDE_HINT[0] && fileExists(AY_BGFX_SHADER_INCLUDE_HINT)) {
        dirs.push_back(AY_BGFX_SHADER_INCLUDE_HINT);
    }
    static const char* kInstallIncludeFallbacks[] = {
        "AYRuntime/AYShader/thirdParty/bgfx-install/debug/include/bgfx",
        "../AYShader/thirdParty/bgfx-install/debug/include/bgfx",
        "../../AYShader/thirdParty/bgfx-install/debug/include/bgfx",
    };
    for (const char* path : kInstallIncludeFallbacks) {
        if (fileExists(path)) {
            dirs.push_back(path);
        }
    }
    return dirs;
}

bool ensureDirectoryExists(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    if (fileExists(path)) {
        return true;
    }
#ifdef _WIN32
    return CreateDirectoryA(path.c_str(), nullptr) != 0
        || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
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
    const std::vector<std::string> includeDirs = shadercIncludeDirs();
    pool.setBgfxIncludeDirs(includeDirs);
    std::fprintf(stderr, "[ShaderPoolSetup] shaderc=%s\n", shadercPath.c_str());
    for (const std::string& dir : includeDirs) {
        std::fprintf(stderr, "[ShaderPoolSetup]   include: %s\n", dir.c_str());
    }
    if (includeDirs.empty()) {
        std::fprintf(stderr,
                     "[ShaderPoolSetup] WARNING: no bgfx include dirs; "
                     "common.sh may be missing\n");
    }
    std::fflush(stderr);
    // Resolve platform/profile from bgfx::getCaps() on first acquire (e.g. D3D11 → s_5_0).
    pool.setAutoProbeFromRendererType(true);
    pool.setHotReloadEnabled(true);

    if (const std::string dumpDir = ayt::io::env::get("AY_SHADER_DUMP_DIR").value_or(""); !dumpDir.empty()) {
        if (dumpDir[0] != '\0' && ensureDirectoryExists(dumpDir)) {
            pool.setIntermediateDumpDirectory(dumpDir);
        }
    }

    return true;
}

} // namespace ayt::render::detail
