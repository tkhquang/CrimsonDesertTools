// overlay_ui/helpers.cpp
//
// Single TU containing the variadic Text helpers (decl-in-header, def-in-cpp pattern to keep MSVC's inline-variadic
// COMDAT failure out of every consumer TU) and the small utility helpers that section files share.

#include "overlay_ui/helpers.hpp"
#include "overlay_ui/state.hpp"

#include "color_override/color_pending_overrides.hpp"
#include "color_override/color_swatch_table.hpp"
#include "carrier_defaults.hpp"
#include "prefab_wrapper_swap.hpp"
#include "preset_manager.hpp"
#include "slot_metadata.hpp"
#include "transmog.hpp"
#include "transmog_apply.hpp"

#pragma warning(push, 0)
#include <imgui.h>
#include <reshade.hpp>
#pragma warning(pop)

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace Transmog
{

    void ui_text(const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        ImGui::TextV(fmt, args);
        va_end(args);
    }

    void ui_text_disabled(const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        ImGui::TextDisabledV(fmt, args);
        va_end(args);
    }

    void ui_text_colored(const ImVec4 &col, const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        ImGui::TextColoredV(col, fmt, args);
        va_end(args);
    }

    void ui_tooltip(const char *text)
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(text);
        ImGui::EndTooltip();
    }

    bool name_contains_ci(const std::string &hay, const char *needle) noexcept
    {
        if (!needle || needle[0] == '\0')
            return true;
        const auto nlen = std::strlen(needle);
        if (nlen > hay.size())
            return false;
        for (std::size_t i = 0; i + nlen <= hay.size(); ++i)
        {
            std::size_t j = 0;
            for (; j < nlen; ++j)
            {
                const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(hay[i + j])));
                const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
                if (a != b)
                    break;
            }
            if (j == nlen)
                return true;
        }
        return false;
    }

    void mirror_override_to_pending(int slot, std::size_t idx, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
    {
        namespace SwT = Transmog::ColorOverride::SwatchTable;
        namespace PO = Transmog::ColorOverride::PendingOverrides;
        auto *ent = SwT::row(slot, idx);
        auto *ovr = SwT::override_row(slot, idx);
        if (ent == nullptr || ovr == nullptr)
            return;
        if (ovr->submesh_name[0] == '\0')
            return;
        const std::uint16_t tok = ent->token_id.load(std::memory_order_relaxed);
        if (tok == 0)
            return;
        PO::set_by_token_id(slot, ovr->submesh_name, tok, r, g, b);
    }

    void erase_override_from_pending(int slot, std::size_t idx) noexcept
    {
        namespace SwT = Transmog::ColorOverride::SwatchTable;
        namespace PO = Transmog::ColorOverride::PendingOverrides;
        auto *ent = SwT::row(slot, idx);
        auto *ovr = SwT::override_row(slot, idx);
        if (ent == nullptr || ovr == nullptr)
            return;
        if (ovr->submesh_name[0] == '\0')
            return;
        const std::uint16_t tok = ent->token_id.load(std::memory_order_relaxed);
        if (tok == 0)
            return;
        PO::erase_by_token_id(slot, ovr->submesh_name, tok);
    }

    // Clear all picked-prefab UI state and deactivate the body-mesh hook. Called BEFORE preset switches so the prefab
    // swap is torn down before the new preset's items are equipped; otherwise the hook would still substitute against
    // the old src wrappers, causing weird state across the transition.
    //
    // Clears UI/PWS selections for slots that had a body-mesh prefab pick and returns the per-slot mask of slots that
    // were cleared. The caller is responsible for the post-apply_to_state lastIds reconciliation:
    // for each cleared slot, if the new preset's carrier equals lastIds[i] the caller must zero lastIds[i] so the apply
    // pass tears down the prior body-mesh fake (otherwise the natural-pipeline cleanup hook never fires and the swap
    // stays visible). When carriers differ, lastIds is left intact so the regular tear_down_fake path runs.
    std::array<bool, k_slotCount> clear_all_picked_prefabs_and_deactivate()
    {
        namespace PWS = Transmog::PrefabWrapperSwap;
        std::array<bool, k_slotCount> hadPick{};
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            hadPick[i] = !s_slotUI[i].pickedPrefabName.empty();
            s_slotUI[i].pickedPrefabName.clear();
            s_slotUI[i].priorCarrierSaved = false;
            const auto tslot = static_cast<Transmog::TransmogSlot>(i);
            // Clear the body-mesh tgt selection. Source is left intact so future picks reuse the same auto-seeded src.
            const int curSrc = PWS::selection_src_index(tslot);
            PWS::set_selection(tslot, curSrc, -1);
        }
        // Selections only: the actual swap-map rebuild happens on the next apply via notify_apply_starting (apply-only
        // lifecycle, mirroring the carrier hybrid pattern).
        return hadPick;
    }

    bool dye_picker_compute_channel_gap_tip(const int present[3][3], char *out, std::size_t cap)
    {
        if (!out || cap < 8)
            return false;
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
            if (n == 3)
                continue; // full coverage
            if (n == 0 && L == 2)
                continue; // detail-mask absence is normal
            any = true;
            int written = 0;
            if (n == 0)
            {
                written = std::snprintf(out + off, cap - off,
                                        "- %s family not exposed (engine never writes "
                                        "these for this submesh)\n",
                                        kFamilyName[L]);
            }
            else
            {
                char missing[8];
                std::size_t mi = 0;
                for (int c = 0; c < 3 && mi < sizeof(missing) - 1; ++c)
                    if (!present[L][c])
                        missing[mi++] = kChan[c];
                missing[mi] = '\0';
                written = std::snprintf(out + off, cap - off,
                                        "- %s only exposes %d/3 channels (missing %s) -- "
                                        "the missing channel(s) stay at the asset's baked "
                                        "value\n",
                                        kFamilyName[L], n, missing);
            }
            if (written > 0)
                off += static_cast<std::size_t>(written);
            if (off >= cap - 1)
                break;
        }
        return any;
    }

    // Compare staged slot_mappings vs last_applied_ids to detect unsaved picker/checkbox edits. Returns true if ANY
    // slot's effective target (targetItemId when active, else 0) differs from what's currently committed to the game,
    // or the per-slot force-apply flag is set after a re-pick on the same carrier id.
    [[nodiscard]] bool has_pending_changes() noexcept
    {
        const auto &mappings = Transmog::slot_mappings();
        const auto &lastIds = Transmog::last_applied_ids();
        const auto &forceApply = Transmog::force_apply_pending();
        for (std::size_t i = 0; i < Transmog::k_slotCount; ++i)
        {
            // A force-apply flag means the user re-picked something on the same carrier id (e.g. a different body-mesh
            // prefab on the existing 0x1521 helm). The staged/lastIds comparison alone would miss it because the
            // carrier id is unchanged.
            if (forceApply[i])
                return true;
            const uint16_t staged = mappings[i].active ? mappings[i].targetItemId : uint16_t{0};
            if (staged != lastIds[i])
                return true;
        }
        return false;
    }

    // Returns true when slot_mappings differs from the active preset's persisted slots, i.e. the user edited rows in
    // the overlay but has not committed the change back into the JSON preset via Save. Used to tint the Save button so
    // unsaved work is visible.
    //
    // Prefab picks are session-only: when one is active, the slot's live `mappings[i].targetItemId` was force-borrowed
    // by `force_active_character_carrier_for_picked_slots()` to the active character's carrier id, NOT the user's saved
    // itemId. The saved itemId snapshot is captured into `priorCarrierActive/ItemId` on the first prefab pick. Compare
    // THAT against the JSON when a prefab pick is in flight, otherwise the Save button stays "Save *" forever even when
    // the saved state matches.
    [[nodiscard]] bool has_pending_save() noexcept
    {
        namespace PWS = Transmog::PrefabWrapperSwap;
        const auto *p = Transmog::PresetManager::instance().active_preset();
        if (!p)
            return false;

        // Dye is mutated directly on the preset, so slot_mappings cannot signal the divergence. The picker sets
        // dye_dirty on every edit; Save / preset-switch / load clear it.
        if (Transmog::dye_dirty().load(std::memory_order_acquire))
            return true;

        const auto &mappings = Transmog::slot_mappings();
        for (std::size_t i = 0; i < Transmog::k_slotCount; ++i)
        {
            // Disabled slots are forced off in the dispatcher and hidden from the picker; their saved row is left
            // untouched on disk and may diverge from `mappings[i]` (which we forced false). Don't let that divergence
            // light up the Save button.
            if (!Transmog::slot_enabled(i))
                continue;

            const auto tslot = static_cast<Transmog::TransmogSlot>(i);

            // Live prefab name from PWS (source of truth for the session, re-read every frame so picker mutations and
            // preset loads both surface here).
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

            // When the saved preset row has a prefab, the carrier itemId and active flag are implementation details:
            // save_path clears itemName (so saved itemId=0) while apply_to_state writes mappings[i].targetItemId =
            // auto-borrowed Kairos carrier. Comparing those two would light up Save permanently. Skip the active/itemId
            // compare for prefab rows -- the prefab name match above is the only check that's meaningful here.
            if (!p->slots[i].prefabName.empty())
                continue;

            // Plain carrier slot. If the user has a session-only prefab pick on this slot (saved preset has no prefab,
            // live PWS does), the prefab-name compare above already returned pending -- so we won't reach here.
            // priorCarrierSaved still matters for the inverse: user is mid-pick, hasn't applied, and the snapshot holds
            // the originally-saved carrier.
            const auto &ui = s_slotUI[i];
            const bool prefabBorrow = ui.priorCarrierSaved;
            const bool stagedActive = prefabBorrow ? ui.priorCarrierActive : mappings[i].active;
            const std::uint16_t stagedItemId = prefabBorrow ? ui.priorCarrierItemId : mappings[i].targetItemId;
            if (stagedActive != p->slots[i].active)
                return true;
            if (stagedItemId != p->slots[i].itemId)
                return true;
        }
        return false;
    }

    // Force the carrier item for each slot that has a session-only prefab pick to the editing-target character's
    // default carrier (from carrier_defaults.hpp). PWS swaps are keyed by the source wrapper that THAT character's body
    // emits, so the matching carrier must be resident at apply time or the swap silently no-ops.
    void force_active_character_carrier_for_picked_slots()
    {
        // For each slot with a picked prefab, force the carrier item to THE BODY THIS APPLY TARGETS. Use the central
        // current_apply_owner() helper so the picker writes the same axis the apply pipeline (and the PWS swap map)
        // will consult -- using a different axis here would install the wrong-family carrier on the targeted body and
        // let the swap-map row miss.
        auto &mappings = Transmog::slot_mappings();
        const auto &carrierOwner = Transmog::current_apply_owner();
        for (std::size_t i = 0; i < Transmog::k_slotCount; ++i)
        {
            if (s_slotUI[i].pickedPrefabName.empty())
                continue;
            const auto carrierId =
                Transmog::default_carrier_for_slot(static_cast<Transmog::TransmogSlot>(i), carrierOwner);
            if (carrierId == 0)
                continue; // Item catalog isn't ready yet -- skip.
            // Snapshot the slot's prior carrier state on the FIRST pick so a later body-mesh clear can restore it
            // (revert to the user's underlying preset gear instead of leaving Kliff plate stuck on the slot).
            // Subsequent re-picks don't overwrite -- the original state is what we want to revert to, not whatever
            // Kliff plate we last force-borrowed.
            if (!s_slotUI[i].priorCarrierSaved)
            {
                s_slotUI[i].priorCarrierSaved = true;
                s_slotUI[i].priorCarrierActive = mappings[i].active;
                s_slotUI[i].priorCarrierItemId = mappings[i].targetItemId;
            }
            // Snapshot prior state to detect "did this call actually change anything for this slot". Without the change
            // check below the function fires force_apply_pending on every already-stable slot every time the user
            // re-picks any other slot, causing unrelated slots to tear down + re-apply on each pick.
            const bool prevActive = mappings[i].active;
            const std::uint16_t prevTargetItemId = mappings[i].targetItemId;
            mappings[i].active = true;
            mappings[i].targetItemId = carrierId;
            const bool changed = (prevActive != mappings[i].active) || (prevTargetItemId != mappings[i].targetItemId);
            // When the new carrier id equals the last-applied id, the apply pipeline would short-circuit on the
            // `targetId == prevId` early-out and never re-run. Set the per-slot force-apply flag so the dispatcher
            // bypasses that early-out while keeping lastIds[i] intact -- Phase A `tear_down_fake` then runs against the
            // prior carrier and the engine's natural-pipeline hook cleans up the prior body-mesh tgt wrapper. In the
            // OTHER case (lastIds differ) the apply path already runs tear-old-then-equip-new naturally, so the flag is
            // harmless. Gated on `changed` so already-stable slots (carrier was already carrierId, mapping unchanged)
            // don't get force-applied when the user re-picks an unrelated slot.
            auto &lastIds = Transmog::last_applied_ids();
            if (changed && lastIds[i] == carrierId)
                Transmog::force_apply_pending()[i] = true;
        }
    }
} // namespace Transmog
