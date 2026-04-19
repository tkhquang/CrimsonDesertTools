cmake_minimum_required(VERSION 3.25)

project(CrimsonDesertEquipHide VERSION 0.2.3 LANGUAGES CXX)

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
option(EQUIPHIDE_DEV_BUILD "Build two-DLL hot-reload configuration for development" OFF)

# --- Source Files ---
set(COMMON_SOURCES
    src/dllmain.cpp
    src/version.cpp
    src/shared_state.cpp
    src/indexed_string_table.cpp
    src/player_detection.cpp
    src/visibility_write.cpp
    src/armor_injection.cpp
    src/bald_fix.cpp
    src/cascade_suppress.cpp
    src/gliding_fix.cpp
    src/background_threads.cpp
    src/input_handler.cpp
    src/categories.cpp
    src/equip_hide.cpp
)

set(ASI_SOURCES
    ${COMMON_SOURCES}
)

# --- Include Directories ---
set(COMMON_INCLUDE_DIRS
    src
)

# --- Game Output Directory (dev builds) ---
set(GAME_BIN_DIR "D:/Games/SteamLibrary/steamapps/common/Crimson Desert/bin64/plugins")

if(EQUIPHIDE_DEV_BUILD)
    # =====================================================================
    # Dev Build: two-DLL hot-reload (loader + logic)
    # =====================================================================
    message(STATUS "DEV BUILD enabled — building loader + logic DLL pair")

    # --- Loader ASI (thin stub, no DetourModKit) ---
    # Sources live in CrimsonDesertCore; parameterized via CDCORE_LOADER_* macros.
    add_library(equip_hide_loader SHARED ${CDCORE_DEV_LOADER_SRC})

    target_compile_definitions(equip_hide_loader PRIVATE
        CDCORE_LOADER_DLL_NAME="CrimsonDesertEquipHide_Logic.dll"
        CDCORE_LOADER_PDB_NAME="CrimsonDesertEquipHide_Logic.pdb"
        CDCORE_LOADER_LOG_PREFIX="[EquipHideLoader] "
    )

    target_link_libraries(equip_hide_loader PRIVATE
        kernel32
        user32
    )

    # Build loader to build dir; post-build tries to copy to game folder.
    # If the ASI is locked (game running), the copy silently fails.
    set_target_properties(equip_hide_loader PROPERTIES
        OUTPUT_NAME "CrimsonDesertEquipHide"
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

    add_custom_command(TARGET equip_hide_loader POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DSRC_ASI=$<TARGET_FILE:equip_hide_loader>
            -DDST_ASI=${GAME_BIN_DIR}/CrimsonDesertEquipHide.asi
            -P "${CMAKE_CURRENT_BINARY_DIR}/deploy_loader.cmake"
        COMMENT "Deploying Loader ASI (skips if locked)"
    )

    target_compile_options(equip_hide_loader PRIVATE /W4)

    # --- Logic DLL (all mod code) ---
    add_library(equip_hide_logic SHARED
        ${COMMON_SOURCES}
        src/dev/logic_exports.cpp
    )

    target_include_directories(equip_hide_logic PRIVATE ${COMMON_INCLUDE_DIRS})

    target_compile_definitions(equip_hide_logic PRIVATE EQUIPHIDE_DEV_BUILD)

    target_link_libraries(equip_hide_logic PRIVATE
        DetourModKit
        CrimsonDesertCore::core
        psapi
        user32
        kernel32
    )

    # Build to staging/ so the linker never conflicts with a game-locked DLL.
    # The loader copies from staging/ before LoadLibrary.
    set(GAME_STAGING_DIR "${GAME_BIN_DIR}/staging")

    set_target_properties(equip_hide_logic PROPERTIES
        OUTPUT_NAME "CrimsonDesertEquipHide_Logic"
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

    # Generate deploy script at configure time (no source file needed).
    # Tries to copy DLL+PDB directly to plugins/ (game not running).
    # On success, cleans the staging copies so the hot-reload loader
    # does not pick up a stale build.  If the DLL is locked (game
    # running), both files stay in staging/ for hot-reload instead.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/deploy_logic.cmake" [=[
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy "${SRC_DLL}" "${DST_DLL}"
    RESULT_VARIABLE rc
)
if(rc EQUAL 0)
    message(STATUS "Direct deploy OK — cleaning staging")
    file(REMOVE "${STAGING_DLL}")
    # PDB: best-effort copy alongside the DLL
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

    add_custom_command(TARGET equip_hide_logic POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DSRC_DLL=$<TARGET_FILE:equip_hide_logic>
            -DDST_DLL=${GAME_BIN_DIR}/CrimsonDesertEquipHide_Logic.dll
            -DSTAGING_DLL=${GAME_STAGING_DIR}/CrimsonDesertEquipHide_Logic.dll
            -DSRC_PDB=$<TARGET_PDB_FILE:equip_hide_logic>
            -DDST_PDB=${GAME_BIN_DIR}/CrimsonDesertEquipHide_Logic.pdb
            -DSTAGING_PDB=${GAME_STAGING_DIR}/CrimsonDesertEquipHide_Logic.pdb
            -P "${CMAKE_CURRENT_BINARY_DIR}/deploy_logic.cmake"
        COMMENT "Deploying Logic DLL+PDB (falls back to staging if locked)"
    )

    target_compile_options(equip_hide_logic PRIVATE /W4 /Zi)
    target_link_options(equip_hide_logic PRIVATE /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF)

else() # EQUIPHIDE_DEV_BUILD=OFF
    # =====================================================================
    # Production Build: single ASI (unchanged)
    # =====================================================================

    # --- ASI Target (.asi file) ---
    add_library(${PROJECT_NAME}-ASI SHARED ${ASI_SOURCES})

    target_include_directories(${PROJECT_NAME}-ASI PRIVATE ${COMMON_INCLUDE_DIRS})

    target_link_libraries(${PROJECT_NAME}-ASI PRIVATE
        DetourModKit
        CrimsonDesertCore::core
        psapi
        user32
        kernel32
    )

    # Set output name and properties for ASI
    set_target_properties(${PROJECT_NAME}-ASI PROPERTIES
        OUTPUT_NAME "CrimsonDesertEquipHide-temp-asi"
        SUFFIX ".asi"
        PREFIX ""
    )

    # --- Target-Scoped Release Optimizations ---
    # /Gy: function-level linking, /Gw: global data optimization.
    # /O1: favor size (appropriate for an injected DLL).
    target_compile_options(${PROJECT_NAME}-ASI PRIVATE
        $<$<CONFIG:Release>:/O1 /Gy /Gw>
    )

    # /OPT:REF removes unreferenced functions/data; /OPT:ICF folds identical COMDATs.
    target_link_options(${PROJECT_NAME}-ASI PRIVATE
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>
    )

    # --- Link-Time Optimization ---
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

    # Post-build steps to rename to desired names
    add_custom_command(TARGET ${PROJECT_NAME}-ASI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:${PROJECT_NAME}-ASI>
        ${CMAKE_CURRENT_BINARY_DIR}/CrimsonDesertEquipHide.asi
        COMMENT "Renaming ASI output to final name"
    )

    # --- Build Summary ---
    message(STATUS "---------------------------------------------------------------------")
    message(STATUS "CrimsonDesertEquipHide (${PROJECT_VERSION}) Configuration Summary:")
    message(STATUS "  Generator:                   ${CMAKE_GENERATOR}")
    message(STATUS "  Build Type:                  ${CMAKE_BUILD_TYPE}")
    message(STATUS "  Targets:")
    message(STATUS "    - ${PROJECT_NAME}-ASI      -> CrimsonDesertEquipHide.asi (via post-build copy)")
    message(STATUS "---------------------------------------------------------------------")
endif() # EQUIPHIDE_DEV_BUILD
