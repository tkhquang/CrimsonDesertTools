#include "color_swatch_table.hpp"
#include "color_carrier_set.hpp"
#include "color_pending_overrides.hpp"
#include "color_state.hpp"
#include "color_token_table.hpp"

#include "../shared_state.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <cstring>
#include <map>
#include <string>

namespace Transmog::ColorOverride
{
    std::array<DyeSlot, k_slotCount> &dye_state() noexcept
    {
        static std::array<DyeSlot, k_slotCount> g_state{};
        return g_state;
    }

    namespace
    {
        bool g_dyeAdvancedView = false;
    }

    bool dye_advanced_view_get() noexcept
    {
        return g_dyeAdvancedView;
    }
    void dye_advanced_view_set(bool v) noexcept
    {
        g_dyeAdvancedView = v;
    }
} // namespace Transmog::ColorOverride

namespace Transmog::ColorOverride::SwatchTable
{
    namespace
    {
        using ::Transmog::ColorOverride::dye_state;
        std::array<std::array<SwatchEntry, k_dyeSwatchesPerSlot>, k_slotCount> g_table{};
        std::array<std::atomic<std::size_t>, k_slotCount> g_count{};
        std::array<std::atomic<bool>, k_slotCount> g_postReinitLock{};
        std::array<std::atomic<bool>, k_slotCount> g_reinitCaptureOpen{};
        // Set by wipe_swatch_table_for_slot (user-triggered Reset
        // Slot) and cleared by populate_from_persisted. Read by PresetManager::snapshot_live_swatches_into to
        // distinguish "live empty because user explicitly wiped" from "live empty because tokens haven't resolved /
        // slot not equipped". The latter wants to preserve the JSON baseline; the former wants the save to commit the
        // empty state.
        std::array<std::atomic<bool>, k_slotCount> g_explicitlyWiped{};

        bool valid_slot(int slot) noexcept
        {
            return slot >= 0 && static_cast<std::size_t>(slot) < k_slotCount;
        }
    } // namespace

    int lookup_or_insert(int slot, std::uint32_t content_hash, std::uint64_t stable_id, std::uint16_t template_id,
                         std::uint16_t token_id, bool expect_open) noexcept
    {
        return lookup_or_insert(slot, content_hash, stable_id, template_id, token_id, expect_open, nullptr);
    }

    int lookup_or_insert(int slot, std::uint32_t content_hash, std::uint64_t stable_id, std::uint16_t template_id,
                         std::uint16_t token_id, bool expect_open, const char *submesh_name) noexcept
    {
        if (!valid_slot(slot) || content_hash == 0 || token_id == 0)
            return -1;
        const auto s = static_cast<std::size_t>(slot);
        auto &table = g_table[s];
        auto &count = g_count[s];
        const auto cnt = count.load(std::memory_order_acquire);
        const auto upper = (cnt < k_dyeSwatchesPerSlot) ? cnt : k_dyeSwatchesPerSlot;
        for (std::size_t i = 0; i < upper; ++i)
        {
            auto &row = table[i];
            // Match the stable-identity tuple (stable_id, template_id, token_id) before validating content_hash -- the
            // stable triple is the strict identity, so most mismatches bail without touching the volatile hash.
            if (row.stable_id.load(std::memory_order_relaxed) != stable_id ||
                row.template_id.load(std::memory_order_relaxed) != template_id ||
                row.token_id.load(std::memory_order_relaxed) != token_id)
                continue;
            if (row.content_hash.load(std::memory_order_relaxed) != content_hash)
                continue;
            // Ghost guard: refuse to resurrect rows that Reinit::Finalize froze out. Returning -1 makes the setter bail
            // without substituting AND without re-marking active_this_apply = true, so the next reinit pass's
            // intersection sees a "miss" on this identity.
            if (row.frozen_hidden.load(std::memory_order_acquire))
                return -1;
            row.active_this_apply.store(true, std::memory_order_release);
            return static_cast<int>(i);
        }

        // ---- Placeholder promotion pass --------------------------------
        //
        // `populate_from_persisted` creates rows with the saved (submesh_name, token_id) but content_hash=0 to mark
        // them as "awaiting promotion". The first engine write whose (submesh_name, token_id) matches a placeholder
        // gets that row's hash/stable/tpl filled in -- subsequent writes hit the fast hash-based loop above. This is
        // how saved presets re-bind to live engine identities without running the 3-pass Reinit capture.
        if (submesh_name != nullptr && submesh_name[0] != '\0')
        {
            for (std::size_t i = 0; i < upper; ++i)
            {
                auto &row = table[i];
                if (row.content_hash.load(std::memory_order_relaxed) != 0)
                    continue; // not a placeholder
                if (row.token_id.load(std::memory_order_relaxed) != token_id)
                    continue;
                if (row.frozen_hidden.load(std::memory_order_acquire))
                    continue;
                auto &ovr = dye_state()[s].swatches[i];
                if (std::strcmp(ovr.submesh_name, submesh_name) != 0)
                    continue;
                // Match! Promote the row: fill the live identity.
                row.content_hash.store(content_hash, std::memory_order_relaxed);
                row.stable_id.store(stable_id, std::memory_order_relaxed);
                row.template_id.store(template_id, std::memory_order_relaxed);
                ovr.submesh_stable_id = stable_id;
                ovr.template_id = template_id;
                row.active_this_apply.store(true, std::memory_order_release);
                return static_cast<int>(i);
            }
        }

        if (!expect_open)
            return -1;
        if (State::swatch_frozen(slot).load(std::memory_order_acquire))
            return -1;
        // Reinit post-lock: once Finalize closes the slot's identity set, refuse new inserts until the user explicitly
        // re-runs reinit / Reset slot / preset switch (which clears the lock).
        if (g_postReinitLock[static_cast<std::size_t>(slot)].load(std::memory_order_acquire))
        {
            // While the reinit capture window is open (during a re-run), allow inserts again so passes 2+3 can refresh
            // the identity set.
            if (!g_reinitCaptureOpen[static_cast<std::size_t>(slot)].load(std::memory_order_acquire))
                return -1;
        }
        const auto idx = count.fetch_add(1, std::memory_order_acq_rel);
        if (idx >= k_dyeSwatchesPerSlot)
        {
            count.fetch_sub(1, std::memory_order_acq_rel);
            return -1;
        }
        auto &row = table[idx];
        row.content_hash.store(content_hash, std::memory_order_relaxed);
        row.stable_id.store(stable_id, std::memory_order_relaxed);
        row.template_id.store(template_id, std::memory_order_relaxed);
        row.token_id.store(token_id, std::memory_order_relaxed);
        row.def_seen_mask.store(0, std::memory_order_relaxed);
        row.def_a_captured.store(false, std::memory_order_relaxed);
        row.active_this_apply.store(true, std::memory_order_release);
        // Mirror row identity to the UI-side SwatchOverride so the picker (which reads `dyeSlot.swatches[]` directly)
        // can render labels and region grouping without joining against SwatchEntry.
        {
            auto &ovr = dye_state()[s].swatches[idx];
            ovr.token_id = token_id;
            ovr.submesh_stable_id = stable_id;
            ovr.template_id = template_id;
            // Seed submesh_name once on insert. Re-fires for the same identity tuple don't re-write (see lookup branch
            // above) so a transient null capture doesn't clobber a good earlier read.
            if (submesh_name && submesh_name[0] != '\0')
            {
                std::size_t n = 0;
                while (n < sizeof(ovr.submesh_name) - 1 && submesh_name[n] != '\0')
                {
                    ovr.submesh_name[n] = submesh_name[n];
                    ++n;
                }
                ovr.submesh_name[n] = '\0';
            }
        }
        return static_cast<int>(idx);
    }

