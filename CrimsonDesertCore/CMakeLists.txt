cmake_minimum_required(VERSION 3.28)

project(CrimsonDesertCore VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Shared library for Crimson Desert mods.
#
# Consumers (CrimsonDesertEquipHide, CrimsonDesertLiveTransmog) pull this in
# via add_subdirectory() from the monorepo root and link against the
# CrimsonDesertCore::core target. Core owns DetourModKit as a submodule
# under external/DetourModKit and re-exports it via PUBLIC link, so
# consumers do not need their own DMK checkout.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- DetourModKit (submodule, owned by Core) ---
if(NOT TARGET DetourModKit)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/DetourModKit/CMakeLists.txt")
        message(STATUS "CrimsonDesertCore: configuring DetourModKit from external/DetourModKit")
        add_subdirectory(external/DetourModKit SYSTEM)
    else()
        message(FATAL_ERROR
            "DetourModKit not found at 'CrimsonDesertCore/external/DetourModKit'. "
            "Run 'git submodule update --init --recursive' from the monorepo root.")
    endif()

    if(NOT TARGET DetourModKit)
        message(FATAL_ERROR "DetourModKit target not created by its subdirectory")
    endif()
endif()

add_library(CrimsonDesertCore STATIC
    src/controlled_char.cpp
    src/indexed_string_table.cpp
)

# Shared dev-build hot-reload loader. A consumer that builds the dev two-DLL
# variant references the source directly from its own
# add_library(..._loader SHARED ...) call, adds the dev directory to its
# include path for protocol.h, and supplies the two CDCORE_LOADER_* macros via
# target_compile_definitions. The loader links DetourModKit::WheelHost ONLY: a
# second full-archive instance would give it its own hook ledger and pin the
# logic DLL again, which is exactly the arrangement DMK's unload proofs exclude.
set(CDCORE_DEV_LOADER_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/dev/mod_loader.cpp"
    CACHE INTERNAL "Path to CrimsonDesertCore shared dev loader source")
set(CDCORE_DEV_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/dev"
    CACHE INTERNAL "Path to the CrimsonDesertCore dev protocol header")

add_library(CrimsonDesertCore::core ALIAS CrimsonDesertCore)

target_include_directories(CrimsonDesertCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(CrimsonDesertCore PUBLIC DetourModKit)

# psapi is required by dmk_glue.cpp's EnumProcessModules /
# GetModuleFileNameExW path used to detect sibling-mod presence.
if(WIN32)
    target_link_libraries(CrimsonDesertCore PRIVATE psapi)
endif()

target_compile_options(CrimsonDesertCore PRIVATE /W4)

if(WIN32)
    target_compile_definitions(CrimsonDesertCore PRIVATE
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()
