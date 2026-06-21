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
        /**
         * @brief Return-address landmark inside createPrefabFromPartPrefab (sub_14261B6C0), on the instruction right
         *        after its inner `call sub_1402E1430` (through which PostfixEval runs for newly-instantiated prefabs).
         *
         * Every PostfixEval invocation that comes from prefab instantiation (NPC creation events at load) carries this
         * return address on its stack; no player-side invocation does. BaldFix uses this stack-presence check as a
         * deterministic call-graph filter -- no ctx caching, no frequency heuristics. Resolved via an AOB anchor at mod
         * init.
         */
        uintptr_t npcPfeReturnAddr = 0;
    };

    ResolvedAddresses &resolved_addrs();

    /**
     * @brief Per-protagonist vis_ctrl pointers and injection state.
     * @details visCharIdx is parallel to visCtrls: visCharIdx[i] holds the 0-based protagonist index (0=Kliff,
     *          1=Damiane, 2=Oongka) that visCtrls[i] belongs to, or -1 when the identity could not be resolved (NPC
     *          follower, unknown protagonist, or transient torn read just after a swap). Entries with idx == -1 fall
     *          back to the active-character hide-mask in the per-character hide write so old single-character behaviour
     *          is preserved for unidentified slots. Stored as int32 atomics because std::atomic<signed char> is not
     *          guaranteed lock-free on x64.
     */
    struct PlayerState
    {
        std::atomic<uintptr_t> visCtrls[k_maxProtagonists]{};
        std::atomic<int> visCharIdx[k_maxProtagonists]{};
        std::atomic<int> count{0};
        std::atomic<uintptr_t> primaryVisCtrl{0};
        std::atomic<bool> armorInjected[k_maxProtagonists]{};
    };

    PlayerState &player_state();

    /** @brief Mutex guarding direct vis-byte writes and original-value map. */
    std::mutex &vis_write_mutex();

    /**
     * @brief Composite key for the per-(visCtrl, address) original-value map.
     * @details Keying purely on the vis-byte address makes character-swap invalidation lossy: two protagonists sharing
     *          the same part name resolve to two distinct vis-byte addresses, but orphan-restoring "every entry whose
     *          hash is not in the active map" would also revert the previously-active character's vis bytes back to
     *          visible because that character's vis ctrl no longer ticks through apply_direct_vis_write. Including
     *          visCtrl in the key scopes restore decisions to the matching vis ctrl and leaves the inactive character's
     *          hide state in place.
     */
    struct VisKey
    {
        uintptr_t visCtrl;
        uintptr_t addr;

        bool operator==(const VisKey &other) const noexcept { return visCtrl == other.visCtrl && addr == other.addr; }
    };

    /** @brief Hash for VisKey -- xor mix of the two pointer fields. */
    struct VisKeyHash
    {
        std::size_t operator()(const VisKey &k) const noexcept
        {
            // The two fields are unrelated heap addresses on x64; xor mix is sufficient because the std lib hashes each
            // field through a strong integer hash before composition is even observable. Rotate one half so two
            // pointers that happen to alias do not collapse to zero.
            const auto a = std::hash<uintptr_t>{}(k.visCtrl);
            const auto b = std::hash<uintptr_t>{}(k.addr);
            return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
        }
    };

    /** @brief Map of (vis_ctrl, vis-byte address) pairs to their original values. */
    std::unordered_map<VisKey, uint8_t, VisKeyHash> &original_vis_map();

    /** @brief Flag set when vis-byte writes need to be flushed. */
    std::atomic<bool> &needs_direct_write();

    // --- Feature flags ---
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

    /**
     * @brief Unsafe pointer read -- use ONLY inside SEH-protected hot paths.
     * @details Forwards to DMKMemory::read_ptr_unchecked, which applies the same low-address result guard plus a
     *          source-address guard and an aliasing-safe memcpy read. No OS-level fault protection: the caller must
     *          keep this inside an SEH frame for stale pointers.
     */
    inline uintptr_t read_ptr_unsafe(uintptr_t base, ptrdiff_t off) noexcept
    {
        return DMKMemory::read_ptr_unchecked(base, off);
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
