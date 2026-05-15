// imgui_function_table_populator.cpp
//
// Thin facade over ReShade's host-side function-table populator, which
// lives verbatim at external/reshade-sdk/source/imgui_function_table_19250.{hpp,cpp}.
//
// Upstream gates the populator body behind a build-time macro triad that
// applies inside ReShade's own build.  We define those macros here so the
// gated body compiles in our TU, then include the .cpp directly so we get
// one TU with the populator function visible -- avoiding per-source
// COMPILE_DEFINITIONS in CMake.
//
// Only this file -- and the .hpp pair next to it -- is hand-written.
// Bumping ImGui is a re-vendor of the two files in external/; this
// facade and its header don't change unless ImGui's IMGUI_VERSION_NUM
// changes (which would rename the upstream files; update the #include
// lines below to match the new number).

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
    // First call materialises the table; subsequent calls return its
    // address.  Layout matches reshade::imgui_function_table_19250
    // byte-for-byte (the SDK header in external/reshade-sdk/include/ and
    // the source header in external/reshade-sdk/source/ come from the
    // same upstream definition; the only spelling difference is type
    // aliases like ImGuiIO& vs imgui_io_19250&, which name the same type).
    static const imgui_function_table_19250 t = init_imgui_function_table_19250();
    return &t;
}
