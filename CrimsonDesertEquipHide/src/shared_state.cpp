#include "shared_state.hpp"

namespace EquipHide
{
    static ResolvedAddresses s_resolvedAddrs{};
    // Initialise visCharIdx to -1 (unknown) so any pre-resolve consumer
    // (e.g. apply_direct_vis_write firing before resolve_player_vis_ctrls
    // has populated the slots) treats the entry as "no per-character
    // override -- fall back to the active character's hide mask".
    // PlayerState's default ctor zero-inits the array; bring up an
    // initialiser helper invoked at first access via construct-on-first-use.
    static PlayerState &make_player_state() noexcept
    {
        static PlayerState state{};
        static const bool s_seeded = []() noexcept
        {
            for (int i = 0; i < k_maxProtagonists; ++i)
                state.visCharIdx[i].store(-1, std::memory_order_relaxed);
            return true;
        }();
        (void)s_seeded;
        return state;
    }

    static std::mutex s_visWriteMtx;
    static std::unordered_map<VisKey, uint8_t, VisKeyHash> s_originalVis;
    static std::atomic<bool> s_needsDirectWrite{false};

    static std::atomic<bool> s_playerOnly{true};
    static std::atomic<bool> s_forceShow{false};
    static std::atomic<bool> s_baldFix{true};
    static std::atomic<bool> s_glidingFix{true};
    static std::atomic<bool> s_fallbackMode{false};
    static std::atomic<bool> s_independentToggle{false};
    static std::atomic<bool> s_cascadeFix{false};

    static std::atomic<bool> s_shutdownRequested{false};
    static std::atomic<bool> s_deferredScanPending{false};
    static std::atomic<bool> s_lazyProbePending{false};
    static std::atomic<int64_t> s_lazyProbeSignal{0};

    ResolvedAddresses &resolved_addrs() { return s_resolvedAddrs; }
    PlayerState &player_state() { return make_player_state(); }
    std::mutex &vis_write_mutex() { return s_visWriteMtx; }
    std::unordered_map<VisKey, uint8_t, VisKeyHash> &original_vis_map() { return s_originalVis; }
    std::atomic<bool> &needs_direct_write() { return s_needsDirectWrite; }

    std::atomic<bool> &flag_player_only() { return s_playerOnly; }
    std::atomic<bool> &flag_force_show() { return s_forceShow; }
    std::atomic<bool> &flag_bald_fix() { return s_baldFix; }
    std::atomic<bool> &flag_gliding_fix() { return s_glidingFix; }
    std::atomic<bool> &flag_fallback_mode() { return s_fallbackMode; }
    std::atomic<bool> &flag_independent_toggle() { return s_independentToggle; }
    std::atomic<bool> &flag_cascade_fix() { return s_cascadeFix; }

    std::atomic<bool> &shutdown_requested() { return s_shutdownRequested; }
    std::atomic<bool> &deferred_scan_pending() { return s_deferredScanPending; }
    std::atomic<bool> &lazy_probe_pending() { return s_lazyProbePending; }
    std::atomic<int64_t> &lazy_probe_signal() { return s_lazyProbeSignal; }

} // namespace EquipHide
