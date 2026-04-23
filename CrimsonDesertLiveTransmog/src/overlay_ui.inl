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
                                   std::size_t slotIdx)
{
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
    ImGui::Checkbox("Exact", &ui.exactFilter);
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
    // active+none path in apply_single_slot.
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

    int shown = 0;
    int filteredByCategory = 0;
    int filteredByUnsafe = 0;
    constexpr int k_maxShown = 512;
    for (const auto &e : entries)
    {
        if (ui.exactFilter && e.category != slotCategory)
        {
            ++filteredByCategory;
            continue;
        }
        const bool nonEquipment =
            (e.category == Transmog::TransmogSlot::Count);
        // Per-character body-kind filter (CE-verified 2026-04-21).
        // Armor rule classifier tokens partition the catalog into
        // disjoint body families:
        //   Male:   {0x0018, 0x0058, 0x02E3}
        //   Female: {0x0072, 0x0382, 0x0300}
        //   Horse / pet / wagon / dragon: separate token pools with
        //     zero overlap with humanoid. Flagged as NonHumanoid and
        //     hidden unconditionally -- these never render on a human
        //     skeleton (horse saddles, cat backpacks, etc.).
        const auto &pm = Transmog::PresetManager::instance();
        using BK = Transmog::ItemNameTable::BodyKind;
        // Resolve the active character's body kind: per-character
        // override in presets.json wins, "Auto" falls through to the
        // hardcoded default (Kliff/Oongka = Male, Damiane = Female).
        BK charBody;
        {
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
        const bool nonHumanoid = (e.bodyKind == BK::NonHumanoid);
        const bool ambiguousBody = (e.bodyKind == BK::Ambiguous);
        // Ambiguous items pass the body-match gate (still humanoid-
        // range), but get an amber tag in the color logic below so
        // the user knows render fidelity is inconsistent.
        const bool bodyMatches =
            !nonHumanoid && (
                ambiguousBody ||
                (e.bodyKind == BK::Generic) ||
                (e.bodyKind == BK::Both) ||
                (charBody == BK::Generic) ||
                (e.bodyKind == charBody));
        // Non-humanoid items are always treated as "incompatible" --
        // there's no toggle to show them; they'd never render.
        const bool incompatible = nonEquipment || nonHumanoid;
        const bool npcVariant = e.hasVariantMeta;
        if (ui.hideIncompatible && incompatible)
        {
            ++filteredByUnsafe;
            continue;
        }
        // Cross-body filter: hide items whose body type doesn't
        // match the active character. Default on; can be toggled
        // off for experimentation (they may still partially render
        // via the carrier path but often look broken).
        if (ui.hideBodyMismatch && !bodyMatches)
        {
            ++filteredByUnsafe;
            continue;
        }
        if (ui.hideVariants && npcVariant)
        {
            ++filteredByUnsafe;
            continue;
        }
        if (!name_contains_ci(e.name, ui.searchBuf) &&
            !name_contains_ci(e.displayName, ui.searchBuf))
            continue;
        if (shown >= k_maxShown)
            break;

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
        // passes bodyMatches above).
        const bool ambiguousCarrier = ambiguousBody && e.hasVariantMeta;
        if (crashRisk)
            tag = "non-player -- CRASH RISK";
        else if (crossBodyCarrier)
            tag = "carrier -- BODY MISMATCH";
        else if (ambiguousCarrier)
            tag = "carrier -- UNCERTAIN BODY";
        else if (usesCarrier)
            tag = "carrier";

        const bool isNavTarget = (shown == ui.navIndex);
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
            ui.navIndex = shown;
        }

        if (crashRisk || crossBodyCarrier || ambiguousCarrier || usesCarrier)
            ImGui::PopStyleColor(); // Header color

        ++shown;
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
    else if (shown >= k_maxShown)
    {
        ui_text_disabled("(truncated -- narrow your search)");
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
    for (std::size_t i = 0; i < Transmog::k_slotCount; ++i)
    {
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
[[nodiscard]] static bool has_pending_save() noexcept
{
    const auto *p = Transmog::PresetManager::instance().active_preset();
    if (!p)
        return false;

    const auto &mappings = Transmog::slot_mappings();
    for (std::size_t i = 0; i < Transmog::k_slotCount; ++i)
    {
        if (mappings[i].active != p->slots[i].active)
            return true;
        if (mappings[i].targetItemId != p->slots[i].itemId)
            return true;
    }
    return false;
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
                    pm.set_active_preset(i);
                    pm.apply_to_state();
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
            bool allActive = true;
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                if (!mappings[i].active)
                {
                    allActive = false;
                    break;
                }
            }
            if (ImGui::Checkbox("All", &allActive))
            {
                for (std::size_t i = 0; i < k_slotCount; ++i)
                    mappings[i].active = allActive;
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
            auto &m = mappings[i];
            auto &ui = s_slotUI[i];
            const char *slotLabel = slot_name(static_cast<TransmogSlot>(i));

            ImGui::PushID(static_cast<int>(i) + 100);

            if (ImGui::Checkbox(slotLabel, &m.active) && s_autoApply)
            {
                flag_enabled().store(true, std::memory_order_relaxed);
                manual_apply_slot(i);
            }

            ImGui::SameLine(s_standaloneMode ? 160.0f : 120.0f);

            // --- Picker button ---
            {
                std::string pickerLabel;
                if (m.targetItemId == 0)
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
                std::snprintf(btnLabel, sizeof(btnLabel),
                              "%s  [0x%04X]##pick", pickerLabel.c_str(),
                              m.targetItemId);

                const float bw = s_standaloneMode ? 520.0f : 380.0f;
                ImGui::SetNextItemWidth(bw);
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                                    ImVec2(0.02f, 0.5f));
                if (ImGui::Button(btnLabel, ImVec2(bw, 0.0f)))
                {
                    if (tableReady)
                    {
                        if (!s_keepSearchText)
                            ui.searchBuf[0] = '\0';
                        ImGui::OpenPopup("##slot_picker");
                    }
                }
                ImGui::PopStyleVar(); // ButtonTextAlign

                if (tableReady &&
                    draw_item_picker_popup(
                        "##slot_picker", ui,
                        static_cast<TransmogSlot>(i),
                        m.targetItemId,
                        s_autoApply, i))
                {
                    std::snprintf(ui.hexBuf, sizeof(ui.hexBuf), "%04X",
                                  m.targetItemId);

                    if (s_autoApply)
                    {
                        flag_enabled().store(true,
                                             std::memory_order_relaxed);
                        manual_apply_slot(i);
                    }

                    {
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

            ImGui::SetNextItemWidth(64.0f);
            if (ImGui::InputText(
                    "##hex", ui.hexBuf, sizeof(ui.hexBuf),
                    ImGuiInputTextFlags_CharsHexadecimal |
                        ImGuiInputTextFlags_CharsUppercase))
            {
                unsigned long val = std::strtoul(ui.hexBuf, nullptr, 16);
                m.targetItemId = static_cast<uint16_t>(val);
            }
            ui.editing = ImGui::IsItemActive();

            // Quick-clear button.
            ImGui::SameLine();
            if (ImGui::SmallButton("X##clr") && m.targetItemId != 0)
            {
                m.targetItemId = 0;
                std::snprintf(ui.hexBuf, sizeof(ui.hexBuf), "0000");
                if (s_autoApply)
                {
                    flag_enabled().store(true, std::memory_order_relaxed);
                    manual_apply_slot(i);
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
    if (ImGui::Button(pending ? "Apply All *" : "Apply All"))
    {
        flag_enabled().store(true, std::memory_order_relaxed);
        manual_apply();
    }
    if (pending)
        ImGui::PopStyleColor(3);

    ImGui::SameLine();

    if (ImGui::Button("Clear All"))
    {
        flag_enabled().store(false, std::memory_order_relaxed);
        manual_clear();
    }

    ImGui::SameLine();

    if (ImGui::Button("Capture Outfit"))
        capture_outfit();

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
