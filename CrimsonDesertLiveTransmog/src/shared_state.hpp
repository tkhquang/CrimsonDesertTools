#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace Transmog
{
    // --- Resolved AOB addresses ---

    struct ResolvedAddresses
    {
        uintptr_t slotPopulator       = 0;
        uintptr_t mapLookup            = 0; // IndexedStringA::lookup — used to resolve CD_* slot hashes at runtime
        uintptr_t subTranslator        = 0; // sub_14076D950 — anchor for iteminfo item-name table scan
        uintptr_t safeTearDown         = 0; // sub_14075FE60 — scene-graph tear-down used by real_part_tear_down
        uintptr_t indexedStringLookup  = 0; // sub_1402D75D0 — IndexedStringA short->hash; resolved via ItemNameTable chain walk (50+ template siblings prevent direct AOB)
        uintptr_t charClassBypass      = 0; // 0x141D5F538 — jz byte in CondPrefab evaluator secondary hash check. Toggle 0x74↔0xEB for NPC item support.
    };

    ResolvedAddresses &resolved_addrs();

    // --- Transmog slot definitions ---

    enum class TransmogSlot : uint8_t
    {
        Helm,
        Chest,
        Cloak,
        Gloves,
        Boots,
        Count
    };

    inline constexpr std::size_t k_slotCount = static_cast<std::size_t>(TransmogSlot::Count);

    struct SlotMapping
    {
        bool active = false;
        uint16_t targetItemId = 0;
    };

    std::array<SlotMapping, k_slotCount> &slot_mappings();

    /// Item IDs that were last written to slot_mappings (saved before preset switch).
    /// Used by clear_all_transmog to know what to unequip.
    std::array<uint16_t, k_slotCount> &last_applied_ids();

    // --- Feature flags ---

    std::atomic<bool> &flag_player_only();
    std::atomic<bool> &flag_enabled();
    std::atomic<bool> &shutdown_requested();

    // --- Trampoline typedefs ---

    // sub_14076C960: Populates slot visual data + calls VisualEquipChange.
    // This is the KEY function for transmog — it loads meshes and transitions.
    using SlotPopulatorFn = __int64(__fastcall *)(__int64 a1, unsigned __int16 *a2_itemData, __int64 a3_swapEntry);
    SlotPopulatorFn &slot_populator_fn();

    // sub_141D451B0: Initializes a swap entry to defaults (all -1/zeros).
    using InitSwapEntryFn = __int64(__fastcall *)(__int64 dest);
    InitSwapEntryFn &init_swap_entry_fn();

    // --- Cross-TU shared state ---
    // Atomics and arrays accessed by multiple translation units (hooks,
    // workers, apply logic). Accessor pattern mirrors the flag/fn-ptr
    // accessors above to keep linkage internal to shared_state.cpp.

    /// Recursion guard: prevents infinite loop when SlotPopulator calls VEC.
    std::atomic<bool> &in_transmog();

    /// Suppress VEC hook scheduling during clear+apply cycle.
    std::atomic<bool> &suppress_vec();

    /// Last known player a1 (captured from BatchEquip hook / VEC hook).
    std::atomic<__int64> &player_a1();

    /// WorldSystem base pointer (game_base + RVA). Atomic because x64
    /// qword stores are naturally atomic on aligned data but the compiler
    /// needs the explicit fence for correct ordering.
    std::atomic<uintptr_t> &world_system_ptr();

    /// Per-slot "real item damaged" flag. Set when phase B tears down the
    /// real item. Once set, the fake==real skip path falls through to
    /// SlotPopulator so the mesh gets restored. Cleared on full clear.
    std::array<bool, k_slotCount> &real_damaged();

    /// Snapshot of auth-table real itemId per armor slot at last apply.
    /// Compared against live auth state to detect real-armor swaps.
    std::array<std::uint16_t, 5> &last_applied_real_ids();

    /// Carrier itemIds used in the last apply, indexed by TransmogSlot.
    /// 0 means no carrier was used (direct apply). Used by tear-down
    /// Phase A to find the correct scene-graph identity.
    std::array<std::uint16_t, k_slotCount> &last_applied_carrier_ids();

    /// When true the debounce worker runs clear instead of apply.
    /// Set by manual_clear, consumed by the worker.
    std::atomic<bool> &clear_pending();

    /// Slot index for single-slot hover-apply. k_slotCount means
    /// "apply all" (the default). Values 0..4 scope the next
    /// debounced apply to one slot only, avoiding full-gear flicker.
    /// Last-writer-wins: if manual_apply (full) and manual_apply_slot
    /// race before the worker wakes, only the latest store takes
    /// effect. This is acceptable — the user's most recent action wins.
    std::atomic<std::size_t> &pending_slot_index();

    // --- Hot-path utilities ---

    inline int64_t steady_ms() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    /// Convert the wide runtime directory to a UTF-8 string with a
    /// trailing path separator. Returns empty on failure. Used by
    /// init and deferred-scan paths to locate sidecar data files.
    std::string runtime_dir_utf8();

} // namespace Transmog
