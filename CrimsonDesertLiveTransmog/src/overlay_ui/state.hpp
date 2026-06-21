// overlay_ui/state.hpp
//
// Module-scope state shared between the overlay UI translation units.
//
// Declared as inline namespace-scope variables (C++17) so every TU that includes this header observes the same storage.
// Types live alongside state because the per-slot SlotUIState struct is consumed by multiple section files and exposing
// it here avoids forward-declaration churn.
//
// State naming uses the `s_` prefix to mark it as module-internal even though it lives in `namespace Transmog` (no
// anonymous namespace -- a single shared anonymous namespace across translation units is not possible; inline vars +
// Transmog scope provide equivalent linkage while making cross-TU access explicit).

#ifndef TRANSMOG_OVERLAY_UI_STATE_HPP
#define TRANSMOG_OVERLAY_UI_STATE_HPP

#include "shared_state.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>

namespace Transmog
{

    // Window title for the standalone host wrapper. ### gives ImGui a stable ID so renaming the visible title later
    // won't lose window state.
    inline constexpr const char *k_windowTitle = "Transmog###TransmogMain";

    // Minimum time (ms) the cursor must rest on a picker item before hover-apply fires. Prevents rapid apply cycles
    // while scrolling.
    inline constexpr std::int64_t k_hoverDebounceMs = 300;

    // Set by the host wrapper before draw_overlay_content runs.
    // true  = standalone overlay (wider layout for high-DPI)
    // false = ReShade addon tab  (compact layout)
    // Cannot use FontGlobalScale to detect this because ReShade at 4K also sets FontGlobalScale > 1.
    inline bool s_standaloneMode = false;

    // Session-only UI-scale multiplier applied on top of the init-time auto-DPI scale (see dx_overlay.cpp). 1.0 = no
    // override; the user picks 1.25/1.5/etc. from the header combo. Only used in the standalone overlay; ReShade has
    // its own scaling. Not persisted -- re-pick on next launch.
    inline float s_uiScale = 1.0f;

    // Instant Apply mode: applies transmog immediately on hover, pick, slot toggle, and clear -- no Apply All click
    // needed. Off by default because each action triggers a tear-down + SlotPopulator cycle. Prefab-mode (cross-slot
    // prefab browser) does NOT honour this -- hover-apply on prefabs needs additional carrier-borrow plumbing that's
    // not yet shipped. Click-to-pick still works in prefab mode.
    inline bool s_autoApply = false;

    // When true, preserve the search text between picker opens so the user can re-open the same slot and keep browsing
    // where they left off. Off by default to match legacy behaviour (clear on open).
    inline bool s_keepSearchText = true;

    // Dye picker region UI state: per-submesh collapse + per-layer R/G/B link toggle. Each unique submesh stable_id has
    // its own entry in the slot's regionUI map.
    struct RegionUIState
    {
        bool collapsed = false;
        // Per-layer "link R/G/B" toggle. Index 0..4 = tint/mask/detail/hair/scratch. When true (default), editing any
        // of the three R/G/B-suffix swatches in this layer cascades to the other two; mirrors merchant "Pure X"
        // semantics. Untick to split into independent per-channel control.
        std::array<bool, 5> linkRgb{{true, true, true, true, true}};
    };

