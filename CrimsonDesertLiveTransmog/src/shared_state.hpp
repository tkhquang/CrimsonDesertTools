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
        uintptr_t slotPopulator = 0;
        uintptr_t mapLookup = 0;           // IndexedStringA::lookup -- used to resolve CD_* slot hashes at runtime
        uintptr_t subTranslator = 0;       // sub_14076D950 -- anchor for iteminfo item-name table scan
        uintptr_t safeTearDown = 0;        // sub_14075FE60 -- scene-graph tear-down used by real_part_tear_down
        uintptr_t indexedStringLookup = 0; // sub_1402D75D0 -- IndexedStringA short->hash; resolved via ItemNameTable
                                           // chain walk (50+ template siblings prevent direct AOB)
        uintptr_t charClassBypass = 0; // jz byte in the CondPrefab evaluator's char-class hash check (AOB-resolved via
                                       // k_charClassBypassCandidates -- moved to a new function on v1.13.00). Toggle
                                       // 0x74<->0xEB for NPC/variant item support.
    };

    ResolvedAddresses &resolved_addrs();

    // --- Transmog slot definitions ---

    // Order is preset-format-stable: existing slots 0..4 (Helm..Boots) MUST keep their indices so legacy presets load
    // unchanged. New accessory/utility slots append after Boots. Engine slot tags for each entry are listed below;
    // resolved via game_slot_from_transmog.
    enum class TransmogSlot : uint8_t
    {
        Helm,     // engine tag 0x03
        Chest,    // engine tag 0x04
        Cloak,    // engine tag 0x10
        Gloves,   // engine tag 0x05
        Boots,    // engine tag 0x06
        Earring1, // engine tag 0x07
        Earring2, // engine tag 0x08
        Necklace, // engine tag 0x09
        Ring1,    // engine tag 0x0A
        Ring2,    // engine tag 0x0B
        Lantern,  // engine tag 0x0F
        Glasses,  // engine tag 0x11
        Mask,     // engine tag 0x12
        Backpack, // engine tag 0x13
        Bracelet, // engine tag 0x14
        // Weapons -- visible meshes on character but live in the cd_phw_* prefab family, not cd_phm_*.
        // prefab_wrapper_swap module skips them (empty prefixes) so they go through the carrier-only path.
        MainHand,      // engine tag 0x00 (mainhand 1H)
        OffHand,       // engine tag 0x01 (offhand / shield / 1H mirror)
        Ranged,        // engine tag 0x02 (bow/pistol)
        SubWeapon,     // engine tag 0x0C (dagger/axe family)
        TwoHandWeapon, // engine tag 0x0D (greatsword)
        // Tag 0x0E is engine-unused for all three protagonists in any observed save. Kept out of the UI picker because
        // its purpose is unknown. The slot-discovery dump still labels it "Unknown" so it stays visible in logs. To
        // experiment with it, add an `Experimental, // engine tag 0x0E` row here AND wire it through the per-slot
        // tables below (same checklist as OongkaRocket).
        //
        // Tag 0x15 is the Oongka-only "Rocket Helm". Excluded because
        // Kliff and Damiane reject this slot (engine metadata does not include tag 0x15 for them) and the slot adds no
        // transmog value for the typical case. To re-enable, add an `OongkaRocket, // engine tag 0x15` row here AND
        // extend every per-slot table that indexes by TransmogSlot:
        //   slot_metadata.hpp       (k_slotMetadata -- master row:
        //                            gameTag, displayName, prefab
        //                            prefixes, partShowHashKey, enabled)
        //   carrier_defaults.hpp    (k_carriers -- one carrier item +
        //                            prefab per character per slot)
        //   prefab_wrapper_swap.cpp (k_slotTagPatterns -- registry
        //                            classifier patterns)
        Count
    };

    inline constexpr std::size_t k_slotCount = static_cast<std::size_t>(TransmogSlot::Count);

    struct SlotMapping
    {
        bool active = false;
        uint16_t targetItemId = 0;
    };

    std::array<SlotMapping, k_slotCount> &slot_mappings();

    /**
     * Item IDs that were last written to slot_mappings (saved before preset switch). Used by clear_all_transmog to know
     * what to unequip.
     */
    std::array<uint16_t, k_slotCount> &last_applied_ids();

    /**
     * Per-character tracking of the last-applied transmog snapshot. The four globals (last_applied_ids / real_damaged /
     * last_-applied_real_ids / last_applied_carrier_ids) describe ONE body's installed state at a time. With
     * multi-character auto-apply and the "Apply To Selected" feature, the worker can apply to any of the three
     * protagonists between hook events, so a single global snapshot would conflate state across bodies (Phase A
     * teardown of Damiane's fakes would try to use Kliff's `lastIds` as the truth source).
     *
     * Each char's snapshot is buffered behind the scenes. Before each apply the worker hydrates the globals from the
     * target character's buffered snapshot; after the apply finishes the new globals are captured back. Pass idx
     * 1=Kliff, 2=Damiane, 3=Oongka. Out-of-range idx is a no-op.
     */
    void rehydrate_applied_state_for_char(std::uint32_t idx) noexcept;
    void capture_applied_state_for_char(std::uint32_t idx) noexcept;

    /**
     * Wipe ONE character's buffered snapshot AND the live globals, marking the character as having no installed fakes.
     * Called by the multi-character auto-apply path when a protagonist's body (CCOIA) has been reallocated -- a
     * despawn/respawn (off-screen stream-out + return, follower injury cooldown + recall) hands back a fresh body
     * wearing vanilla gear, so the prior snapshot lists fakes that no longer exist. Without this, apply_all_transmog
     * would observe preset==last-applied and real==last-real, fire its "no state change, skipping" early-out, and leave
     * the respawned body un-transmogged. Pass idx 1=Kliff, 2=Damiane, 3=Oongka. Out-of-range idx is a no-op.
     */
    void reset_applied_state_for_char(std::uint32_t idx) noexcept;

    /**
     * Wipe both the globals and every per-character buffered snapshot. Called from the save-load wipe path: all bodies
     * have been reallocated by the engine, every fake transmog item from the prior session is gone, so the trackers
     * must reset to match.
     */
    void reset_all_applied_state() noexcept;

    // --- Feature flags ---

    std::atomic<bool> &flag_player_only();
    std::atomic<bool> &flag_enabled();
    std::atomic<bool> &shutdown_requested();

    /**
     * Master gate for the ColorOverride subsystem (publisher hook, setter substitute, host-scope owner-vfunc midhooks,
     * picker UI). False by default; enable via the `[Experimental] ColorOverride` INI key. When false, none of the
     * hooks are installed and the picker UI is hidden -- the mod behaves as it did before the ColorOverride port
     * landed.
     */
    std::atomic<bool> &flag_color_override();

    /**
     * Master gate for the helm voice-unmuffle filter. False by default; enable via the `[Experimental]
     * UnmuffleHelmVoice` INI key to remove the engine's stock plate/heavy-helm voice muffle. When false, the
     * passive-skill registrar inline hook is not installed and the muffle behaves as it does in vanilla. Toggle is read
     * once at startup; takes effect on the next game launch.
     */
    std::atomic<bool> &flag_helm_audio_unmuffle();

    /**
     * One-shot diagnostic dumps gated by `[Diagnostics]` INI section. Off by default. When true the corresponding TSV
     * is written next to the plugin once ItemNameTable::build() returns Ok.
     */
    std::atomic<bool> &flag_dump_item_prefabs();
    std::atomic<bool> &flag_dump_item_catalog();

    /**
     * When the user has the dropdown pinned to a non-controlled character and this flag is set, overlay-UI edits
     * (dropdown switch, picker change, slot toggle, preset cycle, manual buttons) apply to the editing character's body
     * instead of cross-applying onto the controlled body. Engine-triggered equip events still target the controlled
     * body so the controlled character's transmog stays visible across their own gear changes. When false, every apply
     * targets the controlled body (legacy cross-body behaviour).
     */
    std::atomic<bool> &flag_apply_to_editing();

    // --- Trampoline typedefs ---

    // sub_14076C960: Populates slot visual data + calls VisualEquipChange. This is the KEY function for transmog -- it
    // loads meshes and transitions.
    using SlotPopulatorFn = __int64(__fastcall *)(__int64 a1, unsigned __int16 *a2_itemData, __int64 a3_swapEntry);
    SlotPopulatorFn &slot_populator_fn();

    // sub_141D451B0: Initializes a swap entry to defaults (all -1/zeros).
    using InitSwapEntryFn = __int64(__fastcall *)(__int64 dest);
    InitSwapEntryFn &init_swap_entry_fn();

    // --- Cross-TU shared state ---
    // Atomics and arrays accessed by multiple translation units (hooks, workers, apply logic). Accessor pattern mirrors
    // the flag/fn-ptr accessors above to keep linkage internal to shared_state.cpp.

    /// Recursion guard: prevents infinite loop when SlotPopulator calls VEC.
    std::atomic<bool> &in_transmog();

    /// Suppress VEC hook scheduling during clear+apply cycle.
    std::atomic<bool> &suppress_vec();

    /// Last known player a1 (captured from BatchEquip hook / VEC hook).
    std::atomic<__int64> &player_a1();

    /**
     * Returns the currently-controlled character name ("Kliff", "Damiane", "Oongka"). Delegates to the shared Core
     * resolver which walks the static actor chain and classifies the CCOIA by appearance-config asset path. Returns an
     * empty string if the chain is not yet resolved or the classifier failed. Safe to call from any thread.
     */
    std::string current_controlled_character_name() noexcept;

    /**
     * WorldSystem base pointer (game_base + RVA). Atomic because x64 qword stores are naturally atomic on aligned data
     * but the compiler needs the explicit fence for correct ordering.
     */
    std::atomic<uintptr_t> &world_system_ptr();

    /**
     * Per-slot "real item damaged" flag. Set when phase B tears down the real item. Once set, the fake==real skip path
     * falls through to SlotPopulator so the mesh gets restored. Cleared on full clear.
     */
    std::array<bool, k_slotCount> &real_damaged();

    /**
     * Snapshot of auth-table real itemId per slot at last apply, indexed by TransmogSlot enum (0..k_slotCount-1).
     * Compared against live auth state to detect real-item swaps. Covers every supported slot so real-item changes on
     * accessories and weapon slots (e.g. tool vs. 2H sword sharing engine slot tag 0x0D) trigger re-apply. Slots LT
     * does not actively manage stay zeroed.
     */
    std::array<std::uint16_t, k_slotCount> &last_applied_real_ids();

    /**
     * Carrier itemIds used in the last apply, indexed by TransmogSlot. 0 means no carrier was used (direct apply). Used
     * by tear-down
     * Phase A to find the correct scene-graph identity.
     */
    std::array<std::uint16_t, k_slotCount> &last_applied_carrier_ids();

    /**
     * Per-slot one-shot "force apply" flag. When true the next apply for that slot bypasses the `targetId == prevId`
     * early-out and forces slotNeedsWork in apply_all_transmog, while leaving last_applied_ids[i] intact so Phase A
     * `tear_down_fake` still runs against the prior carrier. Used by the body-mesh picker when re-picking a prefab on
     * the same carrier (e.g. 0x1521 -> 0x1521 with a different src->tgt wrapper map): without this the dispatcher would
     * skip Phase A entirely and the prior body-mesh target wrapper would never get cleaned up by the engine's
     * natural-pipeline hook. Cleared by the dispatcher after read.
     */
    std::array<bool, k_slotCount> &force_apply_pending();

    /**
     * When true the debounce worker runs clear instead of apply. Set by manual_clear, consumed by the worker.
     */
    std::atomic<bool> &clear_pending();

    /**
     * True iff the active preset's dye state has been edited via the picker since the last save / preset switch / file
     * load. Drives the Save button's "Save *" pending indicator. Set by the dye picker; cleared by
     * `PresetManager::save()` and on preset switch / load. Dye edits write directly to the active preset (no staging
     * area like slot_mappings has for items), so this flag is the only signal that something needs persisting.
     */
    std::atomic<bool> &dye_dirty();

    /**
     * Slot index for single-slot hover-apply. k_slotCount means "apply all" (the default). Values 0..4 scope the next
     * debounced apply to one slot only, avoiding full-gear flicker.
     * Last-writer-wins: if manual_apply (full) and manual_apply_slot race before the worker wakes, only the latest
     * store takes effect. This is acceptable -- the user's most recent action wins.
     */
    std::atomic<std::size_t> &pending_slot_index();

    // --- Hot-path utilities ---

    inline int64_t steady_ms() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    /**
     * Convert the wide runtime directory to a UTF-8 string with a trailing path separator. Returns empty on failure.
     * Used by init and deferred-scan paths to locate sidecar data files.
     */
    std::string runtime_dir_utf8();

} // namespace Transmog
