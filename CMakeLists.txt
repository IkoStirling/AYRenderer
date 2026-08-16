cmake_minimum_required(VERSION 3.20)

project(AYRenderer
    VERSION 0.1.0
    DESCRIPTION "AYRenderer — bgfx-based renderer (Forward+Deferred + SSAO + UI backend)"
)

set(AY_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../AYShader")
include("${AY_SHADER_ROOT}/cmake/AYBgfx.cmake")

# C++ standard follows the root CMakeLists (currently C++23; may move to C++26).
# Do not pin a standard here.

file(GLOB AYRENDERER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/detail/*.cpp"
)

add_library(AYRenderer STATIC ${AYRENDERER_SOURCES})

target_include_directories(AYRenderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    # INT-04 (2026-07-20): AYApplication/AppEventHost.h (EventBusHostScope) + AYEventSystem/Events/AYEventSystem/Events/AYEventSystem/Events/WindowEvents.h.
    # PUBLIC because AYRenderer/RendererSubSystem.h (in include/) references these types,
    # so consumers of AYRenderer (e.g. AYEntity) need the include path.
    ${CMAKE_CURRENT_SOURCE_DIR}/../AYApplication/interface
)

target_include_directories(AYRenderer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../AYFont
    ${CMAKE_CURRENT_SOURCE_DIR}/../AYResource/include/assetsImpl
    ${CMAKE_CURRENT_SOURCE_DIR}/../AYResource/interface
    ${CMAKE_CURRENT_SOURCE_DIR}/../AYResource/interface/assetsDefs
)

target_link_libraries(AYRenderer PUBLIC
    AYShader
    AYMath
    AYIO
    AYGameLoop
    AYUI
)

target_link_libraries(AYRenderer PRIVATE
    AYResource
    AYFont
    # INT-04 (2026-07-20): EventBusHostScope (Renderer -> Bus bridge).
    # AYApplication transitively pulls AYEventSystem; the type is only
    # referenced inside RendererSubSystem's TU so PRIVATE is sufficient.
    AYApplication
    bgfx::bgfx
    bgfx::bimg
    bgfx::bimg_decode
    bgfx::bimg_encode
)

# UI text uses AYFont (FreeType glyph atlas).

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
