include(TestGroups.cmake)

if(MSVC)
    # bx half conversion used by production-precision GPU readback tests.
    set_source_files_properties(Test_TAAGpu.cpp PROPERTIES
        COMPILE_OPTIONS "/Zc:__cplusplus;/Zc:preprocessor")
endif()

set(AYRENDERER_OWNED_TEST_SOURCES)
set(AYRENDERER_TEST_OBJECTS)
set(AYRENDERER_GROUP_TARGETS)
set(AYRENDERER_INTEGRATION_TARGETS)
foreach(group IN LISTS AYRENDERER_TEST_GROUPS)
    set(objects AYRenderer_Test_${group})
    set(binary AYRenderer_${group}Tests)
    add_library(${objects} OBJECT ${AYRENDERER_TEST_${group}_SOURCES})
    set_target_properties(${objects} PROPERTIES UNITY_BUILD OFF)
    # Link objects directly, keeping registrars alive without whole-archive.
    target_link_libraries(${objects} PUBLIC
        AYRenderer AYResource AYTest AYEventSystem bgfx::bgfx)
    target_include_directories(${objects} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../AYFont
        ${CMAKE_CURRENT_SOURCE_DIR}/../../AYResource/include/assetsImpl
        ${CMAKE_CURRENT_SOURCE_DIR}/../../AYResource/interface
        ${CMAKE_CURRENT_SOURCE_DIR}/../../AYResource/interface/assetsDefs)
    target_compile_definitions(${objects} PRIVATE
        AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
        AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
        AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
        AY_RENDERER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
    add_executable(${binary} main.cpp)
    target_compile_definitions(${binary} PRIVATE AYRENDERER_TEST_MODULE="${binary}")
    target_link_libraries(${binary} PRIVATE ${objects})
    string(TOLOWER "${group}" group_label)
    set(policy)
    if(group STREQUAL "Contracts")
        set(tier fast)
        set(timeout 30)
    elseif(group STREQUAL "Gpu")
        set(tier gpu)
        set(timeout 240)
        set(policy RUN_SERIAL)
    else()
        set(tier integration)
        set(timeout 180)
        # Keep backend/compiler fixtures bounded and mutually exclusive until
        # their process-external scratch paths have been fully audited.
        set(policy RESOURCE_LOCK renderer-backend-fixtures)
        list(APPEND AYRENDERER_INTEGRATION_TARGETS ${binary})
    endif()
    ay_add_test(${binary} LABELS
        "renderer;renderer-full;renderer-${tier};${tier};renderer-${group_label}"
        TIMEOUT ${timeout} ${policy})
    list(APPEND AYRENDERER_TEST_OBJECTS ${objects})
    list(APPEND AYRENDERER_GROUP_TARGETS ${binary})
    list(APPEND AYRENDERER_OWNED_TEST_SOURCES ${AYRENDERER_TEST_${group}_SOURCES})
endforeach()

file(GLOB discovered CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(REMOVE_ITEM discovered main.cpp)
list(SORT discovered)
list(SORT AYRENDERER_OWNED_TEST_SOURCES)
if(NOT "${discovered}" STREQUAL "${AYRENDERER_OWNED_TEST_SOURCES}")
    message(FATAL_ERROR "Renderer test source ownership differs from TestGroups.cmake; assign each source exactly once")
endif()

add_custom_target(AYRenderer_TestGroups DEPENDS ${AYRENDERER_GROUP_TARGETS})
add_custom_target(AYRenderer_IntegrationTests DEPENDS ${AYRENDERER_INTEGRATION_TARGETS})

# Manual compatibility executable. Do not duplicate the grouped CTest full tier.
add_executable(AYRenderer_Test EXCLUDE_FROM_ALL main.cpp)
target_link_libraries(AYRenderer_Test PRIVATE ${AYRENDERER_TEST_OBJECTS})
ay_add_test(AYRenderer_ContractsTests NAME AYRenderer_2DMatrixBaseline
    SUITE BgfxMatrixTests LABELS "2d-baseline;ayrenderer")
ay_add_test(AYRenderer_GeometryTests NAME AYRenderer_2DOverlayBaseline
    SUITE Renderer2DLaneTests LABELS "2d-baseline;ayrenderer"
    RESOURCE_LOCK renderer-backend-fixtures TIMEOUT 60)
ay_add_test(AYRenderer_SceneMaterialTests NAME AYRenderer_2DEditorOverlayBaseline
    SUITE AYRenderer_EditorOverlay LABELS "2d-baseline;ayrenderer"
    RESOURCE_LOCK renderer-backend-fixtures TIMEOUT 60)
ay_add_test(AYRenderer_ContractsTests NAME AYRenderer_MixedSceneVisibility
    SUITE RendererSceneVisibilityTests LABELS "2d-3d-mixed;ayrenderer")
