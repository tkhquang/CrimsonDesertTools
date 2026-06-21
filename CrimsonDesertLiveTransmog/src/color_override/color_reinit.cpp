#include "color_reinit.hpp"
#include "color_swatch_table.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "transmog.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace DMK = DetourModKit;

namespace Transmog::ColorOverride::Reinit
{
    namespace
    {
        // Per-pass timing. The debounced apply worker has ~100 ms of latency, so 1500 ms leaves enough headroom for the
        // untick apply to drive the engine's natural-pipeline real-armor restore before the retick tears it down again.
        constexpr std::int64_t k_reinitTeardownMs = 1500;
        constexpr std::int64_t k_reinitCaptureMs = 1500;

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
            std::vector<SwatchTable::SwatchIdentity> seen[3];
        };

        std::array<SlotReinitState, k_slotCount> g_state{};
        std::mutex g_mutex;
        std::array<std::atomic<std::uint32_t>, k_slotCount> g_lastTargetItemId{};

        std::int64_t now_ms() noexcept
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        bool valid_slot(int slot) noexcept
        {
            return slot >= 0 && static_cast<std::size_t>(slot) < k_slotCount;
        }
    } // namespace

    bool start_slot_reinit_once(int slot) noexcept
    {
        if (!valid_slot(slot))
            return false;
        auto &m = slot_mappings()[static_cast<std::size_t>(slot)];
        if (!m.active || m.targetItemId == 0)
        {
            DMK::Logger::get_instance().debug("[color-reinit] start-once slot={} REJECTED "
                                              "(active={} target={:#06x})",
                                              slot, m.active, m.targetItemId);
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
        SwatchTable::post_reinit_lock(slot).store(false, std::memory_order_release);
        SwatchTable::reinit_capture_open(slot).store(true, std::memory_order_release);
        DMK::Logger::get_instance().debug("[color-reinit] slot={} START 1-pass target={:#06x}", slot, m.targetItemId);
        return true;
    }

    bool schedule_color_commit_retick(int slot) noexcept
    {
        if (!valid_slot(slot))
            return false;
        auto &m = slot_mappings()[static_cast<std::size_t>(slot)];
        if (!m.active || m.targetItemId == 0)
            return false;
        auto &st = g_state[static_cast<std::size_t>(slot)];
        // Coalesce: if a commit-retick is already in flight, drop.
        const int curPhase = st.phase.load(std::memory_order_acquire);
        if (curPhase != SlotReinitState::Idle)
        {
            const int curMode = st.mode.load(std::memory_order_acquire);
            return curMode == SlotReinitState::ModeCommitRetick;
        }
        int expected = SlotReinitState::Idle;
        if (!st.phase.compare_exchange_strong(expected, SlotReinitState::TeardownApply, std::memory_order_acq_rel))
            return st.mode.load(std::memory_order_acquire) == SlotReinitState::ModeCommitRetick;
        st.mode.store(SlotReinitState::ModeCommitRetick, std::memory_order_release);
        st.pass.store(0, std::memory_order_release);
        DMK::Logger::get_instance().debug("[color-reinit] slot={} commit-retick scheduled "
                                          "target={:#06x}",
                                          slot, m.targetItemId);
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
        for (std::size_t i = 0; i < k_slotCount; ++i)
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
        SwatchTable::reinit_capture_open(slot).store(false, std::memory_order_release);
    }

    void notify_transmog_target(int slot, std::uint32_t newTargetItemId) noexcept
    {
        if (!valid_slot(slot))
            return;
        const auto last = g_lastTargetItemId[static_cast<std::size_t>(slot)].load(std::memory_order_acquire);
        if (newTargetItemId == 0)
        {
            // Untick / clear: reset tracking but DO NOT wipe -- user override choices on tick->untick->re-tick of the
            // same item should survive.
            g_lastTargetItemId[static_cast<std::size_t>(slot)].store(0, std::memory_order_release);
            return;
        }
        if (last != 0 && last != newTargetItemId)
        {
            // Target changed -> drop stale rows.
            SwatchTable::wipe_swatch_table_for_slot(slot);
            cancel(slot);
        }
        g_lastTargetItemId[static_cast<std::size_t>(slot)].store(newTargetItemId, std::memory_order_release);
    }

    void reset_target_tracking() noexcept
    {
        for (auto &t : g_lastTargetItemId)
            t.store(0, std::memory_order_release);
    }

    void tick() noexcept
    {
        const auto now = now_ms();
        for (int slot = 0; slot < static_cast<int>(k_slotCount); ++slot)
        {
            auto &st = g_state[static_cast<std::size_t>(slot)];
            const int phase = st.phase.load(std::memory_order_acquire);
            if (phase == SlotReinitState::Idle)
                continue;
            auto &m = slot_mappings()[static_cast<std::size_t>(slot)];

            if (phase == SlotReinitState::TeardownApply)
            {
                const auto prevTarget = m.targetItemId;
                m.active = false;
                // Bypass apply_single_slot_transmog's equality early-out and any other "nothing to do" shortcuts. The
                // reinit's whole point is to force the engine through a full tear-down + restore cycle, even if the
                // engine thinks state is unchanged.
                force_apply_pending()[static_cast<std::size_t>(slot)] = true;
                DMK::Logger::get_instance().debug("[color-reinit] slot={} TeardownApply pass={} "
                                                  "(prev_target={:#06x}, m.active=false; engine "
                                                  "should tear down fake + restore real if any)",
                                                  slot, st.pass.load(std::memory_order_acquire), prevTarget);
                ::Transmog::manual_apply_slot(static_cast<std::size_t>(slot));
                st.deadline_ms.store(now + k_reinitTeardownMs, std::memory_order_release);
                st.phase.store(SlotReinitState::TeardownWait, std::memory_order_release);
                continue;
            }
            if (phase == SlotReinitState::TeardownWait)
            {
                if (now < st.deadline_ms.load(std::memory_order_acquire))
                    continue;
                const auto lastApplied = last_applied_ids()[static_cast<std::size_t>(slot)];
                DMK::Logger::get_instance().debug("[color-reinit] slot={} TeardownWait done "
                                                  "(lastIds[slot]={:#06x}; expected 0 if untick "
                                                  "completed)",
                                                  slot, lastApplied);
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
                // Without this reset, active_this_apply persists (it's only set true on insert/hit, never cleared) so
                // the snapshot would return the full accumulated set rather than the live identities for this capture.
                SwatchTable::mark_all_inactive(slot);
                DMK::Logger::get_instance().debug("[color-reinit] slot={} CarrierApply pass={} "
                                                  "(target={:#06x}, m.active=true, all rows "
                                                  "reset to inactive)",
                                                  slot, st.pass.load(std::memory_order_acquire), m.targetItemId);
                ::Transmog::manual_apply_slot(static_cast<std::size_t>(slot));
                st.deadline_ms.store(now + k_reinitCaptureMs, std::memory_order_release);
                st.phase.store(SlotReinitState::CarrierWait, std::memory_order_release);
                continue;
            }
            if (phase == SlotReinitState::CarrierWait)
            {
                if (now < st.deadline_ms.load(std::memory_order_acquire))
                    continue;
                if (st.mode.load(std::memory_order_acquire) == SlotReinitState::ModeCommitRetick)
                {
                    DMK::Logger::get_instance().debug("[color-reinit] slot={} commit-retick done", slot);
                    st.pass.store(0, std::memory_order_release);
                    st.phase.store(SlotReinitState::Idle, std::memory_order_release);
                    continue;
                }
                const int pass = st.pass.load(std::memory_order_acquire);
                SwatchTable::SwatchIdentity buf[::Transmog::ColorOverride::k_dyeSwatchesPerSlot]{};
                const auto captured =
                    SwatchTable::snapshot_active_identities(slot, buf, ::Transmog::ColorOverride::k_dyeSwatchesPerSlot);
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
                DMK::Logger::get_instance().debug("[color-reinit] slot={} pass={} captured={}", slot, pass + 1,
                                                  captured);
                // Single-pass capture: go straight to Finalize. The intersection logic there handles non-empty pass
                // selection -- with only seen[0] populated it becomes "keep everything captured this pass" (no ghost
                // filtering).
                st.phase.store(SlotReinitState::Finalize, std::memory_order_release);
                continue;
            }
            if (phase == SlotReinitState::Finalize)
            {
                // Intersection over NON-empty passes only. Empty pass means "no data" (publisher pipeline wedged that
                // cycle); treating {} as 0 wipes everything. Skip empty passes instead.
                std::vector<SwatchTable::SwatchIdentity> keep;
                std::array<std::size_t, 3> passCounts{0, 0, 0};
                std::size_t nonEmpty = 0;
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    for (int p = 0; p < 3; ++p)
                    {
                        passCounts[p] = st.seen[p].size();
                        if (!st.seen[p].empty())
                            ++nonEmpty;
                    }
                    // DIAG: log every identity each pass captured. Helps debug the intersection -- you should see up to
                    // 32 unique tuples in seen[0] and a subset in seen[1] / seen[2]. If sizes are inconsistent with the
                    // per-pass `captured` lines, something is racing on g_mutex.
                    for (int p = 0; p < 3; ++p)
                    {
                        DMK::Logger::get_instance().debug("[color-reinit] slot={} seen[{}] size={}", slot, p,
                                                          st.seen[p].size());
                        for (std::size_t i = 0; i < st.seen[p].size(); ++i)
                        {
                            const auto &k = st.seen[p][i];
                            DMK::Logger::get_instance().debug("[color-reinit]   p{}[{}] hash={:08X} "
                                                              "stable={:016X} tpl={:04X} token={:04X}",
                                                              p + 1, i, k.hash, k.stable, k.tpl, k.token);
                        }
                    }
                    int firstNonEmpty = -1;
                    for (int p = 0; p < 3; ++p)
                        if (!st.seen[p].empty())
                        {
                            firstNonEmpty = p;
                            break;
                        }
                    if (firstNonEmpty >= 0)
                    {
                        for (const auto &a : st.seen[firstNonEmpty])
                        {
                            bool in_all = true;
                            for (int p = 0; p < 3 && in_all; ++p)
                            {
                                if (p == firstNonEmpty)
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
                const auto kr = SwatchTable::apply_keep_set(slot, keep.data(), keep.size());
                SwatchTable::post_reinit_lock(slot).store(true, std::memory_order_release);
                SwatchTable::reinit_capture_open(slot).store(false, std::memory_order_release);
                // Auto-tick the master Dye toggle the UI binds to dye_state()[slot].slot_enabled. The UI looks at
                // SwatchTable::slot_enabled_get(slot) for the master toggle and the substitute path gates on it too.
                // After a fresh reinit capture the slot is ready to substitute, so flip the toggle ON so the user
                // doesn't have to click it manually before colours start applying.
                SwatchTable::slot_enabled_set(slot, true);
                // Mark dirty so the user knows to click Save -- but do NOT auto-save. Auto-saving here also commits any
                // unrelated pending edits (e.g. user picks the user might want to revert before saving), which breaks
                // the "pending until Save" contract the other UI buttons honour.
                ::Transmog::dye_dirty().store(true, std::memory_order_release);
                DMK::Logger::get_instance().debug("[color-reinit] slot={} DONE intersection={} "
                                                  "kept={} hidden={} (pass1={} pass2={} pass3={} "
                                                  "nonEmpty={}) -- slot LOCKED",
                                                  slot, keep.size(), kr.kept, kr.hidden, passCounts[0], passCounts[1],
                                                  passCounts[2], nonEmpty);
                st.pass.store(0, std::memory_order_release);
                st.phase.store(SlotReinitState::Idle, std::memory_order_release);
                continue;
            }
        }
    }
} // namespace Transmog::ColorOverride::Reinit
