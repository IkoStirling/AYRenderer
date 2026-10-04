#pragma once
#include "AYShader/ShaderBuild.h"
namespace ayt::render::detail {
inline shader::ShaderBuildJob embeddedShader(const std::string& name,const std::string& source) {
    shader::ShaderBuildJob job; job.name=name; job.variant="default"; job.sourceText=source; return job;
}
inline shader::ShaderBuildJob rawShader(const std::string& name,const std::string& vertex,
    const std::string& fragment,const std::string& varying) {
    shader::ShaderBuildJob job; job.name=name; job.variant="default";
    job.vertexSc=vertex; job.fragmentSc=fragment; job.varyingSc=varying; return job;
}
void appendGBufferPassShaders(std::vector<shader::ShaderBuildJob>& jobs);
void appendColorGradingPassShaders(std::vector<shader::ShaderBuildJob>& jobs);
void appendGBufferDebugPassShaders(std::vector<shader::ShaderBuildJob>& jobs);
void appendMotionVectorPassShaders(std::vector<shader::ShaderBuildJob>& jobs);
void appendPostProcessPassShaders(std::vector<shader::ShaderBuildJob>& jobs);
void appendPresentPassShaders(std::vector<shader::ShaderBuildJob>& jobs);
void appendSkyboxPassShaders(std::vector<shader::ShaderBuildJob>& jobs);
void appendTAAPassShaders(std::vector<shader::ShaderBuildJob>& jobs);
void appendTransparentPassShaders(std::vector<shader::ShaderBuildJob>& jobs);
void appendFXAAPassShaders(std::vector<shader::ShaderBuildJob>& jobs);
void appendSMAAPassShaders(std::vector<shader::ShaderBuildJob>& jobs);
void appendEditorOverlayPassShaders(std::vector<shader::ShaderBuildJob>& jobs);
void appendUiGpuContextShaders(std::vector<shader::ShaderBuildJob>& jobs);
std::vector<shader::ShaderBuildJob> builtinShaderJobs();
}
