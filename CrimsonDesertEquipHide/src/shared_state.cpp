#include "shared_state.hpp"

namespace EquipHide
{
    namespace
    {
        ResolvedAddresses s_resolved_addrs{};

        // Every member carries a constexpr initializer, so this table is constant-initialized and needs no run-time
        // seeding pass. visCharIdx already reads -1 (unidentified) in a pre-resolve consumer: apply_direct_vis_write
        // runs before resolve_player_vis_ctrls fills the slots and falls back to the active character's hide mask.
        constinit PlayerState s_player_state{};

        std::mutex s_vis_write_mutex;
        std::unordered_map<VisKey, std::uint8_t, VisKeyHash> s_original_vis;
        std::atomic<bool> s_needs_direct_write{false};

        std::atomic<bool> s_bald_fix{true};
        std::atomic<bool> s_gliding_fix{true};
        std::atomic<bool> s_fallback_mode{false};
        std::atomic<bool> s_independent_toggle{false};
        std::atomic<bool> s_cascade_fix{false};

        std::atomic<bool> s_shutdown_requested{false};
        std::atomic<bool> s_deferred_scan_pending{false};
        std::atomic<bool> s_lazy_probe_pending{false};
        std::atomic<std::int64_t> s_lazy_probe_signal{0};
    } // namespace

    ResolvedAddresses &resolved_addrs()
    {
        return s_resolved_addrs;
    }
    PlayerState &player_state()
    {
        return s_player_state;
    }
    std::mutex &vis_write_mutex()
    {
        return s_vis_write_mutex;
    }
    std::unordered_map<VisKey, std::uint8_t, VisKeyHash> &original_vis_map()
    {
        return s_original_vis;
    }
    std::atomic<bool> &needs_direct_write()
    {
        return s_needs_direct_write;
    }

    std::atomic<bool> &flag_bald_fix()
    {
        return s_bald_fix;
    }
    std::atomic<bool> &flag_gliding_fix()
    {
        return s_gliding_fix;
    }
    std::atomic<bool> &flag_fallback_mode()
    {
        return s_fallback_mode;
    }
    std::atomic<bool> &flag_independent_toggle()
    {
        return s_independent_toggle;
    }
    std::atomic<bool> &flag_cascade_fix()
    {
        return s_cascade_fix;
    }

    std::atomic<bool> &shutdown_requested()
    {
        return s_shutdown_requested;
    }
    std::atomic<bool> &deferred_scan_pending()
    {
        return s_deferred_scan_pending;
    }
    std::atomic<bool> &lazy_probe_pending()
    {
        return s_lazy_probe_pending;
    }
    std::atomic<std::int64_t> &lazy_probe_signal()
    {
        return s_lazy_probe_signal;
    }

} // namespace EquipHide