    struct SlotUIState
    {
        char hexBuf[8]{"0000"};
        bool editing = false;
        char searchBuf[64]{};
        // "Exact" means: only list items whose auto-detected category matches THIS slot. Defaults to ON so the dropdown
        // shows ~350 relevant items instead of all 6024.
        bool exactFilter = true;
        bool hideIncompatible = true; // hide crash-risk + non-equipment
        bool hideVariants = false;    // hide NPC variants (carrier items)
        bool hideBodyMismatch = true; // hide items whose body type doesn't
                                      // match the active character's
        // Prefab-only mode. When true the picker hides the items list and its filters and shows ALL body-mesh prefabs
        // across every slot, labeled with their native slot. Picking a prefab applies it to the prefab's native slot
        // (not necessarily the popup slot) so the user can browse the full prefab catalog from any slot's picker.
        bool prefabMode = false;
        // Prefab-mode-only Exact filter: when true, the prefab-mode list only shows prefabs whose derived slot
        // (slot_for_prefab_name) matches the popup's slot category. Defaults true so opening
        // Helm's picker in prefab mode shows only Helm-family prefabs; user can untick to see the full cross-slot
        // catalog.
        bool prefabExactFilter = true;
        // Prefab-mode-only "keep open after pick" toggle. When true the picker stays open after committing a prefab so
        // the user can quickly try alternatives without re-opening the popup. Defaults on for the cross-slot browser
        // since users typically iterate through several candidates per slot.
        bool prefabKeepOpenOnPick = true;
        // Hover-apply debounce state. We track which item the cursor is on and when it first landed there. Apply fires
        // only after the cursor has settled on the same item for k_hoverDebounceMs, preventing rapid-fire apply cycles
        // when scrolling the list.
        std::uint16_t hoverPendingId = 0;
        std::uint16_t hoverAppliedId = 0;
        std::int64_t hoverStartMs = 0;
        // Prefab-mode mirror of the hover-apply debounce above. Keyed by prefab name (cd_phm_* / cd_phw_*) since the
        // prefab catalog has no compact integer id like items do. Only the Up/Down nav cursor (GUI buttons or arrow
        // keys) feeds this debounce. Mouse hover is intentionally NOT wired: a body-mesh preview fans out to a full
        // multi-actor manual_apply, which is too disruptive to chain off casual cursor motion. The apply fires via
        // commit_prefab_at(previewOnly=true) so the popup stays open across re-equips.
        //
        // Naming kept as `hover*` (rather than `nav*`) for cross-reading parity with items-mode
        // hoverPendingId/hoverAppliedId/hoverStartMs above. Both serve the same debounce role; the trigger source
        // differs (hover+nav for items, nav-only for prefabs).
        std::string hoverPendingPrefab;
        std::string hoverAppliedPrefab;
        std::int64_t hoverPrefabStartMs = 0;
        // Button-driven navigation index into the visible (filtered) list. -1 = no highlight. Up/Down buttons move
        // this; Enter commits the highlighted item. lastVisibleCount stores the previous frame's visible count for
        // clamping.
        int navIndex = -1;
        int lastVisibleCount = 0;
        bool navMoved = false; // true only on the frame a nav button was pressed
        // When the user picks a prefab from this slot's picker, we surface the prefab name on the slot button so the UI
        // shows "[prefab] cd_nhw_no_ub_20027" instead of the carrier item's display name. Empty string == no prefab
        // picked. Session-only (matches prefab-wrapper-swap selections; no JSON persistence).
        std::string pickedPrefabName;

        // Snapshot of the slot's carrier state BEFORE the first prefab pick auto-borrowed Kliff's plate item. Captured
        // on first pick
        // (priorCarrierSaved -> true) and restored when the user picks
        // the "(no body-mesh override)" entry to clear the prefab. Restoring the original Wellsknight (or whatever the
        // user's active preset had) lets the slot revert visually instead of staying stuck on the Kliff plate carrier
        // after a clear.
        bool priorCarrierSaved = false;
        bool priorCarrierActive = false;
        std::uint16_t priorCarrierItemId = 0;

        // Dye UI: per-region collapse state + per-layer R/G/B link toggles, keyed by submesh stable_id. Session-only;
        // reset on slot-target change. `std::map` for stable iteration without hashing a u64 we don't own.
        std::map<std::uint64_t, RegionUIState> regionUI{};

        // Dye UI: "Show only modified" filter. When true, the picker hides clusters / regions whose rows are all at
        // their captured engine default (`override_active == false`). Lets users with many swatches focus on what
        // they've already touched. Session-only -- the filter is a viewing preference, not preset state.
        bool showOnlyModified = false;
    };

    inline SlotUIState s_slotUI[k_slotCount]{};

    // One-shot auto-trigger for the per-slot Color Override reinit. When the popup opens on a slot whose swatch grid is
    // empty (detected == 0) and no reinit is currently running, we fire a single-pass reinit so the user does not have
    // to click Re-init. Re-armed each time the popup closes (set false in overlay_ui.cpp's popup-closed branch).
    inline bool s_coAutoTriggered[k_slotCount] = {};

    // Preset rename dialog scratch buffers.
    inline char s_renamePresetBuf[64]{};
    inline bool s_renameActive = false;
    inline int s_renameIndex = -1;

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_UI_STATE_HPP
