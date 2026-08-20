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

target_compile_definitions(${TEST_NAME} PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
    AY_RENDERER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

ay_add_test(${TEST_NAME})
