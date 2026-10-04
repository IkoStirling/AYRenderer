#include "detail/ShaderProduction.h"
#include "detail/BuiltinShaderCatalog.h"
#include "AYShader/detail/ShaderRuntimeKey.h"
#include "AYShader/ShaderBuild.h"
#include <set>
#ifdef _WIN32
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif
namespace ayt::render::detail {
std::filesystem::path defaultShaderPackageRoot() {
#ifdef _WIN32
    std::wstring name(32768,L'\0');
    const auto length=GetModuleFileNameW(nullptr,name.data(),DWORD(name.size()));
    if(!length || length>=name.size()) return {};
    name.resize(length); return std::filesystem::path(name).parent_path()/"Shaders";
#elif defined(__APPLE__)
    uint32_t size=0; _NSGetExecutablePath(nullptr,&size);
    std::string name(size,'\0'); if(_NSGetExecutablePath(name.data(),&size)) return {};
    return std::filesystem::path(name.c_str()).parent_path()/"Shaders";
#else
    std::string name(32768,'\0'); const auto size=readlink("/proc/self/exe",name.data(),name.size());
    if(size<=0 || size>=name.size()) return {};
    name.resize(size); return std::filesystem::path(name).parent_path()/"Shaders";
#endif
}
bool loadProductionShaderPackages(shader::ShaderResourcePool& pool,
    const std::filesystem::path& root,bool required,std::vector<std::string>& errors) {
    errors.clear();
    try {
        namespace fs=std::filesystem;
        if(root.empty() || !fs::exists(root)) {
            if(required) errors={"Production shader package root is missing: "+root.string()};
            return !required;
        }
        if(fs::is_symlink(root) || !fs::is_directory(root)) throw std::runtime_error("Invalid shader package root");
        size_t scanned=0;
        std::vector<shader::ShaderBuildReport> reports;
        bool engineFound=false;
        for(const auto& target:fs::directory_iterator(root)) {
            if(++scanned>16) throw std::runtime_error("Too many shader target directories");
            const auto name=target.path().filename().string(); const auto split=name.find('-');
            if(split==std::string::npos || !pool.matchesCompilationTarget(name.substr(0,split),name.substr(split+1))) continue;
            if(target.is_symlink() || !target.is_directory()) throw std::runtime_error("Invalid shader target directory");
            for(const char* kind:{"engine","project"}) {
                const auto directory=target.path()/kind;
                if(!fs::exists(directory)) continue;
                if(fs::is_symlink(directory)) throw std::runtime_error("Symlink shader package rejected");
                shader::ShaderBuildReport report;
                if(!shader::readShaderBuildPackage(directory.string(),report)) { errors=report.errors;return false; }
                if(!pool.matchesCompilationTarget(report.platform,report.profile)
                    || name!=report.platform+"-"+report.profile) throw std::runtime_error("Shader package target mismatch");
                if(std::string_view(kind)=="engine") {
                    std::set<std::string> entries;
                    for(const auto& entry:report.entries) entries.insert(entry.runtimeKey);
                    for(const auto& job:builtinShaderJobs()) {
                        const bool raw=!job.vertexSc.empty();
                        const auto source=raw?shader::detail::frameShaderCacheFields({job.vertexSc,job.fragmentSc,job.varyingSc}):job.sourceText;
                        if(!entries.count(shader::detail::shaderRuntimeKey(raw?"bgfx-sc":"phoskia",source,job.defines,report.platform,report.profile)))
                            throw std::runtime_error("Engine shader catalog is stale/incomplete: "+job.name+"/"+job.variant);
                    }
                    engineFound=true;
                }
                reports.push_back(std::move(report));
            }
        }
        if(required && !engineFound) throw std::runtime_error("No engine shader package for the active backend");
        for(const auto& report:reports) if(!pool.installPrecompiled(report,errors)) return false;
        return true;
    } catch(const std::exception& e) { errors.push_back(e.what());return false; }
}
}
