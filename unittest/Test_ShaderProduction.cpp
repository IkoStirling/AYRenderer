#include <AYTest.h>
#include "detail/BuiltinShaderCatalog.h"
#include "detail/ShaderProduction.h"
#include "detail/ShaderPoolSetup.h"
#include <AYShader/detail/ShaderRuntimeKey.h>
#include <AYIO/File.h>
#include <bgfx/bgfx.h>
#include <chrono>
#include <filesystem>
#include <set>
using namespace ayt::render::detail;
TEST_SUITE(ShaderProductionTests)
TEST_CASE(complete_catalog_prewarms_normal_paths_without_compiler) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("shader_production_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path root; ~Cleanup() {std::error_code ec;fs::remove_all(root,ec);} } cleanup{root};
    const auto jobs=builtinShaderJobs(); CHECK(jobs.size()==64);
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
        CHECK(!loadProductionShaderPackages(pool,root,true,errors));
        pool.shutdown();
    }
    bgfx::shutdown();
}
}
