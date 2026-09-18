cmake_minimum_required(VERSION 3.28)

project(CrimsonDesertEquipHide VERSION 0.2.3 LANGUAGES CXX)

# Language level for every target in this directory and its subdirectories.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(/W4)

# Windows SDK target level and the macros every source in this tree expects.
if(WIN32)
    add_compile_definitions(
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

# CrimsonDesertCore is the monorepo sibling. Core owns DetourModKit as its own submodule and re-exports it through a
# PUBLIC link, so this project gets the DetourModKit target transitively and needs no add_subdirectory for DMK.
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

# Selects the two-DLL hot-reload layout instead of the single shipped ASI.
option(EQUIPHIDE_DEV_BUILD "Build two-DLL hot-reload configuration for development" OFF)

# Every source shared by the dev logic DLL and the shipped ASI.
set(COMMON_SOURCES
    src/aob_resolver.cpp
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

set(COMMON_INCLUDE_DIRS
    src
)

# Plugins directory the dev build deploys into. Override it at configure time on a machine with another install
# path. Every deploy step is guarded on its existence, so a checkout elsewhere still configures and builds.
set(CD_GAME_BIN_DIR "D:/Games/SteamLibrary/steamapps/common/Crimson Desert/bin64/plugins"
    CACHE PATH "Crimson Desert plugins directory the dev build deploys into")

if(EQUIPHIDE_DEV_BUILD)
    message(STATUS "DEV BUILD enabled: building loader + logic DLL pair")

    # Resident staged-generation loader ASI. Its sources live in CrimsonDesertCore and take their parameters from the
    # CDCORE_LOADER_* macros below.
    add_library(equip_hide_loader SHARED ${CDCORE_DEV_LOADER_SRC})

    target_compile_definitions(equip_hide_loader PRIVATE
        CDCORE_LOADER_MOD_NAME="CrimsonDesertEquipHide"
        CDCORE_LOADER_PROCESS_NAME="CrimsonDesert.exe"
    )

    target_include_directories(equip_hide_loader PRIVATE ${CDCORE_DEV_INCLUDE_DIR})

    # The loader hosts wheel capture for the process, so a user-bound mouse-wheel combo books its permanent module
    # keepalive HERE instead of in the logic DLL, which is what lets a generation unmap. It links the resident host
    # ONLY, and that target already carries user32 on its INTERFACE.
    target_link_libraries(equip_hide_loader PRIVATE
        DetourModKit::WheelHost
    )

    # Guard the property above. A link against the full archive gives the loader its own DetourModKit instance, which
    # is the arrangement DMK's unload proofs exclude.
    get_target_property(_eh_loader_links equip_hide_loader LINK_LIBRARIES)
    if(NOT _eh_loader_links MATCHES "DetourModKit::WheelHost")
        message(FATAL_ERROR "equip_hide_loader must link DetourModKit::WheelHost.")
    endif()
    if(_eh_loader_links MATCHES "DetourModKit::DetourModKit" OR _eh_loader_links MATCHES "(^|;)DetourModKit(;|$)")
        message(FATAL_ERROR "equip_hide_loader must not link the full DetourModKit archive.")
    endif()

    # The loader links into the build tree. The post-build step tries to copy it into the game folder and gives up
    # quietly when the ASI is locked by a running game.
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

    if(EXISTS "${CD_GAME_BIN_DIR}")
        add_custom_command(TARGET equip_hide_loader POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DSRC_ASI=$<TARGET_FILE:equip_hide_loader>
                -DDST_ASI=${CD_GAME_BIN_DIR}/CrimsonDesertEquipHide.asi
                -P "${CMAKE_CURRENT_BINARY_DIR}/deploy_loader.cmake"
            COMMENT "Deploying Loader ASI (skips if locked)"
        )
    else()
        message(STATUS "CD_GAME_BIN_DIR does not exist, loader ASI deploy disabled: ${CD_GAME_BIN_DIR}")
    endif()

    target_compile_options(equip_hide_loader PRIVATE /W4)

    # Logic DLL: every line of mod code, replaced wholesale on each hot reload.
    add_library(equip_hide_logic SHARED
        ${COMMON_SOURCES}
        src/dev/mod_logic.cpp
    )

    target_include_directories(equip_hide_logic PRIVATE ${COMMON_INCLUDE_DIRS} ${CDCORE_DEV_INCLUDE_DIR})

    target_compile_definitions(equip_hide_logic PRIVATE EQUIPHIDE_DEV_BUILD)

    target_link_libraries(equip_hide_logic PRIVATE
        DetourModKit
        CrimsonDesertCore::core
    )

    # Fallback drop point for a deploy that loses the race with the loader reading the deployed DLL. The loader copies
    # from staging/ before LoadLibrary.
    set(GAME_STAGING_DIR "${CD_GAME_BIN_DIR}/staging")

    set_target_properties(equip_hide_logic PROPERTIES
        OUTPUT_NAME "CrimsonDesertEquipHide.logic"
        PREFIX ""
    )

    # The deploy script is generated at configure time, so no checked-in source file is needed. It copies the DLL and
    # PDB straight into plugins/ and then clears the staging copies, so the hot-reload loader cannot promote a stale
    # build over this one.
    #
    # The loader maps a per-generation COPY of the deployed DLL, never the file itself, so the direct copy never races
    # a mapped image. One window remains: the loader reads this file as it stages a new generation, and a build that
    # lands exactly then loses the copy. Both files then stay in staging/ and the loader promotes them on the next
    # reload.
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
    message(STATUS "Direct deploy skipped (file locked). Staged for the loader to promote on the next reload.")
    get_filename_component(staging_dir "${STAGING_DLL}" DIRECTORY)
    file(MAKE_DIRECTORY "${staging_dir}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${SRC_DLL}" "${STAGING_DLL}")
    if(EXISTS "${SRC_PDB}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${SRC_PDB}" "${STAGING_PDB}")
    endif()
endif()
]=])

    if(EXISTS "${CD_GAME_BIN_DIR}")
        add_custom_command(TARGET equip_hide_logic POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DSRC_DLL=$<TARGET_FILE:equip_hide_logic>
                -DDST_DLL=${CD_GAME_BIN_DIR}/CrimsonDesertEquipHide.logic.dll
                -DSTAGING_DLL=${GAME_STAGING_DIR}/CrimsonDesertEquipHide.logic.dll
                -DSRC_PDB=$<TARGET_PDB_FILE:equip_hide_logic>
                -DDST_PDB=${CD_GAME_BIN_DIR}/CrimsonDesertEquipHide.logic.pdb
                -DSTAGING_PDB=${GAME_STAGING_DIR}/CrimsonDesertEquipHide.logic.pdb
                -P "${CMAKE_CURRENT_BINARY_DIR}/deploy_logic.cmake"
            COMMENT "Deploying Logic DLL+PDB (falls back to staging if locked)"
        )
    else()
        message(STATUS "CD_GAME_BIN_DIR does not exist, logic DLL deploy disabled: ${CD_GAME_BIN_DIR}")
    endif()

    target_compile_options(equip_hide_logic PRIVATE /W4 /Zi)
    target_link_options(equip_hide_logic PRIVATE /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF)

