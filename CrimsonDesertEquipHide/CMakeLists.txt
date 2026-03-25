cmake_minimum_required(VERSION 3.16)

project(CrimsonDesertEquipHide VERSION 0.1.0 LANGUAGES CXX)

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
