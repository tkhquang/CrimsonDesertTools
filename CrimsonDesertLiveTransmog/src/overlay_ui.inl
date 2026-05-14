// overlay_ui.inl -- Shared transmog overlay widget code.
//
// Included by both overlay.cpp (standalone D3D11 path) and
// overlay_reshade.cpp (ReShade addon path).  Each TU provides its own
// ImGui symbols: the standalone path links against imgui_lib while the
// ReShade path uses the function-table wrappers from reshade.hpp.
//
// Everything here is static (internal linkage per TU).  Only one path
// is active at runtime so the duplicated state is harmless (~2 KB).
//
// Requires the including TU to have these headers already included:
//   imgui.h (either real or ReShade wrapper), constants.hpp,
//   item_name_table.hpp, preset_manager.hpp, shared_state.hpp,
//   transmog.hpp, transmog_map.hpp, <DetourModKit.hpp>,
//   <cstdarg>, <cstdio>, <cstring>, <string>

// ImGui::Text, TextColored, TextDisabled, and SetTooltip are variadic.
// The compiler cannot inline them, so ReShade's reshade_overlay.hpp
// emits out-of-line COMDAT copies that conflict with imgui_lib's strong
// symbols at link time.  These helpers call only non-variadic ImGui
// functions (TextUnformatted, PushStyleColor) which inline correctly
// in both the ReShade and standalone paths.

// Anonymous namespace wraps the entire body so each including TU
// (overlay.cpp, overlay_reshade.cpp) gets its own unique implicit
// namespace name -- makes TU-locality explicit and silences
// IntelliSense's cross-TU "ambiguous declaration" diagnostics.
namespace {

static void ui_text(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ImGui::TextUnformatted(buf);
}

static void ui_text_disabled(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
}

static void ui_text_colored(const ImVec4 &col, const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
}

static void ui_tooltip(const char *text)
{
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(text);
    ImGui::EndTooltip();
}

// Mirror a picker-committed override into PendingOverrides so the
// slot-agnostic substitute path in color_override/setter_substitute.cpp
// picks up the user's edit on the next engine write. Without this,
// picker edits would update SwatchOverride only and the substitute
// would keep serving the JSON-loaded RGB indefinitely.
//
// Caller passes the picker's slot index + row index. The helper
// reads `submesh_name` from the row's `SwatchOverride` and `token_id`
// from the row's `SwatchEntry`, then writes to PendingOverrides.
// Silently no-ops if either field is missing (early-init or a fresh
// row that hasn't been bound yet).
static void mirror_override_to_pending(
    int slot, std::size_t idx,
    std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    namespace SwT = Transmog::ColorOverride::SwatchTable;
    namespace PO  = Transmog::ColorOverride::PendingOverrides;
    auto *ent = SwT::row(slot, idx);
    auto *ovr = SwT::override_row(slot, idx);
    if (ent == nullptr || ovr == nullptr) return;
    if (ovr->submesh_name[0] == '\0') return;
    const std::uint16_t tok = ent->token_id.load(
        std::memory_order_relaxed);
    if (tok == 0) return;
    PO::set_by_token_id(slot, ovr->submesh_name, tok, r, g, b);
}

// Erase any pending entry matching this row's (submesh, token).
// Called when the user reverts a row to engine default or un-ticks
// the override checkbox, so the substitute stops firing for it.
static void erase_override_from_pending(
    int slot, std::size_t idx) noexcept
{
    namespace SwT = Transmog::ColorOverride::SwatchTable;
    namespace PO  = Transmog::ColorOverride::PendingOverrides;
    auto *ent = SwT::row(slot, idx);
    auto *ovr = SwT::override_row(slot, idx);
    if (ent == nullptr || ovr == nullptr) return;
    if (ovr->submesh_name[0] == '\0') return;
    const std::uint16_t tok = ent->token_id.load(
        std::memory_order_relaxed);
    if (tok == 0) return;
    PO::erase_by_token_id(slot, ovr->submesh_name, tok);
}

// Audit a submesh's dye-property channel coverage and emit a
// tooltip body listing gaps. Used by the dye picker to render a
// (!) next to submeshes whose shader doesn't expose the full RGB
// triple for one or more dye families -- the most common reason a
// user can't drive a color outside the asset's baked range (e.g.
// `cd_phw_00_ub_0135_00_01_01` lacks `_tintColorR`, so setting
// tint to black still rides on the baked-high R value).
//
// `present[layer][channel]`: 1 if a SwatchEntry exists for that
// (layer, channel) on the submesh, 0 otherwise. Indices:
//   layer 0 = _tintColor
//   layer 1 = _dyeingColorMask
//   layer 2 = _dyeingDetailLayerColorMask
//   channel 0 = R, 1 = G, 2 = B
//
// Hair (layer 3) is intentionally excluded -- its presence is
// context-dependent (only hair-bearing submeshes carry it, and
// missing it on body / armor submeshes is the normal case).
//
// Detail-mask absence is also silenced because many assets simply
// don't use that family -- treating it as a "gap" would put a (!)
// on every submesh and dilute the signal. Detail-mask is only
// flagged when it's PARTIALLY present (1 or 2 channels), which
// genuinely is a baked-channel lock.
//
// Returns true if at least one gap was found (so the caller knows
// to render the (!) marker). On true, `out` holds bullet lines.
static bool dye_picker_compute_channel_gap_tip(
    const int present[3][3], char *out, std::size_t cap)
{
    if (!out || cap < 8) return false;
    out[0] = '\0';
    static const char *kFamilyName[3] = {
        "_tintColor",
        "_dyeingColorMask",
        "_dyeingDetailLayerColorMask",
    };
    static const char kChan[3] = {'R', 'G', 'B'};
    bool any = false;
    std::size_t off = 0;
    for (int L = 0; L < 3; ++L)
    {
        const int n = present[L][0] + present[L][1] + present[L][2];
        if (n == 3) continue;            // full coverage
        if (n == 0 && L == 2) continue;  // detail-mask absence is normal
        any = true;
        int written = 0;
        if (n == 0)
        {
            written = std::snprintf(
                out + off, cap - off,
                "- %s family not exposed (engine never writes "
                "these for this submesh)\n",
                kFamilyName[L]);
        }
        else
        {
            char missing[8];
            std::size_t mi = 0;
            for (int c = 0; c < 3 && mi < sizeof(missing) - 1; ++c)
                if (!present[L][c]) missing[mi++] = kChan[c];
            missing[mi] = '\0';
            written = std::snprintf(
                out + off, cap - off,
                "- %s only exposes %d/3 channels (missing %s) -- "
                "the missing channel(s) stay at the asset's baked "
                "value\n",
                kFamilyName[L], n, missing);
        }
        if (written > 0)
            off += static_cast<std::size_t>(written);
        if (off >= cap - 1) break;
    }
    return any;
}

// Set by the including TU before draw_overlay_content runs.
// true  = standalone overlay (wider layout for high-DPI)
// false = ReShade addon tab  (compact layout)
// Cannot use FontGlobalScale to detect this because ReShade at 4K
// also sets FontGlobalScale > 1.
static bool s_standaloneMode = false;

// Session-only UI-scale multiplier applied on top of the
// init-time auto-DPI scale (see dx_overlay.cpp). 1.0 = no
// override; user picks 1.25/1.5/etc. from the header combo.
// Only used in the standalone overlay; ReShade has its own
// scaling. Not persisted -- re-pick on next launch.
static float s_uiScale = 1.0f;

// Minimum time (ms) the cursor must rest on a picker item before
// hover-apply fires.  Prevents rapid apply cycles while scrolling.
static constexpr int64_t k_hoverDebounceMs = 300;

// --- Overlay preferences (render-thread only) ---

// Instant Apply mode: applies transmog immediately on hover, pick,
// slot toggle, and clear -- no Apply All click needed.  Off by default
// because each action triggers a tear-down + SlotPopulator cycle.
// Prefab-mode (cross-slot prefab browser) does NOT honour this --
// hover-apply on prefabs needs additional carrier-borrow plumbing
// that's not yet shipped. Click-to-pick still works in prefab mode.
static bool s_autoApply = false;

// When true, preserve the search text between picker opens so the
// user can re-open the same slot and keep browsing where they left
// off.  Off by default to match legacy behaviour (clear on open).
static bool s_keepSearchText = true;

// --- Per-slot UI state ---

// Dye picker region UI state: per-submesh collapse + per-layer
// R/G/B link toggle. Each unique submesh stable_id has its own
// entry in the slot's regionUI map.
struct RegionUIState
{
    bool collapsed = false;
    // Per-layer "link R/G/B" toggle. Index 0..4 =
    // tint/mask/detail/hair/scratch. When true (default), editing any
    // of the three R/G/B-suffix swatches in this layer cascades to
    // the other two; mirrors merchant "Pure X" semantics. Untick to
    // split into independent per-channel control.
    std::array<bool, 5> linkRgb{{true, true, true, true, true}};
};

struct SlotUIState
{
    char hexBuf[8]{"0000"};
    bool editing = false;
    char searchBuf[64]{};
    // "Exact" means: only list items whose auto-detected category
    // matches THIS slot.  Defaults to ON so the dropdown shows ~350
    // relevant items instead of all 6024.
    bool exactFilter = true;
    bool hideIncompatible = true;   // hide crash-risk + non-equipment
    bool hideVariants = false;      // hide NPC variants (carrier items)
    bool hideBodyMismatch = true;   // hide items whose body type doesn't
                                    // match the active character's
    // Prefab-only mode. When true the picker hides the items list and
    // its filters and shows ALL body-mesh prefabs across every slot,
    // labeled with their native slot. Picking a prefab applies it to
    // the prefab's native slot (not necessarily the popup slot) so the
    // user can browse the full prefab catalog from any slot's picker.
    bool prefabMode = false;
    // Prefab-mode-only Exact filter: when true, the prefab-mode list
    // only shows prefabs whose derived slot (slot_for_prefab_name)
    // matches the popup's slot category. Defaults true so opening
    // Helm's picker in prefab mode shows only Helm-family prefabs;
    // user can untick to see the full cross-slot catalog.
    bool prefabExactFilter = true;
    // Hover-apply debounce state.  We track which item the cursor is
    // on and when it first landed there.  Apply fires only after the
    // cursor has settled on the same item for k_hoverDebounceMs,
    // preventing rapid-fire apply cycles when scrolling the list.
    uint16_t hoverPendingId = 0;
    uint16_t hoverAppliedId = 0;
    int64_t hoverStartMs = 0;
    // Button-driven navigation index into the visible (filtered)
    // list.  -1 = no highlight.  Up/Down buttons move this; Enter
    // commits the highlighted item.  lastVisibleCount stores the
    // previous frame's visible count for clamping.
    int navIndex = -1;
    int lastVisibleCount = 0;
    bool navMoved{false}; // true only on the frame a nav button was pressed
    // When the user picks a prefab from this slot's picker, we surface
    // the prefab name on the slot button so the UI shows
    // "[prefab] cd_nhw_no_ub_20027" instead of the carrier item's
    // display name. Empty string == no prefab picked. Session-only
    // (matches prefab-wrapper-swap selections; no JSON persistence).
    std::string pickedPrefabName;

    // Snapshot of the slot's carrier state BEFORE the first prefab
    // pick auto-borrowed Kliff's plate item. Captured on first pick
    // (priorCarrierSaved -> true) and restored when the user picks
    // the "(no body-mesh override)" entry to clear the prefab.
    // Restoring the original Wellsknight (or whatever the user's
    // active preset had) lets the slot revert visually instead of
    // staying stuck on the Kliff plate carrier after a clear.
    bool     priorCarrierSaved   = false;
    bool     priorCarrierActive  = false;
    uint16_t priorCarrierItemId  = 0;

    // Dye UI: per-region collapse state + per-layer R/G/B link
    // toggles, keyed by submesh stable_id. Session-only; reset on
    // slot-target change. `std::map` for stable iteration without
    // hashing a u64 we don't own.
    std::map<std::uint64_t, RegionUIState> regionUI{};

