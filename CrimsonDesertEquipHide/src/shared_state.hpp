#ifndef EQUIPHIDE_SHARED_STATE_HPP
#define EQUIPHIDE_SHARED_STATE_HPP

#include "categories.hpp"

#include <DetourModKit/memory.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace EquipHide
{
    /// Upper bound on the protagonists the mod tracks at once.
    inline constexpr int MAX_PROTAGONISTS = 8;

    /** @brief Addresses resolved once during init via AOB scanning. */
    struct ResolvedAddresses
    {
        std::uintptr_t world_system = 0;
        std::uintptr_t child_actor_vtbl = 0;
        std::uintptr_t map_lookup = 0;
        std::uintptr_t map_insert = 0;
        std::uintptr_t indexed_string_global = 0;
        /**
         * @brief Return address inside the prefab-instantiation routine, on the instruction after its inner rule-eval
         *        call, through which PostfixEval runs for a freshly-built prefab.
         *
         * Every PostfixEval invocation that comes from prefab instantiation (an NPC creation event at load) carries
         * this return address on its stack. No player-side invocation does. BaldFix reads that stack presence as a
         * deterministic call-graph filter, with no ctx cache and no frequency heuristic. The anchor that resolves it
         * is NpcPfeReturnAddr in aob_resolver.hpp, which owns the derivation rules.
         */
        std::uintptr_t npc_pfe_return_addr = 0;
    };

    /**
     * @brief The address store init fills from the anchor registry.
     * @note Not a Result: init writes each field once before any consumer runs, and every reader treats 0 as
     *       unresolved.
     */
    ResolvedAddresses &resolved_addrs();

    /**
     * @brief Per-protagonist vis_ctrl pointers and injection state.
     * @details vis_char_idx is parallel to vis_ctrls. vis_char_idx[i] holds the 0-based protagonist index (0=Kliff,
     *          1=Damiane, 2=Oongka) that vis_ctrls[i] belongs to, or -1 when the resolver cannot identify the
     *          character (an NPC follower, an unknown protagonist, or a torn read after a swap). The per-character
     *          hide write falls back to the active-character hide mask for an entry with idx == -1, which keeps the
     *          old single-character behavior for an unidentified slot. The fields are int32 atomics because
     *          std::atomic<signed char> is not guaranteed lock-free on x64.
     */
    struct PlayerState
    {
        std::atomic<std::uintptr_t> vis_ctrls[MAX_PROTAGONISTS]{};

        // Seeded to -1 here, not by a run-time pass. std::atomic<int>(-1) is constexpr, so the whole table is
        // constant-initialized and every slot already reads as unidentified before the first dynamic initializer in
        // the module runs. A zero fill would instead read as protagonist index 0, which is a real character.
        std::atomic<int> vis_char_idx[MAX_PROTAGONISTS]{-1, -1, -1, -1, -1, -1, -1, -1};
        static_assert(MAX_PROTAGONISTS == 8, "vis_char_idx lists one -1 per slot. Extend the list with the bound.");

        std::atomic<int> count{0};
        std::atomic<std::uintptr_t> primary_vis_ctrl{0};
        std::atomic<bool> armor_injected[MAX_PROTAGONISTS]{};
    };

    /**
     * @brief The per-protagonist vis-ctrl table.
     * @note Not a Result: the table is always available and an unpopulated slot reads as 0.
     */
    PlayerState &player_state();

    /** @brief Mutex guarding direct vis-byte writes and original-value map. */
    std::mutex &vis_write_mutex();

    /**
     * @brief Composite key for the per-(vis_ctrl, address) original-value map.
     * @details A key built purely on the vis-byte address makes character-swap invalidation lossy. Two protagonists
     *          with the same part name resolve to two distinct vis-byte addresses, so an orphan restore of every
     *          entry whose hash is not in the active map also reverts the previously-active character's vis bytes
     *          back to visible, because that character's vis ctrl no longer ticks through apply_direct_vis_write.
     *          The vis_ctrl half of the key scopes each restore decision to the matching vis ctrl and leaves the
     *          inactive character's hide state in place.
     */
    struct VisKey
    {
        std::uintptr_t vis_ctrl{};
        std::uintptr_t addr{};

        bool operator==(const VisKey &other) const noexcept { return vis_ctrl == other.vis_ctrl && addr == other.addr; }
    };

    /** @brief Hash for VisKey - xor mix of the two pointer fields. */
    struct VisKeyHash
    {
        std::size_t operator()(const VisKey &k) const noexcept
        {
            // The two fields are unrelated heap addresses on x64. An xor mix is sufficient because the std lib hashes
            // each field through a strong integer hash before the composition is even observable. Rotate one half so
            // two pointers that happen to alias do not collapse to zero.
            const auto a = std::hash<std::uintptr_t>{}(k.vis_ctrl);
            const auto b = std::hash<std::uintptr_t>{}(k.addr);
            return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
        }
    };

    /** @brief Map of (vis_ctrl, vis-byte address) pairs to their original values. */
    std::unordered_map<VisKey, std::uint8_t, VisKeyHash> &original_vis_map();

    /** @brief Flag the vis-byte writers set when a direct write needs a flush. */
    std::atomic<bool> &needs_direct_write();

    /// Feature flag: the bald fix rejects the prefab-instantiation call path.
    std::atomic<bool> &flag_bald_fix();
    /// Feature flag: the gliding fix keeps a hidden part hidden across a glide transition.
    std::atomic<bool> &flag_gliding_fix();
    /// Feature flag: the mod writes vis bytes directly rather than steer the engine decision.
    std::atomic<bool> &flag_fallback_mode();
    /// Feature flag: each category toggles on its own key instead of through the shared toggle.
    std::atomic<bool> &flag_independent_toggle();
    /// Feature flag: the cascade fix suppresses the chest-driven visual equip cascade.
    std::atomic<bool> &flag_cascade_fix();

    /// Set once at shutdown so every background worker leaves its loop.
    std::atomic<bool> &shutdown_requested();
    /// Set when a deferred category scan waits for the background worker.
    std::atomic<bool> &deferred_scan_pending();
    /// Set when a lazy vis-ctrl probe waits for the background worker.
    std::atomic<bool> &lazy_probe_pending();
    /// Millisecond deadline the lazy-probe worker waits for before it runs.
    std::atomic<std::int64_t> &lazy_probe_signal();

    /**
     * @brief The steady clock in milliseconds, for a hot-path deadline compare.
     * @note Not a Result: the steady clock cannot fail.
     */
    [[nodiscard]] inline std::int64_t steady_ms() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()
        )
            .count();
    }

    /**
     * @brief Unsafe pointer read - use ONLY inside SEH-protected hot paths.
     * @details Forwards to DetourModKit::memory::unchecked::read, the validation-free fast path. A Debug build
     *          asserts the source is readable. A Release build performs a bare copy with no OS-level fault
     *          protection, so the caller must keep this inside an SEH frame for a stale pointer, and must screen the
     *          loaded value itself.
     */
    [[nodiscard]] inline std::uintptr_t read_ptr_unsafe(std::uintptr_t base, std::ptrdiff_t off) noexcept
    {
        return DMK::memory::unchecked::read<std::uintptr_t>(
            DMK::Address{base}.offset(static_cast<std::ptrdiff_t>(off))
        );
    }

    /**
     * @brief Returns true when any category hides the part at @p part_hash_ptr.
     * @param part_hash_ptr Address of the part-hash DWORD. A structurally implausible address reads as not hidden.
     * @warning The read is unchecked, so the caller must keep this inside an SEH frame for a stale pointer.
     */
    [[nodiscard]] inline bool check_part_hidden(std::uint64_t part_hash_ptr)
    {
        const DMK::Address hash_addr{static_cast<std::uintptr_t>(part_hash_ptr)};
        if (!DMK::memory::is_plausible_ptr(hash_addr))
            return false;
        const auto part_hash = DMK::memory::unchecked::read<std::uint32_t>(hash_addr);
        if (!needs_classification(part_hash))
            return false;
        const auto mask = classify_part(part_hash);
        return mask != 0 && is_any_category_hidden(mask);
    }

    /** @brief Game's map lookup function signature. */
    using MapLookupFn = std::uintptr_t(__fastcall *)(std::uintptr_t map_base, const std::uint32_t *key);

} // namespace EquipHide

#endif // EQUIPHIDE_SHARED_STATE_HPP
