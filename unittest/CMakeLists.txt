set(TEST_NAME AYRenderer_Test)

file(GLOB TEST_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(${TEST_NAME} ${TEST_FILES})

target_link_libraries(${TEST_NAME} PRIVATE
    AYRenderer
    AYResource
    AYTest
    # INT-04 (2026-07-20): Renderer -> EventBus bridge tests.
    AYApplication
    bgfx::bgfx
)

target_include_directories(${TEST_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYFont
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYResource/include/assetsImpl
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYResource/interface
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYResource/interface/assetsDefs
    # INT-04: AYApplication/AppEventHost.h (EventBusHostScope) + AYEventSystem/Events/AYEventSystem/Events/AYEventSystem/Events/WindowEvents.h.
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYApplication/interface
)

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

target_compile_definitions(${TEST_NAME} PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
)

ay_configure_test_tmpdir(${TEST_NAME})
add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
