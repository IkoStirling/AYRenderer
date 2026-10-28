cmake_minimum_required(VERSION 3.20)

project(AYRenderer
    VERSION 0.1.0
    DESCRIPTION "AYRenderer — bgfx-based renderer (Forward+Deferred + SSAO + UI backend)"
)

set(AY_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../AYShader")
include("${AY_SHADER_ROOT}/cmake/AYBgfx.cmake")
find_package(nlohmann_json CONFIG REQUIRED)

# C++ standard follows the root CMakeLists (currently C++23; may move to C++26).
# Do not pin a standard here.

file(GLOB AYRENDERER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/detail/*.cpp"
)

# GpuResources.h contains private records shared across nearly every render
# translation unit.  A stale object compiled against an older GpuMaterial
# layout is an ABI violation inside the static library (typically surfacing as
# std::string/vector heap corruption during material destruction).  Keep this
# dependency explicit as a fallback for MSVC/Ninja environments where the
# localized /showIncludes stream was not recorded correctly.
set_property(SOURCE ${AYRENDERER_SOURCES} APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/detail/GpuResources.h"
)

add_library(AYRenderer STATIC ${AYRENDERER_SOURCES})

# Optional Windows compute/indirect path. Runtime consumes embedded DXBC only;
# AYShader is a build-tool dependency, with no compiler paths in the point path.
if(WIN32 AND TARGET AYShader AND TARGET bgfx::bgfx)
    add_executable(AYRenderer_PointShaderBake EXCLUDE_FROM_ALL tools/GpuPointShaderBake.cpp)
    target_link_libraries(AYRenderer_PointShaderBake PRIVATE AYShader)
    set(_ay_point_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/point-gpu")
    file(GLOB_RECURSE _ay_point_shader_includes CONFIGURE_DEPENDS
        "${AY_SHADER_BGFX_SRC_DIR}/*.sh" "${AY_SHADER_BGFX_COMMON_DIR}/*.sh")
    add_custom_command(OUTPUT "${_ay_point_shader_dir}/GpuPointBinaries.h"
        BYPRODUCTS "${_ay_point_shader_dir}/mark.bin" "${_ay_point_shader_dir}/count.bin"
            "${_ay_point_shader_dir}/prefix.bin" "${_ay_point_shader_dir}/compact.bin"
            "${_ay_point_shader_dir}/vertex.bin" "${_ay_point_shader_dir}/fragment.bin"
            "${_ay_point_shader_dir}/manifest.json"
        COMMAND $<TARGET_FILE:AYRenderer_PointShaderBake>
            --shaderc "${AY_SHADER_SHADERC_PATH}" --bgfx-include "${AY_SHADER_BGFX_SRC_DIR}"
            --common-include "${AY_SHADER_BGFX_COMMON_DIR}" --output "${_ay_point_shader_dir}"
        DEPENDS AYRenderer_PointShaderBake src/detail/GpuPointShaders.h src/detail/GpuPointShaderBinary.h
            "${AY_SHADER_SHADERC_PATH}" ${_ay_point_shader_includes}
        COMMENT "Compiling and embedding point cull/compact shaders" VERBATIM)
    add_custom_target(AYRenderer_PointShaders DEPENDS "${_ay_point_shader_dir}/GpuPointBinaries.h")
    target_sources(AYRenderer PRIVATE "${_ay_point_shader_dir}/GpuPointBinaries.h")
    target_include_directories(AYRenderer PRIVATE "${_ay_point_shader_dir}")
    target_compile_definitions(AYRenderer PRIVATE AY_RENDERER_POINT_GPU=1)
endif()


target_include_directories(AYRenderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(AYRenderer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../AYFont
    ${CMAKE_CURRENT_SOURCE_DIR}/../AYResource/include/assetsImpl
    ${CMAKE_CURRENT_SOURCE_DIR}/../AYResource/interface
    ${CMAKE_CURRENT_SOURCE_DIR}/../AYResource/interface/assetsDefs
)

target_link_libraries(AYRenderer PUBLIC
    AY::Module
    AYShader
    AYMath
    AYIO
    AYEventSystem
    AYGameLoop
    AYUI
)

target_link_libraries(AYRenderer PRIVATE
    nlohmann_json::nlohmann_json
    AYResource
    AYFont
    bgfx::bgfx
    bgfx::bimg
    bgfx::bimg_decode
    bgfx::bimg_encode
)

# UI text uses AYFont (FreeType glyph atlas).
add_executable(AYRenderer_PresentationPlanBench EXCLUDE_FROM_ALL tools/PresentationPlanBench.cpp)
target_link_libraries(AYRenderer_PresentationPlanBench PRIVATE AYRenderer nlohmann_json::nlohmann_json)
target_include_directories(AYRenderer_PresentationPlanBench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(WIN32)
    add_executable(AYRenderer_PresentationGpuBench EXCLUDE_FROM_ALL tools/PresentationGpuBench.cpp)
    target_link_libraries(AYRenderer_PresentationGpuBench PRIVATE AYRenderer nlohmann_json::nlohmann_json bgfx::bgfx user32)
endif()

add_executable(AYRenderer_ShaderBuild tools/RendererShaderBuild.cpp)
target_link_libraries(AYRenderer_ShaderBuild PRIVATE AYRenderer)
target_include_directories(AYRenderer_ShaderBuild PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(AYRenderer_ShaderBuild PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AYShaderProduction.cmake")
if(TARGET AYRenderer_PresentationGpuBench)
    ay_stage_production_shaders(AYRenderer_PresentationGpuBench)
endif()

if(MSVC)
    # bgfx/bx headers require conforming preprocessor and __cplusplus on MSVC.
    target_compile_options(AYRenderer PRIVATE /Zc:__cplusplus /Zc:preprocessor)
    target_compile_definitions(AYRenderer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_compile_definitions(AYRenderer PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
    AY_BGFX_SHADER_INCLUDE_HINT="${AY_BGFX_SHADER_INCLUDE_DIR}"
)

# §5.5 cleanup (2026-07-22): the two F1-diag CMake options
# (AY_F1_DIAG_LIGHT, AY_F1_DIAG_FRAME_SHADOW) and their foreach
# target_compile_definitions block are removed. The diagnostic code
# paths those flags gated are permanently retired (RenderScene::Light,
# FrameContext shadow writeback, lastFrameShadowFbo cache). The
# corresponding include/AYRenderer/F1DiagFlags.h still defines both macros to
# 0 unconditionally so any stale `#if AY_F1_DIAG_*` block in a TU
# that hasn't been edited yet still compiles with the diagnostic-off
# branch selected. See docs/f1-sigsegv-repro.md for the historical
# F1 SIGSEGV bisect that motivated these guards.
#
# AY_F1_DIAG_DEFAULT_SHADOW was retired in E4 (§5.4, 2026-07-22).

enable_testing()
add_subdirectory(unittest)
add_subdirectory(demo)

message(STATUS "AYRenderer v0.1.0 (Forward+Deferred + SSAO v2 + GBuffer debug + selection outline) — see ${CMAKE_CURRENT_SOURCE_DIR}/design.md")
