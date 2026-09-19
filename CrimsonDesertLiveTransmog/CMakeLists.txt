cmake_minimum_required(VERSION 3.28)

project(CrimsonDesertLiveTransmog VERSION 0.4.0 LANGUAGES CXX)

# Language standard.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warning level for every target in this directory.
add_compile_options(/W4)

# Windows SDK surface and macro environment.
if(WIN32)
    add_compile_definitions(
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _XM_SSE_INTRINSICS_ # Required for DirectXMath inline functions
    )
endif()

# nlohmann/json, header-only.
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# Dear ImGui, compiled from source so there is no binary dependency.
#
# The tag must match the ReShade SDK's hardcoded function-table version (imgui_function_table_19250 in
# external/reshade-sdk/include/reshade_overlay.hpp). The SDK header builds against the upstream docking branch and
# declares slots for the ImGuiDockNodeFlags and ImGuiViewport multi-window APIs. The plain (non-docking) tag lacks
# those symbols and breaks the function-table populator at compile time.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.5-docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# Build ImGui as a static library with D3D11 + Win32 backends.
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_options(imgui_lib PRIVATE /W0)

# CrimsonDesertCore, the monorepo sibling shared library.
#
# Core owns DetourModKit as its own submodule and re-exports it through a PUBLIC link, so this target gets the
# DetourModKit target transitively. DMK needs no separate add_subdirectory at this level.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../CrimsonDesertCore/CMakeLists.txt")
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../CrimsonDesertCore
        ${CMAKE_BINARY_DIR}/CrimsonDesertCore
    )
else()
    message(FATAL_ERROR "CrimsonDesertCore not found at sibling "
        "'../CrimsonDesertCore'. The monorepo must include it, and its "
        "DetourModKit submodule must be initialized: "
        "'git submodule update --init --recursive'.")
endif()

# Dev build option.
option(TRANSMOG_DEV_BUILD "Build two-DLL hot-reload configuration for development" OFF)

# Source files shared by every configuration.
set(COMMON_SOURCES
    src/aob_resolver.cpp
    src/dllmain.cpp
    src/version.cpp
    src/shared_state.cpp
    src/indexed_string_table.cpp
    src/item_name_table.cpp
    src/itemmesh_dumper.cpp
    src/transmog_map.cpp
    src/input_handler.cpp
    src/transmog.cpp
    src/transmog_apply.cpp
    src/transmog_worker.cpp
    src/dx_overlay.cpp
    src/imgui_function_table_populator.cpp  # includes external/reshade-sdk/source/imgui_function_table_19250.{hpp,cpp}
    src/preset_manager.cpp
    src/part_show_suppress.cpp
    src/real_part_tear_down.cpp
    src/prefab_wrapper_swap.cpp
    src/game_thread.cpp
    src/socket_mesh_override.cpp
    src/helm_audio_filter.cpp
    src/color_override/host_scope.cpp
    src/dye_record_inject.cpp
    src/color_override/setter_substitute.cpp
    src/color_override/color_override.cpp
    src/color_override/color_state.cpp
    src/color_override/color_token_table.cpp
    src/color_override/color_token_discovery.cpp
    src/color_override/color_token_interner_hook.cpp
    src/color_override/color_carrier_set.cpp
    src/color_override/color_swatch_table.cpp
    src/color_override/color_matinst_owner.cpp
    src/color_override/color_publisher_hook.cpp
    src/color_override/color_reinit.cpp
    src/color_override/color_pending_overrides.cpp
    src/color_override/matinst_probe.cpp
    src/generated/dye_color_table.cpp
    src/generated/material_palette_table.cpp
)

set(ASI_SOURCES
    ${COMMON_SOURCES}
)

# Include directories shared by every configuration.
set(COMMON_INCLUDE_DIRS
    src
    # Lets src/imgui_function_table_populator.cpp find its vendored
    # ReShade host-side populator pair via plain #include "imgui_function_table_19250.{hpp,cpp}".
    external/reshade-sdk/source
)

# Overlay UI and lifecycle TU group.
#
# overlay.cpp (lifecycle glue) and overlay_ui.cpp (the whole widget body) compile against the ReShade SDK's <imgui.h>
# and <reshade_overlay.hpp>, so every ImGui:: call routes through imgui_function_table_instance(). ReShade sets that
# pointer when our addon registers. In a standalone run our own populator sets it, built into the main target against
# imgui_lib from src/imgui_function_table_populator.cpp.
#
# This OBJECT library gets ONLY the ReShade SDK include path. imgui_lib's include path here lets the real, non-thunk
# ImGui:: symbols leak in and produce LNK2005.
add_library(overlay_ui_obj OBJECT
    src/overlay.cpp
    src/overlay_ui.cpp
    src/overlay_ui/color_override.cpp
    src/overlay_ui/dye_popup.cpp
    src/overlay_ui/footer.cpp
    src/overlay_ui/helpers.cpp
    src/overlay_ui/item_picker_popup.cpp
    src/overlay_ui/transmog_tab.cpp
)
target_include_directories(overlay_ui_obj PRIVATE
    src
    external/reshade-sdk/include
)
target_link_libraries(overlay_ui_obj PRIVATE
    DetourModKit
    CrimsonDesertCore::core
    nlohmann_json::nlohmann_json
)
target_compile_options(overlay_ui_obj PRIVATE /W4)