    std::size_t count(int slot) noexcept
    {
        if (!valid_slot(slot))
            return 0;
        const auto raw = g_count[static_cast<std::size_t>(slot)].load(std::memory_order_acquire);
        return (raw < k_dyeSwatchesPerSlot) ? raw : k_dyeSwatchesPerSlot;
    }

    SwatchEntry *row(int slot, std::size_t idx) noexcept
    {
        if (!valid_slot(slot) || idx >= k_dyeSwatchesPerSlot)
            return nullptr;
        return &g_table[static_cast<std::size_t>(slot)][idx];
    }

    SwatchOverride *override_row(int slot, std::size_t idx) noexcept
    {
        if (!valid_slot(slot) || idx >= k_dyeSwatchesPerSlot)
            return nullptr;
        return &dye_state()[static_cast<std::size_t>(slot)].swatches[idx];
    }

    void capture_default_if_unset(int slot, std::size_t idx, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                  std::uint8_t a) noexcept
    {
        if (!valid_slot(slot) || idx >= k_dyeSwatchesPerSlot)
            return;
        const auto s = static_cast<std::size_t>(slot);
        auto &row = g_table[s][idx];
        auto &ovr = dye_state()[s].swatches[idx];
        // First-fire RGB capture.
        const auto mask = row.def_seen_mask.load(std::memory_order_acquire);
        if ((mask & 1u) == 0)
        {
            row.def_r.store(r, std::memory_order_relaxed);
            row.def_g.store(g, std::memory_order_relaxed);
            row.def_b.store(b, std::memory_order_relaxed);
            ovr.def_r = r;
            ovr.def_g = g;
            ovr.def_b = b;
            // Seed the user override with the engine default ONLY when the row isn't already user-overridden, otherwise
            // the setter's first capture on a saved or loaded row would clobber the user's picked colour. Capture
            // itself ALWAYS runs so the picker's "default colour reference dot" has live def_r/g/b for every row,
            // including ones the user has already coloured.
            if (!ovr.override_active)
            {
                ovr.r = r;
                ovr.g = g;
                ovr.b = b;
            }
            row.def_seen_mask.store(static_cast<std::uint8_t>(mask | 1u), std::memory_order_release);
        }
        if (!row.def_a_captured.load(std::memory_order_acquire))
        {
            row.def_a.store(a, std::memory_order_relaxed);
            ovr.def_a = a;
            row.def_a_captured.store(true, std::memory_order_release);
            ovr.def_a_captured = true;
            row.def_seen_mask.fetch_or(2u, std::memory_order_acq_rel);
        }
        ovr.default_captured = true;
    }

