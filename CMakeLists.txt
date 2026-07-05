cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB AYRENDERER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/detail/*.cpp"
)

add_library(AYRenderer STATIC ${AYRENDERER_SOURCES})

target_include_directories(AYRenderer PUBLIC
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

list(APPEND CMAKE_PREFIX_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/../AYShader/thirdParty/bgfx-install/debug"
    "${CMAKE_CURRENT_SOURCE_DIR}/../AYShader/thirdParty/bgfx-install/release"
)
find_package(bgfx CONFIG REQUIRED)

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

if(NOT DEFINED AY_SHADER_SHADERC_PATH)
    if(WIN32)
        set(_ays_shaderc_name "shaderc.exe")
    else()
        set(_ays_shaderc_name "shaderc")
    endif()
    set(AY_SHADER_SHADERC_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/../AYShader/thirdParty/bgfx-install/debug/bin/${_ays_shaderc_name}")
endif()

if(NOT DEFINED AY_SHADER_BGFX_COMMON_DIR)
    set(_ays_bgfx_search
        "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/bgfx/examples/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../thirdparty/bgfx/examples/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../thirdparty/bgfx/examples/common"
    )
    set(AY_SHADER_BGFX_COMMON_DIR "")
    foreach(_dir IN LISTS _ays_bgfx_search)
        if(EXISTS "${_dir}/common.sh")
            set(AY_SHADER_BGFX_COMMON_DIR "${_dir}")
            break()
        endif()
    endforeach()
endif()

if(NOT DEFINED AY_SHADER_BGFX_SRC_DIR AND AY_SHADER_BGFX_COMMON_DIR)
    set(AY_SHADER_BGFX_SRC_DIR "${AY_SHADER_BGFX_COMMON_DIR}/../../src")
endif()

if(NOT DEFINED AY_BGFX_SHADER_INCLUDE_DIR)
    set(_ays_bgfx_include_search
        "${CMAKE_CURRENT_SOURCE_DIR}/../AYShader/thirdParty/bgfx-install/debug/include/bgfx"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/bgfx/include"
    )
    set(AY_BGFX_SHADER_INCLUDE_DIR "")
    foreach(_dir IN LISTS _ays_bgfx_include_search)
        if(EXISTS "${_dir}/bgfx_shader.sh")
            set(AY_BGFX_SHADER_INCLUDE_DIR "${_dir}")
            break()
        endif()
    endforeach()
endif()

target_compile_definitions(AYRenderer PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
    AY_BGFX_SHADER_INCLUDE_HINT="${AY_BGFX_SHADER_INCLUDE_DIR}"
)

enable_testing()
add_subdirectory(unittest)
add_subdirectory(demo)
