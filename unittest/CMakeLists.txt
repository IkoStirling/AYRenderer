set(TEST_NAME AYRenderer_Test)

file(GLOB TEST_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(${TEST_NAME} ${TEST_FILES})
if(MSVC)
    # bx half conversion used by production-precision GPU readback tests.
    set_source_files_properties(Test_TAAGpu.cpp PROPERTIES
        COMPILE_OPTIONS "/Zc:__cplusplus;/Zc:preprocessor")
endif()

target_link_libraries(${TEST_NAME} PRIVATE
    AYRenderer
    AYResource
    AYTest
    AYEventSystem
    bgfx::bgfx
)

target_include_directories(${TEST_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYFont
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYResource/include/assetsImpl
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYResource/interface
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYResource/interface/assetsDefs
)

target_compile_definitions(${TEST_NAME} PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
    AY_RENDERER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

ay_add_test(${TEST_NAME})
# This aggregate suite initializes renderer backends and compiles shaders while
# exercising thousands of checks.  Running it beside other heavyweight suites
# makes failures depend on host scheduling rather than renderer behavior.
set_tests_properties(${TEST_NAME} PROPERTIES RUN_SERIAL TRUE)
ay_add_test(${TEST_NAME}
    NAME AYRenderer_2DMatrixBaseline
    SUITE BgfxMatrixTests
    LABELS "2d-baseline;ayrenderer")
ay_add_test(${TEST_NAME}
    NAME AYRenderer_2DOverlayBaseline
    SUITE Renderer2DLaneTests
    LABELS "2d-baseline;ayrenderer")
ay_add_test(${TEST_NAME}
    NAME AYRenderer_2DEditorOverlayBaseline
    SUITE AYRenderer_EditorOverlay
    LABELS "2d-baseline;ayrenderer")
ay_add_test(${TEST_NAME}
    NAME AYRenderer_MixedSceneVisibility
    SUITE RendererSceneVisibilityTests
    LABELS "2d-3d-mixed;ayrenderer")
