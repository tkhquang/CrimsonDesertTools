#include "color_reinit.hpp"

#include "color_state.hpp"
#include "color_swatch_table.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "transmog.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Transmog::color_override::reinit
{
    namespace
    {
        // Per-pass timing. The debounced apply worker has ~100 ms of latency, so 1500 ms leaves enough headroom for the
        // untick apply to drive the engine's natural-pipeline real-armor restore before the retick tears it down again.
        constexpr std::int64_t REINIT_TEARDOWN_MS = 1500;
        constexpr std::int64_t REINIT_CAPTURE_MS = 1500;

        struct SlotReinitState
        {
            enum Phase : int
            {
                Idle = 0,
                TeardownApply,
                TeardownWait,
                CarrierApply,
                CarrierWait,
                Finalize,
            };
            enum Mode : int
            {
                ModeCommitRetick = 1,
                ModeReinit1Pass = 2,
            };
            std::atomic<int> phase{Idle};
            std::atomic<int> mode{ModeReinit1Pass};
            std::atomic<int> pass{0};
            std::atomic<std::int64_t> deadline_ms{0};
            // Captured identity tuples per pass. Guarded by g_mutex because the snapshot grows under a lock in tick()
            // and the UI may peek at it for diagnostics.
            std::vector<swatch_table::SwatchIdentity> seen[3];
        };

        std::array<SlotReinitState, SLOT_COUNT> g_state{};
        std::mutex g_mutex;
        std::array<std::atomic<std::uint32_t>, SLOT_COUNT> g_lastTargetItemId{};

        bool valid_slot(int slot) noexcept
        {
            return slot >= 0 && static_cast<std::size_t>(slot) < SLOT_COUNT;
        }
    } // namespace

    bool start_slot_reinit_once(int slot) noexcept
    {
        if (!valid_slot(slot))
            return false;
        auto &m = slot_mappings()[static_cast<std::size_t>(slot)];
        if (!m.active || m.target_item_id == 0)
        {
            DMK::log().debug(
                "[color-reinit] start-once slot={} REJECTED (active={} target={:#06x})",
                slot,
                m.active,
                m.target_item_id
            );
            return false;
        }
        auto &st = g_state[static_cast<std::size_t>(slot)];
        int expected = SlotReinitState::Idle;
        if (!st.phase.compare_exchange_strong(expected, SlotReinitState::TeardownApply, std::memory_order_acq_rel))
            return false;
        st.mode.store(SlotReinitState::ModeReinit1Pass, std::memory_order_release);
        st.pass.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            for (auto &v : st.seen)
                v.clear();
        }
        swatch_table::post_reinit_lock(slot).store(false, std::memory_order_release);
        swatch_table::reinit_capture_open(slot).store(true, std::memory_order_release);
        DMK::log().debug("[color-reinit] slot={} START 1-pass target={:#06x}", slot, m.target_item_id);
        return true;
    }

    bool schedule_color_commit_retick(int slot) noexcept
    {
        if (!valid_slot(slot))
            return false;
        auto &m = slot_mappings()[static_cast<std::size_t>(slot)];
        if (!m.active || m.target_item_id == 0)
            return false;
        auto &st = g_state[static_cast<std::size_t>(slot)];
        // Coalesce: if a commit-retick is already in flight, drop.
        const int cur_phase = st.phase.load(std::memory_order_acquire);
        if (cur_phase != SlotReinitState::Idle)
        {
            const int cur_mode = st.mode.load(std::memory_order_acquire);
            return cur_mode == SlotReinitState::ModeCommitRetick;
        }
        // The unmount is REQUIRED, not ceremony. Entering at CarrierApply - one apply, no tear-down - runs cleanly
        // and the new color simply does not take: the engine keeps the material state it already resolved and only
        // re-reads it after the slot is torn down. Measured, so do not "optimise" this into a single apply.
        //
        // Dye is different and does not need this: its records are injected on the next DyeCopier call, which a plain
        // per-slot rebuild drives (see refresh_slot_appearance).
        int expected = SlotReinitState::Idle;
        if (!st.phase.compare_exchange_strong(expected, SlotReinitState::TeardownApply, std::memory_order_acq_rel))
            return st.mode.load(std::memory_order_acquire) == SlotReinitState::ModeCommitRetick;
        st.mode.store(SlotReinitState::ModeCommitRetick, std::memory_order_release);
        st.pass.store(0, std::memory_order_release);
        DMK::log().debug("[color-reinit] slot={} commit-retick scheduled target={:#06x}", slot, m.target_item_id);
        return true;
    }

    bool is_slot_reinit_active(int slot) noexcept
    {
        if (!valid_slot(slot))
            return false;
        return g_state[static_cast<std::size_t>(slot)].phase.load(std::memory_order_acquire) != SlotReinitState::Idle;
    }

    bool is_color_commit_retick_active(int slot) noexcept
    {
        if (!valid_slot(slot))
            return false;
        auto &st = g_state[static_cast<std::size_t>(slot)];
        return st.phase.load(std::memory_order_acquire) != SlotReinitState::Idle &&
               st.mode.load(std::memory_order_acquire) == SlotReinitState::ModeCommitRetick;
    }

    bool any_slot_reinit_active() noexcept
    {
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            if (g_state[i].phase.load(std::memory_order_acquire) != SlotReinitState::Idle)
                return true;
        return false;
    }

    void cancel(int slot) noexcept
    {
        if (!valid_slot(slot))
            return;
        auto &st = g_state[static_cast<std::size_t>(slot)];
        st.phase.store(SlotReinitState::Idle, std::memory_order_release);
        st.pass.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            for (auto &v : st.seen)
                v.clear();
        }
        swatch_table::reinit_capture_open(slot).store(false, std::memory_order_release);
    }

    void notify_transmog_target(int slot, std::uint32_t new_target_item_id) noexcept
    {
        if (!valid_slot(slot))
            return;
        const auto last = g_lastTargetItemId[static_cast<std::size_t>(slot)].load(std::memory_order_acquire);
        if (new_target_item_id == 0)
        {
            // Untick / clear: reset tracking but DO NOT wipe - user override choices on tick->untick->re-tick of the
            // same item should survive.
            g_lastTargetItemId[static_cast<std::size_t>(slot)].store(0, std::memory_order_release);
            return;
        }
        if (last != 0 && last != new_target_item_id)
        {
            // Target changed -> drop stale rows.
            swatch_table::wipe_swatch_table_for_slot(slot);
            cancel(slot);
        }
        g_lastTargetItemId[static_cast<std::size_t>(slot)].store(new_target_item_id, std::memory_order_release);
    }

    void reset_target_tracking() noexcept
    {
        for (auto &t : g_lastTargetItemId)
            t.store(0, std::memory_order_release);
    }

    void tick() noexcept
    {
        const auto now = state::now_ms();
        for (int slot = 0; slot < static_cast<int>(SLOT_COUNT); ++slot)
        {
            auto &st = g_state[static_cast<std::size_t>(slot)];
            const int phase = st.phase.load(std::memory_order_acquire);
            if (phase == SlotReinitState::Idle)
                continue;
            auto &m = slot_mappings()[static_cast<std::size_t>(slot)];

            if (phase == SlotReinitState::TeardownApply)
            {
                const auto prev_target = m.target_item_id;
                m.active = false;
                // Bypass apply_single_slot_transmog's equality early-out and any other "nothing to do" shortcuts. The
                // whole point of reinit is to force the engine through a full tear-down and restore cycle, even
                // when the engine thinks state is unchanged.
                force_apply_pending()[static_cast<std::size_t>(slot)] = true;
                DMK::log().debug(
                    "[color-reinit] slot={} TeardownApply pass={} "
                    "(prev_target={:#06x}, m.active=false; engine should tear down fake + restore real if any)",
                    slot,
                    st.pass.load(std::memory_order_acquire),
                    prev_target
                );
                ::Transmog::manual_apply_slot(static_cast<std::size_t>(slot));
                st.deadline_ms.store(now + REINIT_TEARDOWN_MS, std::memory_order_release);
                st.phase.store(SlotReinitState::TeardownWait, std::memory_order_release);
                continue;
            }
            if (phase == SlotReinitState::TeardownWait)
            {
                if (now < st.deadline_ms.load(std::memory_order_acquire))
                    continue;
                const auto last_applied = last_applied_ids()[static_cast<std::size_t>(slot)];
                DMK::log().debug(
                    "[color-reinit] slot={} TeardownWait done (lastIds[slot]={:#06x}; expected 0 if untick completed)",
                    slot,
                    last_applied
                );
                st.phase.store(SlotReinitState::CarrierApply, std::memory_order_release);
                continue;
            }
            if (phase == SlotReinitState::CarrierApply)
            {
                m.active = true;
                force_apply_pending()[static_cast<std::size_t>(slot)] = true;
                // CRITICAL: reset active_this_apply on every row BEFORE the retick fires. The setter sets the flag back
                // to true on rows that fire property writes this pass; rows that DON'T fire stay inactive.
                // snapshot_active_identities then returns only the rows actually seen this pass.
                //
                // Without this reset, active_this_apply persists (it is only set true on insert/hit, never cleared) so
                // the snapshot would return the full accumulated set rather than the live identities for this capture.
                swatch_table::mark_all_inactive(slot);
                DMK::log().debug(
                    "[color-reinit] slot={} CarrierApply pass={} "
                    "(target={:#06x}, m.active=true, all rows reset to inactive)",
                    slot,
                    st.pass.load(std::memory_order_acquire),
                    m.target_item_id
                );
                ::Transmog::manual_apply_slot(static_cast<std::size_t>(slot));
                st.deadline_ms.store(now + REINIT_CAPTURE_MS, std::memory_order_release);
                st.phase.store(SlotReinitState::CarrierWait, std::memory_order_release);
                continue;
            }
            if (phase == SlotReinitState::CarrierWait)
            {
                if (now < st.deadline_ms.load(std::memory_order_acquire))
                    continue;
                if (st.mode.load(std::memory_order_acquire) == SlotReinitState::ModeCommitRetick)
                {
                    DMK::log().debug("[color-reinit] slot={} commit-retick done", slot);
                    st.pass.store(0, std::memory_order_release);
                    st.phase.store(SlotReinitState::Idle, std::memory_order_release);
                    continue;
                }
                const int pass = st.pass.load(std::memory_order_acquire);
                swatch_table::SwatchIdentity buf[::Transmog::color_override::DYE_SWATCHES_PER_SLOT]{};
                const auto captured = swatch_table::snapshot_active_identities(
                    slot,
                    buf,
                    ::Transmog::color_override::DYE_SWATCHES_PER_SLOT
                );
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    if (pass >= 0 && pass < 3)
                    {
                        st.seen[pass].clear();
                        st.seen[pass].reserve(captured);
                        for (std::size_t i = 0; i < captured; ++i)
                            st.seen[pass].push_back(buf[i]);
                    }
                }
                DMK::log().debug("[color-reinit] slot={} pass={} captured={}", slot, pass + 1, captured);
                // Single-pass capture: go straight to Finalize. The intersection logic there handles non-empty pass
                // selection - with only seen[0] populated it becomes "keep everything captured this pass" (no ghost
                // filtering).
                st.phase.store(SlotReinitState::Finalize, std::memory_order_release);
                continue;
            }
            if (phase == SlotReinitState::Finalize)
            {
                // Intersection over NON-empty passes only. Empty pass means "no data" (publisher pipeline wedged that
                // cycle); treating {} as 0 wipes everything. Skip empty passes instead.
                std::vector<swatch_table::SwatchIdentity> keep;
                std::array<std::size_t, 3> pass_counts{
                    0,
                    0,
                    0,
                };
                std::size_t non_empty = 0;
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    for (int p = 0; p < 3; ++p)
                    {
                        pass_counts[p] = st.seen[p].size();
                        if (!st.seen[p].empty())
                            ++non_empty;
                    }
                    // DIAG: log every identity each pass captured. Helps debug the intersection - you should see up to
                    // 32 unique tuples in seen[0] and a subset in seen[1] / seen[2]. If sizes are inconsistent with the
                    // per-pass `captured` lines, something is racing on g_mutex.
                    for (int p = 0; p < 3; ++p)
                    {
                        DMK::log().debug("[color-reinit] slot={} seen[{}] size={}", slot, p, st.seen[p].size());
                        for (std::size_t i = 0; i < st.seen[p].size(); ++i)
                        {
                            const auto &k = st.seen[p][i];
                            DMK::log().debug(
                                "[color-reinit]   p{}[{}] hash={:08X} stable={:016X} tpl={:04X} token={:04X}",
                                p + 1,
                                i,
                                k.hash,
                                k.stable,
                                k.tpl,
                                k.token
                            );
                        }
                    }
                    int first_non_empty = -1;
                    for (int p = 0; p < 3; ++p)
                        if (!st.seen[p].empty())
                        {
                            first_non_empty = p;
                            break;
                        }
                    if (first_non_empty >= 0)
                    {
                        for (const auto &a : st.seen[first_non_empty])
                        {
                            bool in_all = true;
                            for (int p = 0; p < 3 && in_all; ++p)
                            {
                                if (p == first_non_empty)
                                    continue;
                                if (st.seen[p].empty())
                                    continue;
                                bool found = false;
                                for (const auto &x : st.seen[p])
                                    if (x == a)
                                    {
                                        found = true;
                                        break;
                                    }
                                if (!found)
                                    in_all = false;
                            }
                            if (in_all)
                                keep.push_back(a);
                        }
                    }
                    for (auto &v : st.seen)
                        v.clear();
                }
                const auto kr = swatch_table::apply_keep_set(slot, keep.data(), keep.size());
                swatch_table::post_reinit_lock(slot).store(true, std::memory_order_release);
                swatch_table::reinit_capture_open(slot).store(false, std::memory_order_release);
                // Auto-tick the master Dye toggle the UI binds to dye_state()[slot].slot_enabled. The UI looks at
                // swatch_table::slot_enabled_get(slot) for the master toggle and the substitute path gates on it too.
                // After a fresh reinit capture the slot is ready to substitute, so flip the toggle ON so the user
                // does not have to click it manually before colors start applying.
                swatch_table::slot_enabled_set(slot, true);
                // Mark dirty so the user knows to click Save - but do NOT auto-save. Auto-saving here also commits any
                // unrelated pending edits (e.g. user picks the user might want to revert before saving), which breaks
                // the "pending until Save" contract the other UI buttons honour.
                ::Transmog::dye_dirty().store(true, std::memory_order_release);
                DMK::log().debug(
                    "[color-reinit] slot={} DONE intersection={} "
                    "kept={} hidden={} (pass1={} pass2={} pass3={} nonEmpty={}) - slot LOCKED",
                    slot,
                    keep.size(),
                    kr.kept,
                    kr.hidden,
                    pass_counts[0],
                    pass_counts[1],
                    pass_counts[2],
                    non_empty
                );
                st.pass.store(0, std::memory_order_release);
                st.phase.store(SlotReinitState::Idle, std::memory_order_release);
                continue;
            }
        }
    }
} // namespace Transmog::color_override::reinit
