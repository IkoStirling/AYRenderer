set(AY_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../AYShader")
set(AY_RESOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../AYResource")
include("${AY_SHADER_ROOT}/cmake/AYBgfx.cmake")

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

add_executable(AYEngineIntegration_Demo EngineIntegrationDemo.cpp)

target_link_libraries(AYEngineIntegration_Demo PRIVATE
    AYRenderer
    AYEntity
    AYGameLoop
    AYResource
)

target_include_directories(AYEngineIntegration_Demo PRIVATE
    ${AY_RESOURCE_ROOT}/include/assetsImpl
    ${AY_RESOURCE_ROOT}/interface
    ${AY_RESOURCE_ROOT}/interface/assetsDefs
)

if(WIN32)
    target_link_libraries(AYEngineIntegration_Demo PRIVATE user32 gdi32)
endif()

target_compile_definitions(AYEngineIntegration_Demo PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
)

# Phase 1 mid-point: end-to-end GPU skinning demo (4-bone skeleton +
# 24-vertex skinned cube + 2-sec spine-bone animation, screenshot at
# frame 0/30/60).
add_executable(AYSuzanneSkinned_Demo SuzanneSkinnedDemo.cpp)

target_link_libraries(AYSuzanneSkinned_Demo PRIVATE
    AYRenderer
    AYEntity
    AYGameLoop
    AYResource
)

target_include_directories(AYSuzanneSkinned_Demo PRIVATE
    ${AY_RESOURCE_ROOT}/include/assetsImpl
    ${AY_RESOURCE_ROOT}/interface
    ${AY_RESOURCE_ROOT}/interface/assetsDefs
)

if(WIN32)
    target_link_libraries(AYSuzanneSkinned_Demo PRIVATE user32 gdi32)
endif()

target_compile_definitions(AYSuzanneSkinned_Demo PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
)