# Game output directory for dev builds.
set(GAME_BIN_DIR "D:/Games/SteamLibrary/steamapps/common/Crimson Desert/bin64/plugins")

if(TRANSMOG_DEV_BUILD)
    # Dev build: two-DLL hot-reload, a loader plus a logic DLL.
    message(STATUS "DEV BUILD enabled: loader and logic DLL pair")

    # Loader ASI, the resident staged-generation loader. Its sources live in CrimsonDesertCore and take their
    # parameters from the CDCORE_LOADER_* macros.
    add_library(transmog_loader SHARED ${CDCORE_DEV_LOADER_SRC})

    target_compile_definitions(transmog_loader PRIVATE
        CDCORE_LOADER_MOD_NAME="CrimsonDesertLiveTransmog"
        CDCORE_LOADER_PROCESS_NAME="CrimsonDesert.exe"
    )

    target_include_directories(transmog_loader PRIVATE ${CDCORE_DEV_INCLUDE_DIR})

    # The loader hosts wheel capture for the process, so a user-bound mouse-wheel combo books its permanent module
    # keepalive HERE instead of in the logic DLL, which is what lets a generation unmap. It links the resident host
    # ONLY.
    target_link_libraries(transmog_loader PRIVATE
        DetourModKit::WheelHost
        kernel32
        user32
    )

    # Guard the property above. A link against the full archive gives the loader its own DetourModKit instance, which
    # is the arrangement DMK's unload proofs exclude.
    get_target_property(_lt_loader_links transmog_loader LINK_LIBRARIES)
    if(NOT _lt_loader_links MATCHES "DetourModKit::WheelHost")
        message(FATAL_ERROR "transmog_loader must link DetourModKit::WheelHost.")
    endif()
    if(_lt_loader_links MATCHES "DetourModKit::DetourModKit" OR _lt_loader_links MATCHES "(^|;)DetourModKit(;|$)")
        message(FATAL_ERROR "transmog_loader must not link the full DetourModKit archive.")
    endif()

    set_target_properties(transmog_loader PROPERTIES
        OUTPUT_NAME "CrimsonDesertLiveTransmog"
        SUFFIX ".asi"
        PREFIX ""
    )

    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/deploy_loader.cmake" [=[
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy "${SRC_ASI}" "${DST_ASI}"
    RESULT_VARIABLE rc
)
if(rc EQUAL 0)
    message(STATUS "Loader ASI deployed to plugins/")
else()
    message(STATUS "Loader ASI deploy skipped (file locked)")
endif()
]=])

    add_custom_command(TARGET transmog_loader POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DSRC_ASI=$<TARGET_FILE:transmog_loader>
            -DDST_ASI=${GAME_BIN_DIR}/CrimsonDesertLiveTransmog.asi
            -P "${CMAKE_CURRENT_BINARY_DIR}/deploy_loader.cmake"
        COMMENT "Deploying Loader ASI (skips if locked)"
    )

    target_compile_options(transmog_loader PRIVATE /W4)

    # Logic DLL, holding all mod code.
    add_library(transmog_logic SHARED
        ${COMMON_SOURCES}
        src/dev/mod_logic.cpp
    )

    # The object library lands in this DLL, so it needs the same macro environment as every other TU in it.
    target_compile_definitions(overlay_ui_obj PRIVATE TRANSMOG_DEV_BUILD)

    target_include_directories(transmog_logic PRIVATE ${COMMON_INCLUDE_DIRS} ${CDCORE_DEV_INCLUDE_DIR})

    target_compile_definitions(transmog_logic PRIVATE TRANSMOG_DEV_BUILD)

    target_link_libraries(transmog_logic PRIVATE
        overlay_ui_obj
        DetourModKit
        CrimsonDesertCore::core
        nlohmann_json::nlohmann_json
        imgui_lib
        d3d11
        dxgi
        dwmapi
        winmm
        user32
        kernel32
    )

    # Fallback drop point for a deploy that loses a race with the loader reading the deployed DLL.
    set(GAME_STAGING_DIR "${GAME_BIN_DIR}/staging")

    set_target_properties(transmog_logic PROPERTIES
        OUTPUT_NAME "CrimsonDesertLiveTransmog.logic"
        PREFIX ""
    )

    # Link to the build tree (the CMake default) and deploy from there. The loader maps a per-generation COPY of
    # the deployed DLL, never the file itself, so the direct copy is not racing a mapped image and succeeds in the
    # normal case. Staging remains the fallback for the one window where it can fail: the loader reads this file
    # while staging a new generation, and a build landing exactly then loses the copy. The loader promotes whatever
    # it finds in staging on the next reload.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/deploy_logic.cmake" [=[
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy "${SRC_DLL}" "${DST_DLL}"
    RESULT_VARIABLE rc
)
if(rc EQUAL 0)
    message(STATUS "Direct deploy OK")
    if(EXISTS "${SRC_PDB}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${SRC_PDB}" "${DST_PDB}")
    endif()
    # Clear anything an earlier fallback left behind, so the loader cannot promote a stale build over this one.
    file(REMOVE "${STAGING_DLL}" "${STAGING_PDB}")
