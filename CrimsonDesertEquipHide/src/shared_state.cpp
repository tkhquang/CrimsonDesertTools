#include "shared_state.hpp"

namespace EquipHide
{
    static ResolvedAddresses s_resolvedAddrs{};
    static PlayerState s_playerState{};
    static std::mutex s_visWriteMtx;
    static std::unordered_map<uintptr_t, uint8_t> s_originalVis;
    static std::atomic<bool> s_needsDirectWrite{false};

    static std::atomic<bool> s_playerOnly{true};
    static std::atomic<bool> s_forceShow{false};
    static std::atomic<bool> s_baldFix{true};
    static std::atomic<bool> s_glidingFix{true};
    static std::atomic<bool> s_fallbackMode{false};

    static std::atomic<bool> s_shutdownRequested{false};
    static std::atomic<bool> s_deferredScanPending{false};
    static std::atomic<bool> s_lazyProbePending{false};
    static std::atomic<int64_t> s_lazyProbeSignal{0};

    ResolvedAddresses &resolved_addrs() { return s_resolvedAddrs; }
    PlayerState &player_state() { return s_playerState; }
    std::mutex &vis_write_mutex() { return s_visWriteMtx; }
    std::unordered_map<uintptr_t, uint8_t> &original_vis_map() { return s_originalVis; }
    std::atomic<bool> &needs_direct_write() { return s_needsDirectWrite; }

    std::atomic<bool> &flag_player_only() { return s_playerOnly; }
    std::atomic<bool> &flag_force_show() { return s_forceShow; }
    std::atomic<bool> &flag_bald_fix() { return s_baldFix; }
    std::atomic<bool> &flag_gliding_fix() { return s_glidingFix; }
    std::atomic<bool> &flag_fallback_mode() { return s_fallbackMode; }

    std::atomic<bool> &shutdown_requested() { return s_shutdownRequested; }
    std::atomic<bool> &deferred_scan_pending() { return s_deferredScanPending; }
    std::atomic<bool> &lazy_probe_pending() { return s_lazyProbePending; }
    std::atomic<int64_t> &lazy_probe_signal() { return s_lazyProbeSignal; }

} // namespace EquipHide