    void set_override_rgb(int slot, std::size_t idx, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
    {
        auto *ovr = override_row(slot, idx);
        if (!ovr)
            return;
        ovr->r = r;
        ovr->g = g;
        ovr->b = b;
    }

    void set_override_active(int slot, std::size_t idx, bool active) noexcept
    {
        auto *ovr = override_row(slot, idx);
        if (!ovr)
            return;
        ovr->override_active = active;
    }

    bool any_override_active_in_slot(int slot) noexcept
    {
        if (!valid_slot(slot))
            return false;
        const auto s = static_cast<std::size_t>(slot);
        const auto cnt = g_count[s].load(std::memory_order_acquire);
        const auto upper = (cnt < k_dyeSwatchesPerSlot) ? cnt : k_dyeSwatchesPerSlot;
        for (std::size_t i = 0; i < upper; ++i)
            if (dye_state()[s].swatches[i].override_active)
                return true;
        return false;
    }

    void mark_all_inactive(int slot) noexcept
    {
        if (!valid_slot(slot))
            return;
        for (auto &r : g_table[static_cast<std::size_t>(slot)])
            r.active_this_apply.store(false, std::memory_order_release);
    }

    void clear_slot(int slot) noexcept
    {
        if (!valid_slot(slot))
            return;
        const auto s = static_cast<std::size_t>(slot);
        for (auto &r : g_table[s])
        {
            r.content_hash.store(0, std::memory_order_relaxed);
            r.stable_id.store(0, std::memory_order_relaxed);
            r.template_id.store(0, std::memory_order_relaxed);
            r.token_id.store(0, std::memory_order_relaxed);
            r.active_this_apply.store(false, std::memory_order_relaxed);
            r.def_r.store(0, std::memory_order_relaxed);
            r.def_g.store(0, std::memory_order_relaxed);
            r.def_b.store(0, std::memory_order_relaxed);
            r.def_a.store(0xFF, std::memory_order_relaxed);
            r.def_seen_mask.store(0, std::memory_order_relaxed);
            r.def_a_captured.store(false, std::memory_order_relaxed);
        }
        // Preserve user override RGB across wipes so re-tick of the same item restores the user's picks; only kill the
        // captured-default mirror so the next apply re-seeds it.
        for (auto &o : dye_state()[s].swatches)
        {
            o.def_r = 0;
            o.def_g = 0;
            o.def_b = 0;
            o.def_a = 0xFF;
            o.default_captured = false;
            // Identity-bound: same item re-applies will re-capture the name on insert.
            o.submesh_name[0] = '\0';
        }
        g_count[s].store(0, std::memory_order_release);
    }

    void clear_all() noexcept
    {
        for (std::size_t s = 0; s < k_slotCount; ++s)
            clear_slot(static_cast<int>(s));
        // clear_slot preserves override choices; wipe them too on a global reset (preset/character switch).
        for (auto &slot : dye_state())
        {
            slot.slot_enabled = false;
            for (auto &o : slot.swatches)
            {
                o.override_active = false;
                o.r = 0;
                o.g = 0;
                o.b = 0;
            }
        }
        // Strict-init policy: default to LOCKED so unrelated engine writes can't add rows outside an explicit Reinit
        // cycle. Reinit::start_slot_reinit_once re-opens the gate when needed.
        for (auto &b : g_postReinitLock)
            b.store(true, std::memory_order_relaxed);
        for (auto &b : g_reinitCaptureOpen)
            b.store(false, std::memory_order_relaxed);
    }

    // ---- Reinit-aware accessors -------------------------------------

    void wipe_swatch_table_for_slot(int slot) noexcept
    {
        if (!valid_slot(slot))
            return;
        const auto s = static_cast<std::size_t>(slot);
        for (auto &e : g_table[s])
        {
            e.content_hash.store(0, std::memory_order_relaxed);
            e.stable_id.store(0, std::memory_order_relaxed);
            e.template_id.store(0, std::memory_order_relaxed);
            e.token_id.store(0, std::memory_order_relaxed);
            e.active_this_apply.store(false, std::memory_order_relaxed);
            e.def_r.store(0, std::memory_order_relaxed);
            e.def_g.store(0, std::memory_order_relaxed);
            e.def_b.store(0, std::memory_order_relaxed);
            e.def_a.store(0xFF, std::memory_order_relaxed);
            e.def_seen_mask.store(0, std::memory_order_relaxed);
            e.def_a_captured.store(false, std::memory_order_relaxed);
            e.frozen_hidden.store(false, std::memory_order_relaxed);
        }
        for (auto &o : dye_state()[s].swatches)
        {
            o.override_active = false;
            o.r = 0;
            o.g = 0;
            o.b = 0;
            o.def_r = 0;
            o.def_g = 0;
            o.def_b = 0;
            o.def_a = 0xFF;
            o.default_captured = false;
            o.submesh_name[0] = '\0';
        }
        g_count[s].store(0, std::memory_order_release);
        // Wipe leaves the slot LOCKED. Inserts only happen inside an explicit Reinit capture window (user-clicked
        // Re-init or auto-triggered by preset switch). Without an open window, setter writes only MATCH existing rows;
        // a missing identity falls through (the engine renders its natural value, no new row is added), keeping stray
        // captures from polluting the picker between explicit init events.
        g_postReinitLock[s].store(true, std::memory_order_release);
        g_reinitCaptureOpen[s].store(false, std::memory_order_release);
        // Mark the wipe as user-intentional so the next snapshot into the active preset (Save button) writes the empty
        // state instead of treating the empty live table as a token-race and preserving the JSON baseline.
        g_explicitlyWiped[s].store(true, std::memory_order_release);
    }

    bool slot_was_explicitly_wiped(int slot) noexcept
    {
        if (!valid_slot(slot))
            return false;
        return g_explicitlyWiped[static_cast<std::size_t>(slot)].load(std::memory_order_acquire);
    }

    void clear_explicit_wipe_flag(int slot) noexcept
    {
        if (!valid_slot(slot))
            return;
        g_explicitlyWiped[static_cast<std::size_t>(slot)].store(false, std::memory_order_release);
    }

    std::atomic<bool> &post_reinit_lock(int slot) noexcept
    {
        static std::atomic<bool> dummy{false};
        if (!valid_slot(slot))
            return dummy;
        return g_postReinitLock[static_cast<std::size_t>(slot)];
    }

    std::atomic<bool> &reinit_capture_open(int slot) noexcept
    {
        static std::atomic<bool> dummy{false};
        if (!valid_slot(slot))
            return dummy;
        return g_reinitCaptureOpen[static_cast<std::size_t>(slot)];
    }

    std::size_t snapshot_active_identities(int slot, SwatchIdentity *out, std::size_t out_cap) noexcept
    {
        if (!valid_slot(slot) || out == nullptr || out_cap == 0)
            return 0;
        const auto s = static_cast<std::size_t>(slot);
        const auto cnt = g_count[s].load(std::memory_order_acquire);
        const auto upper = (cnt < k_dyeSwatchesPerSlot) ? cnt : k_dyeSwatchesPerSlot;
        std::size_t n = 0;
        for (std::size_t i = 0; i < upper && n < out_cap; ++i)
        {
            auto &e = g_table[s][i];
            if (!e.active_this_apply.load(std::memory_order_acquire))
                continue;
            const auto h = e.content_hash.load(std::memory_order_relaxed);
            if (h == 0)
                continue;
            out[n].hash = h;
            out[n].stable = e.stable_id.load(std::memory_order_relaxed);
            out[n].tpl = e.template_id.load(std::memory_order_relaxed);
            out[n].token = e.token_id.load(std::memory_order_relaxed);
            ++n;
        }
        return n;
    }

    bool slot_enabled_get(int slot) noexcept
    {
        if (!valid_slot(slot))
            return false;
        return dye_state()[static_cast<std::size_t>(slot)].slot_enabled;
    }

    void slot_enabled_set(int slot, bool v) noexcept
    {
        if (!valid_slot(slot))
            return;
        dye_state()[static_cast<std::size_t>(slot)].slot_enabled = v;
    }

    std::size_t detected_swatch_count(int slot) noexcept
    {
        return count(slot);
    }

    void clear_dye_state_for_slot(int slot) noexcept
    {
        // Reset the carrier set, hash-set burst-lock timestamp and freeze flag so the next apply has a clean capture
        // state. Does NOT wipe the swatch table rows or user override RGB / override_active choices -- those survive
        // untick/retick of the same item.
        if (!valid_slot(slot))
            return;
        CarrierSet::clear_slot(slot);
        State::hash_set_last_add_ms(slot).store(0, std::memory_order_release);
        State::swatch_frozen(slot).store(false, std::memory_order_release);
    }

    // Helper: shared row-identity gate used by both getters. Returns the token_name for valid rows, or nullptr to skip.
    static const char *resolve_persist_token_name(const SwatchEntry &e, const SwatchOverride &ovr) noexcept
    {
        if (ovr.submesh_name[0] == '\0')
            return nullptr;
        const auto token = e.token_id.load(std::memory_order_relaxed);
        if (token == 0)
            return nullptr;
        return TokenTable::token_label_for(token);
    }

    std::vector<PersistEntry> get_persistable_overrides(int slot) noexcept
    {
        std::vector<PersistEntry> out;
        if (!::Transmog::flag_color_override().load(std::memory_order_acquire))
            return out;
        if (!valid_slot(slot))
            return out;
        const auto s = static_cast<std::size_t>(slot);
        const auto cnt = g_count[s].load(std::memory_order_acquire);
        const auto upper = (cnt < k_dyeSwatchesPerSlot) ? cnt : k_dyeSwatchesPerSlot;

        // Diagnostic: count + categorise every drop so we can see exactly why some rows aren't persisting.
        std::size_t total = upper;
        std::size_t kept = 0;
        std::size_t drop_inactive = 0;       // override_active==false
        std::size_t drop_no_submesh = 0;     // empty submesh_name
        std::size_t drop_no_token_id = 0;    // token_id == 0
        std::size_t drop_no_token_label = 0; // label lookup fails

        auto &logger = DetourModKit::Logger::get_instance();

        for (std::size_t i = 0; i < upper; ++i)
        {
            const auto &e = g_table[s][i];
            const auto &ovr = dye_state()[s].swatches[i];
            if (!ovr.override_active)
            {
                ++drop_inactive;
                continue;
            }
            // Decompose the resolve_persist_token_name reasons:
            if (ovr.submesh_name[0] == '\0')
            {
                ++drop_no_submesh;
                logger.debug("[persist-probe] slot={} row={} DROP (empty "
                             "submesh) tok=0x{:04X} rgb=({:02X},{:02X},"
                             "{:02X})",
                             slot, i, e.token_id.load(std::memory_order_relaxed), ovr.r, ovr.g, ovr.b);
                continue;
            }
            const auto tokenId = e.token_id.load(std::memory_order_relaxed);
            if (tokenId == 0)
            {
                ++drop_no_token_id;
                logger.debug("[persist-probe] slot={} row={} DROP "
                             "(token_id=0) submesh='{}' rgb=({:02X},"
                             "{:02X},{:02X})",
                             slot, i, ovr.submesh_name, ovr.r, ovr.g, ovr.b);
                continue;
            }
            const char *tokenName = TokenTable::token_label_for(static_cast<std::uint16_t>(tokenId));
            if (tokenName == nullptr)
            {
                ++drop_no_token_label;
                logger.debug("[persist-probe] slot={} row={} DROP "
                             "(unresolved token 0x{:04X}) submesh='{}' "
                             "hash={:#x} stable={:#x} tpl={:#x} rgb=({:02X},"
                             "{:02X},{:02X})",
                             slot, i, tokenId, ovr.submesh_name, e.content_hash.load(std::memory_order_relaxed),
                             e.stable_id.load(std::memory_order_relaxed), e.template_id.load(std::memory_order_relaxed),
                             ovr.r, ovr.g, ovr.b);
                continue;
            }
            PersistEntry pe{};
            pe.submesh_name = ovr.submesh_name;
            pe.token_name = tokenName;
            pe.r = ovr.r;
            pe.g = ovr.g;
            pe.b = ovr.b;
            out.push_back(std::move(pe));
            ++kept;
        }

        logger.debug("[persist-probe] slot={} total_rows={} kept={} "
                     "drop_inactive={} drop_no_submesh={} drop_no_token_id={} "
                     "drop_no_token_label={}",
                     slot, total, kept, drop_inactive, drop_no_submesh, drop_no_token_id, drop_no_token_label);

        return out;
    }

    std::vector<PersistEntry> get_persistable_palette(int slot) noexcept
    {
        std::vector<PersistEntry> out;
        if (!::Transmog::flag_color_override().load(std::memory_order_acquire))
            return out;
        if (!valid_slot(slot))
            return out;
        const auto s = static_cast<std::size_t>(slot);
        const auto cnt = g_count[s].load(std::memory_order_acquire);
        const auto upper = (cnt < k_dyeSwatchesPerSlot) ? cnt : k_dyeSwatchesPerSlot;
        for (std::size_t i = 0; i < upper; ++i)
        {
            const auto &e = g_table[s][i];
            const auto &ovr = dye_state()[s].swatches[i];
            // Identity-only filter: any row whose submesh+token is resolvable gets recorded in the palette, regardless
            // of whether it's user-overridden or has a captured default. r/g/b is left zero -- callers shouldn't read
            // them; the palette JSON serializer writes just the token names.
            const char *tokenName = resolve_persist_token_name(e, ovr);
            if (tokenName == nullptr)
                continue;
            PersistEntry pe{};
            pe.submesh_name = ovr.submesh_name;
            pe.token_name = tokenName;
            out.push_back(std::move(pe));
        }
        return out;
    }

    void restore_persisted_state(int slot, const std::vector<PersistEntry> &entries) noexcept
    {
        if (!::Transmog::flag_color_override().load(std::memory_order_acquire))
            return;
        if (!valid_slot(slot))
            return;
        auto &log = DetourModKit::Logger::get_instance();
        std::size_t queued = 0;
        std::size_t immediate = 0;
        const auto s = static_cast<std::size_t>(slot);
        const auto cnt = g_count[s].load(std::memory_order_acquire);
        const auto upper = (cnt < k_dyeSwatchesPerSlot) ? cnt : k_dyeSwatchesPerSlot;

        for (const auto &pe : entries)
        {
            if (pe.submesh_name.empty() || pe.token_name.empty())
                continue;

            // Always queue into the pending-overrides map; the setter consults this on every successful
            // lookup_or_insert so a freshly-captured row gets its persisted color the moment the engine writes to it.
            PendingOverrides::set(slot, pe.submesh_name, pe.token_name, pe.r, pe.g, pe.b);
            ++queued;

            // Plus an opportunistic immediate-apply pass: if a row matching this (submesh, token) is ALREADY in the
            // live swatch table (e.g. we're restoring after a preset switch where the same outfit is already applied),
            // set its override RGB right now so the picker reflects the new color without waiting for the next engine
            // write.
            const auto tok = TokenTable::token_id_for_name(pe.token_name.c_str());
            if (tok == 0)
                continue;
            for (std::size_t i = 0; i < upper; ++i)
            {
                auto &row = g_table[s][i];
                if (row.token_id.load(std::memory_order_relaxed) != tok)
                    continue;
                auto &ovr = dye_state()[s].swatches[i];
                if (std::strcmp(ovr.submesh_name, pe.submesh_name.c_str()) != 0)
                    continue;
                ovr.r = pe.r;
                ovr.g = pe.g;
                ovr.b = pe.b;
                ovr.override_active = true;
                ++immediate;
                break;
            }
        }
        if (queued != 0 || immediate != 0)
        {
            log.debug("[swatch-persist] slot {} queued {} ({} also "
                      "applied immediately to live rows)",
                      slot, queued, immediate);
        }
    }

    int find_placeholder_slot(const char *submesh_name, std::uint16_t token_id) noexcept
    {
        if (token_id == 0 || submesh_name == nullptr || submesh_name[0] == '\0')
            return -1;
        for (std::size_t s = 0; s < k_slotCount; ++s)
        {
            const auto cnt = g_count[s].load(std::memory_order_acquire);
            const auto upper = (cnt < k_dyeSwatchesPerSlot) ? cnt : k_dyeSwatchesPerSlot;
            for (std::size_t i = 0; i < upper; ++i)
            {
                const auto &row = g_table[s][i];
                // Placeholders are rows seeded by populate_from_persisted: content_hash==0 marks "awaiting promotion".
                // Live rows already match by hash in the substitute path's normal flow.
                if (row.content_hash.load(std::memory_order_relaxed) != 0)
                    continue;
                if (row.token_id.load(std::memory_order_relaxed) != token_id)
                    continue;
                const auto &ovr = dye_state()[s].swatches[i];
                if (std::strcmp(ovr.submesh_name, submesh_name) != 0)
                    continue;
                return static_cast<int>(s);
            }
        }
        return -1;
    }

    bool promote_placeholder_identity(const char *submesh_name, std::uint16_t token_id, std::uint32_t content_hash,
                                      std::uint64_t stable_id, std::uint16_t template_id, std::uint8_t def_r,
                                      std::uint8_t def_g, std::uint8_t def_b, std::uint8_t def_a) noexcept
    {
        if (token_id == 0 || content_hash == 0 || submesh_name == nullptr || submesh_name[0] == '\0')
            return false;
        for (std::size_t s = 0; s < k_slotCount; ++s)
        {
            const auto cnt = g_count[s].load(std::memory_order_acquire);
            const auto upper = (cnt < k_dyeSwatchesPerSlot) ? cnt : k_dyeSwatchesPerSlot;
            for (std::size_t i = 0; i < upper; ++i)
            {
                auto &row = g_table[s][i];
                if (row.content_hash.load(std::memory_order_relaxed) != 0)
                    continue;
                if (row.token_id.load(std::memory_order_relaxed) != token_id)
                    continue;
                auto &ovr = dye_state()[s].swatches[i];
                if (std::strcmp(ovr.submesh_name, submesh_name) != 0)
                    continue;
                // Promote: write live identity fields into the SwatchEntry. template_id / stable_id first, then
                // content_hash last (release-ordered) so any reader that sees a non-zero content_hash is guaranteed to
                // also see the other identity fields.
                row.template_id.store(template_id, std::memory_order_relaxed);
                row.stable_id.store(stable_id, std::memory_order_relaxed);
                row.content_hash.store(content_hash, std::memory_order_release);
                // Mirror template_id into the picker-facing SwatchOverride too. The UI reads `sw.template_id` from
                // SwatchOverride (not SwatchEntry) when building
                // the region-header label "submesh  (tpl 0x%04X)" --
                // without this mirror the label stays at 0x0000 even though SwatchEntry's template_id is correct.
                // submesh_stable_id is already populated by populate_from_persisted at JSON load (FNV of submesh_name)
                // so the UI's region grouping works before promotion; we leave it as-is.
                ovr.template_id = template_id;
                // Capture the engine's intended source RGB (passed in by the setter from ctx.r8 BEFORE the substitute
                // redirected the pointer) as this row's default. Without this the picker never shows a default-colour
                // reference dot for placeholder rows, because the slot-agnostic pending-match path returns before the
                // setter's normal capture path runs. capture_default_if_unset is a no-op once a default is captured, so
                // this is idempotent across repeated engine writes.
                capture_default_if_unset(static_cast<int>(s), i, def_r, def_g, def_b, def_a);
                return true;
            }
        }
        return false;
    }

    std::size_t populate_from_persisted(int slot, const std::vector<PersistEntry> &palette,
                                        const std::vector<PersistEntry> &overrides) noexcept
    {
        if (!::Transmog::flag_color_override().load(std::memory_order_acquire))
            return 0;
        if (!valid_slot(slot))
            return 0;
        auto &log = DetourModKit::Logger::get_instance();
        const auto s = static_cast<std::size_t>(slot);
        // Repopulating from saved JSON replaces any prior wipe state, so the snapshot guard should treat this slot as
        // "has saved content" again. Reached on preset load, preset switch, and restore_swatches_from -- all paths that
        // re-seed placeholders from a non-empty palette/overrides set.
        if (!palette.empty() || !overrides.empty())
            g_explicitlyWiped[s].store(false, std::memory_order_release);
        auto &table = g_table[s];
        auto &count = g_count[s];
        std::size_t seeded = 0;
        std::size_t skippedUnresolved = 0;
        std::size_t mergedOverrides = 0;
        std::size_t orphanOverrides = 0;

        // FNV-1a 64-bit hash for synthetic stable_id assignment to placeholders. Distinct per submesh so the UI's
        // region grouping map produces one bucket per submesh. Promotion in lookup_or_insert later overwrites with the
        // engine's real pool counter when an identity match fires.
        auto fnv1a64 = [](const std::string &s) -> std::uint64_t
        {
            std::uint64_t h = 0xcbf29ce484222325ULL;
            for (char c : s)
            {
                h ^= static_cast<std::uint8_t>(c);
                h *= 0x100000001b3ULL;
            }
            return h | 0x8000000000000000ULL;
        };

        // Lookup existing seeded row by (submesh, token). Returns row idx or -1.
        auto find_seeded = [&](const std::string &submesh, std::uint16_t tok) -> int
        {
            const auto curCnt = count.load(std::memory_order_acquire);
            const auto upper = (curCnt < k_dyeSwatchesPerSlot) ? curCnt : k_dyeSwatchesPerSlot;
            for (std::size_t i = 0; i < upper; ++i)
            {
                const auto &row = table[i];
                if (row.token_id.load(std::memory_order_relaxed) != tok)
                    continue;
                const auto &ovr = dye_state()[s].swatches[i];
                if (std::strcmp(ovr.submesh_name, submesh.c_str()) != 0)
                    continue;
                return static_cast<int>(i);
            }
            return -1;
        };

        // Allocate a fresh placeholder row with the given (submesh, token) identity. All other fields are zeroed;
        // caller fills def_*/r/g/b/override_active after.
        auto alloc_placeholder = [&](const std::string &submesh, std::uint16_t tok) -> int
        {
            const auto raw = count.fetch_add(1, std::memory_order_acq_rel);
            if (raw >= k_dyeSwatchesPerSlot)
            {
                count.fetch_sub(1, std::memory_order_acq_rel);
                log.warning("[swatch-seed] slot {} full, dropped '{}'", slot, submesh);
                return -1;
            }
            const auto idx = raw;
            auto &row = table[idx];
            const auto synthStable = fnv1a64(submesh);
            row.content_hash.store(0, std::memory_order_relaxed);
            row.stable_id.store(synthStable, std::memory_order_relaxed);
            row.template_id.store(0, std::memory_order_relaxed);
            row.token_id.store(tok, std::memory_order_relaxed);
            row.frozen_hidden.store(false, std::memory_order_relaxed);
            row.active_this_apply.store(false, std::memory_order_release);
            row.def_seen_mask.store(0, std::memory_order_relaxed);
            row.def_a_captured.store(false, std::memory_order_relaxed);
            row.def_r.store(0, std::memory_order_relaxed);
            row.def_g.store(0, std::memory_order_relaxed);
            row.def_b.store(0, std::memory_order_relaxed);
            row.def_a.store(0xFF, std::memory_order_relaxed);

            auto &ovr = dye_state()[s].swatches[idx];
            std::size_t n = 0;
            while (n < sizeof(ovr.submesh_name) - 1 && submesh[n] != '\0')
            {
                ovr.submesh_name[n] = submesh[n];
                ++n;
            }
            ovr.submesh_name[n] = '\0';
            ovr.token_id = tok;
            ovr.submesh_stable_id = synthStable;
            ovr.template_id = 0;
            ovr.r = 0;
            ovr.g = 0;
            ovr.b = 0;
            ovr.def_r = 0;
            ovr.def_g = 0;
            ovr.def_b = 0;
            ovr.def_a = 0xFF;
            ovr.default_captured = false;
            ovr.def_a_captured = false;
            ovr.override_active = false;
            ++seeded;
            return static_cast<int>(idx);
        };

        // Pass 1 -- palette: allocate one placeholder per `(submesh, token)` from the captured-row palette. No default
        // colours are seeded; the engine fills `def_*` live on its first matching write via `capture_default_if_unset`.
        // `override_active` is left false; the override pass below promotes the specific rows the user ticked.
        for (const auto &pe : palette)
        {
            if (pe.submesh_name.empty() || pe.token_name.empty())
                continue;
            const auto tok = TokenTable::token_id_for_name(pe.token_name.c_str());
            if (tok == 0)
            {
                ++skippedUnresolved;
                continue;
            }
            (void)alloc_placeholder(pe.submesh_name, tok);
        }

        // Pass 2 -- user overrides: each entry merges onto the matching palette row by `(submesh, token)`. If no
        // palette row exists (an override was saved for a row that wasn't in the palette), allocate a fresh
        // placeholder.
        for (const auto &pe : overrides)
        {
            if (pe.submesh_name.empty() || pe.token_name.empty())
                continue;
            const auto tok = TokenTable::token_id_for_name(pe.token_name.c_str());
            if (tok == 0)
            {
                ++skippedUnresolved;
                continue;
            }
            int idx = find_seeded(pe.submesh_name, tok);
            if (idx < 0)
            {
                idx = alloc_placeholder(pe.submesh_name, tok);
                if (idx < 0)
                    continue;
                ++orphanOverrides;
            }
            else
            {
                ++mergedOverrides;
            }
            auto &ovr = dye_state()[s].swatches[idx];
            ovr.r = pe.r;
            ovr.g = pe.g;
            ovr.b = pe.b;
            ovr.override_active = true;
        }

        // Slot stays LOCKED after seeding -- promotion only updates existing placeholders; unrelated submeshes still
        // fall through (no row created).
        g_postReinitLock[s].store(true, std::memory_order_release);
        g_reinitCaptureOpen[s].store(false, std::memory_order_release);

        if (seeded != 0 || skippedUnresolved != 0)
        {
            log.info("[swatch-seed] slot {} palette={} overrides_merged={} "
                     "overrides_orphan={} unresolved={}; slot LOCKED",
                     slot, palette.size(), mergedOverrides, orphanOverrides, skippedUnresolved);
        }
        return seeded;
    }

    // ---- Diagnostic dump --------------------------------------------

    void dump_slot(int slot) noexcept
    {
        if (!::Transmog::flag_color_override().load(std::memory_order_acquire))
            return;
        if (!valid_slot(slot))
            return;
        auto &log = DetourModKit::Logger::get_instance();
        const auto s = static_cast<std::size_t>(slot);
        const auto cnt = g_count[s].load(std::memory_order_acquire);
        const auto upper = (cnt < k_dyeSwatchesPerSlot) ? cnt : k_dyeSwatchesPerSlot;

        // First pass: group rows by submesh_name so the dump is tree-shaped (one block per region). std::map for stable
        // iteration order (alphabetical) without extra sort.
        //
        // Includes BOTH promoted rows (content_hash != 0) AND placeholder rows seeded by populate_from_persisted
        // (content_hash == 0, but token_id + submesh_name set). The placeholder marker shows up in the per-row state
        // column so the log clearly distinguishes "saved + waiting for engine write" from "live and substituting".
        std::map<std::string, std::vector<std::size_t>> bySubmesh;
        for (std::size_t i = 0; i < upper; ++i)
        {
            const auto &e = g_table[s][i];
            const auto tok = e.token_id.load(std::memory_order_relaxed);
            if (tok == 0)
                continue; // truly empty row
            const auto &ovr = dye_state()[s].swatches[i];
            std::string key = ovr.submesh_name[0] ? std::string(ovr.submesh_name) : std::string("(unnamed)");
            bySubmesh[key].push_back(i);
        }

        const bool slotEn = dye_state()[s].slot_enabled;
        const auto frozenFlag = State::swatch_frozen(slot).load(std::memory_order_acquire);
        const auto postLock = g_postReinitLock[s].load(std::memory_order_acquire);
        log.debug("[swatch-dump] slot={} rows={}/{} unique_submeshes={} "
                  "slot_enabled={} swatch_frozen={} post_reinit_lock={}",
                  slot, upper, k_dyeSwatchesPerSlot, bySubmesh.size(), slotEn, frozenFlag, postLock);
        if (upper == 0)
            return;

        for (auto &kv : bySubmesh)
        {
            const auto &name = kv.first;
            const auto &rows = kv.second;
            // Use the first row's template_id as the region's tpl tag (rows in the same submesh share template_id; this
            // is observation, not invariant -- if it diverges the first one is still useful as a hint).
            const auto firstTpl = g_table[s][rows.front()].template_id.load(std::memory_order_relaxed);
            const auto firstStable = g_table[s][rows.front()].stable_id.load(std::memory_order_relaxed);
            log.debug("  submesh='{}' tpl=0x{:04X} stable=0x{:016X} rows={}", name, firstTpl,
                      static_cast<unsigned long long>(firstStable), rows.size());
            for (auto idx : rows)
            {
                const auto &e = g_table[s][idx];
                const auto &ovr = dye_state()[s].swatches[idx];
                const auto tok = e.token_id.load(std::memory_order_relaxed);
                const char *tokName = TokenTable::token_label_for(tok);
                const auto def_r = e.def_r.load(std::memory_order_relaxed);
                const auto def_g = e.def_g.load(std::memory_order_relaxed);
                const auto def_b = e.def_b.load(std::memory_order_relaxed);
                const auto hash = e.content_hash.load(std::memory_order_relaxed);
                const bool isPlaceholder = (hash == 0);
                const char *state = isPlaceholder          ? "PLACEHOLDER" // seeded from JSON, awaiting promotion
                                    : ovr.override_active  ? "ACTIVE"      // user RGB substituting
                                    : ovr.default_captured ? "DEFAULT"     // engine default flowing through
                                    : e.frozen_hidden.load(std::memory_order_acquire)
                                        ? "FROZEN" // pruned by Reinit
                                                   // first-fire pending capture
                                        : "PENDING";
                log.debug("    {} (0x{:04X}) def=#{:02X}{:02X}{:02X} "
                          "user=#{:02X}{:02X}{:02X} hash=0x{:08X} [{}]",
                          tokName ? tokName : "(unnamed-tok)", tok, def_r, def_g, def_b, ovr.r, ovr.g, ovr.b, hash,
                          state);
            }
        }
    }

    void dump_all_slots() noexcept
    {
        auto &log = DetourModKit::Logger::get_instance();
        log.debug("[swatch-dump] ===== begin all-slot snapshot =====");
        for (std::size_t s = 0; s < k_slotCount; ++s)
            dump_slot(static_cast<int>(s));
        log.debug("[swatch-dump] ===== end all-slot snapshot =====");
    }

    void lock_all_slots() noexcept
    {
        for (auto &b : g_postReinitLock)
            b.store(true, std::memory_order_release);
        for (auto &b : g_reinitCaptureOpen)
            b.store(false, std::memory_order_release);
        DetourModKit::Logger::get_instance().info("[swatch-table] strict-init default applied: all slots "
                                                  "LOCKED (inserts blocked outside Reinit capture window)");
    }

    SlotCounts slot_counts(int slot) noexcept
    {
        SlotCounts out{};
        if (!valid_slot(slot))
            return out;
        const auto s = static_cast<std::size_t>(slot);
        const auto cnt = g_count[s].load(std::memory_order_acquire);
        const auto upper = (cnt < k_dyeSwatchesPerSlot) ? cnt : k_dyeSwatchesPerSlot;
        out.total = upper;
        for (std::size_t i = 0; i < upper; ++i)
        {
            const auto &e = g_table[s][i];
            const auto tok = e.token_id.load(std::memory_order_relaxed);
            if (tok == 0)
                continue;
            const auto hash = e.content_hash.load(std::memory_order_relaxed);
            if (hash == 0)
                ++out.placeholders;
            else
                ++out.promoted;
            if (dye_state()[s].swatches[i].override_active)
                ++out.active_overrides;
        }
        return out;
    }

    KeepResult apply_keep_set(int slot, const SwatchIdentity *keep, std::size_t keep_count) noexcept
    {
        KeepResult r{};
        if (!valid_slot(slot))
            return r;
        const auto s = static_cast<std::size_t>(slot);
        const auto cnt = g_count[s].load(std::memory_order_acquire);
        const auto upper = (cnt < k_dyeSwatchesPerSlot) ? cnt : k_dyeSwatchesPerSlot;
        DetourModKit::Logger::get_instance().info("[swatch-table] apply_keep_set slot={} cnt={} upper={} "
                                                  "keep_count={}",
                                                  slot, cnt, upper, keep_count);
        for (std::size_t i = 0; i < upper; ++i)
        {
            auto &e = g_table[s][i];
            const auto h = e.content_hash.load(std::memory_order_relaxed);
            if (h == 0)
                continue;
            SwatchIdentity k{};
            k.hash = h;
            k.stable = e.stable_id.load(std::memory_order_relaxed);
            k.tpl = e.template_id.load(std::memory_order_relaxed);
            k.token = e.token_id.load(std::memory_order_relaxed);
            bool in_keep = false;
            for (std::size_t j = 0; j < keep_count; ++j)
                if (keep[j] == k)
                {
                    in_keep = true;
                    break;
                }
            if (in_keep)
            {
                e.frozen_hidden.store(false, std::memory_order_release);
                e.active_this_apply.store(true, std::memory_order_release);
                ++r.kept;
            }
            else
            {
                e.active_this_apply.store(false, std::memory_order_release);
                e.frozen_hidden.store(true, std::memory_order_release);
                ++r.hidden;
            }
        }
        return r;
    }
} // namespace Transmog::ColorOverride::SwatchTable
