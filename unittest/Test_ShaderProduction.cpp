#include <AYTest.h>
#include "detail/BuiltinShaderCatalog.h"
#include "detail/ShaderProduction.h"
#include "detail/ShaderPoolSetup.h"
#include "detail/PresentationStageCatalog.h"
#include <AYShader/detail/ShaderRuntimeKey.h>
#include <AYIO/File.h>
#include <bgfx/bgfx.h>
#include <chrono>
#include <filesystem>
#include <set>
#include <array>
#ifdef _WIN32
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <cerrno>
extern char** environ;
#endif
using namespace ayt::render::detail;
TEST_SUITE(ShaderProductionTests)
TEST_CASE(complete_catalog_prewarms_normal_paths_without_compiler) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("shader_production_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path root; ~Cleanup() {std::error_code ec;fs::remove_all(root,ec);} } cleanup{root};
    const auto jobs=builtinShaderJobs(); CHECK(jobs.size()==78);
    bool hasInstancedPoints=false;
    for(const auto& job:jobs)hasInstancedPoints|=job.name=="emissive_point_instanced";
    CHECK(hasInstancedPoints);
    ayt::shader::ShaderBuildManifest manifest;
    manifest.target={AY_SHADER_SHADERC_HINT,"windows","s_5_0",{AY_SHADER_BGFX_COMMON_HINT,AY_SHADER_BGFX_SRC_HINT}};
    manifest.variantBudget=128; manifest.jobs=jobs;
    ayt::shader::ShaderBuildSession session;ayt::shader::ShaderBuildReport report;
    session.build(manifest,report); CHECK(report.success);
    if(!report.success) {for(const auto& e:report.errors) CHECK_MSG(false,e.c_str());return;}
    std::vector<std::string> errors;
    CHECK(ayt::shader::validateShaderBuildCoverage(manifest,report,errors));
    auto renamed=report; renamed.entries.front().name="renamed-same-bytes";
    CHECK(!ayt::shader::validateShaderBuildCoverage(manifest,renamed,errors));
    auto wrongMacro=report; wrongMacro.entries.front().defines={"MISSING=1"};
    CHECK(!ayt::shader::validateShaderBuildCoverage(manifest,wrongMacro,errors));
    const auto package=root/"windows-s_5_0"/"engine";
    CHECK(ayt::shader::writeShaderBuildPackage(report,package.string(),errors));
#ifdef AY_RENDERER_SHADER_BUILD_TOOL
    std::array<std::string,5> checkArguments{AY_RENDERER_SHADER_BUILD_TOOL,"windows","s_5_0",package.string(),"--check"};
    const auto checkPackage=[&]() {
        char* args[]={checkArguments[0].data(),checkArguments[1].data(),checkArguments[2].data(),checkArguments[3].data(),checkArguments[4].data(),nullptr};
#ifdef _WIN32
        return int(_spawnv(_P_WAIT,args[0],args));
#else
        pid_t child=0;if(posix_spawn(&child,args[0],nullptr,nullptr,args,environ)!=0)return -1;
        int status=0;pid_t result;
        do{result=waitpid(child,&status,0);}while(result<0 && errno==EINTR);
        return result==child && WIFEXITED(status)?WEXITSTATUS(status):-1;
#endif
    };
    CHECK(checkPackage()==0);
#endif
    bgfx::Init init;init.type=bgfx::RendererType::Noop;
    const bool initialized=bgfx::init(init);CHECK(initialized);if(!initialized)return;
    {
        ayt::shader::ShaderResourcePool pool;
        pool.setPlatform("windows");pool.setGLSLProfile("s_5_0");
        pool.setShadercExecutable("missing-production-compiler.exe");
        pool.setRuntimeCompilationEnabled(false);
        CHECK(loadProductionShaderPackages(pool,root,true,errors));
        for(const auto& job:jobs) {
            const auto resource=job.vertexSc.empty()?pool.acquire(job.sourceText,"unrelated-runtime-label"):
                pool.acquireFromBgfxSc(job.vertexSc,job.fragmentSc,job.varyingSc,"unrelated-label");
            CHECK_MSG(resource.isValid(),job.name.c_str());
        }
        CHECK(pool.cacheStats().binaryMisses==0); CHECK(pool.cacheStats().sourceMisses==0);
        // Repeated full catalog ownership changes must invalidate old tokens.
        for(int cycle=0;cycle<8;++cycle) {
            const auto& job=jobs.front();
            auto old=job.vertexSc.empty()?pool.acquire(job.sourceText):pool.acquireFromBgfxSc(job.vertexSc,job.fragmentSc,job.varyingSc);
            CHECK(old.isValid()); pool.shutdown(); CHECK(!old.isValid());
            // bgfx defers uniform/program release. Drain the preceding complete
            // catalog before loading another, rather than requiring two full
            // generations to fit the backend's fixed uniform-handle capacity.
            bgfx::frame(); bgfx::frame(); bgfx::frame();
            CHECK(loadProductionShaderPackages(pool,root,true,errors));
            CHECK(pool.cacheStats().binaryMisses==0); CHECK(pool.cacheStats().sourceMisses==0);
            bgfx::frame(); bgfx::frame(); bgfx::frame();
        }
        CHECK(!pool.acquire("material Missing {vertex {return vec4(0.0)} fragment {return vec4(1.0)}}").isValid());
        CHECK(!loadProductionShaderPackages(pool,root/"missing",true,errors));
        CHECK(loadProductionShaderPackages(pool,root/"missing",false,errors));
        // A valid but incomplete package must not silently run missing passes.
        fs::rename(package,root/"saved");
        report.entries.pop_back();
        CHECK(ayt::shader::writeShaderBuildPackage(report,package.string(),errors));
#ifdef AY_RENDERER_SHADER_BUILD_TOOL
        CHECK(checkPackage()==1);
#endif
        CHECK(!loadProductionShaderPackages(pool,root,true,errors));
        pool.shutdown();
    }
    bgfx::shutdown();
}

