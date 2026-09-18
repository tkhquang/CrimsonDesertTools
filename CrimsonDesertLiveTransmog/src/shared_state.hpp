#ifndef TRANSMOG_SHARED_STATE_HPP
#define TRANSMOG_SHARED_STATE_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Transmog
{
    // Resolved AOB addresses

    struct ResolvedAddresses
    {
        uintptr_t slot_populator = 0;
        uintptr_t map_lookup = 0;            // IndexedStringA::lookup - resolves CD_* slot hashes at runtime
        uintptr_t sub_translator = 0;        // Anchor for the iteminfo item-name table scan
        uintptr_t safe_tear_down = 0;        // Scene-graph tear-down real_part_tear_down drives
        uintptr_t indexed_string_lookup = 0; // IndexedStringA short to hash. ItemNameTable resolves it through a chain
                                             // walk, because 50+ template siblings prevent a direct AOB
    };

    ResolvedAddresses &resolved_addrs();

    // Transmog slot definitions

    // Order is preset-format-stable: existing slots 0..4 (Helm..Boots) MUST keep their indices so legacy presets load
    // unchanged. New accessory/utility slots append after Boots. The engine slot tag for each entry is listed below.
    // game_slot_from_transmog resolves it.
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
        // Weapons - visible meshes on character but live in the cd_phw_* prefab family, not cd_phm_*.
        // prefab_wrapper_swap module skips them (empty prefixes) so they go through the carrier-only path.
        MainHand,      // engine tag 0x00 (mainhand 1H)
        OffHand,       // engine tag 0x01 (offhand / shield / 1H mirror)
        Ranged,        // engine tag 0x02 (bow/pistol)
        SubWeapon,     // engine tag 0x0C (dagger/axe family)
        TwoHandWeapon, // engine tag 0x0D (greatsword)
        // Gathering-tool slot. Holds the felling axe / shovel / pickaxe family, which the engine equips into its own
        // tag rather than into TwoHandWeapon even though the meshes are two-handed. Disabled in the metadata table:
        // the items are multi-prefab like every other weapon-family slot.
        Tool, // engine tag 0x0E
        // Overflow slots, not a second loadout the player selects. The engine parks a weapon here when its natural
        // slot is already taken: a shield lands in OffHand2 only while a dual-wielded one-hander occupies OffHand, and
        // equipping that shield properly moves it into OffHand and drops the OffHand2 entry entirely. The ranged pair
        // behaves the same way, with a bow pushed into Ranged2 while a sprayer holds Ranged. Both disabled for the
        // usual weapon-family reason.
        OffHand2, // engine tag 0x17
        Ranged2,  // engine tag 0x18
        //
        // Tag 0x15 is the Oongka-only "Rocket Helm". Excluded because
        // Kliff and Damiane reject this slot (engine metadata does not include tag 0x15 for them) and the slot adds no
        // transmog value for the typical case. To re-enable, add an `OongkaRocket, // engine tag 0x15` row here AND
        // extend every per-slot table that indexes by TransmogSlot:
        //   slot_metadata.hpp       (SLOT_METADATA - master row:
        //                            game_tag, display_name, part_show_hash_key, enabled)
        //   carrier_defaults.hpp    (CARRIERS - one carrier item
        //                            per character per slot)
        //   prefab_wrapper_swap.cpp (slot_tag_patterns - registry
        //                            classifier patterns)
        Count
    };

    inline constexpr std::size_t SLOT_COUNT = static_cast<std::size_t>(TransmogSlot::Count);

    struct SlotMapping
    {
        bool active = false;
        uint16_t target_item_id = 0;
    };

    std::array<SlotMapping, SLOT_COUNT> &slot_mappings();

    /**
     * @brief Which protagonist owns this equip-slot component (`a1`), 1-based, or 0.
     *
     * @details Answered from the table @ref publish_body_owner_table maintains, so a caller pays three integer
     *          compares rather than a walk of the engine's actor array. A matching row is re-derived from the actor
     *          it came from before its index is returned, so an equip-slot address the allocator reissued cannot
     *          borrow a protagonist's identity.
     *
     *          Returns 0 for NPCs and wildlife, while the chain is mid-teardown, and also before the first table is
     *          published or while its rows are between refreshes. Callers must treat 0 as "unknown", never as
     *          "confirmed not a protagonist". @ref char_idx_for_equip_slot_uncached is where that distinction
     *          matters.
     * @warning Use this, NOT resolve_player_component(), to answer "whose body is this?". resolve_player_component()
     *          always returns Kliff's component whatever character is controlled, so comparing against it silently
     *          accepts every other character's body.
     */
    [[nodiscard]] std::uint32_t char_idx_for_equip_slot(std::uintptr_t a1) noexcept;

    /**
     * @brief Live, uncached form of @ref char_idx_for_equip_slot.
     * @details Walks the actor array on every call, so it always answers from current state and never reports
     *          "unknown" merely because the published table is between refreshes. Reserve it for the apply pipeline,
     *          which runs a handful of times a second and where a blind window silently disarms an ownership guard.
     *          No engine-driven hook must call it. The walk is the cost this whole table exists to keep off those
     *          threads.
     */
    [[nodiscard]] std::uint32_t char_idx_for_equip_slot_uncached(std::uintptr_t a1) noexcept;

    /// Upper bound on published protagonist bodies: the game has exactly three playable characters.
    inline constexpr std::size_t BODY_OWNER_CAP = 3;

    /**
     * @brief Republishes the table of protagonist bodies that @ref char_idx_for_equip_slot answers from.
     * @details Ownership is derived from a full walk of the engine's actor array. The walk stops early only once it
     *          finds every companion, so a party of one sweeps the array end to end - far too expensive to repeat per
     *          socket build on an engine thread, which is where the question is asked.
     *
     *          Ownership therefore has exactly one producer: the load-detect worker, which already holds a fresh
     *          snapshot each tick. Engine threads only ever read the published result. Republishing unconditionally
     *          (rather than on a change signal) is what makes the table self-healing: a snapshot taken while the
     *          actor list is still filling publishes a short table, and the next tick replaces it. A change-triggered
     *          scheme cannot recover from that, because the very signal it waits for is the one that already fired.
     *
     *          The two parallel arrays exist so this header stays free of a CDCore include. Its consumers pull it
     *          in for slot mappings and flags alone, and none of them must acquire the actor-chain headers to
     *          reach a publish entry point.
     *
     * @param ccoias   Protagonist CCOIA pointers, @p n entries.
     * @param char_idxs Matching 1-based character indices (1 Kliff, 2 Damiane, 3 Oongka), @p n entries.
     * @param n        Number of entries. The publish ignores anything past @ref BODY_OWNER_CAP.
     * @note An empty snapshot publishes an empty table. Rows held from the previous publish name bodies the engine
     *       already freed and whose addresses it reissues.
     */
    void publish_body_owner_table(const std::uintptr_t *ccoias, const std::uint32_t *char_idxs, std::size_t n) noexcept;

    /**
     * @brief Which character `slot_mappings()` currently describes: 1 Kliff, 2 Damiane, 3 Oongka, 0 unbound.
     *
     * @details `slot_mappings()` is ONE global array reused for whichever character is being edited or applied, so on
     *          its own it cannot say whose targets it holds. Set by PresetManager::apply_to_state, the only place a
     *          different character's preset is loaded in.
     * @warning Anything that writes per-character state derived from `slot_mappings()` must check this against the
     *          character it is writing FOR, or one character's preset lands in another's bucket.
     */
    std::atomic<std::uint32_t> &slot_mappings_owner() noexcept;

    /**
     * @brief Item IDs last written to slot_mappings, saved before a preset switch.
     * @details clear_all_transmog reads it to know what to unequip.
     */
    std::array<uint16_t, SLOT_COUNT> &last_applied_ids();

    /**
     * @brief Per-character tracking of the last-applied transmog snapshot.
     * @param idx 1-based protagonist index: 1 Kliff, 2 Damiane, 3 Oongka. An out-of-range idx is a no-op.
     * @details The four globals (last_applied_ids, real_damaged, last_applied_real_ids, last_applied_carrier_ids)
     *          describe ONE body's installed state at a time. With multi-character auto-apply and the "Apply To
     *          Selected" feature, the worker applies to any of the three protagonists between hook events, so a
     *          single global snapshot conflates state across bodies. Phase A teardown of Damiane's fakes then uses
     *          Kliff's `last_ids` as its truth source.
     *
     *          Each character's snapshot is buffered. Before each apply the worker hydrates the globals from the
     *          target character's buffered snapshot. After the apply finishes it captures the new globals back.
     */
    void rehydrate_applied_state_for_char(std::uint32_t idx) noexcept;
    void capture_applied_state_for_char(std::uint32_t idx) noexcept;

    /**
     * @brief Wipes ONE character's buffered snapshot AND the live globals, so the character carries no installed fake.
     * @param idx 1-based protagonist index: 1 Kliff, 2 Damiane, 3 Oongka. An out-of-range idx is a no-op.
     * @details The multi-character auto-apply path calls it when the engine reallocates a protagonist's body (CCOIA).
     *          A despawn and respawn (off-screen stream-out and return, follower injury cooldown and recall) hands
     *          back a fresh body in vanilla gear, so the prior snapshot lists fakes that no longer exist. Without the
     *          wipe, apply_all_transmog observes preset==last-applied and real==last-real, fires its "no state
     *          change, skipping" early-out, and leaves the respawned body untransmogged.
     */
    void reset_applied_state_for_char(std::uint32_t idx) noexcept;

    /**
     * @brief Wipes both the globals and every per-character buffered snapshot.
     * @details The save-load wipe path calls it. The engine reallocated every body and every fake transmog item from
     *          the prior session is gone, so the trackers must reset to match.
     */
    void reset_all_applied_state() noexcept;

    // Feature flags

    std::atomic<bool> &flag_player_only();
    std::atomic<bool> &flag_enabled();
    std::atomic<bool> &shutdown_requested();

    /**
     * @brief Master gate for the color_override subsystem.
     * @details It covers the publisher hook, the setter substitute, the host-scope owner-vfunc midhooks and the
     *          picker UI. False by default. The `[Experimental] color_override` INI key enables it. When false, LT
     *          installs none of those hooks and hides the picker UI.
     */
    std::atomic<bool> &flag_color_override();

    /**
     * @brief Master gate for the helm voice-unmuffle filter.
     * @details False by default. The `[Experimental] UnmuffleHelmVoice` INI key enables it and removes the engine's
     *          stock plate and heavy-helm voice muffle. When false, LT installs no passive-skill registrar inline
     *          hook and the muffle behaves as it does in vanilla. The toggle is read once at startup and takes effect
     *          on the next game launch.
     */
    std::atomic<bool> &flag_helm_audio_unmuffle();

    /**
     * @brief One-shot diagnostic dumps gated by the `[Diagnostics]` INI section.
     * @details Off by default. When true, LT writes the matching TSV next to the plugin once ItemNameTable::build()
     *          returns Ok.
     */
    std::atomic<bool> &flag_dump_item_prefabs();
    std::atomic<bool> &flag_dump_item_catalog();

    /**
     * @brief Routes overlay-UI edits to the editing character's own body.
     * @details When the user pins the dropdown to a non-controlled character and this flag is set, overlay-UI edits
     *          (dropdown switch, picker change, slot toggle, preset cycle, manual buttons) apply to the editing
     *          character's body rather than cross-apply onto the controlled body. Engine-triggered equip events still
     *          target the controlled body, so the controlled character's transmog survives their own gear changes.
     *          When false, every apply targets the controlled body, which is the legacy cross-body behavior.
     */
    std::atomic<bool> &flag_apply_to_editing();

    // Trampoline typedefs

    // Populates slot visual data and calls VisualEquipChange. This is the key entry point for transmog: it loads
    // meshes and drives the transition.
    using SlotPopulatorFn = __int64(__fastcall *)(__int64 a1, unsigned __int16 *a2_itemData, __int64 a3_swapEntry);
    SlotPopulatorFn &slot_populator_fn();

    // Rebuilds a single slot's visual. f(a1, slot_a, slot_b, swap_entry) - BOTH slot arguments must name the slot to
    // refresh. SlotPopulator passes the item-derived slot for the first, which collapses the two halves of a paired
    // slot onto one. Null when the AOB scan missed.
    using PartSlotRefreshFn = __int64(__fastcall *)(__int64 a1, __int16 slot_a, __int16 slot_b, __int64 a4_swapEntry);
    PartSlotRefreshFn &part_slot_refresh_fn();

    // Resolves a slot TAG to the slot HANDLE PartSlotRefresh needs for its second argument. Writes 0xFFFF when the
    // tag names no live part record.
    using SlotTagToHandleFn = std::uint16_t *(__fastcall *)(__int64 a1,
                                                            std::uint16_t *out,
                                                            std::uint16_t slot_tag,
                                                            char flag);
    SlotTagToHandleFn &slot_tag_to_handle_fn();

    // Item -> slot handle, or 0xFFFF when the engine will not place the item. SlotPopulator calls this first and
    // bails on 0xFFFF, so it decides whether a carrier can be equipped at all. A carrier the engine refuses makes
    // SlotPopulator a silent no-op: it equips nothing and still returns a value that looks like success, so the
    // apply path asks this question itself rather than reading the answer out of SlotPopulator's return.
    using ItemToSlotResolveFn = std::int64_t(__fastcall *)(std::int64_t a1, std::int16_t item_id);
    ItemToSlotResolveFn &item_to_slot_resolve_fn();

    // Initializes a swap entry to defaults (all -1 and zeros).
    using InitSwapEntryFn = __int64(__fastcall *)(__int64 dest);
    InitSwapEntryFn &init_swap_entry_fn();

    // Cross-TU shared state
    // Atomics and arrays accessed by multiple translation units (hooks, workers, apply logic). Accessor pattern mirrors
    // the flag/fn-ptr accessors above to keep linkage internal to shared_state.cpp.

    /**
     * @brief Recursion guard: set while LT is driving an apply, so its own engine calls are not treated as the
     * player's. Read by socket_mesh_override and the prefab-swap hooks.
     */
    std::atomic<bool> &in_transmog();

    /// Last known player a1, resolved lazily and stored by the load-detect thread on each poll.
    std::atomic<__int64> &player_a1();

    /**
     * @brief Returns the currently-controlled character name ("Kliff", "Damiane", "Oongka").
     * @return The character name, or an empty string when the chain is unresolved or the classifier failed.
     * @details It delegates to the shared Core resolver, which walks the static actor chain and classifies the CCOIA
     *          by appearance-config asset path. Safe to call from any thread.
     */
    std::string current_controlled_character_name() noexcept;

    /**
     * @brief WorldSystem base pointer (game_base + RVA).
     * @details It is atomic because x64 qword stores are naturally atomic on aligned data, but the compiler still
     *          needs the explicit fence for correct ordering.
     */
    std::atomic<uintptr_t> &world_system_ptr();

    /**
     * @brief Per-slot "real item damaged" flag.
     * @details Phase B sets it when it tears down the real item. Once set, the fake==real skip path falls through to
     *          SlotPopulator so the engine restores the mesh. A full clear resets it.
     */
    std::array<bool, SLOT_COUNT> &real_damaged();

    /**
     * @brief Snapshot of the auth-table real item_id per slot at the last apply, indexed by TransmogSlot.
     * @details The dispatcher compares it against live auth state to detect real-item swaps. It covers every
     *          supported slot, so a real-item change on an accessory or a weapon slot (e.g. the tool and the 2H sword
     *          that share engine slot tag 0x0D) triggers a re-apply. Slots LT does not manage stay zeroed.
     */
    std::array<std::uint16_t, SLOT_COUNT> &last_applied_real_ids();

    /**
     * @brief Carrier itemIds used in the last apply, indexed by TransmogSlot.
     * @details 0 means the apply used no carrier (direct apply). Tear-down Phase A reads it to find the correct
     *          scene-graph identity.
     */
    std::array<std::uint16_t, SLOT_COUNT> &last_applied_carrier_ids();

    /**
     * @brief Per-slot one-shot "force apply" flag.
     * @details When true, the next apply for that slot bypasses the `targetId == prev_id` early-out and forces
     *          slot_needs_work in apply_all_transmog. It leaves last_applied_ids[i] intact, so Phase A
     *          `tear_down_fake` still runs against the prior carrier. The body-mesh picker sets it when it re-picks a
     *          prefab on the same carrier (e.g. 0x1521 -> 0x1521 with a different src to tgt wrapper map). Without
     *          it, the dispatcher skips Phase A entirely and the engine's natural-pipeline hook never cleans up the
     *          prior body-mesh target wrapper. The dispatcher clears it after the read.
     */
    std::array<bool, SLOT_COUNT> &force_apply_pending();

    /**
     * @brief When true, the debounce worker runs a clear in place of an apply.
     * @details manual_clear sets it and the worker consumes it.
     */
    std::atomic<bool> &clear_pending();

    /**
     * @brief True while the active preset carries a dye edit that no save, preset switch or file load has consumed.
     * @details It drives the Save button's "Save *" pending indicator. The dye picker sets it.
     *          `PresetManager::save()`, a preset switch and a load clear it. A dye edit writes directly to the active
     *          preset, with no staging area of the kind slot_mappings gives items, so this flag is the only signal
     *          that something needs persisting.
     */
    std::atomic<bool> &dye_dirty();

    /**
     * @brief Slot index for a single-slot hover-apply.
     * @details SLOT_COUNT means "apply all", which is the default. A slot index scopes the next debounced apply to
     *          that one slot and avoids full-gear flicker. The last writer wins: when manual_apply and
     *          manual_apply_slot race before the worker wakes, only the latest store takes effect, so the user's most
     *          recent action decides.
     */
    std::atomic<std::size_t> &pending_slot_index();

    // Hot-path utilities

    inline int64_t steady_ms() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()
        )
            .count();
    }

    /**
     * @brief Renders a path as UTF-8 for a log line or a message box.
     * @param path Any path. Typically one built from DMK::filesystem::get_runtime_directory().
     * @return The same path encoded in UTF-8.
     * @details Not a DetourModKit gap. DMK exposes the runtime directory in UTF-8 and nothing more,
     *          and this renders a path the caller already holds. It exists because the logger formats
     *          through std::format, which has no formatter for std::filesystem::path before C++26, and
     *          because path::string() would re-encode with the process ANSI codepage, which is the very
     *          bug this file stopped having. Display only: open a file from the path itself.
     */
    [[nodiscard]] std::string to_utf8(const std::filesystem::path &path);

} // namespace Transmog

#endif // TRANSMOG_SHARED_STATE_HPP
