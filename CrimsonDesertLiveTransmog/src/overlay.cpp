// overlay.cpp -- Overlay lifecycle glue.
//
// Single-interface design: this TU is compiled against the ReShade SDK's
// imgui.h + reshade_overlay.hpp.  In ReShade mode, ReShade installs its
// own function table into imgui_function_table_instance() before our
// addon callback fires.  In standalone mode we install one built from
// imgui_lib's real symbols via lt_get_imgui_function_table().  Either
// way, the UI code in overlay_ui.cpp sees identical ImGui:: calls.
//
// The actual UI implementation (draw_overlay, draw_overlay_content,
// init_reshade_overlay, shutdown_reshade_overlay, is_reshade_overlay_active)
// lives in overlay_ui.cpp -- same compilation TU group, sharing the same
// ReShade SDK include path.

#include "overlay.hpp"
#include "dx_overlay.hpp"
#include "imgui_function_table_populator.hpp"

#include <DetourModKit.hpp>

#pragma warning(push, 0)
#include <imgui.h>
#include <reshade_overlay.hpp>
#pragma warning(pop)

namespace Transmog
{
    // Implemented in overlay_ui.cpp (same TU group).
    bool init_reshade_overlay(HMODULE hModule);
    void shutdown_reshade_overlay();
    bool is_reshade_overlay_active();

    static bool s_forceStandalone = false;

    void set_force_standalone(bool force) { s_forceStandalone = force; }

    // Standalone path: point reshade_overlay.hpp's function-table accessor
    // at our populated table so ImGui:: calls in overlay_ui.cpp route
    // through imgui_lib's real symbols instead of dereferencing nullptr.
    //
    // No-op if ReShade is loaded -- in that case ReShade installs its own
    // table before our addon callback runs.  We only call this on the
    // fallback path where ReShade init failed (or was skipped).
    static void activate_standalone_imgui_table()
    {
        imgui_function_table_instance() =
            static_cast<const imgui_function_table_19250 *>(
                lt_get_imgui_function_table());
    }

    bool init_overlay(HMODULE hModule)
    {
        auto &logger = DMK::Logger::get_instance();
        bool hasOverlay = false;

        // Try ReShade first unless the user opted into strict standalone.
        // Mixed mode (ReShade addon AND standalone window) would clobber
        // imgui_function_table_instance() since it is a single pointer.
        if (!s_forceStandalone)
        {
            if (init_reshade_overlay(hModule))
            {
                logger.info("[overlay] ReShade detected -- registered addon "
                            "tab (open ReShade with Home key)");
                hasOverlay = true;
            }
        }

        if (!hasOverlay)
        {
            activate_standalone_imgui_table();
            if (init_dx_overlay())
            {
                logger.info("[overlay] Standalone overlay active "
                            "(press Home to toggle)");
                hasOverlay = true;
            }
        }

        if (!hasOverlay)
            logger.warning("[overlay] No overlay available -- "
                           "mod still works via hotkeys");
        return hasOverlay;
    }

    void shutdown_overlay() noexcept
    {
        if (is_reshade_overlay_active())
            shutdown_reshade_overlay();
        shutdown_dx_overlay();
    }

} // namespace Transmog
