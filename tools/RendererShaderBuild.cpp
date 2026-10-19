#include "detail/BuiltinShaderCatalog.h"
#include <AYShader/ShaderBuild.h>
#include <AYIO/FileTransaction.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string_view>
int main(int argc,char** argv) {
    const bool check=argc==5 && std::string_view(argv[4])=="--check";
    if(argc!=4 && !check) { std::cerr<<"Usage: AYRenderer_ShaderBuild <platform> <profile> <package-directory> [--check]\n";return 2; }
    namespace fs=std::filesystem;
    try {
        ayt::shader::ShaderBuildManifest manifest;
        manifest.target={AY_SHADER_SHADERC_HINT,argv[1],argv[2],
            {AY_SHADER_BGFX_COMMON_HINT,AY_SHADER_BGFX_SRC_HINT}};
        manifest.variantBudget=128; manifest.jobs=ayt::render::detail::builtinShaderJobs();
        if(check) {
            ayt::shader::ShaderBuildReport existing;std::vector<std::string> errors;
            if(!ayt::shader::readShaderBuildPackage(argv[3],existing)
                || existing.entries.size()!=manifest.jobs.size()
                || !ayt::shader::validateShaderBuildCoverage(manifest,existing,errors)) {
                std::cerr<<"Production shader package is invalid or stale\n";
                for(const auto& error:errors)std::cerr<<error<<'\n';return 1;
            }
            std::cout<<"Validated "<<existing.entries.size()<<" engine shader variants without compilation\n";return 0;
        }
        ayt::shader::ShaderBuildSession session; ayt::shader::ShaderBuildReport report;
        session.build(manifest,report);
        if(!report.success) { for(const auto& e:report.errors) std::cerr<<e<<'\n';return 1; }
        const auto destination=fs::absolute(fs::u8path(argv[3])).lexically_normal();
        if(destination.filename().empty() || destination==destination.root_path()) throw std::runtime_error("Invalid package destination");
        const auto draft=fs::path(destination.string()+".build-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::vector<std::string> errors;
        if(!ayt::shader::writeShaderBuildPackage(report,draft.string(),errors)) {
            for(const auto& e:errors) std::cerr<<e<<'\n';return 1;
        }
        const auto publication=ayt::io::publishDirectory(draft,destination);
        if(!publication.committed) { std::cerr<<publication.error<<'\n';return 1; }
        std::cout<<"Built "<<report.entries.size()<<" engine shader variants for "<<report.platform<<'/'<<report.profile<<" in "<<report.elapsedMs<<" ms\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
