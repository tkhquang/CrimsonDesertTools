// ====================================================================== //
//  overlay.cpp — Standalone overlay path (D3D11 WARP + GDI blit)         //
//                                                                         //
//  This TU includes overlay_ui.inl which resolves ImGui calls against     //
//  imgui_lib (the real ImGui compiled from source).  The ReShade path     //
//  lives in overlay_reshade.cpp which resolves against ReShade's          //
//  function-table wrappers instead.                                       //
// ====================================================================== //

#include "overlay.hpp"
#include "constants.hpp"
#include "dx_overlay.hpp"
#include "item_name_table.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "transmog.hpp"
#include "transmog_map.hpp"

#include <DetourModKit.hpp>

#pragma warning(push, 0)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace Transmog
{
    // Window title for the standalone ImGui overlay.  The ### suffix
    // gives ImGui a stable ID so renaming the visible title later
    // (e.g. appending a status badge) won't lose window state.
    static constexpr const char *k_windowTitle = "Transmog###TransmogMain";

    // Force standalone overlay even when ReShade is available.
    static bool s_forceStandalone = false;

    // Shared UI state and draw_overlay_content().
    #include "overlay_ui.inl"

    void set_force_standalone(bool force) { s_forceStandalone = force; }

    // --- Public wrappers ---

    void draw_overlay()
    {
        // Size relative to display: 33% width, 80% height, capped at 800px wide.
        const auto &displaySize = ImGui::GetIO().DisplaySize;
        const float w = std::min(displaySize.x * 0.33f, 1000.0f);
        const float h = displaySize.y * 0.8f;
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(k_windowTitle))
        {
            ImGui::End();
            return;
        }
        draw_overlay_content();
        ImGui::End();
    }

    // --- Dual-path init/shutdown ---

    // Forward declarations from overlay_reshade.cpp.
    bool init_reshade_overlay(HMODULE hModule);
    void shutdown_reshade_overlay();
    bool is_reshade_overlay_active();

    bool init_overlay(HMODULE hModule)
    {
        auto &logger = DMK::Logger::get_instance();
        bool hasOverlay = false;

        // Always try ReShade — registers the addon tab if ReShade is loaded.
        if (init_reshade_overlay(hModule))
        {
            logger.info("[overlay] ReShade detected — registered addon tab "
                        "(open ReShade with Home key)");
            hasOverlay = true;
        }

        // Standalone overlay: launches when ReShade is absent, or when
        // the user explicitly sets ForceStandaloneOverlay = true.
        if (!hasOverlay || s_forceStandalone)
        {
            if (init_dx_overlay())
            {
                logger.info("[overlay] Standalone overlay active "
                            "(press Home to toggle)");
                hasOverlay = true;
            }
        }

        if (!hasOverlay)
            logger.warning("[overlay] No overlay available — "
                           "mod still works via hotkeys");
        return hasOverlay;
    }

    void shutdown_overlay() noexcept
    {
        if (is_reshade_overlay_active())
            shutdown_reshade_overlay();
        // Standalone may also be active (ForceStandaloneOverlay).
        shutdown_dx_overlay();
    }

} // namespace Transmog
