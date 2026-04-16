// overlay_reshade.cpp — ReShade addon overlay path.
//
// This TU includes the ReShade SDK's imgui.h which defines ImGui
// functions as inline wrappers that call through ReShade's function
// table.  This is a different ImGui from imgui_lib (the real compiled
// ImGui used by the standalone overlay).
//
// overlay_ui.inl is included here so draw_overlay_content() resolves
// ImGui calls to ReShade's wrappers, rendering appears inside
// ReShade's addon tab, not a separate window.
//
// This file must NOT see imgui_lib's include path.  The CMakeLists.txt
// configures this TU with the ReShade SDK include directory instead.

#include "constants.hpp"
#include "item_name_table.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "transmog.hpp"
#include "transmog_map.hpp"

#include <DetourModKit.hpp>

// ReShade SDK headers — imgui.h here is from external/reshade-sdk/include
// and provides the function-table wrappers, NOT the real ImGui.
#pragma warning(push, 0)
#include <imgui.h>
#include <reshade.hpp>
#pragma warning(pop)

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace Transmog
{
    // Shared UI state and draw_overlay_content().
    // ImGui calls in this .inl resolve to ReShade's function-table
    // wrappers because this TU includes the ReShade SDK's imgui.h.
    #include "overlay_ui.inl"

    static HMODULE s_reshadeModule = nullptr;
    static bool s_reshadeActive = false;

    // ReShade tab callback — content is drawn directly inside the
    // ReShade addon tab; no ImGui::Begin/End window management needed.
    static void draw_reshade_overlay(reshade::api::effect_runtime *)
    {
        draw_overlay_content();
    }

    bool init_reshade_overlay(HMODULE hModule)
    {
        if (!reshade::register_addon(hModule))
            return false;

        reshade::register_overlay("Transmog", &draw_reshade_overlay);
        s_reshadeModule = hModule;
        s_reshadeActive = true;
        return true;
    }

    void shutdown_reshade_overlay()
    {
        if (!s_reshadeActive)
            return;

        reshade::unregister_overlay("Transmog", &draw_reshade_overlay);
        reshade::unregister_addon(s_reshadeModule);
        s_reshadeActive = false;
        s_reshadeModule = nullptr;
    }

    bool is_reshade_overlay_active()
    {
        return s_reshadeActive;
    }

} // namespace Transmog
