cmake_minimum_required(VERSION 3.16)

project(CrimsonDesertEquipHide VERSION 0.2.3 LANGUAGES CXX)

# --- Standard and Compiler Options ---
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Override CMake's default -O3 for MinGW Release builds with -O2 for a better
# code-size/performance tradeoff and to avoid GCC 15 LTO ICEs at -O3.
if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
endif()

# --- Compiler Flags ---
if(MSVC)
    add_compile_options(/W4)
else()
    # GCC/Clang/MinGW specific flags
    add_compile_options(-Wall -Wextra)
endif()

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

# --- DetourModKit Dependency ---
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/DetourModKit/CMakeLists.txt")
    message(STATUS "Configuring DetourModKit from: external/DetourModKit")
    add_subdirectory(external/DetourModKit)

    # Use the exact target name from DetourModKit
    if(TARGET DetourModKit)
        set(DETOURMODKIT_TARGET DetourModKit)
        message(STATUS "DetourModKit target found: ${DETOURMODKIT_TARGET}")
    else()
        message(FATAL_ERROR "DetourModKit target not created by subdirectory")
    endif()
else()
    message(FATAL_ERROR "DetourModKit not found at 'external/DetourModKit'. "
        "Please ensure the submodule is initialized: "
        "'git submodule update --init --recursive'")
endif()

# --- Dev Build Option ---
option(EQUIPHIDE_DEV_BUILD "Build two-DLL hot-reload configuration for development" OFF)

# --- Source Files ---
set(COMMON_SOURCES
    src/dllmain.cpp
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
    add_library(equip_hide_loader SHARED src/dev/loader_main.cpp)

    target_link_libraries(equip_hide_loader PRIVATE
        kernel32
        user32
    )

    set_target_properties(equip_hide_loader PROPERTIES
        OUTPUT_NAME "CrimsonDesertEquipHide"
        SUFFIX ".asi"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${GAME_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${GAME_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${GAME_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${GAME_BIN_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${GAME_BIN_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${GAME_BIN_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${GAME_BIN_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${GAME_BIN_DIR}"
    )

    if(MSVC)
        target_compile_options(equip_hide_loader PRIVATE /W4)
    else()
        target_link_options(equip_hide_loader PRIVATE
            -static-libgcc
            -static-libstdc++
            -static
        )
    endif()

    # --- Logic DLL (all mod code) ---
    add_library(equip_hide_logic SHARED
        ${COMMON_SOURCES}
        src/dev/logic_exports.cpp
    )

    target_include_directories(equip_hide_logic PRIVATE ${COMMON_INCLUDE_DIRS})

    target_compile_definitions(equip_hide_logic PRIVATE EQUIPHIDE_DEV_BUILD)

    target_link_libraries(equip_hide_logic PRIVATE
        DetourModKit
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

    if(MSVC)
        target_compile_options(equip_hide_logic PRIVATE /W4 /Zi)
        target_link_options(equip_hide_logic PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    else()
        target_link_options(equip_hide_logic PRIVATE
            -Wl,--exclude-all-symbols
            -static-libgcc
            -static-libstdc++
            -static
        )
    endif()

else() # EQUIPHIDE_DEV_BUILD=OFF
    # =====================================================================
    # Production Build: single ASI (unchanged)
    # =====================================================================

    # --- ASI Target (.asi file) ---
    add_library(${PROJECT_NAME}-ASI SHARED ${ASI_SOURCES})

    target_include_directories(${PROJECT_NAME}-ASI PRIVATE ${COMMON_INCLUDE_DIRS})

    target_link_libraries(${PROJECT_NAME}-ASI PRIVATE
        DetourModKit
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
    if(MSVC)
        # /Gy: function-level linking, /Gw: global data optimization.
        # /O1: favor size (appropriate for an injected DLL).
        target_compile_options(${PROJECT_NAME}-ASI PRIVATE
            $<$<CONFIG:Release>:/O1 /Gy /Gw>
        )

        # /OPT:REF removes unreferenced functions/data; /OPT:ICF folds identical COMDATs.
        target_link_options(${PROJECT_NAME}-ASI PRIVATE
            $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>
        )
    else()
        # -ffunction-sections/-fdata-sections enable --gc-sections for dead code elimination.
        # -O2 is set globally via CMAKE_CXX_FLAGS_RELEASE.
        target_compile_options(${PROJECT_NAME}-ASI PRIVATE
            $<$<CONFIG:Release>:-ffunction-sections -fdata-sections>
        )
        target_link_options(${PROJECT_NAME}-ASI PRIVATE
            -Wl,--gc-sections
            -Wl,--exclude-all-symbols
            -static-libgcc
            -static-libstdc++
            -static

            # Strip all symbols in Release to minimize binary size.
            $<$<CONFIG:Release>:-s>
        )
    endif()

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
