add_executable(AYRenderer_Demo RotatingCubeDemo.cpp)

target_link_libraries(AYRenderer_Demo PRIVATE
    AYRenderer
)

if(WIN32)
    target_link_libraries(AYRenderer_Demo PRIVATE user32 gdi32)
endif()

if(NOT DEFINED AY_SHADER_SHADERC_PATH)
    set(AY_SHADER_SHADERC_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/../AYShader/thirdParty/bgfx-install/debug/bin/shaderc.exe")
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
