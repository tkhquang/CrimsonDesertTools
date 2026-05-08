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

// Set by the including TU before draw_overlay_content runs.
// true  = standalone overlay (wider layout for high-DPI)
// false = ReShade addon tab  (compact layout)
// Cannot use FontGlobalScale to detect this because ReShade at 4K
// also sets FontGlobalScale > 1.
static bool s_standaloneMode = false;

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
    const float popupW = s_standaloneMode ? 720.0f : 560.0f;
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
        const std::string ov =
            pm.body_kind_of(pm.active_character());
        if (ov == "Male")
            charBody = BK::Male;
        else if (ov == "Female")
            charBody = BK::Female;
        else if (ov == "Both")
            charBody = BK::Both;
        else // "Auto" or unrecognised
            charBody =
                Transmog::ItemNameTable::body_kind_for_character(
                    pm.active_character());
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
            const float rowH = ImGui::GetTextLineHeightWithSpacing();
            const float scrollY = ImGui::GetScrollY();
            const float winH = ImGui::GetWindowHeight();
            const int total = static_cast<int>(totalShown);
            int firstVis = static_cast<int>(scrollY / rowH) - 1;
            int lastVis = static_cast<int>(
                (scrollY + winH) / rowH) + 2;
            if (firstVis < 0) firstVis = 0;
            if (lastVis > total) lastVis = total;
            if (firstVis > total) firstVis = total;

            if (firstVis > 0)
                ImGui::Dummy(ImVec2(0.0f, firstVis * rowH));
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
                ImGui::Dummy(ImVec2(0.0f, (total - lastVis) * rowH));
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
            // (the cross-slot browser block above). The user said the
            // per-slot prefab list bloats the items dropdown, so when
            // prefabMode is OFF we don't render rows at all -- only
            // expose the clear-override option when an active prefab
            // selection exists, so the user can always undo without
            // having to flip the prefabMode toggle on first.
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

// Identify the "Kairos" character -- the default-carrier preset whose
// chest item resolves to `cd_phm_00_ub_00_0435` wrappers (Kliff's
// canonical body-mesh source). The body-mesh hook's swap-map source
// only matches when this character's gear is loaded, so picking a
// body-mesh prefab while another character (e.g. Wellsknight) is
// active must temporarily borrow Kairos's slot itemIds.
//
// Resolution policy (in order):
//   1. Exact name match "Kliff" -- the canonical default carrier.
//   2. First character returned by character_names().
//
// Documented this way so adding a new "Kairos"-equivalent character
// later is a one-line change. Returns empty string only if the
// PresetManager has no characters at all (impossible at runtime).
[[nodiscard]] static std::string kairos_character_name()
{
    const auto &pm = Transmog::PresetManager::instance();
    const auto names = pm.character_names();
    for (const auto &n : names)
    {
        if (n == "Kliff")
            return n;
    }
    return names.empty() ? std::string{} : names.front();
}

// Borrow the Kairos preset's slot itemIds into the live slot_mappings.
// Body-mesh swap matches by SOURCE wrapper (e.g. `0435`), which is
// only resident when the matching carrier item is loaded. When the
// user picks a body-mesh prefab while controlling another character,
// the engine has Wellsknight wrappers loaded and the hook never fires.
// Forcing Kairos's itemIds into slot_mappings makes the engine load
// Kairos's gear, which loads the source wrappers, which the hook
// then substitutes with the chosen prefab.
//
// Side-effect contract (intentional):
//   - mutates slot_mappings()[*].targetItemId / .active for slots
//     where the user has an active body-mesh prefab pick.
//   - DOES NOT mutate PresetManager state (no set_active_character,
//     no save). The user's saved preset is untouched.
//
// Slots WITHOUT a pickedPrefabName are left alone so the user can
// stack a body-mesh override on chest while keeping Wellsknight's
// helm/gloves visible.
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
    // time. Previously hardcoded to "Kliff" -- which forced Kliff's
    // plate item onto Damiane / Oongka when they had any prefab
    // selection active, breaking their carrier-equip path.
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
                if (names[i] == pm.active_character())
                    selectedIdx = static_cast<int>(i);
            }

            // Character picker is READ-ONLY for now. Load-detect writes
            // active_character on every char-swap (ActorManager+0x30
            // byte) so manual picks get snapped back instantly. The
            // fix is to split active_character into controlled (game-
            // driven) and editing (UI-driven) states plus per-char
            // actor enumeration from user+0xD0..+0x108 -- see memory
            // note `project_per_companion_edit_deferred`. Until then
            // the combo just shows which character load-detect
            // currently thinks is controlled.
            ImGui::SetNextItemWidth(200.0f);
            ImGui::BeginDisabled(true);
            if (ImGui::Combo("Character##char_picker",
                             &selectedIdx,
                             cstrs,
                             static_cast<int>(n)))
            {
                const auto &pick = names[static_cast<std::size_t>(selectedIdx)];
                if (pick != pm.active_character())
                {
                    pm.set_active_character(pick);
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
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ui_tooltip("Read-only for now -- follows the character you "
                           "control in-game. Editing another character's "
                           "preset while controlling someone else is "
                           "planned (see per-companion-edit memory note).");

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
                pm.body_kind_of(pm.active_character());
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
                pm.set_body_kind_of(pm.active_character(),
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

        // Copy forks the current slot rows (from the active preset + any
        // edits) into a brand-new preset so the user can tweak without
        // overwriting the source.
        if (ImGui::Button("Copy"))
        {
            pm.duplicate_current();
            manual_apply();
        }
        if (ImGui::IsItemHovered())
            ui_tooltip("Duplicate the current slot rows into a new "
                       "preset, then apply it. Use after selecting a "
                       "preset and tweaking the rows to fork it.");

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


            // Bumped 2026-05-07 from 160/120 -> 200/170 so the longer
            // slot names ("TwoHandWeapon", "MainHand", "SubWeapon")
            // don't overlap the picker button.
            ImGui::SameLine(s_standaloneMode ? 200.0f : 170.0f);

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

                const float bw = s_standaloneMode ? 520.0f : 380.0f;
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
