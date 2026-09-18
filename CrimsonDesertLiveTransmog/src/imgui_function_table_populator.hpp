// imgui_function_table_populator.hpp
//
// Exposes a populated `imgui_function_table_19250` whose slots point at imgui_lib's real ImGui symbols. Used to drive
// the function-table indirection that reshade_overlay.hpp wraps every `ImGui::` call in, so the overlay UI runs
// identically whether ReShade has set the table pointer (addon mode) or we have set it ourselves (standalone mode).
//
// The companion .cpp is a thin facade over the verbatim ReShade host-side populator vendored at
// external/reshade-sdk/source/imgui_function_table_19250.{hpp,cpp}. Refresh the vendored files with
// scripts/refresh_reshade_populator.sh when bumping ImGui to a new IMGUI_VERSION_NUM.
//
// The return type is `const void *` so the overlay TU (which sees the SDK's struct definition) and the populator TU
// (which sees the source-tree struct definition with the same byte layout but slightly different type-alias
// spellings) can pass the table across a TU boundary without a redeclaration of the struct.

#ifndef TRANSMOG_IMGUI_FUNCTION_TABLE_POPULATOR_HPP
#define TRANSMOG_IMGUI_FUNCTION_TABLE_POPULATOR_HPP

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Returns the address of the populated imgui_function_table_19250 for this process.
     * @return A pointer to the function-local table, which lives for the rest of the process.
     * @note The call has no observable effect beyond the return value, so a discarded result is always a bug.
     */
    [[nodiscard]] const void *lt_get_imgui_function_table();

#ifdef __cplusplus
} // extern "C"
#endif

#endif // TRANSMOG_IMGUI_FUNCTION_TABLE_POPULATOR_HPP
