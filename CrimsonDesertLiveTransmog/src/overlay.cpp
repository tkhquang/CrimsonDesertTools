// overlay.cpp - Overlay lifecycle glue.
//
// Single-interface design: this TU is compiled against the ReShade SDK's imgui.h + reshade_overlay.hpp. In ReShade
// mode, ReShade installs its own function table into imgui_function_table_instance() before our addon callback fires.
// In standalone mode we install one built from imgui_lib's real symbols via lt_get_imgui_function_table(). Either way,
// the UI code in overlay_ui.cpp sees identical ImGui:: calls.
//
// The actual UI implementation (draw_overlay, draw_overlay_content, init_reshade_overlay, shutdown_reshade_overlay,
// is_reshade_overlay_active) lives in overlay_ui.cpp - same compilation TU group, sharing the same ReShade SDK include
// path.

#include "overlay.hpp"
#include "dx_overlay.hpp"
#include "imgui_function_table_populator.hpp"

#include <DetourModKit/logger.hpp>
#include <DetourModKit/region.hpp>

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

    namespace
    {
        bool s_forceStandalone = false;

        // Standalone path: point reshade_overlay.hpp's function-table accessor at our populated table so ImGui::
        // calls in overlay_ui.cpp route through imgui_lib's real symbols instead of dereferencing nullptr.
        //
        // This is a no-op when ReShade is loaded, because ReShade installs its own table before our addon callback
        // runs. Only the fallback path where ReShade init failed, or was skipped, calls it.
        void activate_standalone_imgui_table()
        {
            imgui_function_table_instance() =
                static_cast<const imgui_function_table_19250 *>(lt_get_imgui_function_table());
        }

        /**
         * @brief Returns the handle of the module this code is compiled into, without adding a reference.
         * @details DMK::Region::own() takes no reference, which is load-bearing rather than an optimization: a counted
         *          reference on the dev build's logic DLL keeps the generation mapped forever, and the loader's
         *          unload verdict then refuses every hot reload after the first. DetourModKit links statically into
         *          this binary, so its own module is this module.
         * @return The module handle, or nullptr when the loader lookup failed.
         */
        [[nodiscard]] HMODULE self_module() noexcept
        {
            return DMK::Region::own().base.as<HMODULE>();
        }
    } // namespace

    void set_force_standalone(bool force)
    {
        s_forceStandalone = force;
    }

    bool init_overlay()
    {
        auto &logger = DMK::log();
        bool has_overlay = false;

        // Try ReShade first unless the user opted into strict standalone. Mixed mode (ReShade addon AND standalone
        // window) clobbers imgui_function_table_instance(), which is a single pointer.
        //
        // Nothing hands a null module to register_addon. ReShade caches the first handle it receives in a
        // function-local static, so one null call poisons addon registration for the rest of this module's life, and
        // the failure then reads as "ReShade not present" rather than as the bug it is.
        const HMODULE self = self_module();
        if (self == nullptr)
            logger.warning("[overlay] Module handle lookup failed - ReShade registration skipped");

        if (!s_forceStandalone && self != nullptr)
        {
            if (init_reshade_overlay(self))
            {
                logger.info("[overlay] ReShade detected - registered addon tab (open ReShade with Home key)");
                has_overlay = true;
            }
        }

        if (!has_overlay)
        {
            activate_standalone_imgui_table();
            if (init_dx_overlay())
            {
                logger.info("[overlay] Standalone overlay active (press Home to toggle)");
                has_overlay = true;
            }
        }

        if (!has_overlay)
            logger.warning("[overlay] No overlay available - mod still works via hotkeys");
        return has_overlay;
    }

    void shutdown_overlay() noexcept
    {
        if (is_reshade_overlay_active())
            shutdown_reshade_overlay();
        shutdown_dx_overlay();
    }

} // namespace Transmog