TEST_CASE(stage_local_extra_programs_roundtrip_and_install_without_runtime_compilation)
{
    namespace fs=std::filesystem;
    constexpr const char* finalSource="material CompositeFinal {vertex {in p : position; return vec4(p,1.0)} fragment {return vec4(0.2,0.3,0.4,1.0)}}";
    constexpr const char* horizontalSource="material CompositeHorizontal {vertex {in p : position; return vec4(p,1.0)} fragment {return vec4(0.5,0.2,0.1,1.0)}}";
    constexpr const char* verticalSource="material CompositeVertical {vertex {in p : position; return vec4(p,1.0)} fragment {return vec4(0.1,0.4,0.8,1.0)}}";
    auto info=kPresentationStages.front();info.shaderJob="composite_final";
    std::vector<ayt::shader::ShaderBuildJob> jobs;
    appendPresentationStageProgramSet(jobs,info,finalSource,[&](auto& ownJobs) {
        ownJobs.push_back(embeddedShader("composite_horizontal",horizontalSource));
        ownJobs.push_back(embeddedShader("composite_vertical",verticalSource));
    });
    CHECK(jobs.size()==3);CHECK(jobs.front().name==info.shaderJob);
    ayt::shader::ShaderBuildManifest manifest;
    manifest.target={AY_SHADER_SHADERC_HINT,"windows","s_5_0",{AY_SHADER_BGFX_COMMON_HINT,AY_SHADER_BGFX_SRC_HINT}};
    manifest.variantBudget=3;manifest.jobs=jobs;
    ayt::shader::ShaderBuildSession session;ayt::shader::ShaderBuildReport built;
    session.build(manifest,built);CHECK(built.success);
    if(!built.success){for(const auto& error:built.errors)CHECK_MSG(false,error.c_str());return;}
    const auto parent=fs::temp_directory_path();
    const auto root=parent/("presentation_program_set_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const bool created=fs::create_directory(root);CHECK(created);if(!created)return;
    struct Cleanup {
        fs::path parent,root;
        ~Cleanup() {
            std::error_code error;
            const auto allowed=fs::weakly_canonical(parent,error)/root.filename();if(error)return;
            const auto resolved=fs::weakly_canonical(root,error);
            if(!error && resolved==allowed && root.filename().string().starts_with("presentation_program_set_"))
                fs::remove_all(resolved,error);
        }
    } cleanup{parent,root};
    const auto package=root/"package";
    std::vector<std::string> errors;
    const bool written=ayt::shader::writeShaderBuildPackage(built,package.string(),errors);CHECK(written);
    if(!written){for(const auto& error:errors)CHECK_MSG(false,error.c_str());return;}
    ayt::shader::ShaderBuildReport loaded;
    const bool read=ayt::shader::readShaderBuildPackage(package.string(),loaded);CHECK(read);if(!read)return;
    CHECK(loaded.entries.size()==3);
    CHECK(ayt::shader::validateShaderBuildCoverage(manifest,loaded,errors));
    bgfx::Init init;init.type=bgfx::RendererType::Noop;
    const bool initialized=bgfx::init(init);CHECK(initialized);if(!initialized)return;
    struct DeviceCleanup{~DeviceCleanup(){bgfx::shutdown();}} deviceCleanup;
    ayt::shader::ShaderResourcePool pool;
    pool.setPlatform("windows");pool.setGLSLProfile("s_5_0");
    pool.setShadercExecutable("missing-composite-compiler.exe");pool.setRuntimeCompilationEnabled(false);
    CHECK(pool.installPrecompiled(loaded,errors));
    for(const auto& job:jobs)CHECK_MSG(pool.acquire(job.sourceText,"unrelated-label").isValid(),job.name.c_str());
    CHECK(pool.cacheStats().binaryMisses==0);CHECK(pool.cacheStats().sourceMisses==0);
    pool.shutdown();
}
}
