cmake_minimum_required(VERSION 3.25)

project(CrimsonDesertLiveTransmog VERSION 0.4.0 LANGUAGES CXX)

# --- Standard and Compiler Options ---
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Compiler Flags ---
add_compile_options(/W4)

# --- Windows Definitions ---
if(WIN32)
    add_compile_definitions(
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _XM_SSE_INTRINSICS_ # Required for DirectXMath inline functions
    )
endif()

# --- nlohmann/json (header-only) ---
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# --- Dear ImGui (compiled from source — no binary dependency) ---
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8
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

# --- CrimsonDesertCore (shared library, monorepo sibling) ---
# Core owns DetourModKit as its own submodule and re-exports it via PUBLIC
# link, so this target gets the DetourModKit target transitively — no
# separate add_subdirectory for DMK is needed at this level.
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

# --- Dev Build Option ---
option(TRANSMOG_DEV_BUILD "Build two-DLL hot-reload configuration for development" OFF)

# --- Source Files ---
set(COMMON_SOURCES
    src/dllmain.cpp
    src/version.cpp
    src/shared_state.cpp
    src/indexed_string_table.cpp
    src/item_name_table.cpp
    src/transmog_map.cpp
    src/input_handler.cpp
    src/transmog.cpp
    src/transmog_apply.cpp
    src/transmog_hooks.cpp
    src/transmog_worker.cpp
    src/overlay.cpp
    src/dx_overlay.cpp
    src/preset_manager.cpp
    src/part_show_suppress.cpp
    src/real_part_tear_down.cpp
)

set(ASI_SOURCES
    ${COMMON_SOURCES}
)

# --- Include Directories ---
set(COMMON_INCLUDE_DIRS
    src
)

# overlay_reshade.cpp uses ReShade SDK's imgui.h (function-table wrappers)
# instead of imgui_lib's imgui.h (real compiled ImGui).  It is compiled as
# a separate OBJECT library with the ReShade SDK include path and WITHOUT
# imgui_lib's include path to avoid symbol conflicts.
add_library(overlay_reshade_obj OBJECT src/overlay_reshade.cpp)
target_include_directories(overlay_reshade_obj PRIVATE
    src
    external/reshade-sdk/include
)
target_link_libraries(overlay_reshade_obj PRIVATE
    DetourModKit
)
target_compile_options(overlay_reshade_obj PRIVATE /W4)

# --- Game Output Directory (dev builds) ---
set(GAME_BIN_DIR "D:/Games/SteamLibrary/steamapps/common/Crimson Desert/bin64/plugins")

