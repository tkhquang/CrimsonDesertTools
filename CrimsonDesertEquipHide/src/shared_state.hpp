#pragma once

#include "categories.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace EquipHide
{
    inline constexpr int k_maxProtagonists = 8;

    /** @brief Addresses resolved once during init via AOB scanning. */
    struct ResolvedAddresses
    {
        uintptr_t worldSystem = 0;
        uintptr_t childActorVtbl = 0;
        uintptr_t mapLookup = 0;
        uintptr_t mapInsert = 0;
        uintptr_t indexedStringGlobal = 0;
    };

    ResolvedAddresses &resolved_addrs();

    /** @brief Per-protagonist vis_ctrl pointers and injection state. */
    struct PlayerState
    {
        std::atomic<uintptr_t> visCtrls[k_maxProtagonists]{};
        std::atomic<int> count{0};
        std::atomic<uintptr_t> primaryVisCtrl{0};
        std::atomic<bool> armorInjected[k_maxProtagonists]{};
    };

    PlayerState &player_state();

    /** @brief Mutex guarding direct vis-byte writes and original-value map. */
    std::mutex &vis_write_mutex();

    /** @brief Map of vis-byte addresses to their original values before modification. */
    std::unordered_map<uintptr_t, uint8_t> &original_vis_map();

    /** @brief Flag set when vis-byte writes need to be flushed. */
    std::atomic<bool> &needs_direct_write();

    // --- Feature flags ---
    std::atomic<bool> &flag_player_only();
    std::atomic<bool> &flag_force_show();
    std::atomic<bool> &flag_bald_fix();
    std::atomic<bool> &flag_gliding_fix();
    std::atomic<bool> &flag_fallback_mode();
    std::atomic<bool> &flag_independent_toggle();
    std::atomic<bool> &flag_cascade_fix();

    // --- Background thread control ---
    std::atomic<bool> &shutdown_requested();
    std::atomic<bool> &deferred_scan_pending();
    std::atomic<bool> &lazy_probe_pending();
    std::atomic<int64_t> &lazy_probe_signal();

    // --- Hot-path utilities ---

    inline int64_t steady_ms() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    /** @brief Unsafe pointer read — use ONLY inside SEH-protected hot paths. */
    inline uintptr_t read_ptr_unsafe(uintptr_t base, ptrdiff_t off) noexcept
    {
        auto addr = *reinterpret_cast<const uintptr_t *>(base + off);
        return (addr > 0x10000) ? addr : 0;
    }

    /** @brief Returns true if the part at the given hash pointer is hidden by any category. */
    inline bool check_part_hidden(uint64_t partHashPtr)
    {
        if (partHashPtr < 0x10000)
            return false;
        auto partHash = *reinterpret_cast<const uint32_t *>(partHashPtr);
        if (!needs_classification(partHash))
            return false;
        auto mask = classify_part(partHash);
        return mask != 0 && is_any_category_hidden(mask);
    }

    /** @brief Game's map lookup function signature. */
    using MapLookupFn = uintptr_t(__fastcall *)(uintptr_t map_base, const uint32_t *key);

} // namespace EquipHide
