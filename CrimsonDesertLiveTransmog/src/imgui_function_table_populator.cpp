// imgui_function_table_populator.cpp
//
// Thin facade over ReShade's host-side function-table populator, which lives verbatim at
// external/reshade-sdk/source/imgui_function_table_19250.{hpp,cpp}.
//
// Upstream gates the populator body behind a build-time macro triad that applies inside ReShade's own build. This file
// defines those macros so the gated body compiles in our TU, then includes the .cpp directly so one TU holds the
// populator function. That keeps per-source COMPILE_DEFINITIONS out of CMake.
//
// Only this file, and the .hpp pair next to it, is hand-written. An ImGui bump re-vendors the two files in external/.
// This facade and its header stay as they are unless ImGui's IMGUI_VERSION_NUM changes. A new number renames the
// upstream files, so update the #include lines below to match it.

#define RESHADE_API_LIBRARY_EXPORT
#define RESHADE_GUI 1
#define RESHADE_ADDON 1

#pragma warning(push, 0)
#include <imgui.h>
#include "imgui_function_table_19250.hpp"
#include "imgui_function_table_19250.cpp"
#pragma warning(pop)

#include "imgui_function_table_populator.hpp"

extern "C" const void *lt_get_imgui_function_table()
{
    // The first call materializes the table. Later calls return its address. The layout matches
    // reshade::imgui_function_table_19250 byte-for-byte, because the SDK header in external/reshade-sdk/include/ and
    // the source header in external/reshade-sdk/source/ come from the same upstream definition. The only spelling
    // difference is type aliases like ImGuiIO& versus imgui_io_19250&, which name the same type.
    static const imgui_function_table_19250 t = init_imgui_function_table_19250();
    return &t;
}