if(TRANSMOG_DEV_BUILD)
    # =====================================================================
    # Dev Build: two-DLL hot-reload (loader + logic)
    # =====================================================================
    message(STATUS "DEV BUILD enabled — building loader + logic DLL pair")

    # --- Loader ASI (thin stub, no DetourModKit) ---
    # Sources live in CrimsonDesertCore; parameterized via CDCORE_LOADER_* macros.
    add_library(transmog_loader SHARED ${CDCORE_DEV_LOADER_SRC})

    target_compile_definitions(transmog_loader PRIVATE
        CDCORE_LOADER_DLL_NAME="CrimsonDesertLiveTransmog_Logic.dll"
        CDCORE_LOADER_PDB_NAME="CrimsonDesertLiveTransmog_Logic.pdb"
        CDCORE_LOADER_LOG_PREFIX="[TransmogLoader] "
    )

    target_link_libraries(transmog_loader PRIVATE
        kernel32
        user32
    )

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

    # --- Logic DLL (all mod code) ---
    add_library(transmog_logic SHARED
        ${COMMON_SOURCES}
        src/dev/logic_exports.cpp
        $<TARGET_OBJECTS:overlay_reshade_obj>
    )

    target_include_directories(transmog_logic PRIVATE ${COMMON_INCLUDE_DIRS})

    target_compile_definitions(transmog_logic PRIVATE TRANSMOG_DEV_BUILD)

    target_link_libraries(transmog_logic PRIVATE
        DetourModKit
        CrimsonDesertCore::core
        nlohmann_json::nlohmann_json
        imgui_lib
        d3d11
        dxgi
        dwmapi
        psapi
        user32
        kernel32
    )

    # Build to staging/ so the linker never conflicts with a game-locked DLL.
    set(GAME_STAGING_DIR "${GAME_BIN_DIR}/staging")

    set_target_properties(transmog_logic PROPERTIES
        OUTPUT_NAME "CrimsonDesertLiveTransmog_Logic"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${GAME_STAGING_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${GAME_STAGING_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${GAME_STAGING_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${GAME_STAGING_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${GAME_STAGING_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${GAME_STAGING_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${GAME_STAGING_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${GAME_STAGING_DIR}"
        PDB_OUTPUT_DIRECTORY "${GAME_STAGING_DIR}"
        PDB_OUTPUT_DIRECTORY_DEBUG "${GAME_STAGING_DIR}"
        PDB_OUTPUT_DIRECTORY_RELEASE "${GAME_STAGING_DIR}"
        PDB_OUTPUT_DIRECTORY_RELWITHDEBINFO "${GAME_STAGING_DIR}"
    )

    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/deploy_logic.cmake" [=[
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy "${SRC_DLL}" "${DST_DLL}"
    RESULT_VARIABLE rc
)
if(rc EQUAL 0)
    message(STATUS "Direct deploy OK -- cleaning staging")
    file(REMOVE "${STAGING_DLL}")
    if(EXISTS "${SRC_PDB}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy "${SRC_PDB}" "${DST_PDB}"
            RESULT_VARIABLE pdb_rc
        )
        if(pdb_rc EQUAL 0)
            file(REMOVE "${STAGING_PDB}")
        endif()
    endif()
else()
    message(STATUS "Direct deploy skipped (file locked) — using staging")
endif()
]=])

    add_custom_command(TARGET transmog_logic POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DSRC_DLL=$<TARGET_FILE:transmog_logic>
            -DDST_DLL=${GAME_BIN_DIR}/CrimsonDesertLiveTransmog_Logic.dll
            -DSTAGING_DLL=${GAME_STAGING_DIR}/CrimsonDesertLiveTransmog_Logic.dll
            -DSRC_PDB=$<TARGET_PDB_FILE:transmog_logic>
            -DDST_PDB=${GAME_BIN_DIR}/CrimsonDesertLiveTransmog_Logic.pdb
            -DSTAGING_PDB=${GAME_STAGING_DIR}/CrimsonDesertLiveTransmog_Logic.pdb
            -P "${CMAKE_CURRENT_BINARY_DIR}/deploy_logic.cmake"
        COMMENT "Deploying Logic DLL+PDB (falls back to staging if locked)"
    )

    target_compile_options(transmog_logic PRIVATE /W4 /Zi)
    target_link_options(transmog_logic PRIVATE /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF ${RESHADE_FORCE_LINK_FLAGS})

else() # TRANSMOG_DEV_BUILD=OFF
    # =====================================================================
    # Production Build: single ASI
    # =====================================================================

    add_library(${PROJECT_NAME}-ASI SHARED
        ${ASI_SOURCES}
        $<TARGET_OBJECTS:overlay_reshade_obj>
    )

    target_include_directories(${PROJECT_NAME}-ASI PRIVATE ${COMMON_INCLUDE_DIRS})

    target_link_libraries(${PROJECT_NAME}-ASI PRIVATE
        DetourModKit
        CrimsonDesertCore::core
        nlohmann_json::nlohmann_json
        imgui_lib
        d3d11
        dxgi
        dwmapi
        psapi
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

    message(STATUS "---------------------------------------------------------------------")
    message(STATUS "CrimsonDesertLiveTransmog (${PROJECT_VERSION}) Configuration Summary:")
    message(STATUS "  Generator:                   ${CMAKE_GENERATOR}")
    message(STATUS "  Build Type:                  ${CMAKE_BUILD_TYPE}")
    message(STATUS "  Targets:")
    message(STATUS "    - ${PROJECT_NAME}-ASI      -> CrimsonDesertLiveTransmog.asi (via post-build copy)")
    message(STATUS "---------------------------------------------------------------------")
endif() # TRANSMOG_DEV_BUILD
