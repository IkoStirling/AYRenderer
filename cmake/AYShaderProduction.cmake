include_guard(GLOBAL)
# Engine source owns the shader catalog. Games consume packages, not compiler
# paths. Targets may override these for a supported non-default backend.
if(WIN32)
    set(AY_PRODUCTION_SHADER_PLATFORM windows CACHE STRING "Offline shader platform")
    set(AY_PRODUCTION_SHADER_PROFILE s_5_0 CACHE STRING "Offline shader profile")
elseif(APPLE)
    set(AY_PRODUCTION_SHADER_PLATFORM osx CACHE STRING "Offline shader platform")
    set(AY_PRODUCTION_SHADER_PROFILE metal CACHE STRING "Offline shader profile")
else()
    set(AY_PRODUCTION_SHADER_PLATFORM linux CACHE STRING "Offline shader platform")
    set(AY_PRODUCTION_SHADER_PROFILE spirv CACHE STRING "Offline shader profile")
endif()
function(ay_stage_production_shaders target)
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Production shader builds require a native host compiler tool; cross-compilation integration is not implemented")
    endif()
    set(_shader_package "${CMAKE_BINARY_DIR}/ay-shaders/${AY_PRODUCTION_SHADER_PLATFORM}-${AY_PRODUCTION_SHADER_PROFILE}/engine")
    # Incremental content follows the linked tool (its catalog includes actual
    # Pass sources), shaderc and every text kind in the include fingerprint.
    set(_shader_dependencies)
    foreach(_include_root "${AY_SHADER_BGFX_COMMON_DIR}" "${AY_SHADER_BGFX_SRC_DIR}")
        file(GLOB_RECURSE _include_files CONFIGURE_DEPENDS
            "${_include_root}/*.sh" "${_include_root}/*.sc" "${_include_root}/*.h"
            "${_include_root}/*.inc" "${_include_root}/*.glsl" "${_include_root}/*.hlsl")
        list(APPEND _shader_dependencies ${_include_files})
    endforeach()
    if(NOT TARGET AYProductionShaders)
        add_custom_command(OUTPUT "${_shader_package}/manifest.json"
            COMMAND $<TARGET_FILE:AYRenderer_ShaderBuild>
                "${AY_PRODUCTION_SHADER_PLATFORM}" "${AY_PRODUCTION_SHADER_PROFILE}" "${_shader_package}"
            DEPENDS AYRenderer_ShaderBuild "${AY_SHADER_SHADERC_PATH}" ${_shader_dependencies}
            VERBATIM COMMENT "Building production shader variants")
        add_custom_target(AYProductionShaders DEPENDS "${_shader_package}/manifest.json")
    endif()
    set(_stage_commands COMMAND ${CMAKE_COMMAND} -E copy_directory "${_shader_package}"
            "$<TARGET_FILE_DIR:${target}>/Shaders/${AY_PRODUCTION_SHADER_PLATFORM}-${AY_PRODUCTION_SHADER_PROFILE}/engine")
    if(ARGC GREATER 1 AND ARGV1 STREQUAL "AUTHORING")
        list(APPEND _stage_commands COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/authoring.policy" "$<TARGET_FILE_DIR:${target}>/Shaders/authoring.policy")
    endif()
    add_custom_target(${target}_ProductionShaders ${_stage_commands}
        VERBATIM COMMENT "Staging production shaders for ${target}")
    add_dependencies(${target}_ProductionShaders AYProductionShaders)
    add_dependencies(${target} ${target}_ProductionShaders)
endfunction()

# Games use strict policy; an authoring editor also needs dynamic source editing.
# runtime_destination is the directory containing the installed executable.
function(ay_install_production_shaders target runtime_destination component)
    install(DIRECTORY "$<TARGET_FILE_DIR:${target}>/Shaders/${AY_PRODUCTION_SHADER_PLATFORM}-${AY_PRODUCTION_SHADER_PROFILE}/engine/"
        DESTINATION "${runtime_destination}/Shaders/${AY_PRODUCTION_SHADER_PLATFORM}-${AY_PRODUCTION_SHADER_PROFILE}/engine"
        COMPONENT "${component}")
    if(ARGC GREATER 3 AND ARGV3 STREQUAL "AUTHORING")
        if(NOT EXISTS "${AY_SHADER_SHADERC_PATH}" OR NOT IS_DIRECTORY "${AY_SHADER_BGFX_COMMON_DIR}" OR NOT IS_DIRECTORY "${AY_SHADER_BGFX_SRC_DIR}")
            message(FATAL_ERROR "Authoring installation requires shaderc and both configured include roots")
        endif()
        install(FILES "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/authoring.policy"
            DESTINATION "${runtime_destination}/Shaders" COMPONENT "${component}")
        install(FILES "${AY_SHADER_SHADERC_PATH}" DESTINATION "${runtime_destination}/Shaders/authoring" COMPONENT "${component}")
        install(DIRECTORY "${AY_SHADER_BGFX_COMMON_DIR}/" DESTINATION "${runtime_destination}/Shaders/authoring/include-common" COMPONENT "${component}")
        install(DIRECTORY "${AY_SHADER_BGFX_SRC_DIR}/" DESTINATION "${runtime_destination}/Shaders/authoring/include-src" COMPONENT "${component}")
    else()
        install(FILES "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/production.policy"
            DESTINATION "${runtime_destination}/Shaders" COMPONENT "${component}")
    endif()
endfunction()
