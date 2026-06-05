#include "color_override.hpp"
#include "color_carrier_set.hpp"
#include "color_matinst_owner.hpp"
#include "color_pending_overrides.hpp"
#include "color_publisher_hook.hpp"
#include "color_reinit.hpp"
#include "color_state.hpp"
#include "color_swatch_table.hpp"
#include "color_token_table.hpp"

#include "../shared_state.hpp"

#include <DetourModKit.hpp>

#include <atomic>

namespace DMK = DetourModKit;

namespace Transmog::ColorOverride
{
    namespace
    {
        std::atomic<bool> g_initDone{false};
    }

    bool init()
    {
        if (g_initDone.load(std::memory_order_acquire))
            return true;

        auto &log = DMK::Logger::get_instance();
        log.info("[color-override] init");

        // Every slot starts LOCKED. Setter inserts happen only inside
        // an explicit Reinit capture window so unrelated property
        // writes can't bloat the swatch table outside the apply path.
        SwatchTable::lock_all_slots();

        TokenTable::bootstrap_snapshot();

        // The setter mid-hook is installed separately by
        // `SetterSubstitute::init()` from the top-level startup path;
        // this entry point only wires the publisher hook.
        const bool pubOk = PublisherHook::init();

        log.info("[color-override] hook install: publisher={}", pubOk);

        g_initDone.store(true, std::memory_order_release);
        return pubOk;
    }

    void mark_apply_begin(int slot) noexcept
    {
        if (!::Transmog::flag_color_override().load(
                std::memory_order_acquire))
            return;
        State::active_apply_slot().store(slot, std::memory_order_release);
        State::active_apply_valid_until_ms().store(
            State::now_ms() + State::k_batchApplyExtendMs,
            std::memory_order_release);
        State::hash_set_last_add_ms(slot).store(0, std::memory_order_release);
        SwatchTable::mark_all_inactive(slot);
    }

    void mark_apply_end() noexcept
    {
        if (!::Transmog::flag_color_override().load(
                std::memory_order_acquire))
            return;
        // The active slot is NOT cleared here. The publisher fires
        // per-frame asynchronously after the synchronous slotPop
        // returns; clearing the slot would cause every async
        // publisher fire to bail at the slot=-1 gate and miss
        // legitimate captures. Window validity is instead governed
        // by `active_apply_valid_until_ms`, set to `now + 3s` in
        // mark_apply_begin -- the publisher's window check rejects
        // late fires without needing the slot field to flip.
        log_counters();
        const int slot = State::active_apply_slot().load(
            std::memory_order_acquire);
        if (slot >= 0)
        {
            // A non-zero `placeholders` count after the apply means
            // saved (submesh, token) pairs never matched a live
            // engine write -- the user's colours won't substitute
            // until the player re-runs Reinit to capture fresh
            // engine writes for those rows.
            const auto sc = SwatchTable::slot_counts(slot);
            DMK::Logger::get_instance().info(
                "[swatch-summary] slot={} total={} promoted={} "
                "placeholders={} active_overrides={}",
                slot, sc.total, sc.promoted, sc.placeholders,
                sc.active_overrides);
            SwatchTable::dump_slot(slot);
        }
    }

    void wipe_slot(int slot) noexcept
    {
        if (!::Transmog::flag_color_override().load(
                std::memory_order_acquire))
            return;
        CarrierSet::clear_slot(slot);
        SwatchTable::clear_slot(slot);
        MatInstOwner::clear_for_slot(slot);
        // PendingOverrides is deliberately NOT cleared here. wipe_slot
        // fires on every preset-driven slot rebuild; clearing pending
        // would drop persisted user picks the moment a preset is
        // applied, before the setter has a chance to consume them.
        // The pending map is only cleared by reset_all() on explicit
        // preset / character switch.
        State::block_publisher_inserts_until_ms().store(
            State::now_ms() + 500, std::memory_order_release);
    }

    void reset_all() noexcept
    {
        if (!::Transmog::flag_color_override().load(
                std::memory_order_acquire))
            return;
        DMK::Logger::get_instance().debug(
            "[color-override] reset_all -- wiping all slot swatch "
            "tables + pending overrides (preset / character switch)");
        CarrierSet::clear_all();
        SwatchTable::clear_all();
        MatInstOwner::clear_all();
        // Pending overrides ARE cleared here. The new preset's
        // entries must replace (not augment) the old set; PresetManager
        // calls restore_swatches_from() immediately after, which
        // repopulates the pending map via
        // SwatchTable::restore_persisted_state.
        PendingOverrides::clear_all();
        // Zero the "last applied transmog target" tracking too -- the
        // apply pass that follows a preset switch otherwise sees
        // (last_target != new_target) inside notify_transmog_target
        // and calls wipe_swatch_table_for_slot, which would destroy
        // the placeholders just seeded for the new preset.
        Reinit::reset_target_tracking();
    }

    void log_counters() noexcept
    {
        const auto p = PublisherHook::snapshot_stats();
        const auto pe = PendingOverrides::snapshot_stats();
        DMK::Logger::get_instance().info(
            "[color-override] pub[entries={} inserts={} batch={} window={} "
            "host={} arec={}] "
            "pending[entries={} hits={} misses={}]",
            p.entries, p.inserts, p.batch_rejects, p.window_rejects,
            p.host_rejects, p.arec_rejects,
            pe.entries_total, pe.lookups_hit, pe.lookups_miss);
    }
}