    // Dye UI: "Show only modified" filter. When true, the picker
    // hides clusters / regions whose rows are all at their captured
    // engine default (`override_active == false`). Lets users with
    // many swatches focus on what they've already touched. Session-
    // only -- the filter is a viewing preference, not preset state.
    bool showOnlyModified = false;
};

static SlotUIState s_slotUI[Transmog::k_slotCount]{};

// --- Item picker helpers ---

// Case-insensitive substring search.  Returns true if `needle` is
// empty or found anywhere in `hay`.
static bool name_contains_ci(const std::string &hay, const char *needle) noexcept
{
    if (!needle || needle[0] == '\0')
        return true;
    const auto nlen = std::strlen(needle);
    if (nlen > hay.size())
        return false;
    for (std::size_t i = 0; i + nlen <= hay.size(); ++i)
    {
        std::size_t k = 0;
        for (; k < nlen; ++k)
        {
            const auto a = static_cast<unsigned char>(
                std::tolower(static_cast<unsigned char>(hay[i + k])));
            const auto b = static_cast<unsigned char>(
                std::tolower(static_cast<unsigned char>(needle[k])));
            if (a != b)
                break;
        }
        if (k == nlen)
            return true;
    }
    return false;
}

// Draw a search-filterable picker popup for one slot.  Returns true
// if the user committed a selection this frame (caller is responsible
// for persisting it).  When autoApply is true, hovering an item
// starts a debounce timer; once it expires the slot-scoped apply
// fires via manual_apply_slot so only the hovered slot re-equips.
[[nodiscard]] static bool draw_item_picker_popup(const char *popupId,
                                   SlotUIState &ui,
                                   Transmog::TransmogSlot slotCategory,
                                   uint16_t &targetItemId,
                                   bool autoApply,
                                   std::size_t slotIdx,
                                   int *outPrefabIdx = nullptr)
{
    // outPrefabIdx contract:
    //   nullptr  : caller doesn't care about prefab picks (legacy path).
    //   non-null : initialized to -1 by the caller; set to a body-mesh
    //              catalog index (>= 0) when the user picks a prefab in
    //              the popup. The function also clears targetItemId on
    //              prefab pick so the caller's existing carrier-clear
    //              path runs naturally; -1 on commit means a real item
    //              (or "(none)") was picked, in which case the caller
    //              should clear any prior body-mesh selection so the
    //              two states stay mutually exclusive.
    bool committed = false;
    if (!ImGui::BeginPopup(popupId))
        return false;

    // Reset hover-debounce state on first frame so stale state from
    // a previous open doesn't suppress or prematurely fire an apply.
    if (ImGui::IsWindowAppearing())
    {
        ui.hoverPendingId = targetItemId;
        ui.hoverAppliedId = targetItemId;
        ui.hoverStartMs = 0;
    }

    ui_text_disabled("Item picker");
    ImGui::Separator();

    // --- Filter toggles ---
    //
    // Exact: only show items whose auto-detected category matches
    //        this slot (helm-suffixed items for the helm slot, etc.)
    // Hide variants: hide items flagged by has_variant_meta() (see
    //        item_name_table.cpp::k_descVariantMetaOffset for the
    //        per-version descriptor offset); all tested samples in
    //        that bucket failed to render via runtime transmog, so
    //        hiding them by default keeps the picker free of
    //        non-functional items.
    // Body-aware filters only matter for the 5 armor slots (Helm/Chest/
    // Cloak/Gloves/Boots) where the body-mesh family is gender-specific
    // (cd_phm_* vs cd_phw_*). Accessory + weapon slots share their
    // mesh families across genders, so the filter has no effect there
    // and the checkboxes just create confusion -- hide them.
    const bool isArmorSlotPickerHeader =
        Transmog::slot_meta(slotCategory).partShowHashKey != nullptr;
    if (!ui.prefabMode)
    {
        ImGui::Checkbox("Exact", &ui.exactFilter);
        if (isArmorSlotPickerHeader)
        {
            ImGui::SameLine();
            ImGui::Checkbox("Safe only", &ui.hideIncompatible);
            ImGui::SameLine();
            ImGui::Checkbox("Hide variants", &ui.hideVariants);
            ImGui::SameLine();
            ImGui::Checkbox("Hide cross-body", &ui.hideBodyMismatch);
            if (ImGui::IsItemHovered())
                ui_tooltip("Hide items whose body type (male/female) "
                           "doesn't match the active character. Cross-body "
                           "items may render with broken meshes.");
        }
    }
    else
    {
        ui_text_disabled("Prefabs mode (cross-slot)");
        ImGui::SameLine();
        ImGui::Checkbox("Exact##prefab_exact", &ui.prefabExactFilter);
        if (ImGui::IsItemHovered())
            ui_tooltip(
                "Limit the list to prefabs whose body-mesh family "
                "matches this slot. Untick to browse the full cross-"
                "slot catalog.");
    }

    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::IsWindowAppearing())
    {
        ImGui::SetKeyboardFocusHere();
        ui.navIndex = -1;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ImGui::GetStyle().FramePadding.x, 6.0f));
    const bool searchEdited = ImGui::InputTextWithHint(
        "##search", "Search by name or id...",
        ui.searchBuf, sizeof(ui.searchBuf));
    ImGui::PopStyleVar();
    if (searchEdited)
        ui.navIndex = 0;

    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##search"))
    {
        ui.searchBuf[0] = '\0';
        ui.navIndex = 0;
    }

    // Navigation buttons: move the highlight through the visible
    // list.  Placed next to the search bar so they're always
    // reachable without scrolling.  Enter commits the highlighted
    // item.
    ImGui::SameLine();
    ui.navMoved = false;
    {
        const int maxNav = ui.lastVisibleCount - 1;
        if (ImGui::SmallButton("^##nav_up"))
        {
            ui.navIndex = (ui.navIndex > 0) ? ui.navIndex - 1 : 0;
            ui.navMoved = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("v##nav_down"))
        {
            ui.navIndex = (ui.navIndex < maxNav)
                              ? ui.navIndex + 1
                              : (maxNav >= 0 ? maxNav : 0);
            ui.navMoved = true;
        }
    }
    // Prefabs toggle: only meaningful for slots that have a body-mesh
    // carrier (default source from k_defaultSrcByEnumSlot OR an INI
    // Pair_N_Source). Accessory slots without a mesh carrier
    // (Earring1/2, Necklace, Ring1/2) get -1 from selection_src_index
    // so the toggle is hidden -- there's no point browsing prefabs
    // from a slot that can never receive one. Force prefabMode off
    // for those slots so a pre-existing true value doesn't blank
    // the popup.
    {
        const bool slotHasCarrier =
            (Transmog::PrefabWrapperSwap::selection_src_index(
                 slotCategory) >= 0);
        if (slotHasCarrier)
        {
            ImGui::SameLine();
            ImGui::Checkbox("Prefabs##picker_prefab_mode",
                            &ui.prefabMode);
            if (ImGui::IsItemHovered())
                ui_tooltip("Browse all body-mesh prefabs across every "
                           "slot. The search bar filters across the "
                           "merged prefab list. Picking applies to the "
                           "prefab's native slot.");
        }
        else if (ui.prefabMode)
        {
            ui.prefabMode = false;
        }
    }

    const auto &table = Transmog::ItemNameTable::instance();
    const auto &entries = table.sorted_entries();

    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    const float lineH = ImGui::GetTextLineHeight();
    // Two-line row: display name + smaller internal name line.
    const float twoLineH = lineH * 2.0f + 4.0f;

    // Fixed-height scrollable region so the popup doesn't resize
    // per keystroke as the filter narrows.
    //
    // Width is content-derived in standalone mode: ~60 'M' glyphs
    // at the current font is enough for the longest item names
    // without leaving a wide empty band. The previous DPI-linear
    // value scaled past 1800px on 4K screens and dominated the
    // row. Reshade mode keeps a fixed 560px because its host UI
    // already constrains the parent window.
    const float popupW = s_standaloneMode
        ? (ImGui::CalcTextSize("M").x * 60.0f) : 560.0f;
    ImGui::BeginChild("##itemlist",
                      ImVec2(popupW, twoLineH * 12.0f),
                      true);

    // Increase vertical spacing between items for easier click/hover
    // targets.  Pushed INSIDE BeginChild so the popup-level spacing
    // stays default -- otherwise the popup's own scroll region may
    // capture mouse wheel events instead of the child.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x,
                               rowH * 0.35f));

    // "None" / clear entry at the top of the scroll region.
    // Hover-preview shows bare head/body (empty slot) via the
    // active+none path in apply_single_slot. Suppressed in
    // prefabMode where the popup is a prefab-browser only.
    if (!ui.prefabMode)
    {
        const bool selected = (targetItemId == 0);
        if (ImGui::Selectable("(none -- id 0)##picker_none", selected))
        {
            targetItemId = 0;
            committed = true;
            ImGui::CloseCurrentPopup();
        }
        if (selected)
            ImGui::SetItemDefaultFocus();
        if (autoApply && ImGui::IsItemHovered() &&
            ui.hoverPendingId != 0)
        {
            ui.hoverPendingId = 0;
            ui.hoverStartMs = Transmog::steady_ms();
        }
    }

    // Hoist draw-list pointer outside the loop -- it is stable for
    // the entire BeginChild region and avoids a per-item lookup.
    ImDrawList *const dl = ImGui::GetWindowDrawList();

    // Semantic color constants for the two-line item display.
    static constexpr ImU32 k_colorCrashRisk = IM_COL32(255, 89, 89, 255);
    static constexpr ImU32 k_colorCarrier = IM_COL32(128, 217, 255, 255);
    // Orange -- carrier path works but body type mismatched. Item
    // will equip and may display with minor mesh artifacts; we show
    // it when the user disables "Hide cross-body".
    static constexpr ImU32 k_colorCrossBody = IM_COL32(255, 176, 64, 255);
    // Amber -- item has humanoid-range classifier tokens but none are
    // in the male/female body set (e.g. NPC variants like
    // Antumbra/Badran gloves with only `0x012F`). Equip goes through
    // the carrier path but render fidelity is inconsistent -- some
    // items resolve correctly, others show as broken meshes.
    static constexpr ImU32 k_colorAmbiguous = IM_COL32(230, 210, 120, 255);
    static constexpr ImU32 k_colorDimmed = IM_COL32(160, 160, 160, 200);

    // Active character's body kind (CE-verified 2026-04-21). Stable
    // for the entire popup invocation: PresetManager state cannot
    // mutate between picker frames on the render thread, so resolving
    // it once here avoids ~6k redundant PresetManager + string
    // comparisons per frame inside the filter loop. Armor rule
    // classifier tokens partition the catalog into disjoint body
    // families:
    //   Male:   {0x0018, 0x0058, 0x02E3}
    //   Female: {0x0072, 0x0382, 0x0300}
    //   Horse / pet / wagon / dragon: separate token pools with
    //     zero overlap with humanoid. Flagged as NonHumanoid and
    //     hidden unconditionally -- these never render on a human
    //     skeleton (horse saddles, cat backpacks, etc.).
    using BK = Transmog::ItemNameTable::BodyKind;
    BK charBody;
    {
        const auto &pm = Transmog::PresetManager::instance();
        // The picker filter follows the editing character: the user
        // is populating that character's preset and wants to see
        // items from that body's catalog. Falls back to the
        // controlled character automatically when editing is
        // unpinned because the two axes are identical in that state.
        const std::string ov =
            pm.body_kind_of(pm.editing_character());
        if (ov == "Male")
            charBody = BK::Male;
        else if (ov == "Female")
            charBody = BK::Female;
        else if (ov == "Both")
            charBody = BK::Both;
        else // "Auto" or unrecognised
            charBody =
                Transmog::ItemNameTable::body_kind_for_character(
                    pm.editing_character());
    }

    // Two-pass filter+draw. Pass 1 collects matching catalog indices;
    // pass 2 renders. The split lets the post-loop nav-clamp see the
    // full visible count without rescanning, and supports unbounded
    // result sets (the catalog has more chest items than any single
    // hardcoded cap could safely truncate).
    //
    // Do NOT replace this with ImGuiListClipper: reshade_overlay.hpp
    // (used by overlay_reshade.cpp) does not provide inline thunks for
    // ImGuiListClipper::Begin/End/Step. Adding any such reference pulls
    // imgui_lib's real symbols into the link, which collides with every
    // COMDAT ImGui:: thunk in overlay_reshade.obj (LNK2005 cascade).
    // Any new ImGui:: symbol added in this file must already exist as
    // an inline thunk in external/reshade-sdk/include/reshade_overlay.hpp.
    // ~6k Selectables per popup frame is acceptable in measurement.
    //
    // s_filtered is thread_local + static so the storage is reused
    // across frames without per-frame allocation. Safe because only
    // one picker popup can be open at a time (single call site, no
    // nested-popup re-entry path exists).
    static thread_local std::vector<std::size_t> s_filtered;
    s_filtered.clear();
    s_filtered.reserve(entries.size());
    int filteredByCategory = 0;
    int filteredByUnsafe = 0;
    int shown = 0;
    // Body-aware filters (Safe / Hide variants / Hide cross-body) only
    // apply to the 5 armor slots (Helm/Chest/Cloak/Gloves/Boots) where
    // the body-mesh family is gender-specific (cd_phm_* vs cd_phw_*).
    // Earrings/Necklace/Rings/Lantern/Glasses/Mask/Backpack/Bracelet
    // and weapons share their mesh families across genders, so the
    // gender / body-kind filter just hides perfectly-good options.
    // partShowHashKey is non-null exactly for the 5 armor slots in
    // slot_metadata.hpp -- reuse that table as the gate.
    const bool isArmorSlot =
        Transmog::slot_meta(slotCategory).partShowHashKey != nullptr;

    if (!ui.prefabMode)
    {
    for (std::size_t idx = 0; idx < entries.size(); ++idx)
    {
        const auto &e = entries[idx];
        // Exact filter accepts the slot itself OR its picker partner
        // (Earring1/2, Ring1/2, MainHand/OffHand share items because
        // their descriptor typeCodes are identical -- see
        // ItemNameTable::category_of which returns the lower-indexed
        // half for these pairs).
        if (ui.exactFilter &&
            !Transmog::slots_share_picker(e.category, slotCategory))
        {
            ++filteredByCategory;
            continue;
        }
        const bool nonHumanoid = (e.bodyKind == BK::NonHumanoid);
        const bool incompatible =
            (e.category == Transmog::TransmogSlot::Count) || nonHumanoid;
        if (isArmorSlot && ui.hideIncompatible && incompatible)
        {
            ++filteredByUnsafe;
            continue;
        }
        const bool ambiguousBody = (e.bodyKind == BK::Ambiguous);
        const bool bodyMatches =
            !nonHumanoid && (
                ambiguousBody ||
                (e.bodyKind == BK::Generic) ||
                (e.bodyKind == BK::Both) ||
                (charBody == BK::Generic) ||
                (e.bodyKind == charBody));
        if (isArmorSlot && ui.hideBodyMismatch && !bodyMatches)
        {
            ++filteredByUnsafe;
            continue;
        }
        if (isArmorSlot && ui.hideVariants && e.hasVariantMeta)
        {
            ++filteredByUnsafe;
            continue;
        }
        if (!name_contains_ci(e.name, ui.searchBuf) &&
            !name_contains_ci(e.displayName, ui.searchBuf))
            continue;
        s_filtered.push_back(idx);
    }

    shown = static_cast<int>(s_filtered.size());

    // Pass 2: render the filtered rows. bodyMatches is recomputed
    // here rather than cached from pass 1 because the per-item flag
    // also drives the colour tier and the carrier/crash-risk tag --
    // recomputation is cheaper than a parallel side-vector allocation.
    for (int row = 0; row < shown; ++row)
    {
        const auto &e = entries[s_filtered[row]];
        const bool nonHumanoid = (e.bodyKind == BK::NonHumanoid);
        const bool ambiguousBody = (e.bodyKind == BK::Ambiguous);
        // Body-mesh family is gender-specific only for the 5 armor
        // slots (cd_phm_* vs cd_phw_*). Accessory + weapon slots share
        // their meshes across genders, so a "BODY MISMATCH" or
        // "UNCERTAIN BODY" tag would be misleading. Treat every entry
        // as body-matching for non-armor slots so the colour/tag flow
        // below collapses to plain `usesCarrier` (cyan) or untagged.
        const bool bodyMatches =
            !isArmorSlot ||
            (!nonHumanoid && (
                ambiguousBody ||
                (e.bodyKind == BK::Generic) ||
                (e.bodyKind == BK::Both) ||
                (charBody == BK::Generic) ||
                (e.bodyKind == charBody)));

        // Tag items with visible badges:
        //   - "CRASH RISK"  -> !isPlayerCompatible (red)
        //   - "carrier"     -> hasVariantMeta, rendered via carrier
        //                      + char-class bypass (cyan)
        const char *tag = nullptr;
        // NPC variant items (hasVariantMeta) are humanoid and now
        // render via carrier + char-class bypass.  True crash risks
        // are non-player items WITHOUT variant meta (horse tack, etc).
        const bool usesCarrier = e.hasVariantMeta;
        // Colour tiers:
        //   red    CRASH RISK         -- rule list rejects this body,
        //                               no carrier path rescue.
        //   orange BODY MISMATCH      -- cross-body carrier, will equip
        //                               but mesh likely breaks.
        //   amber  UNCERTAIN BODY     -- humanoid-range but no body-
        //                               set match (e.g. 0x012F-only
        //                               NPC variants); render may or
        //                               may not resolve correctly.
        //   blue   carrier            -- NPC variant, same-body match,
        //                               reliable carrier path.
        const bool crashRisk = !bodyMatches && !e.hasVariantMeta;
        const bool crossBodyCarrier = !bodyMatches && e.hasVariantMeta;
        // Only flag ambiguous when it's actually a carrier item and
        // not already bucketed as cross-body (it isn't -- ambiguous
        // passes bodyMatches above). Suppressed entirely on non-armor
        // slots where body kind is irrelevant.
        const bool ambiguousCarrier =
            isArmorSlot && ambiguousBody && e.hasVariantMeta;
        if (crashRisk)
            tag = "non-player -- CRASH RISK";
        else if (crossBodyCarrier)
            tag = "carrier -- BODY MISMATCH";
        else if (ambiguousCarrier)
            tag = "carrier -- UNCERTAIN BODY";
        else if (usesCarrier)
            tag = "carrier";

        const bool isNavTarget = (row == ui.navIndex);
        const bool highlighted =
            (targetItemId == e.id) || isNavTarget;

        // Two-line selectable: hidden label, custom text overlay.
        char hiddenId[32];
        std::snprintf(hiddenId, sizeof(hiddenId),
                      "##picker_%04X", e.id);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        if (crashRisk)
            ImGui::PushStyleColor(ImGuiCol_Header,
                                  ImVec4(0.4f, 0.1f, 0.1f, 0.6f));
        else if (crossBodyCarrier)
            ImGui::PushStyleColor(ImGuiCol_Header,
                                  ImVec4(0.45f, 0.28f, 0.08f, 0.6f));
        else if (ambiguousCarrier)
            ImGui::PushStyleColor(ImGuiCol_Header,
                                  ImVec4(0.38f, 0.32f, 0.12f, 0.6f));
        else if (usesCarrier)
            ImGui::PushStyleColor(ImGuiCol_Header,
                                  ImVec4(0.1f, 0.2f, 0.35f, 0.6f));
        if (ImGui::Selectable(hiddenId, highlighted,
                              0, ImVec2(0, twoLineH)))
        {
            targetItemId = e.id;
            committed = true;
            ImGui::CloseCurrentPopup();
        }

        // Overlay text on top of the selectable region.
        const bool hasDisplay = !e.displayName.empty();
        const char *primaryName =
            hasDisplay ? e.displayName.c_str() : e.name.c_str();

        // Line 1: display name (or internal name fallback) + tag.
        char line1[256];
        if (tag)
            std::snprintf(line1, sizeof(line1), "%s  (%s)",
                          primaryName, tag);
        else
            std::snprintf(line1, sizeof(line1), "%s", primaryName);

        const ImU32 mainColor = crashRisk          ? k_colorCrashRisk
                                : crossBodyCarrier ? k_colorCrossBody
                                : ambiguousCarrier ? k_colorAmbiguous
                                : usesCarrier      ? k_colorCarrier
                                : ImGui::GetColorU32(ImGuiCol_Text);

        dl->AddText(ImVec2(pos.x + 2.0f, pos.y + 1.0f),
                    mainColor, line1);

        // Line 2: internal name + hex id (dimmed).
        char line2[256];
        if (hasDisplay)
            std::snprintf(line2, sizeof(line2),
                          "  %s  [0x%04X]", e.name.c_str(), e.id);
        else
            std::snprintf(line2, sizeof(line2),
                          "  [0x%04X]", e.id);
        dl->AddText(ImVec2(pos.x + 2.0f, pos.y + lineH + 2.0f),
                    k_colorDimmed, line2);

        // Scroll the nav-highlighted row into view and handle
        // Enter to commit.
        if (isNavTarget)
        {
            if (ui.navMoved)
                ImGui::SetScrollHereY();
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
            {
                targetItemId = e.id;
                committed = true;
                ImGui::CloseCurrentPopup();
            }
            // Feed nav target into hover-apply debounce so
            // button navigation previews items when auto-apply
            // is on.
            if (autoApply && ui.hoverPendingId != e.id)
            {
                ui.hoverPendingId = e.id;
                ui.hoverStartMs = Transmog::steady_ms();
            }
        }
        else if (targetItemId == e.id)
        {
            // Scroll to the currently selected item on popup
            // open so it's visible without manual scrolling.
            ImGui::SetItemDefaultFocus();
        }

        // Live preview: record the mouse-hovered item and start
        // the debounce timer.  Sync nav cursor to mouse so the
        // highlight follows the cursor.
        if (autoApply && ImGui::IsItemHovered() &&
            ui.hoverPendingId != e.id)
        {
            ui.hoverPendingId = e.id;
            ui.hoverStartMs = Transmog::steady_ms();
            ui.navIndex = row;
        }

        if (crashRisk || crossBodyCarrier || ambiguousCarrier || usesCarrier)
            ImGui::PopStyleColor(); // Header color
    }

    if (shown == 0)
    {
        if (ui.exactFilter && filteredByCategory > 0)
            ui_text_disabled("no matches in this category -- "
                            "uncheck Exact to widen");
        else if ((ui.hideIncompatible || ui.hideVariants ||
                  ui.hideBodyMismatch) &&
                 filteredByUnsafe > 0)
            ui_text_disabled("no matches -- uncheck filters "
                            "to show more items");
        else
            ui_text_disabled("no matches");
    }
    } // if (!ui.prefabMode)

    // --- Body-Mesh Prefab section ---
    //
    // Mirrors the carrier picker but for raw body-mesh prefabs. Picking
    // an entry here is equivalent to setting a body-mesh override on
    // the slot: the engine still equips the player's actual carrier
    // item, but the body-mesh hook substitutes the wrapper at apply
    // time so the visible mesh becomes the chosen prefab. The source
    // wrapper is auto-derived from the slot's INI source pair (the
    // Kliff carrier defaults). When outPrefabIdx is null the caller
    // doesn't want this section -- skip it so legacy callers see no
    // change.
    if (outPrefabIdx != nullptr)
    {
        namespace PWS = Transmog::PrefabWrapperSwap;

        ImGui::Separator();
        if (ui.prefabMode)
        {
            // --- Cross-slot prefab browser ---
            //
            // Walks every TransmogSlot's catalog, applies the search
            // filter, and labels each entry with its native slot.
            // Clicking applies the prefab to its NATIVE slot (not the
            // popup slot) -- the popup is just an entry point for
            // browsing the merged catalog. This way the user can pick
            // any prefab from any slot's picker.
            ui_text_disabled(
                "All Prefabs (search filters across every slot)");

            // After populate_slot_catalogs unified the per-slot vectors
            // (every slot now holds the FULL prefab set), iterating
            // every slot would push each prefab 20x with a meaningless
            // [Type] tag. Iterate slot 0 only and derive the visual
            // [Type] label per row from the prefab name's prefix
            // (slot_metadata's prefabPrefixMale/Female columns).
            //
            // Pass 1 builds a flat index of catalog positions matching
            // the search filter; pass 2 manually clips to the visible
            // scroll window (see manual virtualization comment below).
            const auto &cat0 = PWS::slot_catalog(
                static_cast<Transmog::TransmogSlot>(0));
            const std::size_t catalogedTotal = cat0.size();

            static thread_local std::vector<std::uint32_t> s_prefabFlat;
            s_prefabFlat.clear();
            for (std::size_t pi = 0; pi < cat0.size(); ++pi)
            {
                if (!name_contains_ci(cat0[pi].name, ui.searchBuf))
                    continue;
                if (ui.prefabExactFilter)
                {
                    auto derived = Transmog::slot_for_prefab_name(
                        cat0[pi].name);
                    if (!derived.has_value() ||
                        !Transmog::slots_share_prefab_family(
                            *derived, slotCategory))
                        continue;
                }
                s_prefabFlat.push_back(
                    static_cast<std::uint32_t>(pi));
            }
            const std::size_t totalShown = s_prefabFlat.size();

            // Derive a slot label from a prefab name. Substring-based
            // via slot_for_prefab_name() so NPC families (cd_nhw_*,
            // cd_nhm_*), other player roles (cd_pdm_*, cd_pgm_*, ...)
            // and accessory families (cd_t0000_*) all classify
            // correctly. See slot_metadata.hpp for the priority table.
            auto label_for_prefab = [&](const std::string &name) -> const char * {
                if (auto slot = Transmog::slot_for_prefab_name(name))
                    return Transmog::slot_meta(*slot).displayName;
                return "?";
            };

            // Manual virtualization (ImGuiListClipper symbol clashes
            // between imgui_lib and the ReShade SDK headers when this
            // .inl is compiled into both TUs). Skip off-screen rows by
            // emitting a top/bottom Dummy spacer of the right height,
            // and only Selectable() the visible window. Net cost per
            // frame: O(visible_rows) instead of O(totalShown). For the
            // observed lag this drops from ~5000 emits to ~30.
            const float prefabRowH = ImGui::GetTextLineHeightWithSpacing();
            const float scrollY = ImGui::GetScrollY();
            const float winH = ImGui::GetWindowHeight();
            const int total = static_cast<int>(totalShown);
            int firstVis = static_cast<int>(scrollY / prefabRowH) - 1;
            int lastVis = static_cast<int>(
                (scrollY + winH) / prefabRowH) + 2;
            if (firstVis < 0) firstVis = 0;
            if (lastVis > total) lastVis = total;
            if (firstVis > total) firstVis = total;

            if (firstVis > 0)
                ImGui::Dummy(ImVec2(0.0f, firstVis * prefabRowH));
            {
                for (int row = firstVis; row < lastVis; ++row)
                {
                    const auto pi = static_cast<std::size_t>(
                        s_prefabFlat[static_cast<std::size_t>(row)]);
                    const auto &pe = cat0[pi];
                    const char *natName = label_for_prefab(pe.name);
                    char pickerId[224];
                    if (pe.is_loaded)
                    {
                        std::snprintf(
                            pickerId, sizeof(pickerId),
                            "[%s] %s##all_prefab_%zu",
                            natName, pe.name.c_str(), pi);
                    }
                    else
                    {
                        std::snprintf(
                            pickerId, sizeof(pickerId),
                            "[%s] %s  (unloaded)##all_prefab_%zu",
                            natName, pe.name.c_str(), pi);
                    }
                    if (ImGui::Selectable(pickerId))
                    {
                        if (!pe.is_loaded)
                            continue;
                        // Per-slot catalogs were identical right after
                        // populate_slot_catalogs seeded them from the
                        // shared StringInfo walk, but
                        // enumerate_loader_registry_into_catalog adds
                        // slot-specific NPC entries (helm-only to Helm,
                        // chest-only to Chest, etc.) and re-sorts each
                        // slot independently. Indexing a popup-slot
                        // selection by cat0's pi therefore points at a
                        // DIFFERENT prefab in the popup slot's catalog
                        // and the row label drifts off the row the user
                        // actually clicked. Resolve by name through
                        // adopt_into_slot_and_select, which finds the
                        // matching entry in slotCategory's catalog (or
                        // inserts a copy when the prefab is unique to
                        // cat0) and returns the index that is valid for
                        // slotCategory specifically.
                        const auto adoptedIdx =
                            PWS::adopt_into_slot_and_select(
                                slotCategory,
                                static_cast<Transmog::TransmogSlot>(0),
                                static_cast<int>(pi));
                        if (adoptedIdx < 0)
                            continue;
                        // Tell the caller a prefab was picked on the
                        // popup slot so its existing
                        // pickedPrefabName / carrier-borrow path runs.
                        if (outPrefabIdx)
                            *outPrefabIdx = adoptedIdx;
                        committed = true;
                        DMK::Logger::get_instance().info(
                            "[picker] prefabs-mode pick: popupSlot={} "
                            "label={} adoptedIdx={} name='{}'",
                            Transmog::slot_name(slotCategory),
                            natName, adoptedIdx, pe.name.c_str());
                        ImGui::CloseCurrentPopup();
                    }
                    // Instant Apply intentionally NOT wired for prefab
                    // mode this release -- click-to-pick remains the
                    // only commit path here. Hover-apply needs extra
                    // carrier-borrow plumbing that's a follow-up.
                }
            }
            if (lastVis < total)
                ImGui::Dummy(ImVec2(0.0f, (total - lastVis) * prefabRowH));
            if (catalogedTotal == 0)
            {
                ui_text_disabled(
                    "no prefabs cataloged yet -- catalog may still "
                    "be populating, try Refresh");
            }
            else if (totalShown == 0)
            {
                ui_text_disabled("no matches");
            }
        }
        else
        {
            // Prefab catalog rendering is gated to prefabMode=true
            // (the cross-slot browser block above). The per-slot prefab
            // list otherwise bloats the items dropdown, so when
            // prefabMode is OFF we render no prefab rows; only the
            // clear-override option appears when an active prefab
            // selection exists, so it can be undone without flipping
            // the prefabMode toggle on first.
            const int curTgt = PWS::selection_tgt_index(slotCategory);
            if (curTgt >= 0)
            {
                char clrLabel[64];
                std::snprintf(clrLabel, sizeof(clrLabel),
                              "(clear active prefab override)##prefab_clr");
                if (ImGui::Selectable(clrLabel))
                {
                    if (outPrefabIdx) *outPrefabIdx = -2;
                    committed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
    }

    // Clamp nav index to the actual visible count so stale values
    // from a wider filter don't point past the end of the list.
    ui.lastVisibleCount = shown;
    if (ui.navIndex >= shown)
        ui.navIndex = shown > 0 ? shown - 1 : -1;

    ImGui::PopStyleVar(); // ItemSpacing
    ImGui::EndChild();

    // --- Hover-apply debounce ---
    // Fire manual_apply_slot() only after the cursor has rested on
    // the same item for k_hoverDebounceMs.  Uses the slot-scoped
    // apply path so only this slot re-equips -- other slots are
    // untouched and don't flicker.
    if (autoApply &&
        ui.hoverPendingId != ui.hoverAppliedId &&
        ui.hoverStartMs != 0 &&
        (Transmog::steady_ms() - ui.hoverStartMs) >= k_hoverDebounceMs)
    {
        targetItemId = ui.hoverPendingId;
        ui.hoverAppliedId = ui.hoverPendingId;
        Transmog::flag_enabled().store(true, std::memory_order_relaxed);
        Transmog::manual_apply_slot(slotIdx);
    }

    ImGui::EndPopup();

    return committed;
}

// --- Preset UI state ---

static char s_newCharName[64]{};
static char s_renamePresetBuf[64]{};
static bool s_renameActive = false;
static int s_renameIndex = -1;

// Compare staged slot_mappings vs last_applied_ids to detect unsaved
// picker/checkbox edits.  Returns true if ANY slot's effective target
// (targetItemId when active, else 0) differs from what's currently
// committed to the game.
[[nodiscard]] static bool has_pending_changes() noexcept
{
    const auto &mappings = Transmog::slot_mappings();
    const auto &lastIds = Transmog::last_applied_ids();
    const auto &forceApply = Transmog::force_apply_pending();
    for (std::size_t i = 0; i < Transmog::k_slotCount; ++i)
    {
        // A force-apply flag means the user re-picked something on the
        // same carrier id (e.g. a different body-mesh prefab on the
        // existing 0x1521 helm). The staged/lastIds comparison alone
        // would miss it because the carrier id is unchanged.
        if (forceApply[i])
            return true;
        const uint16_t staged =
            mappings[i].active ? mappings[i].targetItemId : uint16_t{0};
        if (staged != lastIds[i])
            return true;
    }
    return false;
}

// Returns true when slot_mappings differs from the active preset's
// persisted slots -- i.e. the user edited rows in the overlay but has
// not committed the change back into the JSON preset via Save. Used to
// tint the Save button so unsaved work is visible.
//
// Prefab picks are session-only: when one is active, the slot's live
// `mappings[i].targetItemId` was force-borrowed by
// `force_active_character_carrier_for_picked_slots()` to the active
// character's carrier id, NOT the user's saved itemId. The saved
// itemId snapshot is captured into `priorCarrierActive/ItemId` on the
// first prefab pick. Compare THAT against the JSON when a prefab pick
// is in flight, otherwise the Save button stays "Save *" forever even
// when the saved state matches.
[[nodiscard]] static bool has_pending_save() noexcept
{
    namespace PWS = Transmog::PrefabWrapperSwap;
    const auto *p = Transmog::PresetManager::instance().active_preset();
    if (!p)
        return false;

    // Dye is mutated directly on the preset, so slot_mappings cannot
    // signal the divergence. The picker sets dye_dirty on every edit;
    // Save / preset-switch / load clear it.
    if (Transmog::dye_dirty().load(std::memory_order_acquire))
        return true;

    const auto &mappings = Transmog::slot_mappings();
    for (std::size_t i = 0; i < Transmog::k_slotCount; ++i)
    {
        // Disabled slots are forced off in the dispatcher and hidden
        // from the picker; their saved row is left untouched on disk
        // and may diverge from `mappings[i]` (which we forced false).
        // Don't let that divergence light up the Save button.
        if (!Transmog::slot_enabled(i))
            continue;

        const auto tslot = static_cast<Transmog::TransmogSlot>(i);

        // Live prefab name from PWS (source of truth for the session,
        // re-read every frame so picker mutations and preset loads
        // both surface here).
        const int tgtIdx = PWS::selection_tgt_index(tslot);
        std::string stagedPrefabName;
        if (tgtIdx >= 0)
        {
            const auto &cat = PWS::slot_catalog(tslot);
            if (static_cast<std::size_t>(tgtIdx) < cat.size())
                stagedPrefabName = cat[tgtIdx].name;
        }

        // Prefab name mismatch -> pending. Covers both directions:
        // user added/changed a prefab AND user cleared a saved one.
        if (stagedPrefabName != p->slots[i].prefabName)
            return true;

        // When the saved preset row has a prefab, the carrier itemId
        // and active flag are implementation details: save_path clears
        // itemName (so saved itemId=0) while apply_to_state writes
        // mappings[i].targetItemId = auto-borrowed Kairos carrier.
        // Comparing those two would light up Save permanently. Skip
        // the active/itemId compare for prefab rows -- the prefab
        // name match above is the only check that's meaningful here.
        if (!p->slots[i].prefabName.empty())
            continue;

        // Plain carrier slot. If the user has a session-only prefab
        // pick on this slot (saved preset has no prefab, live PWS
        // does), the prefab-name compare above already returned
        // pending -- so we won't reach here. priorCarrierSaved still
        // matters for the inverse: user is mid-pick, hasn't applied,
        // and the snapshot holds the originally-saved carrier.
        const auto &ui = s_slotUI[i];
        const bool prefabBorrow = ui.priorCarrierSaved;
        const bool stagedActive =
            prefabBorrow ? ui.priorCarrierActive : mappings[i].active;
        const std::uint16_t stagedItemId =
            prefabBorrow ? ui.priorCarrierItemId : mappings[i].targetItemId;
        if (stagedActive != p->slots[i].active)
            return true;
        if (stagedItemId != p->slots[i].itemId)
            return true;
    }
    return false;
}

// Clear all picked-prefab UI state and deactivate the body-mesh hook.
// Called BEFORE preset switches so the prefab swap is torn down before
// the new preset's items are equipped -- otherwise the hook would still
// substitute against the old src wrappers, causing weird state across
// the transition.
// Clear UI/PWS selections for slots that had a body-mesh prefab pick,
// and return the set of slot indices that were cleared. The caller is
// responsible for the post-apply_to_state lastIds reconciliation: for
// each cleared slot, if the new preset's carrier equals lastIds[i], the
// caller must zero lastIds[i] so the apply pass tears down the prior
// body-mesh fake (otherwise the natural-pipeline cleanup hook never
// fires and the swap stays visible). When carriers differ, lastIds is
// left intact so the regular tear_down_fake path runs.
static std::array<bool, Transmog::k_slotCount>
clear_all_picked_prefabs_and_deactivate()
{
    namespace PWS = Transmog::PrefabWrapperSwap;
    std::array<bool, Transmog::k_slotCount> hadPick{};
    for (std::size_t i = 0; i < Transmog::k_slotCount; ++i)
    {
        hadPick[i] = !s_slotUI[i].pickedPrefabName.empty();
        s_slotUI[i].pickedPrefabName.clear();
        s_slotUI[i].priorCarrierSaved = false;
        const auto tslot = static_cast<Transmog::TransmogSlot>(i);
        // Clear the body-mesh tgt selection. Source is left intact so
        // future picks reuse the same auto-seeded src.
        const int curSrc = PWS::selection_src_index(tslot);
        PWS::set_selection(tslot, curSrc, -1);
    }
    // Selections only -- the actual swap-map rebuild happens on the
    // next apply via notify_apply_starting (apply-only lifecycle,
    // mirroring the carrier hybrid pattern).
    return hadPick;
}

static void force_active_character_carrier_for_picked_slots()
{
    // For each slot with a picked prefab, force the carrier item to
    // the ACTIVE CHARACTER's carrier (from carrier_defaults.hpp via
    // default_carrier_for_slot). The PWS swap is keyed by the source
    // wrapper that THAT character's body emits, so we need the
    // matching carrier to make the source wrapper resident at apply
    // time. Hardcoding "Kliff" here would force Kliff's plate item
    // onto Damiane / Oongka whenever they had a prefab selection
    // active, breaking their carrier-equip path.
    auto &mappings = Transmog::slot_mappings();
    const auto &activeChar =
        Transmog::PresetManager::instance().active_character();
    for (std::size_t i = 0; i < Transmog::k_slotCount; ++i)
    {
        if (s_slotUI[i].pickedPrefabName.empty())
            continue;
        const auto carrierId = Transmog::default_carrier_for_slot(
            static_cast<Transmog::TransmogSlot>(i), activeChar);
        if (carrierId == 0)
            continue;  // Item catalog isn't ready yet -- skip.
        // Snapshot the slot's prior carrier state on the FIRST pick
        // so a later body-mesh clear can restore it (revert to the
        // user's underlying preset gear instead of leaving Kliff
        // plate stuck on the slot). Subsequent re-picks don't
        // overwrite -- the original state is what we want to revert
        // to, not whatever Kliff plate we last force-borrowed.
        if (!s_slotUI[i].priorCarrierSaved)
        {
            s_slotUI[i].priorCarrierSaved  = true;
            s_slotUI[i].priorCarrierActive = mappings[i].active;
            s_slotUI[i].priorCarrierItemId = mappings[i].targetItemId;
        }
        // Snapshot prior state to detect "did this call actually
        // change anything for this slot". Without the change check
        // below the function fires force_apply_pending on every
        // already-stable slot every time the user re-picks any other
        // slot, causing unrelated slots to tear down + re-apply on
        // each pick.
        const bool prevActive = mappings[i].active;
        const std::uint16_t prevTargetItemId = mappings[i].targetItemId;
        mappings[i].active = true;
        mappings[i].targetItemId = carrierId;
        const bool changed =
            (prevActive != mappings[i].active) ||
            (prevTargetItemId != mappings[i].targetItemId);
        // When the new carrier id equals the last-applied id, the
        // apply pipeline would short-circuit on the `targetId == prevId`
        // early-out and never re-run. Set the per-slot force-apply flag
        // so the dispatcher bypasses that early-out while keeping
        // lastIds[i] intact -- Phase A `tear_down_fake` then runs
        // against the prior carrier and the engine's natural-pipeline
        // hook cleans up the prior body-mesh tgt wrapper. In the OTHER
        // case (lastIds differ) the apply path already runs
        // tear-old-then-equip-new naturally, so the flag is harmless.
        // Gated on `changed` so already-stable slots (carrier was
        // already carrierId, mapping unchanged) don't get force-applied
        // when the user re-picks an unrelated slot.
        auto &lastIds = Transmog::last_applied_ids();
        if (changed && lastIds[i] == carrierId)
            Transmog::force_apply_pending()[i] = true;
    }
}

// --- draw_overlay_content ---
// All ImGui widget calls for the transmog UI.  Called by both the
// ReShade tab callback and the standalone window wrapper.

static void draw_overlay_content()
{
    using namespace Transmog;

    // Standalone-only: apply the user's scale override to the
    // font. Stacks on top of the init-time auto-DPI scale from
    // dx_overlay.cpp. Title bar catches up next frame (drawn in
    // the preceding Begin call); content reflects it immediately.
    if (s_standaloneMode)
        ImGui::GetIO().FontGlobalScale = s_uiScale;

    // Drive every slot's reinit state machine forward. Cheap when
    // every slot is Idle (just atomic loads).
    Transmog::ColorOverride::Reinit::tick();

    auto &pm = PresetManager::instance();
    // When auto-apply is on, picks are applied immediately so
    // there's never a meaningful "pending" state.  Suppress the
    // badge and yellow button tint to reduce visual noise.
    const bool pending =
        !s_autoApply && has_pending_changes();
    const bool pendingSave = has_pending_save();

    // --- Header ---

    ImGui::TextUnformatted(MOD_NAME);
    ImGui::SameLine();
    ui_text_disabled("v%s", MOD_VERSION);

    // Standalone-only UI scale combo. Stacks on the init-time
    // auto-DPI baseline. Session-only -- not persisted.
    if (s_standaloneMode)
    {
        static constexpr float kScales[] = {
            0.5f, 0.75f, 0.85f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
        static constexpr const char *kLabels[] = {
            "0.5x", "0.75x", "0.85x", "1.0x",
            "1.25x", "1.5x", "1.75x", "2.0x" };
        constexpr int kCount =
            static_cast<int>(sizeof(kScales) / sizeof(kScales[0]));
        int sel = 3; // default points at 1.0x
        for (int i = 0; i < kCount; ++i)
            if (s_uiScale == kScales[i]) { sel = i; break; }
        // Separate from the version/mod-title group with a gap +
        // visible "UI Scale" label so users discover the control.
        // Combo width must accommodate the longest label ("1.75x")
        // plus the arrow glyph; scales with font size so the value
        // text isn't clipped at any UI-scale step.
        ImGui::SameLine(0.0f, 24.0f);
        ui_text("UI Scale:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::Combo("##uiScale", &sel, kLabels, kCount))
            s_uiScale = kScales[sel];
        if (ImGui::IsItemHovered())
            ui_tooltip(
                "UI scale (standalone overlay).\n"
                "Stacks on top of the auto-resolution scale.\n"
                "Glyphs may blur at higher values.");
    }

    if (pending)
    {
        ImGui::SameLine();
        ui_text_colored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                        "  [PENDING -- click Apply All]");
    }

    if (pendingSave)
    {
        ImGui::SameLine();
        ui_text_colored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                        "  [UNSAVED -- click Save]");
    }

    ImGui::Separator();

    // --- Global toggles ---

    bool enabled = flag_enabled().load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Enabled", &enabled))
    {
        flag_enabled().store(enabled, std::memory_order_relaxed);
        if (enabled)
            manual_apply();
        else
            manual_clear();
    }

    ImGui::Checkbox("Instant Apply", &s_autoApply);
    if (ImGui::IsItemHovered())
        ui_tooltip("Apply changes immediately on hover, "
                   "pick, toggle, and clear -- no Apply All "
                   "needed");
    ImGui::SameLine();
    ui_text_disabled("(?)");
    if (ImGui::IsItemHovered())
        ui_tooltip("Prefab picker (Prefabs checkbox inside the popup) "
                   "does NOT honour Instant Apply this release. Click "
                   "a prefab row to commit it; hover-preview is items-"
                   "only for now.");

    ImGui::SameLine();
    ImGui::Checkbox("Keep Search Text", &s_keepSearchText);
    if (ImGui::IsItemHovered())
        ui_tooltip("Preserve the search field when "
                   "re-opening a slot picker");

    ImGui::Separator();

    // --- Character picker ---
    //
    // Presets are stored per character (Kliff / Damiane / Oongka).
    // Selecting a character swaps the active preset list to that
    // character's, re-applies its active preset, and clears the
    // drop-detection state so the next apply pass does not confuse
    // the previous character's cached itemIds with the new one's.
    //
    // Auto-detection of the currently-controlled character from a1
    // is deferred: the type-byte at *(actor+0x88)+1 is role-based
    // (controlled vs background), not character-based, and the
    // static WorldSystem → ActorManager → UserActor chain did not
    // update live on control-swap in our 1.03.01 probe. Until that
    // RE lands, the user picks the character manually here.
    {
        // Fixed stack buffer -- the game has a small, closed roster of
        // playable characters (Kliff / Damiane / Oongka at time of
        // writing). k_maxChars is oversized so adding a new playable
        // character later doesn't need a code change here beyond
        // adding its preset in the JSON.
        constexpr std::size_t k_maxChars = 8;
        const auto names = pm.character_names();
        const std::size_t n =
            (names.size() < k_maxChars) ? names.size() : k_maxChars;
        if (n > 0)
        {
            const char *cstrs[k_maxChars]{};
            int selectedIdx = 0;
            for (std::size_t i = 0; i < n; ++i)
            {
                cstrs[i] = names[i].c_str();
                if (names[i] == pm.editing_character())
                    selectedIdx = static_cast<int>(i);
            }

            // Dropdown writes the editing character only. The pin
            // engages automatically when editing differs from
            // controlled; picking the controlled character clears
            // the pin. Apply is cross-body when pinned: the
            // controlled body wears the editing character's preset
            // items via PWS / carrier substitution. Gendered or
            // character-specific items may not have a renderable
            // variant on the controlled body and silently no-op.
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::Combo("Character##char_picker",
                             &selectedIdx,
                             cstrs,
                             static_cast<int>(n)))
            {
                const auto &pick = names[static_cast<std::size_t>(selectedIdx)];
                if (pick != pm.editing_character())
                {
                    pm.set_editing_character(pick);
                    for (auto &m : slot_mappings())
                    {
                        m.active = false;
                        m.targetItemId = 0;
                    }
                    pm.apply_to_state();
                    last_applied_ids().fill(0);
                    real_damaged().fill(false);
                    last_applied_real_ids().fill(0);
                    last_applied_carrier_ids().fill(0);
                    manual_apply();
                    pm.save();
                }
            }
            if (ImGui::IsItemHovered())
            {
                if (pm.editing_pinned())
                    ui_tooltip("Editing pinned: this character's preset is "
                               "loaded into the slot rows, but the body on "
                               "screen is whoever you control in-game. "
                               "Cross-body apply -- some items (gender-"
                               "specific, weapons) may not render. Pick "
                               "the controlled character to unpin.");
                else
                    ui_tooltip("Pick a different character to edit their "
                               "preset while controlling someone else. "
                               "The controlled body will wear the picked "
                               "character's preset items (cross-body "
                               "apply -- partial coverage expected).");
            }
            if (pm.editing_pinned())
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Unpin##char_unpin"))
                {
                    pm.clear_editing_pin();
                    for (auto &m : slot_mappings())
                    {
                        m.active = false;
                        m.targetItemId = 0;
                    }
                    pm.apply_to_state();
                    last_applied_ids().fill(0);
                    real_damaged().fill(false);
                    last_applied_real_ids().fill(0);
                    last_applied_carrier_ids().fill(0);
                    manual_apply();
                    pm.save();
                }
                if (ImGui::IsItemHovered())
                    ui_tooltip("Drop the editing pin and follow the "
                               "controlled character again.");
            }

            // Body-kind override. Lets body-swap mod users mark e.g.
            // Kliff as Female so the picker shows the female-body-
            // token pool. "Auto" defers to the hardcoded default
            // (Kliff/Oongka = Male, Damiane = Female).
            //
            // Saved per character in presets.json. Only affects the
            // picker filter -- no effect on apply / render behaviour.
            static constexpr const char *k_bodyItems[] = {
                "Auto", "Male", "Female", "Both",
            };
            constexpr int k_bodyCount =
                static_cast<int>(sizeof(k_bodyItems) / sizeof(k_bodyItems[0]));

            const std::string currentBody =
                pm.body_kind_of(pm.editing_character());
            int bodyIdx = 0;
            for (int i = 0; i < k_bodyCount; ++i)
            {
                if (currentBody == k_bodyItems[i])
                {
                    bodyIdx = i;
                    break;
                }
            }
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::Combo("Body##body_kind",
                             &bodyIdx,
                             k_bodyItems,
                             k_bodyCount))
            {
                pm.set_body_kind_of(pm.editing_character(),
                                    k_bodyItems[bodyIdx]);
            }
            if (ImGui::IsItemHovered())
                ui_tooltip("Override the body type used for picker "
                           "filtering. Use if you run a body-swap "
                           "mod that changes this character's "
                           "skeleton (e.g. Kliff -> Female).");
        }
    }

    ImGui::Separator();

    // --- Presets ---

    if (ImGui::CollapsingHeader("Presets", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto &presetList = pm.presets();
        int activeIdx = pm.active_preset_index();
        int count = pm.preset_count();

        for (int i = 0; i < count; ++i)
        {
            ImGui::PushID(i);

            bool isActive = (i == activeIdx);

            if (s_renameActive && s_renameIndex == i)
            {
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::InputText("##rename", s_renamePresetBuf,
                                     sizeof(s_renamePresetBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    if (auto *p = pm.active_preset_mut())
                    {
                    }
                    pm.set_active_preset(i);
                    if (auto *p = pm.active_preset_mut())
                        p->name = s_renamePresetBuf;
                    pm.set_active_preset(activeIdx);
                    pm.save();
                    s_renameActive = false;
                    s_renameIndex = -1;
                }
                ImGui::SameLine();
                if (ImGui::Button("OK"))
                {
                    pm.set_active_preset(i);
                    if (auto *p = pm.active_preset_mut())
                        p->name = s_renamePresetBuf;
                    pm.set_active_preset(activeIdx);
                    pm.save();
                    s_renameActive = false;
                    s_renameIndex = -1;
                }
            }
            else
            {
                char label[128];
                std::snprintf(label, sizeof(label), "%s [%d]##preset",
                              presetList[static_cast<std::size_t>(i)].name.c_str(), i);

                if (ImGui::Selectable(label, isActive))
                {
                    // Tear down any active body-mesh prefab picks
                    // BEFORE switching presets. Otherwise the hook
                    // keeps substituting the old src wrappers while
                    // the new preset's items are being equipped, which
                    // produces stale visuals across the transition.
                    const auto hadPick =
                        clear_all_picked_prefabs_and_deactivate();
                    pm.set_active_preset(i);
                    pm.apply_to_state();
                    // Post-apply_to_state reconciliation: for each
                    // slot where a body-mesh pick was just cleared AND
                    // the new preset's carrier equals last_applied
                    // (the early-out case), set the force-apply flag
                    // so the dispatcher bypasses the `targetId ==
                    // prevId` early-out while leaving lastIds intact
                    // -- Phase A `tear_down_fake` then runs against
                    // the prior carrier and the natpipe-hook cleans
                    // up the prior body-mesh tgt wrapper. When
                    // carriers differ the regular tear_down_fake path
                    // already handles cleanup naturally, so the flag
                    // is harmless.
                    auto &lastIds = Transmog::last_applied_ids();
                    auto &mods = Transmog::slot_mappings();
                    for (std::size_t s = 0; s < Transmog::k_slotCount;
                         ++s)
                    {
                        if (hadPick[s] &&
                            mods[s].targetItemId == lastIds[s])
                        {
                            Transmog::force_apply_pending()[s] = true;
                        }
                    }
                    manual_apply();
                    pm.save();
                }

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                {
                    s_renameActive = true;
                    s_renameIndex = i;
                    std::snprintf(s_renamePresetBuf, sizeof(s_renamePresetBuf), "%s",
                                  presetList[static_cast<std::size_t>(i)].name.c_str());
                }
            }

            ImGui::PopID();
        }

        if (count == 0)
            ui_text_disabled("No presets -- use Append to create one");

        ImGui::Spacing();

        if (ImGui::Button("Append"))
        {
            pm.append_from_state();
            manual_apply();
        }
        if (ImGui::IsItemHovered())
            ui_tooltip("Append a new preset with every slot ticked + "
                       "none (hides all armor), then apply it.");

        ImGui::SameLine();

        // Copy forks the active preset's saved state into a brand-new
        // preset, ignoring any unsaved edits in slot_mappings. Use to
        // get a clean clone to start altering on. For forking the
        // current pending edits, use "Save as New" instead.
        if (ImGui::Button("Copy"))
        {
            pm.duplicate_current();
            manual_apply();
        }
        if (ImGui::IsItemHovered())
            ui_tooltip("Clone the active preset's saved state into a "
                       "new preset (pending edits are discarded). Use "
                       "to start altering a clean copy without touching "
                       "the source.");

        ImGui::SameLine();

        // Save as New forks the current pending state (slot_mappings +
        // in-place dye/swatch) into a new preset, leaving the active
        // preset's saved rows untouched. Lets the user alter freely
        // mid-edit and bottle the result up without overwriting.
        if (ImGui::Button("Save as New"))
        {
            pm.save_as_new_from_state();
            manual_apply();
        }
        if (ImGui::IsItemHovered())
            ui_tooltip("Save the current pending state (including "
                       "unsaved picks) as a new preset. The active "
                       "preset's saved rows are left unchanged.");

        ImGui::SameLine();

        if (ImGui::Button("Remove") && count > 0)
        {
            pm.remove_current();
            // remove_current auto-selects a neighbouring preset.
            // Apply it so the visible outfit matches the new active
            // preset instead of leaving the old (now-deleted) one on
            // screen.
            if (pm.preset_count() > 0)
                manual_apply();
            else
                manual_clear();
        }

        ImGui::SameLine();

        if (ImGui::Button("Prev") && count > 1)
        {
            pm.prev_preset();
            manual_apply();
        }

        ImGui::SameLine();

        if (ImGui::Button("Next") && count > 1)
        {
            pm.next_preset();
            manual_apply();
        }

        if (count > 0)
            ui_text("Active: %d / %d", activeIdx + 1, count);
    }

    ImGui::Separator();

    // --- Per-slot controls ---

    if (ImGui::CollapsingHeader("Slot Details", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto &mappings = slot_mappings();
        const auto &table = ItemNameTable::instance();
        const bool tableReady = table.ready();

        if (!tableReady)
            ui_text_disabled("Item catalog not ready -- hex entry only.");

        ui_text_disabled("Toggle which slots the next Apply All will touch.");

        {
            // "All" tracks ENABLED slots only -- disabled slots
            // (multi-prefab non-armor, duplicate-tag; see
            // slot_metadata.hpp `enabled` doc) are forced off in the
            // dispatcher, so including them in the "All" check would
            // make the box stay unticked forever.
            bool allActive = true;
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                if (!Transmog::slot_enabled(i))
                    continue;
                if (!mappings[i].active)
                {
                    allActive = false;
                    break;
                }
            }
            if (ImGui::Checkbox("All", &allActive))
            {
                for (std::size_t i = 0; i < k_slotCount; ++i)
                {
                    if (!Transmog::slot_enabled(i))
                        continue;
                    mappings[i].active = allActive;
                }
                if (s_autoApply)
                {
                    flag_enabled().store(true,
                                         std::memory_order_relaxed);
                    manual_apply();
                }
            }
            if (!s_autoApply)
            {
                ImGui::SameLine();
                ui_text_disabled("(pending -- Apply All to commit)");
            }
        }

        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            // Skip disabled slots entirely -- the row, picker popup,
            // and label-sync logic below would all be misleading for
            // a slot that never reaches the dispatcher. The slot's
            // mapping is left as-is on disk so re-enabling later
            // restores prior selections without preset migration.
            if (!Transmog::slot_enabled(i))
                continue;

            auto &m = mappings[i];
            auto &ui = s_slotUI[i];
            const char *slotLabel = slot_name(static_cast<TransmogSlot>(i));

            // Lazy sync: when the PWS module has a target selection
            // for this slot but the UI label is empty, derive the
            // label from the catalog. Covers boot-time auto-apply
            // (preset's stored prefabName resolved by PWS after the
            // heap walk completes) and any other path that mutates
            // PWS selections without going through the picker.
            {
                namespace PWS = Transmog::PrefabWrapperSwap;
                const auto tslot = static_cast<TransmogSlot>(i);
                const int tgtIdx = PWS::selection_tgt_index(tslot);
                if (tgtIdx >= 0 && ui.pickedPrefabName.empty())
                {
                    const auto &cat = PWS::slot_catalog(tslot);
                    if (static_cast<std::size_t>(tgtIdx) < cat.size())
                        ui.pickedPrefabName = cat[tgtIdx].name;
                }
                else if (tgtIdx < 0 && !ui.pickedPrefabName.empty())
                {
                    ui.pickedPrefabName.clear();
                }
            }

            ImGui::PushID(static_cast<int>(i) + 100);

            if (ImGui::Checkbox(slotLabel, &m.active))
            {
                // Toggling active alone may not change the staged id
                // -- e.g. an active-none slot (active=true, targetId=0)
                // unticked back to inactive both yield staged=0, so
                // has_pending_changes would miss the toggle and the
                // Apply All button would stay grayed out. Set the
                // force-apply flag so the UI sees a pending change and
                // the dispatcher's slotNeedsWork picks the slot up
                // (its hasActiveNone path already handles the apply
                // semantics; the flag just guarantees the slot is
                // considered).
                force_apply_pending()[i] = true;
                if (s_autoApply)
                {
                    flag_enabled().store(true, std::memory_order_relaxed);
                    manual_apply_slot(i);
                }
            }


            // Slot-label column width: pick the longest slot name
            // ("TwoHandWeapon" plus a trailing pad) and measure it
            // with the live font. Content-derived rather than
            // DPI-linear so the column stays tight at 4K instead
            // of opening a 500px gap. Reshade mode keeps a fixed
            // 170px because its host UI applies its own scaling.
            const float slotColW = s_standaloneMode
                ? (ImGui::CalcTextSize("TwoHandWeapon  ").x
                   + ImGui::GetStyle().FramePadding.x * 2.0f)
                : 170.0f;
            ImGui::SameLine(slotColW);

            // --- Picker button ---
            {
                std::string pickerLabel;
                // When the user has picked a body-mesh prefab on this
                // slot, surface the prefab name on the button instead
                // of the carrier item's display name -- the carrier
                // is now an implementation detail (Kairos's gear
                // forced behind the scenes to feed the source wrapper).
                const bool showPrefabLabel = !ui.pickedPrefabName.empty();
                if (showPrefabLabel)
                {
                    pickerLabel = ui.pickedPrefabName + " [prefab]";
                }
                else if (m.targetItemId == 0)
                {
                    pickerLabel = "(none)";
                }
                else if (tableReady)
                {
                    auto internalName = table.name_of(m.targetItemId);
                    auto dispName = table.display_name_of(internalName);
                    if (!dispName.empty())
                        pickerLabel = dispName;
                    else if (!internalName.empty())
                        pickerLabel = internalName;
                    else
                        pickerLabel = "(unknown)";
                }
                else
                {
                    pickerLabel = "(catalog N/A)";
                }

                char btnLabel[192];
                if (showPrefabLabel)
                    std::snprintf(btnLabel, sizeof(btnLabel),
                                  "%s##pick", pickerLabel.c_str());
                else
                    std::snprintf(btnLabel, sizeof(btnLabel),
                                  "%s  [0x%04X]##pick", pickerLabel.c_str(),
                                  m.targetItemId);

                // Picker button width: ~32 glyphs at the live font
                // plus padding. Display names that exceed it are
                // truncated by ImGui's button text rendering. The
                // previous DPI-linear value (520 * scale) ballooned
                // to ~1360px at 4K.
                const float bw = s_standaloneMode
                    ? (ImGui::CalcTextSize("M").x * 32.0f
                       + ImGui::GetStyle().FramePadding.x * 2.0f)
                    : 380.0f;
                ImGui::SetNextItemWidth(bw);
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                                    ImVec2(0.02f, 0.5f));
                // Tint the button cyan when a body-mesh prefab is
                // picked so the override is visually distinct from
                // an ordinary carrier slot.
                if (showPrefabLabel)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(0.30f, 0.85f, 1.00f, 1.0f));
                }
                if (ImGui::Button(btnLabel, ImVec2(bw, 0.0f)))
                {
                    if (tableReady)
                    {
                        if (!s_keepSearchText)
                            ui.searchBuf[0] = '\0';
                        ImGui::OpenPopup("##slot_picker");
                    }
                }
                if (showPrefabLabel)
                    ImGui::PopStyleColor(); // Text
                ImGui::PopStyleVar(); // ButtonTextAlign

                // outPrefabIdx contract from the popup:
                //   -1: popup did NOT touch the body-mesh (real item
                //       or "(none) item" was picked) -- leave any
                //       prior body-mesh selection alone.
                //   -2: explicit "clear body-mesh override" pick.
                //   >=0: prefab N picked.
                int prefabIdx = -1;
                if (tableReady &&
                    draw_item_picker_popup(
                        "##slot_picker", ui,
                        static_cast<TransmogSlot>(i),
                        m.targetItemId,
                        s_autoApply, i,
                        &prefabIdx))
                {
                    std::snprintf(ui.hexBuf, sizeof(ui.hexBuf), "%04X",
                                  m.targetItemId);

                    namespace PWS = Transmog::PrefabWrapperSwap;
                    const auto tslot = static_cast<TransmogSlot>(i);
                    const int curSrc = PWS::selection_src_index(tslot);

                    if (prefabIdx >= 0 || prefabIdx == -2)
                    {
                        // Body-mesh interaction. Selection only --
                        // the swap map is rebuilt on the next apply
                        // via notify_apply_starting (apply-only
                        // lifecycle, mirroring the carrier pattern).
                        PWS::set_selection(
                            tslot, curSrc,
                            (prefabIdx >= 0) ? prefabIdx : -1);

                        // Update the per-slot UI label state. On a
                        // prefab pick (>= 0) we pull the name straight
                        // out of the catalog; on an explicit clear
                        // (-2) we drop the label so the row reverts
                        // to showing the carrier item.
                        if (prefabIdx >= 0)
                        {
                            const auto &cat = PWS::slot_catalog(tslot);
                            if (static_cast<std::size_t>(prefabIdx) <
                                cat.size())
                            {
                                ui.pickedPrefabName =
                                    cat[static_cast<std::size_t>(prefabIdx)]
                                        .name;
                            }
                            // Force the carrier into the ACTIVE
                            // CHARACTER's carrier so the source wrapper
                            // (e.g. cd_phw_00_ub_00_0163_index01 for
                            // Damiane chest) becomes resident and the
                            // swap-map hook can match it. Without this
                            // step, picking a prefab while controlling
                            // Wellsknight leaves the engine with
                            // Wellsknight wrappers and the hook never
                            // fires. (Used to be hardcoded to Kliff;
                            // now per-character so Damiane / Oongka get
                            // their own carriers, not Kliff's.)
                            force_active_character_carrier_for_picked_slots();
                        }
                        else
                        {
                            // Explicit body-mesh clear (-2): drop the
                            // label AND restore the slot's carrier to
                            // whatever the user had before the prefab
                            // pick auto-borrowed Kliff. Without this
                            // restore, the slot stays stuck on the
                            // Kliff plate carrier (e.g. 0x1521 for
                            // helm) instead of reverting to the user's
                            // underlying preset gear (e.g. Wellsknight
                            // 0x1520).
                            ui.pickedPrefabName.clear();
                            if (ui.priorCarrierSaved)
                            {
                                m.active = ui.priorCarrierActive;
                                m.targetItemId = ui.priorCarrierItemId;
                                ui.priorCarrierSaved = false;
                            }
                        }

                        // Set the force-apply flag UNCONDITIONALLY
                        // (regardless of s_autoApply). When the new
                        // carrier id equals the prior one (e.g.
                        // re-picking a body-mesh prefab on the same
                        // Kliff carrier 0x1521), staged/lastIds match,
                        // so without this flag has_pending_changes
                        // returns false -- the Apply All button would
                        // stay grayed out and a manual apply would
                        // hit the dispatcher's `targetId == prevId`
                        // early-out anyway. Setting the flag ensures
                        // (a) the Apply All button activates and
                        // (b) the dispatcher bypasses the early-out
                        // while leaving lastIds[i] intact, so Phase A
                        // `tear_down_fake` runs against 0x1521 and
                        // the engine's natural-pipeline hook
                        // substitutes src->tgt to clean up the prior
                        // body-mesh target wrapper. When ids differ
                        // (e.g. body-mesh clear that restored
                        // Wellsknight 0x1520 over the prior Kliff
                        // 0x1521), the dispatcher's own change
                        // detection covers it -- the flag is harmless.
                        if (last_applied_ids()[i] == m.targetItemId)
                            force_apply_pending()[i] = true;
                        if (s_autoApply)
                        {
                            flag_enabled().store(
                                true, std::memory_order_relaxed);
                            manual_apply_slot(i);
                        }
                        DMK::Logger::get_instance().info(
                            "[picker] slot={} BODY-MESH "
                            "{}={} (srcSeeded={}) -- carrier=0x{:04X} "
                            "auto-applied",
                            slotLabel,
                            (prefabIdx >= 0) ? "prefabIdx" : "cleared",
                            (prefabIdx >= 0) ? prefabIdx : 0,
                            curSrc >= 0, m.targetItemId);
                    }
                    else
                    {
                        // Real item or "(none) item" pick. The carrier
                        // itemId was already updated by the popup. We
                        // also drop any prior body-mesh prefab label
                        // for this slot -- picking a real item is the
                        // user's signal to revert to plain carrier
                        // mode. The existing body-mesh swap selection
                        // is also cleared via reactivate so the two
                        // states stay mutually exclusive.
                        const bool hadPrior = !ui.pickedPrefabName.empty();
                        if (hadPrior)
                        {
                            ui.pickedPrefabName.clear();
                            PWS::set_selection(tslot, curSrc, -1);
                            // Drop the saved prior-carrier snapshot.
                            // The user explicitly picked a new real
                            // item, so the popup-set m.targetItemId is
                            // authoritative -- restoring the prior
                            // carrier here would clobber the fresh
                            // pick.
                            ui.priorCarrierSaved = false;
                        }
                        // Set the force-apply flag UNCONDITIONALLY so
                        // has_pending_changes also returns true when
                        // the user clears a prefab pick onto a real
                        // item that happens to share the prior
                        // auto-borrowed Kliff carrier id (e.g. helm
                        // 0x1521 → 0x1521). Without it the Apply All
                        // button would stay grayed out. Different-id
                        // case is covered by the dispatcher's normal
                        // change detection -- flag is harmless.
                        if (hadPrior &&
                            last_applied_ids()[i] == m.targetItemId)
                            force_apply_pending()[i] = true;
                        if (s_autoApply)
                        {
                            flag_enabled().store(
                                true, std::memory_order_relaxed);
                            manual_apply_slot(i);
                        }

                        const auto name = (m.targetItemId == 0)
                                              ? std::string("(none)")
                                              : table.name_of(m.targetItemId);
                        const auto cat =
                            (m.targetItemId == 0)
                                ? TransmogSlot::Count
                                : table.category_of(m.targetItemId);
                        const char *catName =
                            (m.targetItemId == 0)
                                ? "clear"
                            : (cat == static_cast<TransmogSlot>(i))
                                ? "slot-match"
                                : "slot-MISMATCH";
                        DMK::Logger::get_instance().info(
                            "[picker] slot={} id=0x{:04X} name='{}' "
                            "category={} ({}) -- pending Apply All",
                            slotLabel, m.targetItemId,
                            name.empty() ? "<unknown>" : name,
                            static_cast<int>(cat), catName);
                    }
                }
            }

            // --- Hex input ---
            ImGui::SameLine();

            if (!ui.editing)
                std::snprintf(ui.hexBuf, sizeof(ui.hexBuf), "%04X",
                              m.targetItemId);

            // When a body-mesh prefab is active on this slot the
            // carrier itemId is auto-borrowed (Kairos plate) and the
            // user shouldn't be editing it directly -- show the hex
            // disabled+dim with an explanatory tooltip so it's clear
            // the body-mesh prefab is the source of truth, and the
            // hex is a peek at the underlying carrier.
            const bool prefabActive = !ui.pickedPrefabName.empty();
            ImGui::BeginDisabled(prefabActive);
            ImGui::SetNextItemWidth(64.0f);
            if (ImGui::InputText(
                    "##hex", ui.hexBuf, sizeof(ui.hexBuf),
                    ImGuiInputTextFlags_CharsHexadecimal |
                        ImGuiInputTextFlags_CharsUppercase))
            {
                unsigned long val = std::strtoul(ui.hexBuf, nullptr, 16);
                m.targetItemId = static_cast<uint16_t>(val);
            }
            ui.editing = !prefabActive && ImGui::IsItemActive();
            ImGui::EndDisabled();
            if (prefabActive && ImGui::IsItemHovered())
            {
                ui_tooltip(
                    "Auto-borrowed carrier id (Kairos plate). The "
                    "body-mesh prefab is the visual override; this "
                    "hex is just the engine plumbing. Clear the "
                    "prefab via the picker's '(none) prefab' to "
                    "edit the carrier directly.");
            }

            // Quick-clear button.
            ImGui::SameLine();
            if (ImGui::SmallButton("X##clr") && m.targetItemId != 0)
            {
                m.targetItemId = 0;
                std::snprintf(ui.hexBuf, sizeof(ui.hexBuf), "0000");
                // X clears both carrier and any body-mesh override on
                // this slot -- the user's intent is "remove everything
                // visible from this slot". Mirrors the (none) pick.
                if (!ui.pickedPrefabName.empty())
                {
                    namespace PWS = Transmog::PrefabWrapperSwap;
                    const auto tslot = static_cast<TransmogSlot>(i);
                    const int curSrc = PWS::selection_src_index(tslot);
                    ui.pickedPrefabName.clear();
                    PWS::set_selection(tslot, curSrc, -1);
                }
                if (s_autoApply)
                {
                    flag_enabled().store(true, std::memory_order_relaxed);
                    manual_apply_slot(i);
                }
            }

            // Cross-class limitation marker for weapon slots. Each
            // weapon prefab carries its own bone-binding + attachment-
            // socket data (which skeleton bone it parents to, which
            // socket transform it uses, hand offsets, sheath pose).
            // The engine resolves those bindings from the REAL equipped
            // item's class -- not from the LT swap target -- so a
            // mismatched-class transmog renders into the wrong socket
            // (or no socket) and goes invisible. Tooltip emphasises the
            // practical rule: match the prefab's class to whatever
            // weapon family the player has equipped right now.
            {
                const auto tslotMarker = static_cast<TransmogSlot>(i);
                const bool isWeaponSlot =
                    tslotMarker == TransmogSlot::MainHand ||
                    tslotMarker == TransmogSlot::OffHand ||
                    tslotMarker == TransmogSlot::Ranged ||
                    tslotMarker == TransmogSlot::SubWeapon ||
                    tslotMarker == TransmogSlot::TwoHandWeapon;
                if (isWeaponSlot)
                {
                    ImGui::SameLine();
                    ui_text_disabled("(?)");
                    if (ImGui::IsItemHovered())
                    {
                        const bool isTwoHand =
                            tslotMarker == TransmogSlot::TwoHandWeapon;
                        ui_tooltip(
                            isTwoHand
                            ? "EXPERIMENTAL.\n"
                              "\n"
                              "Heads-up: in this game, tools (pickaxe,\n"
                              "mallet, etc.) share the same engine slot\n"
                              "as 2H weapons. Setting a transmog here\n"
                              "may also affect the tool you have\n"
                              "equipped. Investigation ongoing.\n"
                              "\n"
                              "Best practice: pick a transmog from the\n"
                              "SAME weapon family you have equipped.\n"
                              "Sword on a sword, spear on a spear, 1H\n"
                              "on a 1H, 2H on a 2H -- always renders\n"
                              "correctly, drawn or sheathed.\n"
                              "\n"
                              "Why the limit exists:\n"
                              "Each weapon prefab carries its own\n"
                              "bone-binding and attachment-socket data\n"
                              "(which bone it parents to, which socket\n"
                              "transform it uses, hand offsets, sheath\n"
                              "pose). The engine looks those bindings\n"
                              "up from the REAL equipped item's class --\n"
                              "not from this transmog target -- so a\n"
                              "mismatched-class swap lands in the wrong\n"
                              "socket and goes invisible.\n"
                              "\n"
                              "Examples:\n"
                              " - 2H prefab while a 1H is equipped:\n"
                              "   mesh shows on the back; the real 1H\n"
                              "   goes invisible when drawn.\n"
                              " - Spear/hammer prefab while a sword is\n"
                              "   equipped: invisible when drawn\n"
                              "   (sheathed on back still renders).\n"
                              " - Same family (sword<->sword, etc.):\n"
                              "   renders correctly in every pose."
                            : "EXPERIMENTAL.\n"
                              "\n"
                              "Best practice: pick a transmog from the\n"
                              "SAME weapon family you have equipped.\n"
                              "Sword on a sword, spear on a spear, 1H\n"
                              "on a 1H, 2H on a 2H -- always renders\n"
                              "correctly, drawn or sheathed.\n"
                              "\n"
                              "Why the limit exists:\n"
                              "Each weapon prefab carries its own\n"
                              "bone-binding and attachment-socket data\n"
                              "(which bone it parents to, which socket\n"
                              "transform it uses, hand offsets, sheath\n"
                              "pose). The engine looks those bindings\n"
                              "up from the REAL equipped item's class --\n"
                              "not from this transmog target -- so a\n"
                              "mismatched-class swap lands in the wrong\n"
                              "socket and goes invisible.\n"
                              "\n"
                              "Examples:\n"
                              " - 2H prefab while a 1H is equipped:\n"
                              "   mesh shows on the back; the real 1H\n"
                              "   goes invisible when drawn.\n"
                              " - Spear/hammer prefab while a sword is\n"
                              "   equipped: invisible when drawn\n"
                              "   (sheathed on back still renders).\n"
                              " - Same family (sword<->sword, etc.):\n"
                              "   renders correctly in every pose.");
                    }
                }
            }

            // Lantern "(none)" gotcha. Unlike armor, the Lantern slot
            // controls a light source on the engine side, not just a
            // visual. Clearing it to (none) doesn't merely hide the
            // mesh -- the equipped item's light emission goes with
            // it, leaving the player without any handheld light.
            // Mirrors the weapon-slot (?) marker pattern.
            {
                const auto tslotL = static_cast<TransmogSlot>(i);
                if (tslotL == TransmogSlot::Lantern
                    && m.targetItemId == 0)
                {
                    ImGui::SameLine();
                    ui_text_colored(
                        ImVec4(0.95f, 0.65f, 0.20f, 1.0f),
                        "(!)");
                    if (ImGui::IsItemHovered())
                    {
                        ui_tooltip(
                            "Heads-up: Lantern set to (none) does\n"
                            "MORE than hide the mesh -- the engine\n"
                            "removes the light source too. Your\n"
                            "character will emit NO light, even at\n"
                            "night or in dungeons.\n"
                            "\n"
                            "If you wanted to hide the lantern\n"
                            "visually but keep the light, pick a\n"
                            "different lantern prefab here instead\n"
                            "of clearing the slot.");
                    }
                }
            }

            // --- Dye picker (per-slot per-mod color override) ---
            //
            // Cracked 2026-05-10 PM: dye record bytes +7/+8/+9 are
            // literal R/G/B of the picked shade. Each slot has up to
            // 16 ARMOR_MOD records (channels). The popup lists all
            // 16 mods as inline-expandable rows; expanding a row
            // reveals the color picker + repair slider for that
            // mod alone. Selection writes to the active preset's
            // PresetSlot::dye[idx]; the dispatch loop reads the
            // 16-element array each apply.
            //
            // Only shown for armor slots (Helm/Chest/Cloak/Gloves/
            // Boots). Weapons + accessories don't go through the
            // ARMOR_MOD pipeline so the picker has nothing to drive.
            {
                const auto curSlot = static_cast<TransmogSlot>(i);
                const bool isArmorSlot =
                    curSlot == TransmogSlot::Helm ||
                    curSlot == TransmogSlot::Chest ||
                    curSlot == TransmogSlot::Cloak ||
                    curSlot == TransmogSlot::Gloves ||
                    curSlot == TransmogSlot::Boots;
                if (!isArmorSlot)
                {
                    ImGui::PopID();
                    continue;
                }
                using namespace Transmog::DyeColorTable;

                // Pull the active preset's slot dye state. We mutate
                // it in place; the dispatch loop reads the same
                // memory next apply.
                Preset *editPreset =
                    PresetManager::instance().active_preset_mut();
                SlotDyeChannels *slotDye =
                    editPreset
                        ? &editPreset->slots[i].dye
                        : nullptr;

                // Declared in the dye-picker outer scope so the
                // Color Override tab below can read them without a
                // separate top-level lookup block.
                auto &dyeSlot =
                    Transmog::ColorOverride::dye_state()[i];
                const std::size_t detected =
                    Transmog::ColorOverride::SwatchTable::
                        detected_swatch_count(static_cast<int>(i));
                const bool detectedReady = detected > 0;

                ImGui::SameLine();

                // Find first active channel (for adjacent swatch).
                const ChannelDye *firstActiveCh = nullptr;
                int activeChCount = 0;
                if (slotDye)
                {
                    for (const auto &c : *slotDye)
                        if (c.active())
                        {
                            if (!firstActiveCh) firstActiveCh = &c;
                            ++activeChCount;
                        }
                }

                // Dye button: always plain "Dye" with no background
                // color override -- keeps text legible regardless of
                // dye state. Active state is signalled by a small
                // color-swatch chip rendered next to the button.
                // Auto-sized so the label fits at any font scale.
                const bool dyeBtn = ImGui::Button("Dye##dyeBtn");

                if (firstActiveCh)
                {
                    ImGui::SameLine(0.0f, 4.0f);
                    ImVec4 sw(firstActiveCh->r / 255.0f,
                              firstActiveCh->g / 255.0f,
                              firstActiveCh->b / 255.0f, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button, sw);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sw);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, sw);
                    // Match the Dye button's height so the chip sits
                    // on the row baseline at any font scale.
                    const float chip_size = ImGui::GetFrameHeight();
                    if (ImGui::Button("##dyeSw",
                                      ImVec2(chip_size, chip_size)))
                    {
                        ImGui::OpenPopup("##dye_picker");
                    }
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered())
                    {
                        char tip[48];
                        std::snprintf(tip, sizeof(tip),
                                      "%d active mod%s",
                                      activeChCount,
                                      activeChCount == 1 ? "" : "s");
                        ui_tooltip(tip);
                    }
                }

                // Color-override chip: circular (to distinguish from
                // the square dye chip), shows the first active
                // override colour for this slot. Clicking opens the
                // popup AND jumps to the Color Override tab.
                const Transmog::ColorOverride::SwatchTable::SwatchOverride
                    *firstOv = nullptr;
                int overrideCount = 0;
                for (std::size_t k = 0; k < detected; ++k)
                {
                    const auto &sw = dyeSlot.swatches[k];
                    if (sw.override_active)
                    {
                        if (!firstOv) firstOv = &sw;
                        ++overrideCount;
                    }
                }
                static bool s_dyePopupJumpToColor[k_slotCount] = {};
                if (firstOv)
                {
                    ImGui::SameLine(0.0f, 4.0f);
                    const float chip_d = ImGui::GetFrameHeight();
                    const ImVec2 cur = ImGui::GetCursorScreenPos();
                    const ImVec2 center(cur.x + chip_d * 0.5f,
                                        cur.y + chip_d * 0.5f);
                    const float radius = chip_d * 0.5f - 1.0f;
                    const ImVec4 colv(firstOv->r / 255.0f,
                                      firstOv->g / 255.0f,
                                      firstOv->b / 255.0f, 1.0f);
                    const bool clicked = ImGui::InvisibleButton(
                        "##coSw", ImVec2(chip_d, chip_d));
                    const bool hovered = ImGui::IsItemHovered();
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    dl->AddCircleFilled(
                        center, radius,
                        ImGui::ColorConvertFloat4ToU32(colv), 32);
                    dl->AddCircle(
                        center, radius,
                        ImGui::GetColorU32(ImGuiCol_Border), 32, 1.0f);
                    if (clicked)
                    {
                        ImGui::OpenPopup("##dye_picker");
                        s_dyePopupJumpToColor[i] = true;
                    }
                    if (hovered)
                    {
                        char tip[64];
                        std::snprintf(tip, sizeof(tip),
                                      "%d active override%s",
                                      overrideCount,
                                      overrideCount == 1 ? "" : "s");
                        ui_tooltip(tip);
                    }
                }

                if (dyeBtn && slotDye)
                    ImGui::OpenPopup("##dye_picker");

                // 10 color groups, ordered by HSL hue (red -> rose).
                // Anchored by `string_key` (the data file's _stringKey
                // field). Hash + sample swatch are looked up at render
                // time from the dye_color_table -- nothing in this UI
                // hardcodes a `_key` integer, so the picker survives a
                // game patch that renumbers `_key` (we just regen the
                // dye_color_table from the new dump).
                //
                // Sample swatch = each group's saturated end-of-row
                // shade (idx 18 for most groups, idx 17 for Bar_I).
                // We pick idx 18 universally; the visual difference
                // for Bar_I is one row of saturation, acceptable.
                struct GroupRow {
                    const char *string_key;
                    const char *label; // UI-only display text
                };
                static constexpr GroupRow kGroupRows[] = {
                    {"Her_Color_Group_I",   "Red"},
                    {"Tom_Color_Group_I",   "Orange"},
                    {"Por_Color_Group_I",   "Yellow"},
                    {"Bar_Color_Group_I",   "Lime"},
                    {"Cal_Color_Group_I",   "Green"},
                    {"Kwe_Color_Group_I",   "Teal"},
                    {"Del_Color_Group_I",   "Cyan"},
                    {"Dem_Color_Group_III", "Blue"},
                    {"Dem_Color_Group_II",  "Magenta"},
                    {"Dem_Color_Group_I",   "Rose"},
                };
                constexpr std::uint32_t kSampleShadeIdx = 18;

                // Per-slot UI state: which mod row is expanded
                // (only one at a time; clicking a row collapses
                // siblings). Static so it persists across frames.
                static int s_expandedMod[k_slotCount] = {};
                static bool s_expandedInit = false;
                if (!s_expandedInit)
                {
                    for (auto &v : s_expandedMod) v = -1;
                    s_expandedInit = true;
                }

                // Dye changes are visual-only with no side effects,
                // so we always re-apply on edit (regardless of the
                // global auto-apply toggle). force_apply_pending
                // bypasses the dispatcher's no-change skip; without
                // it a pure dye edit (item id unchanged) would never
                // re-fire the slotpop chain. flag_enabled is set so
                // the user sees their pick even if LT was toggled
                // off when they opened the picker.
                auto reapply_now = [i]()
                {
                    force_apply_pending()[i] = true;
                    flag_enabled().store(true,
                        std::memory_order_relaxed);
                    // Mark unsaved -- picker writes dye directly into
                    // the active preset, so a Save button check
                    // against slot_mappings can't catch it.
                    dye_dirty().store(true,
                        std::memory_order_relaxed);
                    manual_apply_slot(i);
                };

                // Helper lambda: render the picker body for one
                // specific channel index. Used inside the expanded
                // mod row.
                auto draw_channel_picker = [&](std::size_t chIdx,
                                               ChannelDye &ch)
                {
                    ImGui::PushID(static_cast<int>(chIdx) + 9000);

                    // Scale swatch button dims by the current font
                    // size so the picker block stays proportional
                    // to surrounding text. Without this, fixed-pixel
                    // buttons look tiny when the host (standalone
                    // DPI-scaled atlas / user override / ReShade
                    // host scale) inflates everything else.
                    const float pickerScale =
                        ImGui::GetFontSize() / 13.0f;

                    // Group buttons row -- color swatches only,
                    // hovering shows the family name. Selected
                    // group gets a thin border so the user knows
                    // which palette they're picking shades from.
                    // Hash + sample BGRA looked up dynamically from
                    // the data table: zero hardcoded `_key` integers
                    // in this UI.
                    for (std::size_t g = 0;
                         g < std::size(kGroupRows); ++g)
                    {
                        if (g != 0) ImGui::SameLine();
                        const auto &row = kGroupRows[g];
                        const Group *grp =
                            find_group_by_name(row.string_key);
                        // Sample = saturated shade idx 18 (or shade 0
                        // if group missing -- shouldn't happen on a
                        // matched build, but guards against patch
                        // mismatch).
                        const std::uint32_t sampleBgra =
                            (grp && grp->shades &&
                             grp->shade_count > kSampleShadeIdx)
                                ? grp->shades[kSampleShadeIdx].bgra
                                : 0xFF888888u;
                        const float br = ((sampleBgra >> 16) & 0xFF) / 255.0f;
                        const float bg = ((sampleBgra >> 8) & 0xFF) / 255.0f;
                        const float bb = ((sampleBgra) & 0xFF) / 255.0f;
                        ImGui::PushStyleColor(ImGuiCol_Button,
                            ImVec4(br, bg, bb, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(br * 1.15f, bg * 1.15f, bb * 1.15f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                            ImVec4(br, bg, bb, 1.0f));
                        const bool selected =
                            !ch.group_name.empty() &&
                            ch.group_name == row.string_key;
                        if (selected)
                            ImGui::PushStyleColor(ImGuiCol_Border,
                                ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                        char gid[16];
                        std::snprintf(gid, sizeof(gid),
                                      "##g%zu", g);
                        if (ImGui::Button(gid,
                                          ImVec2(17.0f * pickerScale,
                                                 14.0f * pickerScale)))
                        {
                            // string_key is the source-of-truth; hash
                            // is derived. If the group is missing
                            // (patch mismatch) we leave hash at 0 and
                            // the dye won't apply, but the picker
                            // state is preserved.
                            ch.group_name = row.string_key;
                            ch.group_hash = grp ? grp->key : 0;
                        }
                        if (selected)
                            ImGui::PopStyleColor();
                        ImGui::PopStyleColor(3);
                        if (ImGui::IsItemHovered())
                            ui_tooltip(row.label);
                    }

                    // Shade grid for the chosen group.
                    if (ch.group_hash != 0)
                    {
                        const Group *grp = find_group(ch.group_hash);
                        if (grp != nullptr && grp->shades != nullptr)
                        {
                            const std::uint32_t maxIdx =
                                grp->shade_count;

                            ui_text("Neutrals:");
                            for (std::uint32_t s = 0;
                                 s < 9 && s < maxIdx; ++s)
                            {
                                const std::uint32_t bgra =
                                    grp->shades[s].bgra;
                                const float r = ((bgra >> 16) & 0xFF) / 255.0f;
                                const float g2 = ((bgra >> 8) & 0xFF) / 255.0f;
                                const float b = ((bgra) & 0xFF) / 255.0f;
                                if (s != 0) ImGui::SameLine();
                                ImGui::PushStyleColor(ImGuiCol_Button,
                                    ImVec4(r, g2, b, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                    ImVec4(r, g2, b, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                    ImVec4(r, g2, b, 1.0f));
                                char nlabel[16];
                                std::snprintf(nlabel, sizeof(nlabel),
                                              "##n%u", s);
                                if (ImGui::Button(nlabel,
                                                  ImVec2(14.0f * pickerScale,
                                                         14.0f * pickerScale)))
                                {
                                    ch.r = (bgra >> 16) & 0xFF;
                                    ch.g = (bgra >> 8) & 0xFF;
                                    ch.b = bgra & 0xFF;
                                    reapply_now();
                                }
                                ImGui::PopStyleColor(3);
                                if (ImGui::IsItemHovered())
                                {
                                    char tip[64];
                                    std::snprintf(tip, sizeof(tip),
                                        "shade %u  RGB=(%u,%u,%u)",
                                        s,
                                        (bgra >> 16) & 0xFF,
                                        (bgra >> 8) & 0xFF,
                                        bgra & 0xFF);
                                    ui_tooltip(tip);
                                }
                            }

                            ui_text("Hues:");
                            int col = 0;
                            for (std::uint32_t s = 9;
                                 s < maxIdx; ++s)
                            {
                                const std::uint32_t bgra =
                                    grp->shades[s].bgra;
                                const float r = ((bgra >> 16) & 0xFF) / 255.0f;
                                const float g2 = ((bgra >> 8) & 0xFF) / 255.0f;
                                const float b = ((bgra) & 0xFF) / 255.0f;
                                if (col != 0) ImGui::SameLine();
                                ImGui::PushStyleColor(ImGuiCol_Button,
                                    ImVec4(r, g2, b, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                    ImVec4(r, g2, b, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                    ImVec4(r, g2, b, 1.0f));
                                char slabel[32];
                                std::snprintf(slabel, sizeof(slabel),
                                              "##s%u", s);
                                if (ImGui::Button(slabel,
                                                  ImVec2(14.0f * pickerScale,
                                                         14.0f * pickerScale)))
                                {
                                    ch.r = (bgra >> 16) & 0xFF;
                                    ch.g = (bgra >> 8) & 0xFF;
                                    ch.b = bgra & 0xFF;
                                    reapply_now();
                                }
                                ImGui::PopStyleColor(3);
                                if (ImGui::IsItemHovered())
                                {
                                    char tip[64];
                                    std::snprintf(tip, sizeof(tip),
                                        "shade %u  RGB=(%u,%u,%u)",
                                        s,
                                        (bgra >> 16) & 0xFF,
                                        (bgra >> 8) & 0xFF,
                                        bgra & 0xFF);
                                    ui_tooltip(tip);
                                }
                                ++col;
                                if (col >= 10) col = 0;
                            }
                        }
                    }
                    else
                    {
                        ui_text_disabled("(pick a group above)");
                    }

                    // Repair slider (offset +11 of dye record).
                    // The engine stores wear as a 0..127 byte; 0 means
                    // pristine, 127 means max wear. We surface the
                    // inverse as "Repair %" so 100 = pristine and 0 =
                    // max wear, matching the conventional reading.
                    // 0xFF is the legacy "no override" sentinel and is
                    // treated as 100% on read for old presets.
                    int repairPct = (ch.repair_byte == 0xFF)
                        ? 100
                        : 100 - ((ch.repair_byte * 100 + 63) / 127);
                    ImGui::SetNextItemWidth(200.0f);
                    if (ImGui::SliderInt("Repair %", &repairPct, 0, 100))
                    {
                        if (repairPct >= 100) ch.repair_byte = 0;
                        else if (repairPct <= 0) ch.repair_byte = 127;
                        else ch.repair_byte = static_cast<std::uint8_t>(
                            ((100 - repairPct) * 127) / 100);
                        reapply_now();
                    }

                    // --- Material picker (template u16 at +4..+5) ---
                    //
                    // The dye record's u16 at +4..+5 is a TEMPLATE
                    // INDEX (1..10) into partprefabdyetexturepalleteinfo.
                    // Each template specifies texture variants per
                    // cat_code slot (cat1=cloth, cat2=secondary fabric,
                    // cat3=metal). The engine resolves each channel's
                    // cat_code per-item and pulls the variant from the
                    // template. So template 5 picks DIFFERENT textures
                    // for cat_001/002/003 simultaneously, not "variant
                    // 5 of one category."
                    namespace MPT =
                        Transmog::MaterialPaletteTable;
                    ui_text("Material template:");
                    // Width sized to the widest two-digit label so the
                    // "10" button is not cut off; all rows align at the
                    // same width regardless of single- vs two-digit.
                    const float mat_btn_w =
                        ImGui::CalcTextSize("10").x
                        + ImGui::GetStyle().FramePadding.x * 2.0f
                        + 8.0f;
                    for (std::uint16_t v = 1; v <= 10; ++v)
                    {
                        if (v != 1) ImGui::SameLine();
                        const bool selected =
                            ch.material_id == v;
                        if (selected)
                            ImGui::PushStyleColor(ImGuiCol_Button,
                                ImVec4(0.3f, 0.6f, 0.9f, 1.0f));
                        char mlabel[16];
                        std::snprintf(mlabel, sizeof(mlabel),
                                      "%u##m%u", v, v);
                        if (ImGui::Button(mlabel,
                                          ImVec2(mat_btn_w, 0.0f)))
                        {
                            ch.material_id = v;
                            reapply_now();
                        }
                        if (selected)
                            ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered())
                        {
                            const auto *t = MPT::find(v);
                            if (t)
                            {
                                char tip[512];
                                int n = std::snprintf(
                                    tip, sizeof(tip),
                                    "Template %u (blend %.2f)\n",
                                    t->idx, t->blend);
                                for (std::size_t s = 0;
                                     s < t->slot_count
                                     && n < (int)sizeof(tip) - 1;
                                     ++s)
                                {
                                    const auto &slot = t->slots[s];
                                    if (slot.alias)
                                        n += std::snprintf(
                                            tip + n, sizeof(tip) - n,
                                            "  cat%u %s/%s -> %u\n",
                                            slot.cat_code,
                                            slot.label,
                                            slot.alias,
                                            slot.variant);
                                    else
                                        n += std::snprintf(
                                            tip + n, sizeof(tip) - n,
                                            "  cat%u %s -> %u\n",
                                            slot.cat_code,
                                            slot.label,
                                            slot.variant);
                                }
                                ui_tooltip(tip);
                            }
                            else
                            {
                                char tip[64];
                                std::snprintf(tip, sizeof(tip),
                                    "Template %u (out of range)", v);
                                ui_tooltip(tip);
                            }
                        }
                    }
                    ImGui::SameLine();
                    {
                        const bool selected =
                            ch.material_id == 0xFFFF;
                        if (selected)
                            ImGui::PushStyleColor(ImGuiCol_Button,
                                ImVec4(0.3f, 0.6f, 0.9f, 1.0f));
                        // Auto-sized so the label always fits regardless
                        // of overlay font scale; prior fixed widths
                        // truncated the text on chunky bitmap fonts.
                        if (ImGui::Button("Default##mFFFF"))
                        {
                            ch.material_id = 0xFFFF;
                            reapply_now();
                        }
                        if (selected)
                            ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered())
                            ui_tooltip(
                                "0xFFFF -- engine picks the "
                                "natural variant for this channel.");
                    }
                    // Render the label on the left so the trailing
                    // ImGui auto-label does not clip on the popup's
                    // right edge in the standalone overlay.
                    int rawMat = static_cast<int>(ch.material_id);
                    ui_text("Raw u16:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::InputInt("##matRaw",
                                        &rawMat, 1, 16,
                                        ImGuiInputTextFlags_CharsHexadecimal
                                        | ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        if (rawMat < 0) rawMat = 0;
                        if (rawMat > 0xFFFF) rawMat = 0xFFFF;
                        ch.material_id =
                            static_cast<std::uint16_t>(rawMat);
                        reapply_now();
                    }

                    if (ImGui::Button("Clear this mod"))
                    {
                        ch = ChannelDye{};
                        reapply_now();
                    }

                    ImGui::PopID();
                };

                // Per-popup-session auto-trigger flag for the Color
                // Override tab's first-activation 1-pass reinit. One
                // entry per slot; reset on popup close so the next
                // open re-fires on still-empty slots.
                static bool s_coAutoTriggered[k_slotCount] = {};
                // Pin a min size so the popup doesn't visibly
                // collapse + re-expand while the 1-pass reinit runs
                // (swatches arrive ~1.5s after the tab is selected).
                // Scaled by font size so the floor stays sensible on
                // chunky DPI / overlay scale.
                {
                    const float fs = ImGui::GetFontSize();
                    ImGui::SetNextWindowSizeConstraints(
                        ImVec2(fs * 36.0f, fs * 28.0f),
                        ImVec2(FLT_MAX, FLT_MAX));
                }
                if (ImGui::BeginPopup("##dye_picker"))
                {
                    // Two-tab popup: Dye (engine partprefab dye-record
                    // picker) and Color Override (post-binding matInst
                    // colour injection). Distinct pipelines, unified
                    // entry point.
                    if (ImGui::BeginTabBar("##dye_tabs"))
                    {
                    if (ImGui::BeginTabItem("Dye"))
                    {
                    // --- Top action bar ---
                    if (ImGui::Button("Mirror Mod 0 to all"))
                    {
                        if (slotDye)
                        {
                            const auto src = (*slotDye)[0];
                            for (auto &ch : *slotDye) ch = src;
                            reapply_now();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clear all mods"))
                    {
                        if (slotDye) *slotDye = SlotDyeChannels{};
                        reapply_now();
                    }
                    ImGui::SameLine();
                    // Per-slot "sync from live" -- pulls the engine's
                    // current dye records for this slot (e.g. after
                    // the user applied dye at an in-game dye station)
                    // into the active preset, overwriting any
                    // picker-set channels on this slot only. The
                    // real-item restore path already auto-mirrors on
                    // picker -> real transitions; this button covers
                    // the case where the slot stays bound to a fake
                    // but the underlying real item's dye changed
                    // in-game.
                    if (ImGui::Button("Sync from live##dye_sync"))
                    {
                        if (Transmog::sync_live_dye_for_slot(i))
                            reapply_now();
                    }
                    if (ImGui::IsItemHovered())
                        ui_tooltip(
                            "Re-read this slot's dye from the engine's\n"
                            "current auth table and overwrite the\n"
                            "preset's saved channels. Use after you\n"
                            "apply dye at an in-game dye station and\n"
                            "want those bytes saved into this preset\n"
                            "without re-running Capture Outfit.\n"
                            "\n"
                            "No-op if the slot has no live auth-table\n"
                            "entry (nothing equipped) or no dye records.");
                    ImGui::SameLine();
                    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();

                    // Per-slot dye-inject mode toggle. See
                    // PresetSlot::dyeSparse in preset_manager.hpp
                    // for the full semantics. Toggling reapplies
                    // immediately so the user sees the visual
                    // change without an extra Apply All click.
                    if (editPreset != nullptr &&
                        i < editPreset->slots.size())
                    {
                        bool sparse = editPreset->slots[i].dyeSparse;
                        if (ImGui::Checkbox("Sparse##dye_sparse",
                                            &sparse))
                        {
                            editPreset->slots[i].dyeSparse = sparse;
                            dye_dirty().store(
                                true, std::memory_order_release);
                            reapply_now();
                        }
                        if (ImGui::IsItemHovered())
                            ui_tooltip(
                                "Sparse (default, matches the merchant dye UI):\n"
                                "emit only the channels you set; engine paints\n"
                                "the rest with its own defaults.\n"
                                "\n"
                                "Turn off for cross-class fake transmog where\n"
                                "every channel needs your colour to suppress\n"
                                "the carrier's default palette.");
                    }

                    ui_text_disabled(
                        "Tip: items typically use mods (dye slots) 1-12; "
                        "rest no-ops.\n"
                        "This mod cannot tell which item is dyeable or "
                        "has many slots.");
                    ImGui::Separator();

                    // --- 16 mod rows, each inline-expandable ---
                    if (slotDye)
                    {
                        for (std::size_t k = 0;
                             k < slotDye->size(); ++k)
                        {
                            ChannelDye &ch = (*slotDye)[k];
                            ImGui::PushID(static_cast<int>(k) + 5000);

                            // Header: chevron + "Mod K" + swatch +
                            // summary. Width is computed from the
                            // widest label ("v Mod 15") so all rows
                            // align in a column regardless of font
                            // scale; auto-size would let single-digit
                            // rows shrink and break the column.
                            const bool expanded =
                                s_expandedMod[i] ==
                                static_cast<int>(k);
                            char hdr[24];
                            std::snprintf(hdr, sizeof(hdr),
                                          "%s Mod %2zu",
                                          expanded ? "v" : ">", k);
                            const float row_btn_w =
                                ImGui::CalcTextSize("v Mod 15").x
                                + ImGui::GetStyle().FramePadding.x * 2.0f
                                + 8.0f;
                            if (ImGui::Button(hdr,
                                              ImVec2(row_btn_w, 0.0f)))
                            {
                                s_expandedMod[i] = expanded
                                    ? -1
                                    : static_cast<int>(k);
                            }

                            // Color swatch. Sized to the row button's
                            // height so the two widgets sit on the
                            // same baseline regardless of font scale,
                            // and made clickable so the swatch acts
                            // as a second hit target for expanding
                            // the row (small fixed sizes were hard to
                            // hit in the standalone overlay).
                            ImGui::SameLine();
                            ImVec4 sw = ch.active()
                                ? ImVec4(ch.r / 255.0f,
                                         ch.g / 255.0f,
                                         ch.b / 255.0f, 1.0f)
                                : ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
                            ImGui::PushStyleColor(ImGuiCol_Button, sw);
                            ImGui::PushStyleColor(
                                ImGuiCol_ButtonHovered, sw);
                            const float sw_size =
                                ImGui::GetFrameHeight();
                            if (ImGui::Button(
                                    "##sw",
                                    ImVec2(sw_size, sw_size)))
                            {
                                s_expandedMod[i] = expanded
                                    ? -1
                                    : static_cast<int>(k);
                            }
                            ImGui::PopStyleColor(2);

                            // Summary text.
                            ImGui::SameLine();
                            if (ch.active())
                            {
                                int repairPct = (ch.repair_byte == 0xFF)
                                    ? 100
                                    : 100 - ((ch.repair_byte * 100 + 63) / 127);
                                ui_text(
                                    "RGB=(%u,%u,%u) rep=%d%%",
                                    ch.r, ch.g, ch.b, repairPct);
                            }
                            else
                            {
                                ui_text_disabled("(default)");
                            }

                            // Inline expanded body.
                            if (expanded)
                            {
                                ImGui::Indent(20.0f);
                                draw_channel_picker(k, ch);
                                ImGui::Unindent(20.0f);
                                ImGui::Separator();
                            }

                            ImGui::PopID();
                        }
                    }

                    ImGui::EndTabItem();
                    } // end Dye tab

                    // --- Color Override tab ---
                    // Post-binding matInst colour injection. Different
                    // pipeline from dye records but presented in the
                    // same popup as a unified per-slot picker. On
                    // first activation per popup session, auto-fires
                    // a single-pass reinit so the swatch grid is
                    // populated without the user clicking Re-init --
                    // one pass is enough; the 3-pass ghost-filtered
                    // variant remains available via the Re-init
                    // button below.
                    // Honor "jump to Color Override tab" set by the
                    // circular override chip on the row. One-shot:
                    // clear the flag after consuming so subsequent
                    // tab clicks behave normally.
                    ImGuiTabItemFlags coTabFlags =
                        s_dyePopupJumpToColor[i]
                            ? ImGuiTabItemFlags_SetSelected
                            : 0;
                    s_dyePopupJumpToColor[i] = false;
                    if (Transmog::flag_color_override().load(
                            std::memory_order_acquire)
                        && ImGui::BeginTabItem("Color Override",
                                               nullptr, coTabFlags))
                    {
                        if (!s_coAutoTriggered[i]
                            && detected == 0
                            && !Transmog::ColorOverride::Reinit::
                                   any_slot_reinit_active())
                        {
                            Transmog::flag_enabled().store(
                                true, std::memory_order_relaxed);
                            Transmog::ColorOverride::Reinit::
                                start_slot_reinit_once(
                                    static_cast<int>(i));
                            s_coAutoTriggered[i] = true;
                        }

                        // --- Per-slot color override picker ---
                        // Each transmog slot maps to one or more
                        // dye-able regions ("swatches"). The hook
                        // auto-detects how many regions the currently
                        // equipped item has from the per-(slot,
                        // channel) write counter, and captures each
                        // region's asset-default color so the UI
                        // displays it as a starting value.
                        //
                        //   Slot toggle ('Dye'): master enable for
                        //                        all swatches
                        //   Per-swatch checkbox: enable user override
                        //                        for that swatch
                        //   Color picker: shows asset default until
                        //                 toggled, then pre-fills
                        //                 user RGB and lets them edit
                        ImGui::BeginDisabled(!detectedReady);
                        if (ImGui::Checkbox("Dye##dye_on", &dyeSlot.slot_enabled))
                {
                    // The Dye master toggle gates the substitution
                    // path (dye_override.cpp: !slot_enabled →
                    // return false, engine default writes through).
                    // To make the toggle visually take effect we
                    // need the engine to actually write again; bare
                    // manual_apply_slot short-circuits as a same-state
                    // apply. Route through commit-retick so the
                    // engine tears down and re-instantiates the
                    // carrier, producing fresh writes that respect
                    // the new slot_enabled value.
                    if (!Transmog::ColorOverride::Reinit::any_slot_reinit_active())
                    {
                        flag_enabled().store(true, std::memory_order_relaxed);
                        Transmog::ColorOverride::Reinit::
                            schedule_color_commit_retick(static_cast<int>(i));
                    }
                    Transmog::dye_dirty().store(
                        true, std::memory_order_release);
                }
                ImGui::EndDisabled();
                if (!detectedReady &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(
                        "No apply yet -- swatches unknown.\n"
                        "Run Re-init to capture the slot's dye\n"
                        "swatches before toggling the master Dye\n"
                        "switch.");
                    ImGui::EndTooltip();
                }

                // Per-slot 3-pass swatch re-init. Drives clear + apply
                // 3 times spaced ~1s apart, then prunes any swatch row
                // whose identity didn't appear in all 3 passes (ghost
                // filter). Use when the swatch list is empty / stale
                // and you'd otherwise untick-retick by hand.
                ImGui::SameLine(0.0f, 6.0f);
                const bool reinitActive =
                    Transmog::ColorOverride::Reinit::is_slot_reinit_active(
                        static_cast<int>(i));
                if (reinitActive)
                {
                    const int rpass =
                        Transmog::ColorOverride::Reinit::slot_reinit_pass(
                            static_cast<int>(i));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    ImGui::BeginDisabled(true);
                    char rlbl[32];
                    std::snprintf(rlbl, sizeof(rlbl),
                                  "Re-init %d/3##sw_reinit_busy",
                                  rpass + 1);
                    ImGui::SmallButton(rlbl);
                    ImGui::EndDisabled();
                    ImGui::PopStyleVar();
                }
                else
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    const bool reinitClicked =
                        ImGui::SmallButton("Re-init##sw_reinit");
                    ImGui::PopStyleVar();
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(
                            "Auto-cycle clear+apply 3 times (~1s each).\n"
                            "Keeps only swatches that appear in all 3\n"
                            "passes; drops ghosts. Use when the swatch\n"
                            "list looks empty or has stale rows.\n\n"
                            "Replaces the manual 'untick / retick'\n"
                            "loop -- you can leave this slot alone\n"
                            "while it runs.");
                        ImGui::EndTooltip();
                    }
                    if (reinitClicked)
                    {
                        flag_enabled().store(true, std::memory_order_relaxed);
                        Transmog::ColorOverride::Reinit::start_slot_reinit(
                            static_cast<int>(i));
                    }

                    // Reset slot button stays enabled even when
                    // `detected == 0` so the user can recover from
                    // stale rows in exactly the state where Re-init
                    // would otherwise stall.
                    ImGui::SameLine(0.0f, 6.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    const bool topResetClicked =
                        ImGui::SmallButton("Reset slot##sw_reset_top");
                    ImGui::PopStyleVar();
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(
                            "Wipe ALL captured swatches for THIS slot,\n"
                            "clear the post-reinit lock + frozen-hidden\n"
                            "rows, then trigger a fresh apply.\n\n"
                            "Use when the slot is stuck with stale rows\n"
                            "(e.g. JSON-restored entries that no longer\n"
                            "match the current item) and Re-init can't\n"
                            "make progress.");
                        ImGui::EndTooltip();
                    }
                    if (topResetClicked)
                    {
                        Transmog::ColorOverride::SwatchTable::wipe_swatch_table_for_slot(
                            static_cast<int>(i));
                        Transmog::ColorOverride::SwatchTable::clear_dye_state_for_slot(
                            static_cast<int>(i));
                        // Drop any persisted overrides for this slot
                        // that are queued via the setter's pending
                        // map -- otherwise the next engine write
                        // would re-substitute the saved colour on
                        // top of the freshly-wiped row.
                        Transmog::ColorOverride::PendingOverrides::clear_slot(
                            static_cast<int>(i));
                        // Reset is a pending change: the JSON still
                        // has the old swatch rows. Flip dirty so the
                        // top "Save *" button surfaces, but don't
                        // auto-write JSON -- user commits explicitly.
                        Transmog::dye_dirty().store(
                            true, std::memory_order_release);
                        if (s_autoApply)
                        {
                            flag_enabled().store(true, std::memory_order_relaxed);
                            manual_apply_slot(i);
                        }
                    }

                }

                // Auto-detected swatch count drives whether to show
                // the swatch UI. Pre-apply (detected==0) we render
                // nothing here so the disabled-Dye-checkbox tooltip
                // can explain the state without an empty
                // CollapsingHeader pushing the Apply button down.
                if (detectedReady)
                {
                    // Collapse the per-slot swatch UI behind a
                    // CollapsingHeader so users who only want to swap
                    // items don't get a wall of dye controls pushing
                    // the Apply button down. Default-closed first
                    // time; ImGui remembers per-header state across
                    // frames after that. The body below is one indent
                    // level shallower than strict nesting would
                    // dictate to keep the wrap purely additive.
                    char dyeHdrBuf[160];
                    // Count rows with a user-picked colour. The badge
                    // shows "M rows (N coloured)" where M is the full
                    // captured set and N is how many the user has
                    // explicitly picked a colour for.
                    std::size_t coloured = 0;
                    for (const auto &sw : dyeSlot.swatches)
                    {
                        if (sw.override_active) ++coloured;
                    }
                    const bool hasOverride = coloured > 0;
                    // Mid-retick badge (b): show "(applying...)" while
                    // a colour-commit retick cycle is in-flight on
                    // this slot. The cycle takes ~1.9s; this badge
                    // tells the user the engine hasn't finished yet.
                    const bool reticking =
                        Transmog::ColorOverride::Reinit::is_color_commit_retick_active(
                            static_cast<int>(i));
                    // Master-disabled badge: if the user unticked the
                    // Dye checkbox, the swatch list is inert. Surface
                    // that clearly in the closed header.
                    const bool masterDisabled = !dyeSlot.slot_enabled;
                    // Plain text label now -- the popup tab is the
                    // container, no CollapsingHeader ID stability
                    // concern. Dropped the `###dye_sec` ID suffix
                    // that the prior CollapsingHeader relied on.
                    if (coloured > 0)
                    {
                        std::snprintf(dyeHdrBuf, sizeof(dyeHdrBuf),
                                      "%zu rows (%zu coloured)%s%s%s",
                                      detected, coloured,
                                      hasOverride   ? " *"           : "",
                                      reticking     ? " (applying)"  : "",
                                      masterDisabled? " (off)"       : "");
                    }
                    else
                    {
                        std::snprintf(dyeHdrBuf, sizeof(dyeHdrBuf),
                                      "%zu rows%s%s",
                                      detected,
                                      reticking     ? " (applying)"  : "",
                                      masterDisabled? " (off)"       : "");
                    }
                    ui_text("%s", dyeHdrBuf);
                    ImGui::Separator();
                    {
                    auto clampByte = [](float v) -> std::uint8_t {
                        if (v < 0.0f) v = 0.0f;
                        if (v > 1.0f) v = 1.0f;
                        return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
                    };

                    // 2026-04-29 UX rewrite: per-submesh + per-token
                    // granularity gives ~9 swatches per submesh, which
                    // gets unwieldy fast. New layout:
                    //   - Group rows by (submesh_stable_id, template_id)
                    //     into collapsible "Region N" cards.
                    //   - Within each region, group by layer
                    //     (tint / mask / detail / hair / misc).
                    //   - For triplet layers (R/G/B suffix) show one
                    //     colour picker by default with the R/G/B
                    //     channels linked, mirroring merchant "Pure X"
                    //     semantics. Untick "link" to expose three
                    //     independent per-channel pickers.
                    //   - The previous "Show all" ghost filter is
                    //     removed: each row is now labelled, so noise
                    //     hiding is unnecessary. Rows still waiting for
                    //     their first capture (default_captured == false)
                    //     are skipped silently; they're inert until
                    //     the engine writes them.

                    // Pass 1: group swatch indices by submesh, then by
                    // layer. Each region's `layerSlot[layer][channel]`
                    // holds the swatch index in dyeSlot.swatches, or -1.
                    //
                    // Layers: 0=tint, 1=mask, 2=detail, 3=hair, 4=scratch.
                    // `layerSlot[L][0..2]` = R/G/B siblings for that family.
                    // `layerSingletons[L]` = ch=-1 tokens (e.g.
                    // `_dyeingDetailLayerColorBlend`, `_hairDyeingColor`)
                    // rendered under the same family header.
                    // `miscIndices` = truly unknown tokens (layer=-1).
                    struct RegionView
                    {
                        std::uint64_t stable_id = 0;
                        std::uint16_t tpl       = 0;
                        int layerSlot[5][3]{};
                        std::vector<int> layerSingletons[5];
                        std::vector<int> miscIndices;
                    };
                    auto initRegion = [](RegionView &rv) {
                        for (int L = 0; L < 5; ++L)
                            for (int C = 0; C < 3; ++C)
                                rv.layerSlot[L][C] = -1;
                    };

                    std::map<std::uint64_t, RegionView> regions;
                    // Slot-level "recolor all" cascade list: every
                    // visible swatch in the slot.
                    std::vector<int> slotAllIndices;
                    slotAllIndices.reserve(detected);

                    for (std::size_t s = 0; s < detected; ++s)
                    {
                        auto &sw = dyeSlot.swatches[s];
                        // Skip rows that haven't captured a default
                        // yet; picking on them would write zeros.
                        // (override_active rows always pass below.)
                        if (!sw.default_captured && !sw.override_active)
                            continue;

                        slotAllIndices.push_back(static_cast<int>(s));

                        // Group rows by hash-of-submesh-name rather
                        // than by submesh_stable_id. Two SwatchOverride
                        // entries can share submesh_name but carry
                        // different submesh_stable_ids: JSON-loaded
                        // placeholders synthesise stable_id via
                        // FNV(name) in populate_from_persisted, while
                        // engine-inserted rows (via the setter's
                        // lookup_or_insert) carry the live matInst's
                        // engine-side stable_id. Without this name-
                        // based key the picker shows the same submesh
                        // as two separate regions.
                        auto fnv1a64_name = [](const char *p) noexcept
                            -> std::uint64_t {
                            std::uint64_t h = 0xcbf29ce484222325ULL;
                            while (*p)
                            {
                                h ^= static_cast<std::uint8_t>(*p++);
                                h *= 0x100000001b3ULL;
                            }
                            return h;
                        };
                        const std::uint64_t sid =
                            (sw.submesh_name[0] != '\0')
                                ? fnv1a64_name(sw.submesh_name)
                                : sw.submesh_stable_id;
                        auto &rv = regions[sid];
                        if (rv.stable_id == 0)
                        {
                            initRegion(rv);
                            rv.stable_id = sid;
                            rv.tpl       = sw.template_id;
                        }
                        const int layer =
                            Transmog::ColorOverride::TokenTable::token_layer(sw.token_id);
                        const int ch =
                            Transmog::ColorOverride::TokenTable::token_channel(sw.token_id);
                        if (layer >= 0 && layer <= 4)
                        {
                            if (ch >= 0 && ch <= 2)
                                rv.layerSlot[layer][ch] = static_cast<int>(s);
                            else
                                rv.layerSingletons[layer].push_back(
                                    static_cast<int>(s));
                        }
                        else
                        {
                            rv.miscIndices.push_back(static_cast<int>(s));
                        }
                    }

                    // Render swatch sub-block: override toggle, picker,
                    // tooltip. Reused for both linked and per-channel
                    // rendering. `cascadeIdx[]` lists the indices that
                    // should receive the colour edit; first index is
                    // the "primary" displayed.
                    auto renderSwatchControls =
                        [&](const int *cascadeIdx, int cascadeCount,
                            const char *channelLabel)
                    {
                        if (cascadeCount <= 0 || cascadeIdx[0] < 0) return;
                        auto &primary = dyeSlot.swatches[cascadeIdx[0]];
                        ImGui::PushID(cascadeIdx[0] + 200);

                        if (channelLabel && channelLabel[0])
                        {
                            ImGui::TextUnformatted(channelLabel);
                            ImGui::SameLine(0.0f, 4.0f);
                        }

                        const bool prevActive = primary.override_active;
                        bool active = prevActive;
                        // Bordered checkbox per UX request.
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                        const bool toggled = ImGui::Checkbox("##sw_on", &active);
                        ImGui::PopStyleVar();
                        if (toggled)
                        {
                            for (int k = 0; k < cascadeCount; ++k)
                            {
                                int idx = cascadeIdx[k];
                                if (idx < 0) continue;
                                auto &row = dyeSlot.swatches[idx];
                                // Skip tick-on for rows whose def
                                // was never captured -- otherwise
                                // we'd set row.r/g/b = (0,0,0) and
                                // the substitute path would write
                                // black for that token, shifting the
                                // rendered colour away from the
                                // engine's natural blend. Affects
                                // assets whose engine pipeline
                                // emits writes for only a subset of
                                // the 9-prop chord (e.g. orcumer
                                // armours ship _tintColor + _detail
                                // but no _dyeingColorMask). Tick-off
                                // (active=false) always runs so the
                                // user can always un-override.
                                if (active && !row.default_captured)
                                    continue;
                                row.override_active = active;
                                if (active && !prevActive)
                                {
                                    row.r = row.def_r;
                                    row.g = row.def_g;
                                    row.b = row.def_b;
                                }
                                // Mirror to PendingOverrides so the
                                // slot-agnostic substitute path picks
                                // up this row's new state. Active=>
                                // write current RGB into pending;
                                // inactive=>erase so substitute stops
                                // firing for this (submesh, token).
                                if (active)
                                    mirror_override_to_pending(
                                        static_cast<int>(i),
                                        static_cast<std::size_t>(idx),
                                        row.r, row.g, row.b);
                                else
                                    erase_override_from_pending(
                                        static_cast<int>(i),
                                        static_cast<std::size_t>(idx));
                            }
                            // Override-active checkbox toggle is a
                            // colour change (rows go default to user
                            // override or vice-versa), so retick
                            // unconditionally with the same semantics
                            // as the picker commits below.
                            if (!Transmog::ColorOverride::Reinit::
                                    any_slot_reinit_active())
                            {
                                flag_enabled().store(true, std::memory_order_relaxed);
                                Transmog::ColorOverride::Reinit::
                                    schedule_color_commit_retick(
                                        static_cast<int>(i));
                            }
                            // Tick toggle flips override_active, which
                            // also flips whether get_persistable_state
                            // includes the row. The picker applies
                            // overrides live but does NOT write JSON
                            // -- dye_dirty lights up the top-level
                            // "Save *" button so the user commits to
                            // disk explicitly. Preset switch with
                            // dirty=true discards pending edits.
                            Transmog::dye_dirty().store(
                                true, std::memory_order_release);
                        }
                        ImGui::SameLine(0.0f, 2.0f);

                        const bool showUser = active;
                        float rgb[3];
                        if (showUser)
                        {
                            rgb[0] = primary.r / 255.0f;
                            rgb[1] = primary.g / 255.0f;
                            rgb[2] = primary.b / 255.0f;
                        }
                        else
                        {
                            rgb[0] = primary.def_r / 255.0f;
                            rgb[1] = primary.def_g / 255.0f;
                            rgb[2] = primary.def_b / 255.0f;
                        }

                        // True circle swatch: ImGui's `ColorEdit3` /
                        // `ColorButton` use `RenderFrame` whose
                        // `PathRect` clamps rounding to dim/2 - 1,
                        // leaving a ~2px flat zone in the middle even
                        // with `FrameRounding = 999`. To get a real
                        // circle we draw it ourselves with
                        // `ImDrawList::AddCircleFilled`, using an
                        // InvisibleButton for hit-testing in the
                        // override-on state and a Dummy for the
                        // display-only off state. Trade-off: lost
                        // ColorEdit3's right-click options menu /
                        // drag-drop, gained a real circle and full
                        // off-state opacity.
                        const float diameter = ImGui::GetFrameHeight();
                        const ImVec2 cursorPos = ImGui::GetCursorScreenPos();
                        const ImVec2 sw_center(cursorPos.x + diameter * 0.5f,
                                               cursorPos.y + diameter * 0.5f);
                        const float sw_radius = diameter * 0.5f - 1.0f;
                        const ImVec4 sw_colVec(rgb[0], rgb[1], rgb[2], 1.0f);
                        const ImU32 sw_colU =
                            ImGui::ColorConvertFloat4ToU32(sw_colVec);
                        const ImU32 sw_borderU =
                            ImGui::GetColorU32(ImGuiCol_Border);

                        // The swatch itself is always clickable. If
                        // the user clicks it while override is OFF,
                        // auto-tick override (cascade across linked
                        // rows), pre-fill from captured default, then
                        // open the picker. End-state matches ticking
                        // the checkbox first, just one click instead
                        // of two.
                        const bool sw_clicked = ImGui::InvisibleButton(
                            "##sw_btn",
                            ImVec2(diameter, diameter));
                        const bool sw_hovered = ImGui::IsItemHovered();

                        ImDrawList *sw_dl = ImGui::GetWindowDrawList();
                        sw_dl->AddCircleFilled(sw_center, sw_radius,
                                               sw_colU, 32);
                        sw_dl->AddCircle(sw_center, sw_radius,
                                         sw_borderU, 32, 1.0f);

                        // Default-colour reference dot next to the
                        // active picker swatch. Shows the engine-
                        // captured default (def_r/g/b) so the user
                        // has a quick visual anchor for "what was the
                        // asset's original colour?" -- useful when
                        // dialing in a custom colour or deciding
                        // whether to revert an override. Display-only,
                        // non-interactive. Skipped if no default has
                        // been captured yet (placeholders pre-promote
                        // or rows never touched by the engine).
                        if (primary.default_captured)
                        {
                            ImGui::SameLine(0.0f, 4.0f);
                            const float refDiam = diameter * 0.70f;
                            const ImVec2 refCursor =
                                ImGui::GetCursorScreenPos();
                            // Centre the smaller circle vertically
                            // against the larger picker swatch.
                            const float yOff = (diameter - refDiam) * 0.5f;
                            const ImVec2 refCenter(
                                refCursor.x + refDiam * 0.5f,
                                refCursor.y + yOff + refDiam * 0.5f);
                            const float refRadius = refDiam * 0.5f - 1.0f;
                            const ImVec4 refColVec(
                                primary.def_r / 255.0f,
                                primary.def_g / 255.0f,
                                primary.def_b / 255.0f,
                                1.0f);
                            const ImU32 refColU =
                                ImGui::ColorConvertFloat4ToU32(refColVec);
                            ImGui::Dummy(ImVec2(refDiam, diameter));
                            const bool refHovered = ImGui::IsItemHovered();
                            sw_dl->AddCircleFilled(refCenter, refRadius,
                                                   refColU, 24);
                            sw_dl->AddCircle(refCenter, refRadius,
                                             sw_borderU, 24, 1.0f);
                            if (refHovered)
                            {
                                char refTip[48];
                                std::snprintf(refTip, sizeof(refTip),
                                              "Asset default #%02X%02X%02X",
                                              primary.def_r,
                                              primary.def_g,
                                              primary.def_b);
                                ImGui::BeginTooltip();
                                ImGui::TextUnformatted(refTip);
                                ImGui::EndTooltip();
                            }
                        }

                        if (sw_clicked)
                        {
                            if (!showUser)
                            {
                                for (int k = 0; k < cascadeCount; ++k)
                                {
                                    int idx = cascadeIdx[k];
                                    if (idx < 0) continue;
                                    auto &row = dyeSlot.swatches[idx];
                                    // Same gate as the checkbox
                                    // path: rows without a captured
                                    // def must stay un-overridden,
                                    // else substitute writes 0,0,0
                                    // and the rendered colour shifts.
                                    if (!row.default_captured)
                                        continue;
                                    row.override_active = true;
                                    row.r = row.def_r;
                                    row.g = row.def_g;
                                    row.b = row.def_b;
                                    // Mirror to pending so the
                                    // substitute path agrees with
                                    // the row's new default RGB.
                                    mirror_override_to_pending(
                                        static_cast<int>(i),
                                        static_cast<std::size_t>(idx),
                                        row.r, row.g, row.b);
                                }
                                if (!Transmog::ColorOverride::Reinit::
                                        any_slot_reinit_active())
                                {
                                    flag_enabled().store(true, std::memory_order_relaxed);
                                    // Tear-down + reapply: engine
                                    // short-circuits a same-state
                                    // single apply, so we toggle
                                    // m.active off→on around a wait
                                    // window. See dye_override.cpp
                                    // SlotReinitState (CommitRetick).
                                    // Unconditional on swatch click
                                    // (override flips on); no
                                    // s_autoApply gate.
                                    Transmog::ColorOverride::Reinit::
                                        schedule_color_commit_retick(
                                            static_cast<int>(i));
                                }
                                // Auto-tick on click flipped
                                // override_active. The picker writes
                                // live but does NOT touch JSON;
                                // dye_dirty lights up the top-level
                                // "Save *" button so the user commits
                                // to disk explicitly. Preset switch
                                // with dirty=true discards pending
                                // edits.
                                Transmog::dye_dirty().store(
                                    true, std::memory_order_release);
                            }
                            ImGui::OpenPopup("##sw_picker");
                        }
                        if (ImGui::BeginPopup("##sw_picker"))
                        {
                            // Re-seed the picker each frame from
                            // primary's current values so live edits
                            // through the picker reflect back into
                            // the swatch correctly across frames.
                            float pickerRgb[3] = {
                                primary.r / 255.0f,
                                primary.g / 255.0f,
                                primary.b / 255.0f,
                            };
                            if (ImGui::ColorPicker3(
                                    "##sw_pick", pickerRgb,
                                    ImGuiColorEditFlags_NoSidePreview |
                                        ImGuiColorEditFlags_NoSmallPreview))
                            {
                                const std::uint8_t r = clampByte(pickerRgb[0]);
                                const std::uint8_t g = clampByte(pickerRgb[1]);
                                const std::uint8_t b = clampByte(pickerRgb[2]);
                                for (int k = 0; k < cascadeCount; ++k)
                                {
                                    int idx = cascadeIdx[k];
                                    if (idx < 0) continue;
                                    auto &row = dyeSlot.swatches[idx];
                                    row.r = r;
                                    row.g = g;
                                    row.b = b;
                                    // Mirror to pending so the
                                    // substitute path picks up the
                                    // user's edit on the next
                                    // engine write.
                                    mirror_override_to_pending(
                                        static_cast<int>(i),
                                        static_cast<std::size_t>(idx),
                                        r, g, b);
                                }
                                // The picker applies overrides live
                                // but does NOT write JSON; dye_dirty
                                // lights up the top-level "Save *"
                                // button so the user commits to disk
                                // explicitly. Preset switch with
                                // dirty=true discards pending edits.
                                Transmog::dye_dirty().store(
                                    true, std::memory_order_release);
                                // Trigger a single-pass tear-down +
                                // reapply so the engine re-builds the
                                // carrier matInst with the new colour.
                                // Fires unconditionally on every
                                // colour commit -- independent of the
                                // "Instant Apply" checkbox which
                                // governs hover/pick-apply only.
                                // Coalesces with any in-flight retick
                                // so a 60Hz drag fires ~1 retick per
                                // ~1.9s cycle, not 60.
                                if (!Transmog::ColorOverride::Reinit::
                                        any_slot_reinit_active())
                                {
                                    flag_enabled().store(true,
                                        std::memory_order_relaxed);
                                    Transmog::ColorOverride::Reinit::
                                        schedule_color_commit_retick(
                                            static_cast<int>(i));
                                }
                            }
                            ImGui::EndPopup();
                        }
                        if (sw_hovered)
                        {
                            ImGui::BeginTooltip();
                            const char *tokenName =
                                Transmog::ColorOverride::TokenTable::token_label_for(primary.token_id);
                            char tokenBuf[64];
                            if (tokenName)
                            {
                                std::snprintf(tokenBuf, sizeof(tokenBuf),
                                              "%s (0x%04X)", tokenName, primary.token_id);
                            }
                            else
                            {
                                std::snprintf(tokenBuf, sizeof(tokenBuf),
                                              "0x%04X", primary.token_id);
                            }
                            char tipBuf[256];
                            if (cascadeCount > 1)
                            {
                                std::snprintf(tipBuf, sizeof(tipBuf),
                                              "Linked R/G/B  %s\n"
                                              "submesh 0x%016llX  tpl 0x%04X\n"
                                              "asset def #%02X%02X%02X  blend=%u/255",
                                              tokenBuf,
                                              static_cast<unsigned long long>(primary.submesh_stable_id),
                                              static_cast<unsigned>(primary.template_id),
                                              primary.def_r, primary.def_g, primary.def_b, primary.def_a);
                            }
                            else
                            {
                                std::snprintf(tipBuf, sizeof(tipBuf),
                                              "%s\n"
                                              "submesh 0x%016llX  tpl 0x%04X\n"
                                              "asset def #%02X%02X%02X  blend=%u/255",
                                              tokenBuf,
                                              static_cast<unsigned long long>(primary.submesh_stable_id),
                                              static_cast<unsigned>(primary.template_id),
                                              primary.def_r, primary.def_g, primary.def_b, primary.def_a);
                            }
                            ImGui::TextUnformatted(tipBuf);
                            ImGui::EndTooltip();
                        }
                        ImGui::PopID();
                    };

                    // Batch recolor helper. Renders a clickable circle
                    // and on click opens a picker popup. Each value
                    // change cascades the chosen RGB to every index in
                    // `cascade`, ticking `override_active = true`.
                    //
                    // displayColor is derived from cascade[0] each
                    // frame (current r/g/b if override active, else
                    // captured def). This is the same trick the per-
                    // swatch picker uses: writes propagate through
                    // cascade[0] to displayColor next frame, so
                    // picker drag stays smooth. If we re-seeded from
                    // a fixed colour (e.g. the cluster's def) each
                    // frame, the wheel would snap back every frame
                    // and drag would jitter.
                    auto recolorBatch = [&](const char *idTag,
                                            const std::vector<int> &cascade)
                    {
                        if (cascade.empty()) return;
                        const auto &first = dyeSlot.swatches[cascade[0]];
                        const ImVec4 displayColor = first.override_active
                            ? ImVec4(first.r / 255.0f, first.g / 255.0f, first.b / 255.0f, 1.0f)
                            : ImVec4(first.def_r / 255.0f, first.def_g / 255.0f, first.def_b / 255.0f, 1.0f);

                        ImGui::PushID(idTag);
                        const float bDiameter = ImGui::GetFrameHeight();
                        const ImVec2 bCursor = ImGui::GetCursorScreenPos();
                        const ImVec2 bCenter(bCursor.x + bDiameter * 0.5f,
                                             bCursor.y + bDiameter * 0.5f);
                        const float bRadius = bDiameter * 0.5f - 1.0f;
                        const ImU32 bColU =
                            ImGui::ColorConvertFloat4ToU32(displayColor);
                        const ImU32 bBorderU =
                            ImGui::GetColorU32(ImGuiCol_Border);
                        const bool bClicked = ImGui::InvisibleButton(
                            "##rb_btn",
                            ImVec2(bDiameter, bDiameter));
                        ImDrawList *bDl = ImGui::GetWindowDrawList();
                        bDl->AddCircleFilled(bCenter, bRadius, bColU, 32);
                        bDl->AddCircle(bCenter, bRadius, bBorderU, 32, 1.0f);
                        if (bClicked)
                            ImGui::OpenPopup("##rb_pop");
                        if (ImGui::BeginPopup("##rb_pop"))
                        {
                            float prgb[3] = {
                                displayColor.x,
                                displayColor.y,
                                displayColor.z,
                            };
                            if (ImGui::ColorPicker3(
                                    "##rb_pick", prgb,
                                    ImGuiColorEditFlags_NoSidePreview |
                                        ImGuiColorEditFlags_NoSmallPreview))
                            {
                                const std::uint8_t pr = clampByte(prgb[0]);
                                const std::uint8_t pg = clampByte(prgb[1]);
                                const std::uint8_t pb = clampByte(prgb[2]);
                                for (int idx : cascade)
                                {
                                    if (idx < 0) continue;
                                    auto &row = dyeSlot.swatches[idx];
                                    row.override_active = true;
                                    row.r = pr;
                                    row.g = pg;
                                    row.b = pb;
                                    // Mirror to pending so the
                                    // substitute path picks up the
                                    // recolor-all edit.
                                    mirror_override_to_pending(
                                        static_cast<int>(i),
                                        static_cast<std::size_t>(idx),
                                        pr, pg, pb);
                                }
                                if (!Transmog::ColorOverride::Reinit::
                                        any_slot_reinit_active())
                                {
                                    flag_enabled().store(true, std::memory_order_relaxed);
                                    // Unconditional retick on
                                    // batch-recolor commit; no
                                    // s_autoApply gate (matches
                                    // per-row picker semantics).
                                    Transmog::ColorOverride::Reinit::
                                        schedule_color_commit_retick(
                                            static_cast<int>(i));
                                }
                                // Mark dirty for the explicit Save
                                // commit -- see the matching call
                                // site in renderSwatchControls for
                                // the full rationale.
                                Transmog::dye_dirty().store(
                                    true, std::memory_order_release);
                            }
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                    };

                    // --- Slot-level "Recolor all" + auto-clusters ---
                    // Single picker drives every visible swatch in the
                    // slot. Lets users do "make this whole helm red"
                    // without touching individual rows.
                    ImGui::TextUnformatted("Recolor all:");
                    ImGui::SameLine(0.0f, 6.0f);
                    recolorBatch("slot_all", slotAllIndices);

                    // "Reset slot": escape hatch for cross-slot bleed
                    // (e.g. Kairos cloak + boots sharing one material
                    // means whichever slot applies first absorbs both
                    // submeshes' rows; the other slot is permanently
                    // empty). Wipes the swatch table, clears freeze
                    // + carrier state, then triggers a fresh apply
                    // for this slot. User picks for this slot are
                    // lost; cross-slot pollution is gone.
                    ImGui::SameLine(0.0f, 12.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    const bool resetClicked =
                        ImGui::SmallButton("Reset slot##sw_reset");
                    ImGui::PopStyleVar();
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(
                            "Wipe ALL captured swatches for THIS slot.\n"
                            "Use when cross-slot bleed has misattributed\n"
                            "rows from another transmog item (common with\n"
                            "Kairos and other shared-material sets).\n\n"
                            "Triggers a fresh apply on this slot, which\n"
                            "re-captures cleanly. Loses any colours you've\n"
                            "picked for this slot.");
                        ImGui::EndTooltip();
                    }
                    if (resetClicked)
                    {
                        Transmog::ColorOverride::SwatchTable::wipe_swatch_table_for_slot(
                            static_cast<int>(i));
                        Transmog::ColorOverride::SwatchTable::clear_dye_state_for_slot(
                            static_cast<int>(i));
                        Transmog::ColorOverride::PendingOverrides::clear_slot(
                            static_cast<int>(i));
                        Transmog::dye_dirty().store(
                            true, std::memory_order_release);
                        if (s_autoApply)
                        {
                            flag_enabled().store(true, std::memory_order_relaxed);
                            manual_apply_slot(i);
                        }
                    }

                    // "Revert": soft reset that keeps the captured swatch
                    // list intact (no Re-init needed afterwards), but
                    // flips every row's override_active=false so the
                    // engine's defaults render. Distinct from "Reset
                    // slot" which wipes the captured list entirely.
                    ImGui::SameLine(0.0f, 6.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    const bool revertClicked =
                        ImGui::SmallButton("Revert to default##sw_revert");
                    ImGui::PopStyleVar();
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(
                            "Revert ALL user overrides on THIS slot\n"
                            "back to engine defaults. Keeps the\n"
                            "captured swatch list intact (no Re-init\n"
                            "needed afterwards).\n\n"
                            "Use to compare your dye picks against\n"
                            "the original asset colours, or to undo\n"
                            "a slot's colour edits without losing\n"
                            "the captured row structure.");
                        ImGui::EndTooltip();
                    }
                    if (revertClicked)
                    {
                        for (auto &sw : dyeSlot.swatches)
                            sw.override_active = false;
                        // Drop the slot's queued pending-overrides
                        // map. PendingOverrides holds the JSON-loaded
                        // user picks and gets consulted on every
                        // successful lookup_or_insert -- when an
                        // engine write hits a matched row it calls
                        // set_override_active(true). Without clearing
                        // here, the schedule_color_commit_retick
                        // below would fire engine writes that
                        // immediately re-enable every JSON override
                        // we just cleared. With clearing, the retick
                        // sees empty pending -> override stays off
                        // -> substitute bails -> engine natural
                        // colour flows. Re-loading the preset (or
                        // switching away without saving) restores
                        // PendingOverrides from JSON, so the revert
                        // stays a pending change until Save commits.
                        Transmog::ColorOverride::PendingOverrides::clear_slot(
                            static_cast<int>(i));
                        if (!Transmog::ColorOverride::Reinit::any_slot_reinit_active())
                        {
                            flag_enabled().store(true, std::memory_order_relaxed);
                            Transmog::ColorOverride::Reinit::
                                schedule_color_commit_retick(
                                    static_cast<int>(i));
                        }
                        Transmog::dye_dirty().store(
                            true, std::memory_order_release);
                    }

                    // Auto-detected clusters: rows that share the same
                    // (layer, def_r, def_g, def_b). One picker drives
                    // all rows in the cluster. Wrapped in a collapsible
                    // tree node so users who don't want the suggestions
                    // can fold them away without us silently hiding
                    // them. Default-open since the suggestions are the
                    // whole point of the cluster bar.
                    // Advanced view toggle, per-character preference.
                    // Default off: render a merchant-like flat list,
                    // one picker per UNIQUE def colour (cluster of ≥1
                    // member). Tick to reveal per-region per-token
                    // trees with full shader-property granularity.
                    bool advancedView =
                        Transmog::ColorOverride::dye_advanced_view_get();
                    if (ImGui::Checkbox("Advanced view##dye_adv", &advancedView))
                    {
                        Transmog::ColorOverride::dye_advanced_view_set(advancedView);
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(
                            "Show per-region per-token trees with full\n"
                            "shader-property granularity beneath the\n"
                            "merchant-like flat picker list.\n\n"
                            "Default off -- most users get the same UX\n"
                            "as the merchant dye UI: one picker per\n"
                            "distinct asset-default colour. Tick this\n"
                            "only if you need to dye individual tokens\n"
                            "(e.g. tint vs. mask vs. detail layers\n"
                            "separately).");
                        ImGui::EndTooltip();
                    }

                    // "Show only modified" filter: per-slot session
                    // toggle that hides rows / clusters / regions
                    // whose `override_active` is false. Useful when an
                    // item exposes 30+ swatches and the user wants to
                    // revisit only the colours they actually changed.
                    {
                        auto &uiSlot = s_slotUI[i];
                        ImGui::SameLine();
                        ImGui::Checkbox("Modified only##dye_mod",
                                        &uiSlot.showOnlyModified);
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::BeginTooltip();
                            ImGui::TextUnformatted(
                                "Hide rows / regions whose colour is\n"
                                "still at the captured engine default.\n"
                                "Untick to see every swatch this slot\n"
                                "exposed during apply.");
                            ImGui::EndTooltip();
                        }
                    }

                    ImGui::Separator();

                    if (!advancedView)
                    {
                        // Merchant-style region-channel picker.
                        // Per region: 1-3 ColorPicker3s, one per
                        // mask-texture channel suffix (R/G/B).
                        // Each picker cascade-writes its color to every
                        // captured swatch sharing that channel-suffix
                        // family in this region -- mirroring the engine
                        // merchant's `sub_14274A3C0` chord (9 token
                        // writes per channel: dyeingColorMask + detail
                        // layers + tintColor + 5 mask-overlay layers).
                        //
                        // The R/G/B suffixes are NOT R/G/B colour
                        // components -- they're the engine's 3 mask-
                        // texture channels, each holding a full RGB
                        // colour for one physical region of the mesh.
                        // Materials with masks active on only some
                        // channels get fewer pickers (channels with no
                        // captured rows are hidden).
                        const bool filterMod = s_slotUI[i].showOnlyModified;
                        for (auto &kv : regions)
                        {
                            auto &rv = kv.second;

                            // Walk swatches in this region and bucket
                            // by channel_kind (0=R, 1=G, 2=B, else=-1).
                            // Misc tokens with no detectable channel
                            // are skipped here; they show in Advanced
                            // view for power users.
                            //
                            // Layer iteration order matches the
                            // engine merchant function sub_14274A3C0.
                            // The merchant writes a 9-prop "chord"
                            // per channel in this priority order:
                            //   0  _dyeingColorMask         <-- L=1
                            //   1  _dyeingDetailLayerColorMask <-- L=2
                            //   2..7 _detail* / _dyeingCustom* mask layers
                            //   8  _tintColor              <-- L=0
                            // `primary` in renderSwatchControls is
                            // cascadeIdx[0], so whatever lands first
                            // in chRows[ch] drives the picker's
                            // displayed reference colour. PC armour
                            // ships _dyeingColorMask (L=1) so the
                            // picker shows the merchant's primary
                            // base; orcumer / mob assets ship only
                            // detail+tint, so the picker falls back
                            // to _dyeingDetailLayerColorMask (L=2).
                            //   L=1 (mask)    first
                            //   L=2 (detail)  second
                            //   L=0 (tint)    third
                            //   L=4 (scratch) fourth
                            //   L=3 (hair)    fifth
                            // Cascade still writes to every captured
                            // token sharing the channel suffix --
                            // only the displayed reference changes.
                            // Per-layer singletons (ch=-1 tokens like
                            // `_dyeingDetailLayerColorBlend`) never
                            // make it into the merchant chord -- they
                            // surface only in Advanced view.
                            std::vector<int> chRows[3];
                            static const int k_layerOrder[5] =
                                {1, 2, 0, 4, 3};
                            for (int Li = 0; Li < 5; ++Li)
                            {
                                const int L = k_layerOrder[Li];
                                for (int C = 0; C < 3; ++C)
                                {
                                    const int sIdx = rv.layerSlot[L][C];
                                    if (sIdx < 0) continue;
                                    chRows[C].push_back(sIdx);
                                }
                            }

                            // Hair singletons surfaced in simple view
                            // too -- merchants don't ship a hair
                            // picker, but users want one-click hair
                            // recolor from the same panel as armor
                            // (no need to flip into Advanced).
                            const auto &hairRows = rv.layerSingletons[3];
                            const bool anyChannel = !chRows[0].empty()
                                || !chRows[1].empty()
                                || !chRows[2].empty()
                                || !hairRows.empty();
                            if (!anyChannel) continue;

                            // "Modified only" filter: skip region if
                            // no channel / hair row has an override.
                            if (filterMod)
                            {
                                bool anyOverridden = false;
                                for (int ch = 0; ch < 3 && !anyOverridden; ++ch)
                                    for (int idx : chRows[ch])
                                        if (dyeSlot.swatches[idx].override_active)
                                        { anyOverridden = true; break; }
                                if (!anyOverridden)
                                    for (int idx : hairRows)
                                        if (dyeSlot.swatches[idx].override_active)
                                        { anyOverridden = true; break; }
                                if (!anyOverridden) continue;
                            }

                            // Region header: prefer captured submesh
                            // name; fall back to hair-aware label or
                            // "Region N" exactly as the Advanced-mode
                            // header does (kept consistent so a user
                            // toggling views doesn't see different
                            // labels for the same region).
                            const char *sm = nullptr;
                            for (int L = 0; L < 5 && !sm; ++L)
                                for (int C = 0; C < 3 && !sm; ++C)
                                {
                                    const int sIdx = rv.layerSlot[L][C];
                                    if (sIdx >= 0
                                        && dyeSlot.swatches[sIdx].submesh_name[0] != '\0')
                                        sm = dyeSlot.swatches[sIdx].submesh_name;
                                }
                            for (int L = 0; L < 5 && !sm; ++L)
                                for (int sIdx2 : rv.layerSingletons[L])
                                    if (dyeSlot.swatches[sIdx2].submesh_name[0] != '\0')
                                    { sm = dyeSlot.swatches[sIdx2].submesh_name; break; }
                            if (!sm && !rv.miscIndices.empty()
                                && dyeSlot.swatches[rv.miscIndices.front()]
                                       .submesh_name[0] != '\0')
                                sm = dyeSlot.swatches[rv.miscIndices.front()]
                                         .submesh_name;
                            char hdrBuf[128];
                            if (sm)
                            {
                                std::snprintf(hdrBuf, sizeof(hdrBuf),
                                              "%s", sm);
                            }
                            else
                            {
                                bool hasHair = !rv.layerSingletons[3].empty();
                                if (!hasHair)
                                    for (int C = 0; C < 3 && !hasHair; ++C)
                                        if (rv.layerSlot[3][C] >= 0) hasHair = true;
                                std::snprintf(hdrBuf, sizeof(hdrBuf),
                                              hasHair
                                                ? "[Hair -- hidden under helm]"
                                                : "[Unnamed]");
                            }

                            ImGui::PushID(static_cast<int>(rv.stable_id) ^
                                          static_cast<int>(rv.stable_id >> 32) ^
                                          0xC0DE);
                            // Collapsible region header. Closed by
                            // default to match Advanced mode -- users
                            // expand only the regions they want to
                            // touch. The header text matches the
                            // Advanced-mode `(tpl 0xXXXX)` format so
                            // toggling views keeps the same labels.
                            char treeBuf[160];
                            std::snprintf(treeBuf, sizeof(treeBuf),
                                          "%s  (tpl 0x%04X)",
                                          hdrBuf, rv.tpl);
                            // Default-open: the Color Override tab
                            // is now the wrapper, so collapsing each
                            // submesh by default just hides what the
                            // user came to see.
                            const bool open = ImGui::TreeNodeEx(treeBuf,
                                ImGuiTreeNodeFlags_SpanAvailWidth
                                | ImGuiTreeNodeFlags_DefaultOpen);
                            // Channel-coverage marker. Stays on the
                            // header row so it's visible whether the
                            // user expands the region or not. See
                            // `dye_picker_compute_channel_gap_tip`.
                            {
                                int present[3][3] = {{0}};
                                for (int L = 0; L < 3; ++L)
                                    for (int C = 0; C < 3; ++C)
                                        present[L][C] =
                                            (rv.layerSlot[L][C] >= 0) ? 1 : 0;
                                char gapTip[600];
                                if (dye_picker_compute_channel_gap_tip(
                                        present, gapTip, sizeof(gapTip)))
                                {
                                    ImGui::SameLine();
                                    ui_text_colored(
                                        ImVec4(0.95f, 0.65f, 0.20f, 1.0f),
                                        "(!)");
                                    if (ImGui::IsItemHovered())
                                    {
                                        char fullTip[900];
                                        std::snprintf(fullTip, sizeof(fullTip),
                                            "This submesh's shader doesn't "
                                            "expose all dye channels.\n"
                                            "You can still edit the channels "
                                            "that ARE present, but missing "
                                            "channels keep their baked default"
                                            " -- limiting how dark / bright "
                                            "you can drive the rendered "
                                            "color.\n"
                                            "\n"
                                            "Gaps:\n%s", gapTip);
                                        ui_tooltip(fullTip);
                                    }
                                }
                            }
                            if (!open)
                            {
                                ImGui::PopID();
                                continue;
                            }

                            // Render one circle-swatch + popup picker
                            // per active channel via the shared
                            // renderSwatchControls helper. The helper
                            // already implements:
                            //   - bordered override-active checkbox
                            //   - manually-drawn circle swatch button
                            //     (click to open popup ColorPicker3)
                            //   - auto-tick + retick on click
                            //   - persistence save on commit
                            // We pass it the channel's full cascade
                            // list so a single picker commit ripples
                            // to all 1-9 captured shader tokens that
                            // share this channel suffix.
                            static const char *k_channelLabels[3] =
                                {"R", "G", "B"};
                            for (int ch = 0; ch < 3; ++ch)
                            {
                                if (chRows[ch].empty()) continue;
                                renderSwatchControls(
                                    chRows[ch].data(),
                                    static_cast<int>(chRows[ch].size()),
                                    k_channelLabels[ch]);
                            }
                            // Hair singletons (any `_hair*` token --
                            // `_hairDyeingColor`, `_hairDyeingScratch`,
                            // etc.). Each one gets its own row so the
                            // user can recolor hair / hair-overlay
                            // independently from a single cascade.
                            for (int idx : hairRows)
                            {
                                int single[1] = {idx};
                                renderSwatchControls(single, 1, "hair");
                            }
                            ImGui::TreePop();
                            ImGui::PopID();
                        }
                    }

                    // Render each region as a tree node, gated on
                    // the per-character advanced toggle. Default-off
                    // gives the merchant-style flat cluster view; tick
                    // to reveal full per-region per-token control.
                    if (advancedView) for (auto &kv : regions)
                    {
                        auto &rv = kv.second;
                        auto &rstate = ui.regionUI[rv.stable_id];

                        // "Show only modified" filter: skip region if
                        // no swatch inside it is user-overridden.
                        // Filter runs BEFORE PushID -- continue does
                        // not need a matching PopID.
                        if (ui.showOnlyModified)
                        {
                            bool anyOverridden = false;
                            for (int L = 0; L < 5 && !anyOverridden; ++L)
                                for (int C = 0; C < 3 && !anyOverridden; ++C)
                                {
                                    const int sIdx = rv.layerSlot[L][C];
                                    if (sIdx >= 0
                                        && dyeSlot.swatches[sIdx].override_active)
                                        anyOverridden = true;
                                }
                            for (int L = 0; L < 5 && !anyOverridden; ++L)
                                for (int sIdx : rv.layerSingletons[L])
                                    if (dyeSlot.swatches[sIdx].override_active)
                                    { anyOverridden = true; break; }
                            if (!anyOverridden)
                                for (int sIdx : rv.miscIndices)
                                    if (dyeSlot.swatches[sIdx].override_active)
                                    { anyOverridden = true; break; }
                            if (!anyOverridden) continue;
                        }

                        ImGui::PushID(static_cast<int>(rv.stable_id) ^
                                      static_cast<int>(rv.stable_id >> 32));
                        // Regions default-open: the Color Override
                        // tab is now the wrapper, so collapsing each
                        // submesh by default just hides what the user
                        // came to see.
                        ImGuiTreeNodeFlags flags =
                            ImGuiTreeNodeFlags_SpanAvailWidth
                            | ImGuiTreeNodeFlags_DefaultOpen;
                        char headerBuf[128];
                        // Header label: prefer the raw `_subMeshName`
                        // captured at apply time (e.g.
                        // `cd_phm_00_hel_00_0377_01`) so distinct
                        // submeshes that parse to the same friendly
                        // label remain distinguishable (the engine
                        // emits multiple variants like `..._0377_01`
                        // / `..._0377_04` that all map to "Helm #0377"
                        // under any prefix-only scheme). The leading
                        // `cd_` is stripped to save space. Falls back
                        // to "Region N" only when capture failed
                        // (Material has no parent wrapper, or wrapper
                        // has the empty-string sentinel at +0x28).
                        const char *sm = nullptr;
                        for (int L = 0; L < 5 && !sm; ++L)
                            for (int C = 0; C < 3 && !sm; ++C)
                            {
                                const int sIdx = rv.layerSlot[L][C];
                                if (sIdx >= 0
                                    && dyeSlot.swatches[sIdx].submesh_name[0] != '\0')
                                {
                                    sm = dyeSlot.swatches[sIdx].submesh_name;
                                }
                            }
                        for (int L = 0; L < 5 && !sm; ++L)
                            for (int sIdx2 : rv.layerSingletons[L])
                                if (dyeSlot.swatches[sIdx2].submesh_name[0] != '\0')
                                { sm = dyeSlot.swatches[sIdx2].submesh_name; break; }
                        if (!sm && !rv.miscIndices.empty()
                            && dyeSlot.swatches[rv.miscIndices.front()]
                                   .submesh_name[0] != '\0')
                        {
                            sm = dyeSlot.swatches[rv.miscIndices.front()]
                                     .submesh_name;
                        }
                        if (sm)
                        {
                            std::snprintf(headerBuf, sizeof(headerBuf),
                                          "%s  (tpl 0x%04X)",
                                          sm, rv.tpl);
                        }
                        else
                        {
                            // No submesh name captured -- the Material
                            // has no parent SkinnedMeshMaterialWrapper
                            // (or wrapper holds the empty-string
                            // module sentinel at +0x28). Most commonly
                            // this is the player's HAIR material:
                            // SkinnedMeshHair shader (tpl 0x3ADC on
                            // v1.06) is bound via a different parent
                            // path than armor materials. Hair re-
                            // renders every player frame so the
                            // setter captures it into every slot's
                            // carrier set during the 3-second apply
                            // window. Coloring has no visible effect
                            // under full-face helms (hair occluded)
                            // -- visible on head/face slots without
                            // a helm.
                            //
                            // Detect via layer-3 tokens (hair family)
                            // and label clearly so users don't waste
                            // time picking colors on it.
                            bool hasHair = !rv.layerSingletons[3].empty();
                            if (!hasHair)
                                for (int C = 0; C < 3 && !hasHair; ++C)
                                    if (rv.layerSlot[3][C] >= 0) hasHair = true;
                            if (hasHair)
                            {
                                std::snprintf(headerBuf, sizeof(headerBuf),
                                              "[Hair -- hidden under helm]  (tpl 0x%04X)",
                                              rv.tpl);
                            }
                            else
                            {
                                std::snprintf(headerBuf, sizeof(headerBuf),
                                              "[Unnamed]  (tpl 0x%04X)",
                                              rv.tpl);
                            }
                        }
                        const bool open = ImGui::TreeNodeEx(headerBuf, flags);
                        // Channel-coverage marker -- same audit as the
                        // simple-mode picker (see
                        // `dye_picker_compute_channel_gap_tip`).
                        {
                            int present[3][3] = {{0}};
                            for (int L = 0; L < 3; ++L)
                                for (int C = 0; C < 3; ++C)
                                    present[L][C] =
                                        (rv.layerSlot[L][C] >= 0) ? 1 : 0;
                            char gapTip[600];
                            if (dye_picker_compute_channel_gap_tip(
                                    present, gapTip, sizeof(gapTip)))
                            {
                                ImGui::SameLine();
                                ui_text_colored(
                                    ImVec4(0.95f, 0.65f, 0.20f, 1.0f),
                                    "(!)");
                                if (ImGui::IsItemHovered())
                                {
                                    char fullTip[900];
                                    std::snprintf(fullTip, sizeof(fullTip),
                                        "This submesh's shader doesn't expose "
                                        "all dye channels.\n"
                                        "You can still edit the channels that "
                                        "ARE present, but missing channels "
                                        "keep their baked default -- limiting "
                                        "how dark / bright you can drive the "
                                        "rendered color.\n"
                                        "\n"
                                        "Gaps:\n%s", gapTip);
                                    ui_tooltip(fullTip);
                                }
                            }
                        }
                        if (open)
                        {
                            // Iterate the 5 known families in a stable
                            // display order: tint, mask, detail,
                            // scratch, hair. Each family renders its
                            // R/G/B triplet (when present) followed by
                            // any per-family singleton properties
                            // (e.g. `_dyeingDetailLayerColorBlend`,
                            // `_hairDyeingColor`) under the same
                            // header so the user sees the full family
                            // together rather than scattered into a
                            // misc bucket.
                            static const int k_advLayerOrder[5] =
                                {0, 1, 2, 4, 3};
                            for (int Li = 0; Li < 5; ++Li)
                            {
                                const int L = k_advLayerOrder[Li];
                                int idxR = rv.layerSlot[L][0];
                                int idxG = rv.layerSlot[L][1];
                                int idxB = rv.layerSlot[L][2];
                                int present = 0;
                                if (idxR >= 0) ++present;
                                if (idxG >= 0) ++present;
                                if (idxB >= 0) ++present;
                                const auto &singletons = rv.layerSingletons[L];
                                if (present == 0 && singletons.empty())
                                    continue;

                                const char *layerName =
                                    Transmog::ColorOverride::TokenTable::layer_long_name(L);

                                if (present == 1)
                                {
                                    // Single-channel layer: no link UI
                                    // (nothing to link to). Show a
                                    // "<layer>·<channel>" label + swatch.
                                    const int onlyCh =
                                        (idxR >= 0) ? 0
                                        : (idxG >= 0) ? 1
                                        : 2;
                                    const int onlyIdx =
                                        (onlyCh == 0) ? idxR
                                        : (onlyCh == 1) ? idxG
                                        : idxB;
                                    char labelBuf[48];
                                    std::snprintf(labelBuf, sizeof(labelBuf),
                                        "%s\xC2\xB7%s",
                                        layerName,
                                        Transmog::ColorOverride::TokenTable::channel_short_name(onlyCh));
                                    ImGui::TextUnformatted(labelBuf);
                                    ImGui::SameLine(0.0f, 6.0f);
                                    int single[1] = {onlyIdx};
                                    renderSwatchControls(single, 1, "");
                                    ImGui::NewLine();
                                }
                                else if (present >= 2)
                                {
                                    // 2 or 3 channels present: show
                                    // link toggle. Linked mode drives
                                    // all PRESENT channels with one
                                    // colour (merchant behaviour);
                                    // unlinked exposes a per-channel
                                    // row for each present channel.
                                    char layerHdrBuf[48];
                                    char chList[8]; std::size_t chLen = 0;
                                    if (idxR >= 0 && chLen+1 < sizeof(chList)) chList[chLen++] = 'R';
                                    if (idxG >= 0 && chLen+1 < sizeof(chList)) { if (chLen) chList[chLen++] = '+'; chList[chLen++] = 'G'; }
                                    if (idxB >= 0 && chLen+1 < sizeof(chList)) { if (chLen) chList[chLen++] = '+'; chList[chLen++] = 'B'; }
                                    chList[chLen] = '\0';
                                    std::snprintf(layerHdrBuf, sizeof(layerHdrBuf),
                                        "%s \xC2\xB7 %s", layerName, chList);
                                    ImGui::TextUnformatted(layerHdrBuf);
                                    ImGui::SameLine(0.0f, 6.0f);

                                    bool &linked = rstate.linkRgb[L];
                                    ImGui::PushID(L * 31 + 7);
                                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                                    ImGui::Checkbox("link##lnk", &linked);
                                    ImGui::PopStyleVar();
                                    if (ImGui::IsItemHovered())
                                    {
                                        ImGui::BeginTooltip();
                                        ImGui::TextUnformatted(
                                            "When ON: pick one colour, all "
                                            "present R/G/B channel-suffix\n"
                                            "Properties get it (merchant "
                                            "behaviour).\nUntick to edit "
                                            "each channel independently.");
                                        ImGui::EndTooltip();
                                    }
                                    ImGui::SameLine(0.0f, 6.0f);

                                    if (linked)
                                    {
                                        int cascade[3];
                                        int n = 0;
                                        if (idxR >= 0) cascade[n++] = idxR;
                                        if (idxG >= 0) cascade[n++] = idxG;
                                        if (idxB >= 0) cascade[n++] = idxB;
                                        renderSwatchControls(cascade, n, "");
                                    }
                                    else
                                    {
                                        int order[3] = {idxR, idxG, idxB};
                                        bool firstShown = true;
                                        for (int c = 0; c < 3; ++c)
                                        {
                                            if (order[c] < 0) continue;
                                            if (!firstShown)
                                                ImGui::SameLine(0.0f, 8.0f);
                                            firstShown = false;
                                            int single[1] = {order[c]};
                                            renderSwatchControls(
                                                single, 1,
                                                Transmog::ColorOverride::TokenTable::channel_short_name(c));
                                        }
                                    }
                                    ImGui::PopID();
                                    ImGui::NewLine();
                                }

                                // Family singletons: tokens classified
                                // into this layer but with no R/G/B
                                // suffix (channel=-1). Show the actual
                                // shader-property name so it isn't
                                // confused with the R/G/B triplet.
                                for (int sIdx : singletons)
                                {
                                    auto &sw = dyeSlot.swatches[sIdx];
                                    const char *propName =
                                        Transmog::ColorOverride::TokenTable::
                                            token_label_for(sw.token_id);
                                    char nameBuf[64];
                                    if (propName && propName[0])
                                    {
                                        std::snprintf(nameBuf, sizeof(nameBuf),
                                                      "%s", propName);
                                    }
                                    else
                                    {
                                        std::snprintf(nameBuf, sizeof(nameBuf),
                                            "%s \xC2\xB7 0x%04X",
                                            layerName,
                                            sw.token_id & 0xFFFFu);
                                    }
                                    ImGui::TextUnformatted(nameBuf);
                                    ImGui::SameLine(0.0f, 6.0f);
                                    int single[1] = {sIdx};
                                    renderSwatchControls(single, 1, "");
                                    ImGui::NewLine();
                                }
                            }
                            // Tokens whose prefix didn't match any
                            // known family (layer == -1). Show the
                            // shader-property name when the interner
                            // captured it -- only fall back to the
                            // "misc 0xXXXX" hex label when we have no
                            // name at all. A resolvable name is more
                            // useful than a bare hex blob.
                            for (int idx : rv.miscIndices)
                            {
                                auto &mSw = dyeSlot.swatches[idx];
                                const char *propName =
                                    Transmog::ColorOverride::TokenTable::
                                        token_label_for(mSw.token_id);
                                char labelBuf[64];
                                if (propName && propName[0])
                                {
                                    std::snprintf(labelBuf, sizeof(labelBuf),
                                                  "%s", propName);
                                }
                                else
                                {
                                    std::snprintf(labelBuf, sizeof(labelBuf),
                                        "misc 0x%04X",
                                        mSw.token_id & 0xFFFFu);
                                }
                                ImGui::TextUnformatted(labelBuf);
                                ImGui::SameLine(0.0f, 6.0f);
                                int single[1] = {idx};
                                renderSwatchControls(single, 1, "");
                                ImGui::NewLine();
                            }
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                    } // close status-section block (was CollapsingHeader wrapper)

                    } // close if (detectedReady)
                        ImGui::EndTabItem();
                    } // close Color Override tab body
                    ImGui::EndTabBar();
                    } // close if (BeginTabBar) body
                    ImGui::EndPopup();
                } // close if (BeginPopup) body
                else
                {
                    // Popup closed -- re-arm the auto-trigger so the
                    // next open will re-fire on still-empty slots.
                    s_coAutoTriggered[i] = false;
                }
            } // close dye picker block (merged dye + color override)

            ImGui::PopID();
        }
    }

    ImGui::Separator();

    // --- Action buttons ---

    // Gate on WorldSystem so we don't spam "player not found" before
    // the first world load.
    const bool worldReady = Transmog::is_world_ready();
    ImGui::BeginDisabled(!worldReady);

    if (pending)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.75f, 0.55f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.90f, 0.65f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(1.00f, 0.72f, 0.20f, 1.0f));
    }
    // Body-mesh catalog is populated asynchronously by a boot thread
    // (heap walk ~1-5s). If the user has any body-mesh selection while
    // the catalog is still loading, block Apply -- the swap map cannot
    // resolve source wrappers without the catalog and an apply would
    // silently produce a no-op (or worse, partial substitution).
    namespace PWS = Transmog::PrefabWrapperSwap;
    const bool catalogLoading =
        PWS::has_any_selection() && !PWS::is_catalog_populated();

    ImGui::BeginDisabled(catalogLoading);
    if (ImGui::Button(pending ? "Apply All *" : "Apply All"))
    {
        flag_enabled().store(true, std::memory_order_relaxed);
        manual_apply();
    }
    ImGui::EndDisabled();
    if (pending)
        ImGui::PopStyleColor(3);

    if (catalogLoading)
    {
        ImGui::SameLine();
        ui_text_colored(ImVec4(0.85f, 0.75f, 0.20f, 1.0f),
                        "(loading body-mesh catalog...)");
    }

    ImGui::SameLine();

    if (ImGui::Button("Clear All"))
    {
        flag_enabled().store(false, std::memory_order_relaxed);
        manual_clear();
    }

    ImGui::SameLine();

    if (ImGui::Button("Capture Outfit"))
    {
        // Capture replaces the current state with the live equipped
        // outfit, so any session-only prefab picks must surrender too
        // -- otherwise the cyan label hides the captured gear and a
        // later "(none) prefab" would restore the stale pre-capture
        // carrier from `priorCarrierItemId`. Clears PWS + s_slotUI
        // state in one shot; capture_outfit then writes fresh mappings.
        clear_all_picked_prefabs_and_deactivate();
        capture_outfit();
    }

    ImGui::SameLine();

    if (pendingSave)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.80f, 0.40f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.95f, 0.52f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(1.00f, 0.60f, 0.25f, 1.0f));
    }
    if (ImGui::Button(pendingSave ? "Save *" : "Save"))
    {
        pm.replace_current_from_state();
        pm.save();
    }
    if (pendingSave)
        ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ui_tooltip(pendingSave
                       ? "Commit current slot rows into the active "
                         "preset (unsaved edits pending)."
                       : "Commit current slot rows into the active "
                         "preset.");

    ImGui::EndDisabled();

    if (!worldReady)
    {
        ImGui::SameLine();
        ui_text_disabled("(waiting for world load)");
    }

    // --- Status footer ---

    ImGui::Separator();
    ui_text_disabled("Status");

    if (slot_populator_fn())
        ui_text_colored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "SlotPopulator: READY");
    else
        ui_text_colored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "SlotPopulator: UNAVAILABLE");

    auto &mappings = slot_mappings();
    int activeCount = 0;
    for (std::size_t i = 0; i < k_slotCount; ++i)
    {
        if (mappings[i].active && mappings[i].targetItemId != 0)
            ++activeCount;
    }

    ui_text("Active slots: %d / %zu", activeCount, k_slotCount);
}

} // anonymous namespace
