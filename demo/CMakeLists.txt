set(AY_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../AYShader")
set(AY_RESOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../AYResource")

list(APPEND CMAKE_PREFIX_PATH
    "${AY_SHADER_ROOT}/thirdParty/bgfx-install/debug"
    "${AY_SHADER_ROOT}/thirdParty/bgfx-install/release"
)
find_package(bgfx CONFIG REQUIRED)

if(NOT DEFINED AY_SHADER_SHADERC_PATH)
    if(WIN32)
        set(_ays_shaderc_name "shaderc.exe")
    else()
        set(_ays_shaderc_name "shaderc")
    endif()
    set(AY_SHADER_SHADERC_PATH
        "${AY_SHADER_ROOT}/thirdParty/bgfx-install/debug/bin/${_ays_shaderc_name}")
endif()

if(NOT DEFINED AY_BGFX_SHADER_INCLUDE_DIR)
    set(AY_BGFX_SHADER_INCLUDE_DIR
        "${AY_SHADER_ROOT}/thirdParty/bgfx-install/debug/include/bgfx")
endif()

get_filename_component(AY_SHADER_SHADERC_PATH "${AY_SHADER_SHADERC_PATH}" ABSOLUTE)
get_filename_component(AY_BGFX_SHADER_INCLUDE_DIR "${AY_BGFX_SHADER_INCLUDE_DIR}" ABSOLUTE)

add_executable(AYRenderer_Demo RotatingCubeDemo.cpp)

target_link_libraries(AYRenderer_Demo PRIVATE
    AYRenderer
    AYResource
)

target_include_directories(AYRenderer_Demo PRIVATE
    ${AY_RESOURCE_ROOT}/include/assetsImpl
    ${AY_RESOURCE_ROOT}/interface
    ${AY_RESOURCE_ROOT}/interface/assetsDefs
)

if(WIN32)
    target_link_libraries(AYRenderer_Demo PRIVATE user32 gdi32)
endif()

if(NOT DEFINED AY_SHADER_BGFX_COMMON_DIR)
    set(_ays_bgfx_search
        "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/bgfx/examples/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../thirdparty/bgfx/examples/common"
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

target_compile_definitions(AYRenderer_Demo PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
)

add_executable(AYRenderer_BgfxSanityDemo BgfxSanityDemo.cpp)

target_link_libraries(AYRenderer_BgfxSanityDemo PRIVATE
    AYShader
    bgfx::bgfx
    AYIO
)

if(WIN32)
    target_link_libraries(AYRenderer_BgfxSanityDemo PRIVATE user32 gdi32)
endif()

if(MSVC)
    target_compile_options(AYRenderer_BgfxSanityDemo PRIVATE /Zc:__cplusplus /Zc:preprocessor)
    target_compile_definitions(AYRenderer_BgfxSanityDemo PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_compile_definitions(AYRenderer_BgfxSanityDemo PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_BGFX_SHADER_INCLUDE_HINT="${AY_BGFX_SHADER_INCLUDE_DIR}"
)
