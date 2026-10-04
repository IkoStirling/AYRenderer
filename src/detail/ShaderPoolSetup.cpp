#include "detail/ShaderPoolSetup.h"
#include "detail/ShaderProduction.h"

#include "AYShader/ShadercDriver.h"
#include <AYIO/File.h>

#include <AYIO/Env.h>
#include <algorithm>
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
#  define AY_SHADER_SHADERC_HINT ""
#endif

#ifndef AY_SHADER_BGFX_COMMON_HINT
#  define AY_SHADER_BGFX_COMMON_HINT ""
#endif

#ifndef AY_SHADER_BGFX_SRC_HINT
#  define AY_SHADER_BGFX_SRC_HINT ""
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
    if (AY_BGFX_SHADER_INCLUDE_HINT[0] && fileExists(AY_BGFX_SHADER_INCLUDE_HINT)
        && std::find(dirs.begin(),dirs.end(),AY_BGFX_SHADER_INCLUDE_HINT)==dirs.end()) {
        dirs.push_back(AY_BGFX_SHADER_INCLUDE_HINT);
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
try {
    const auto packageOverride=ayt::io::env::get("AY_SHADER_PACKAGE_ROOT");
    const auto packageRoot=packageOverride?std::filesystem::u8path(*packageOverride):defaultShaderPackageRoot();
    bool authoring=false;
    bool development=
#ifdef NDEBUG
        false;
#else
        true;
#endif
    if(std::filesystem::exists(packageRoot/"production.policy")) {
        if(ayt::io::File::readAllText((packageRoot/"production.policy").string())!="AYPhoskiaProduction/1\n") {
            std::fprintf(stderr,"Invalid production shader policy\n");return false;
        }
        development=false; // Published Debug packages also use production policy.
    }
    const auto override=ayt::io::env::get("AY_SHADER_RUNTIME_COMPILE");
    if(std::filesystem::exists(packageRoot/"authoring.policy")) {
        if(ayt::io::File::readAllText((packageRoot/"authoring.policy").string())!="AYPhoskiaAuthoring/1\n") {
            std::fprintf(stderr,"Invalid authoring shader policy\n");return false;
        }
        authoring=true; development=true; // Explicit application role, even in Release.
    }
    if(override) {
        if(*override!="0" && *override!="1") { std::fprintf(stderr,"Invalid AY_SHADER_RUNTIME_COMPILE (expected 0/1)\n");return false; }
        development=*override=="1";
    }
    pool.setAutoProbeFromRendererType(true);
    pool.resolvePlatformFromRenderer();
    pool.setRuntimeCompilationEnabled(development);
    auto includeDirs=development?shadercIncludeDirs():std::vector<std::string>{};
    std::string shadercPath=AY_SHADER_SHADERC_HINT;
    if(development && authoring && std::filesystem::exists(packageRoot/"authoring")) {
        const auto toolsRoot=packageRoot/"authoring";
#ifdef _WIN32
        shadercPath=(toolsRoot/"shaderc.exe").string();
#else
        shadercPath=(toolsRoot/"shaderc").string();
#endif
        includeDirs={(toolsRoot/"include-common").string(),(toolsRoot/"include-src").string()};
        if(!fileExists(shadercPath) || !std::filesystem::is_directory(includeDirs[0]) || !std::filesystem::is_directory(includeDirs[1])) {
            std::fprintf(stderr,"Incomplete authoring shader compiler package\n");return false;
        }
    }
    pool.setBgfxIncludeDirs(development?includeDirs:std::vector<std::string>{});
    std::vector<std::string> errors;
    if(!loadProductionShaderPackages(pool,packageRoot,!development || authoring || packageOverride.has_value(),errors)) {
        for(const auto& error:errors) std::fprintf(stderr,"[ShaderPoolSetup] %s\n",error.c_str());
        return false;
    }
    if(!development) return true; // No shaderc probe or development paths in shipping.
    if (!fileExists(shadercPath)) {
        // §P1 L6 (2026-08-24) — surface the missing shaderc at the call
        // site. Previously the function returned false silently and the
        // host saw only "all Phoskia materials compile to white" much
        // later. Print the path + build-time hint so the developer can
        // act immediately (vcpkg install shaderc, point AY_SHADER_SHADERC_HINT,
        // etc.).
        std::fprintf(stderr,
                     "[ShaderPoolSetup] shaderc executable not found at '%s' "
                     "(build-time AY_SHADER_SHADERC_HINT). Phoskia material "
                     "compiles will fail until resolved.\n",
                     shadercPath.c_str());
        std::fflush(stderr);
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
} catch(const std::exception& error) {
    std::fprintf(stderr,"[ShaderPoolSetup] configuration failed: %s\n",error.what());
    return false;
}

} // namespace ayt::render::detail
