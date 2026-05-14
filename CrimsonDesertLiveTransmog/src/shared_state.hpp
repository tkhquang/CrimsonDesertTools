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
        uintptr_t mapLookup            = 0; // IndexedStringA::lookup -- used to resolve CD_* slot hashes at runtime
        uintptr_t subTranslator        = 0; // sub_14076D950 -- anchor for iteminfo item-name table scan
        uintptr_t safeTearDown         = 0; // sub_14075FE60 -- scene-graph tear-down used by real_part_tear_down
        uintptr_t indexedStringLookup  = 0; // sub_1402D75D0 -- IndexedStringA short->hash; resolved via ItemNameTable chain walk (50+ template siblings prevent direct AOB)
        uintptr_t charClassBypass      = 0; // 0x141D5F538 -- jz byte in CondPrefab evaluator secondary hash check. Toggle 0x74↔0xEB for NPC item support.
    };

    ResolvedAddresses &resolved_addrs();

    // --- Transmog slot definitions ---

    // Order is preset-format-stable: existing slots 0..4 (Helm..Boots)
    // MUST keep their indices so legacy presets load unchanged. New
    // accessory/utility slots append after Boots. Engine slot tags for
    // each entry are listed below; resolved via game_slot_from_transmog.
    enum class TransmogSlot : uint8_t
    {
        Helm,           // engine tag 0x03
        Chest,          // engine tag 0x04
        Cloak,          // engine tag 0x10
        Gloves,         // engine tag 0x05
        Boots,          // engine tag 0x06
        Earring1,       // engine tag 0x07
        Earring2,       // engine tag 0x08
        Necklace,       // engine tag 0x09
        Ring1,          // engine tag 0x0A
        Ring2,          // engine tag 0x0B
        Lantern,        // engine tag 0x0F
        Glasses,        // engine tag 0x11
        Mask,           // engine tag 0x12
        Backpack,       // engine tag 0x13
        Bracelet,       // engine tag 0x14
        // Weapons -- visible meshes on character but live in the
        // cd_phw_* prefab family, not cd_phm_*. prefab_wrapper_swap
        // module skips them (empty prefixes) so they go through the
        // carrier-only path.
        MainHand,      // engine tag 0x00 (mainhand 1H)
        OffHand,       // engine tag 0x01 (offhand / shield / 1H mirror)
        Ranged,         // engine tag 0x02 (bow/pistol)
        SubWeapon,      // engine tag 0x0C (dagger/axe family)
        TwoHandWeapon,  // engine tag 0x0D (greatsword)
        // Tag 0x0E is engine-unused for all three protagonists in any
        // observed save. Commented out 2026-05-07 -- we don't know what
        // it does and didn't want it in the UI picker. The slot-discovery
        // dump still labels it "Unknown" so it stays visible in logs;
        // re-add by uncommenting AND restoring the matching commented-
        // out rows (same checklist as OongkaRocket below).
        // Experimental,   // engine tag 0x0E
        // OongkaRocket -- engine tag 0x15 (Oongka-only "Rocket Helm").
        // Removed from the UI picker on 2026-05-07 because Kliff and
        // Damiane reject this slot (engine metadata doesn't include
        // tag 0x15 for them) and the slot adds no transmog value for
        // the typical case. Re-add later by uncommenting AND restoring
        // the matching commented-out rows in:
        //   transmog_map.cpp        (k_slotNames, slot_from_game_slot,
        //                            game_slot_from_transmog)
        //   transmog_apply.cpp      (k_kliffCarriers, k_damianeCarriers,
        //                            k_oongkaCarriers, k_gameSlotTags,
        //                            k_tearDownSlots, k_clearSlots)
        //   prefab_wrapper_swap.cpp (k_slotPrefixes, k_slotPrefixesFemale,
        //                            k_slotTags, k_slotTagPatterns)
        // OongkaRocket,   // engine tag 0x15
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

    /// Master gate for the ColorOverride subsystem (publisher hook,
    /// setter substitute, host-scope owner-vfunc midhooks, picker
    /// UI). False by default; enable via the `[Experimental]
    /// ColorOverride` INI key. When false, none of the hooks are
    /// installed and the picker UI is hidden -- the mod behaves as
    /// it did before the ColorOverride port landed.
    std::atomic<bool> &flag_color_override();

    // --- Trampoline typedefs ---

    // sub_14076C960: Populates slot visual data + calls VisualEquipChange.
    // This is the KEY function for transmog -- it loads meshes and transitions.
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

    /// Returns the currently-controlled character name ("Kliff",
    /// "Damiane", "Oongka") by reading the 1-based index byte at
    /// `*(WS+0x30)+0x30` (CE-verified 2026-04-21 on v1.03.01).
    /// Returns an empty string if the chain is not yet resolved,
    /// the byte holds an unknown value, or a memory fault is
    /// caught. Safe to call from any thread.
    std::string current_controlled_character_name() noexcept;

    /// WorldSystem base pointer (game_base + RVA). Atomic because x64
    /// qword stores are naturally atomic on aligned data but the compiler
    /// needs the explicit fence for correct ordering.
    std::atomic<uintptr_t> &world_system_ptr();

    /// Per-slot "real item damaged" flag. Set when phase B tears down the
    /// real item. Once set, the fake==real skip path falls through to
    /// SlotPopulator so the mesh gets restored. Cleared on full clear.
    std::array<bool, k_slotCount> &real_damaged();

    /// Snapshot of auth-table real itemId per slot at last apply,
    /// indexed by TransmogSlot enum (0..k_slotCount-1). Compared
    /// against live auth state to detect real-item swaps. Originally
    /// 5-armor only; widened to all supported slots on 2026-05-07 so
    /// real-item changes on accessories and weapon slots (e.g. tool
    /// vs. 2H sword sharing engine slot tag 0x0D) trigger re-apply.
    /// Slots LT does not actively manage stay zeroed.
    std::array<std::uint16_t, k_slotCount> &last_applied_real_ids();

    /// Carrier itemIds used in the last apply, indexed by TransmogSlot.
    /// 0 means no carrier was used (direct apply). Used by tear-down
    /// Phase A to find the correct scene-graph identity.
    std::array<std::uint16_t, k_slotCount> &last_applied_carrier_ids();

    /// Per-slot one-shot "force apply" flag. When true the next apply
    /// for that slot bypasses the `targetId == prevId` early-out and
    /// forces slotNeedsWork in apply_all_transmog, while leaving
    /// last_applied_ids[i] intact so Phase A `tear_down_fake` still
    /// runs against the prior carrier. Used by the body-mesh picker
    /// when re-picking a prefab on the same carrier (e.g. 0x1521 →
    /// 0x1521 with a different src→tgt wrapper map): without this the
    /// dispatcher would skip Phase A entirely and the prior body-mesh
    /// target wrapper would never get cleaned up by the engine's
    /// natural-pipeline hook. Cleared by the dispatcher after read.
    std::array<bool, k_slotCount> &force_apply_pending();

    /// When true the debounce worker runs clear instead of apply.
    /// Set by manual_clear, consumed by the worker.
    std::atomic<bool> &clear_pending();

    /// True iff the active preset's dye state has been edited via the
    /// picker since the last save / preset switch / file load. Drives
    /// the Save button's "Save *" pending indicator. Set by the dye
    /// picker; cleared by `PresetManager::save()` and on preset
    /// switch / load. Dye edits write directly to the active preset
    /// (no staging area like slot_mappings has for items), so this
    /// flag is the only signal that something needs persisting.
    std::atomic<bool> &dye_dirty();

    /// Slot index for single-slot hover-apply. k_slotCount means
    /// "apply all" (the default). Values 0..4 scope the next
    /// debounced apply to one slot only, avoiding full-gear flicker.
    /// Last-writer-wins: if manual_apply (full) and manual_apply_slot
    /// race before the worker wakes, only the latest store takes
    /// effect. This is acceptable -- the user's most recent action wins.
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