else() # EQUIPHIDE_DEV_BUILD=OFF
    # Production build: one shipped ASI carrying every source.
    add_library(${PROJECT_NAME}-ASI SHARED ${ASI_SOURCES})

    target_include_directories(${PROJECT_NAME}-ASI PRIVATE ${COMMON_INCLUDE_DIRS})

    target_link_libraries(${PROJECT_NAME}-ASI PRIVATE
        DetourModKit
        CrimsonDesertCore::core
    )

    # Set output name and properties for ASI
    set_target_properties(${PROJECT_NAME}-ASI PROPERTIES
        OUTPUT_NAME "CrimsonDesertEquipHide-temp-asi"
        SUFFIX ".asi"
        PREFIX ""
    )

    # /Gy gives function-level linking, /Gw optimizes global data, and /O1 favors size, which suits an injected DLL.
    target_compile_options(${PROJECT_NAME}-ASI PRIVATE
        $<$<CONFIG:Release>:/O1 /Gy /Gw>
    )

    # /OPT:REF removes unreferenced functions/data; /OPT:ICF folds identical COMDATs.
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

    # Post-build steps to rename to desired names
    add_custom_command(TARGET ${PROJECT_NAME}-ASI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:${PROJECT_NAME}-ASI>
        ${CMAKE_CURRENT_BINARY_DIR}/CrimsonDesertEquipHide.asi
        COMMENT "Renaming ASI output to final name"
    )

    message(STATUS "CrimsonDesertEquipHide (${PROJECT_VERSION}) Configuration Summary:")
    message(STATUS "  Generator:                   ${CMAKE_GENERATOR}")
    message(STATUS "  Build Type:                  ${CMAKE_BUILD_TYPE}")
    message(STATUS "  Targets:")
    message(STATUS "    - ${PROJECT_NAME}-ASI      -> CrimsonDesertEquipHide.asi (via post-build copy)")
endif() # EQUIPHIDE_DEV_BUILD