else()
    message(STATUS "Direct deploy skipped (file locked): staging for the loader to promote on next reload")
    get_filename_component(staging_dir "${STAGING_DLL}" DIRECTORY)
    file(MAKE_DIRECTORY "${staging_dir}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${SRC_DLL}" "${STAGING_DLL}")
    if(EXISTS "${SRC_PDB}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${SRC_PDB}" "${STAGING_PDB}")
    endif()
endif()
]=])

    add_custom_command(TARGET transmog_logic POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DSRC_DLL=$<TARGET_FILE:transmog_logic>
            -DDST_DLL=${GAME_BIN_DIR}/CrimsonDesertLiveTransmog.logic.dll
            -DSTAGING_DLL=${GAME_STAGING_DIR}/CrimsonDesertLiveTransmog.logic.dll
            -DSRC_PDB=$<TARGET_PDB_FILE:transmog_logic>
            -DDST_PDB=${GAME_BIN_DIR}/CrimsonDesertLiveTransmog.logic.pdb
            -DSTAGING_PDB=${GAME_STAGING_DIR}/CrimsonDesertLiveTransmog.logic.pdb
            -P "${CMAKE_CURRENT_BINARY_DIR}/deploy_logic.cmake"
        COMMENT "Deploying Logic DLL+PDB (falls back to staging if locked)"
    )

    # The display_names TSV (item names plus the wearer-body "Male"/"Female" column that
    # scripts/gen_item_body_table.py fills) is data the mod loads at runtime. Keep the deployed copy in sync so it
    # never goes stale. Unlike the DLL it is never game-locked, so a direct copy_if_different is enough and needs no
    # staging dance.
    add_custom_command(TARGET transmog_logic POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/build/template/CrimsonDesertLiveTransmog_display_names.tsv"
            "${GAME_BIN_DIR}/CrimsonDesertLiveTransmog_display_names.tsv"
        COMMENT "Deploying display_names.tsv (item names + wearer-body column)"
    )

    target_compile_options(transmog_logic PRIVATE /W4 /Zi)
    target_link_options(transmog_logic PRIVATE /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF)

else() # TRANSMOG_DEV_BUILD=OFF
    # Production build: a single ASI.

    add_library(${PROJECT_NAME}-ASI SHARED
        ${ASI_SOURCES}
    )

    target_include_directories(${PROJECT_NAME}-ASI PRIVATE ${COMMON_INCLUDE_DIRS})

    target_link_libraries(${PROJECT_NAME}-ASI PRIVATE
        overlay_ui_obj
        DetourModKit
        CrimsonDesertCore::core
        nlohmann_json::nlohmann_json
        imgui_lib
        d3d11
        dxgi
        dwmapi
        winmm
        user32
        kernel32
    )

    set_target_properties(${PROJECT_NAME}-ASI PROPERTIES
        OUTPUT_NAME "CrimsonDesertLiveTransmog-temp-asi"
        SUFFIX ".asi"
        PREFIX ""
    )

    target_compile_options(${PROJECT_NAME}-ASI PRIVATE
        $<$<CONFIG:Release>:/O1 /Gy /Gw>
    )

    target_link_options(${PROJECT_NAME}-ASI PRIVATE
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>
    )

    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_supported LANGUAGES CXX OUTPUT _ipo_error)

    if(_ipo_supported)
        set_target_properties(${PROJECT_NAME}-ASI PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
        )
        message(STATUS "LTO enabled for ${PROJECT_NAME}-ASI Release builds")
    else()
        message(STATUS "LTO not supported for ${PROJECT_NAME}-ASI: ${_ipo_error}")
    endif()

    add_custom_command(TARGET ${PROJECT_NAME}-ASI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:${PROJECT_NAME}-ASI>
        ${CMAKE_CURRENT_BINARY_DIR}/CrimsonDesertLiveTransmog.asi
        COMMENT "Renaming ASI output to final name"
    )

    message(STATUS "CrimsonDesertLiveTransmog (${PROJECT_VERSION}) Configuration Summary:")
    message(STATUS "  Generator:                   ${CMAKE_GENERATOR}")
    message(STATUS "  Build Type:                  ${CMAKE_BUILD_TYPE}")
    message(STATUS "  Targets:")
    message(STATUS "    - ${PROJECT_NAME}-ASI      -> CrimsonDesertLiveTransmog.asi (via post-build copy)")
endif() # TRANSMOG_DEV_BUILD
