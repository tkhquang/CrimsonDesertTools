#ifndef TRANSMOG_PREFAB_WRAPPER_SWAP_HPP
#define TRANSMOG_PREFAB_WRAPPER_SWAP_HPP

#include "shared_state.hpp"

#include <DetourModKit/hook.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// pws state-lifetime model
// pws keeps three pieces of state, each with a different scope:
//
//   - s_sel_src_idx_per_char[N] / s_sel_tgt_idx_per_char[N] - session-scoped, ONE row per protagonist.
//     The UI writes here
//     through set_selection, mirrored from the active editing-view globals. The rows preserve uncommitted picks across
//     editing-character switches, so a flip of the dropdown does not silently drop the outgoing character's picks.
//
//   - s_swap_map_per_char[3] / s_target_wrappers_per_char[3] - per-character apply-scoped. Each apply rebuilds ONLY the
//     active character's bucket (s_active_char_idx - 1). The other two characters' buckets stay intact, so a pending
//     teardown on a body that was applied to in an earlier pass can still find its installed target wrappers. The
//     natpipe-hook consults the active character's bucket at install-time wrapper traversal to redirect src->tgt. The
//     redirected wrapper is then materially installed, after which subsequent rendering reads it directly with no
//     further hook involvement.
//
//     Per-character keying matters because protagonists can share carrier prefab names (e.g. Kliff and Oongka both
//     default to `cd_phm_00_ub_00_0054` for Chest). A union of the rows into one map cross-talks one character's
//     selection onto another character's body. Dispatch by s_active_char_idx keeps each body's install window resolving
//     only its own row.
//
// Driving character: PresetManager::apply_to_state() primes s_active_char_idx with the editing-character idx. The next
// apply build reads that idx to pick the right per-char bucket. See the "current_apply_owner" helper for the canonical
// pin/flag derivation every site uses.

// Pointer-swap body-mesh override.
//
// Hooks the 96-byte record-copy operator (AnchorId::StructCopy). When the source record's wrapper names a prefab the
// active character's swap map holds, the wrapper-ptr is replaced with a registered target before the copy completes.
//
// The swap map keys on the prefab NAME HASH the wrapper carries at `+0x0C`, not on a wrapper pointer. Every instance
// of a name carries the same hash, so the engine can hand over any pool instance and the lookup still resolves. The
// catalog and the engine draw from different pools, which is what pointer keying can never bridge.
//
// The wrapper for each StringInfo entry lives at `*(QWORD*)(entry + 0x18)`. AnchorId::StringInfoRegistry resolves the
// registry the walk starts from. Wrapper layout: `+0x00` string ptr, `+0x08` length, `+0x0C` name hash, `+0x10`
// refcount.
//
// Refcount handling: the `Target` wrapper is `_InterlockedIncrement`'d before substitution, so the destination's
// eventual destruction-time decrement stays balanced. The `Source` wrapper keeps its leftover +1 from the pre-copy
// bump. The net effect is a per-substitution +1 leak on the source - acceptable for these small wrapper allocations
// that StringInfo keeps alive for the session.
//
// Why this does not break inventory: the swap map is the filter. It carries entries only for the slots LT drives, and
// the per-actor scope derived by the part-list-merge hook keeps each substitution on the protagonist that owns it. A
// non-protagonist assembly is refused outright.
//
// No INI keys. The hook installs unconditionally at boot. Per-slot source defaults derive at runtime from each
// character's carrier item, and target selection is user-driven through the overlay picker.

namespace Transmog::prefab_wrapper_swap
{
    /// Registers this module's INI keys. The body-mesh swap declares none.
    void register_config() noexcept;

    /**
     * @brief Install the record-copy, natural-pipeline, claim-removal and part-list-merge hooks.
     *
     * @param hooks Stack that owns every hook this module installs.
     * @return True when the record-copy hook armed. A miss on any other anchor degrades that feature alone.
     */
    [[nodiscard]] bool init(DetourModKit::hook::HookStack &hooks);

    /// Sweeps installed visuals, then drops every catalog, swap map and target set.
    void shutdown();

    /**
     * @brief Programmatic deactivation. Clears the active flag and preserves the swap map for a cheap re-arm.
     *
     * @details The natpipe hook stops substituting once the flag is false. `Transmog::clear_all_transmog` calls this
     *          so a "Clear" action also removes any pointer-swap effect. Without it the re-apply-real pass
     *          re-substitutes target wrappers and the user stays in the body-mesh override outfit instead of the
     *          carrier's mesh. Idempotent.
     */
    void deactivate_for_clear();

    /**
     * @brief Arm the swap for a single-slot install, without disturbance of any other slot.
     *
     * @return The number of slots bound.
     * @details `notify_apply_starting` cannot serve this path. It routes through `reactivate_with_selections`. That
     *          function starts with an unconditional `deactivate_for_clear` whenever the swap is already active.
     *          `deactivate_for_clear` drains the WHOLE substitution ledger and reverse-writes every live record,
     *          across every slot and every character. A single-slot apply re-installs only its own slot. Every other
     *          slot then stays reverted to its base mesh until the next full apply.
     *
     *          This entry point instead rebuilds the name-to-wrapper map from the current selections and raises the
     *          active flag. It never touches the ledger. The re-install appends its own record through the
     *          struct-copy hook.
     * @warning Call it only when the slot INSTALLS a fake. On a single-slot tear-down the swap must stay as it is. An
     *          armed map rewrites the wrapper that the engine's unlink pass looks for, the unlink misses, and the old
     *          mesh stays painted on the actor.
     */
    std::size_t ensure_armed_for_slot_apply() noexcept;

    /**
     * @brief Character index (1-based, matching CDCore) the per-slot target table currently holds targets for. Zero
     *        means unbound.
     *
     * @details The table holds ONE character's targets at a time. Anything that installs those targets onto a body
     *          must check this against the body's own character (Transmog::char_idx_for_equip_slot), or it dresses
     *          whichever body it is handed with whichever character's table happens to be current.
     */
    [[nodiscard]] std::uint32_t target_table_char_idx() noexcept;

    /**
     * @brief Target wrapper bound to @p slot_idx for the character currently being applied, or 0 when the slot has no
     *        target.
     *
     * @details Answers what this SOCKET must wear, which the source-hash-keyed swap map cannot.
     */
    [[nodiscard]] std::uintptr_t target_wrapper_for_slot(std::size_t slot_idx) noexcept;

    /**
     * @brief Discard uncommitted prefab picks so a save-load starts from the preset alone.
     *
     * @details Call before PresetManager::apply_to_state, which re-mirrors the preset's own picks. Clears the TARGET
     *          column only. The source column is a derived default, seeded once from each character's carrier item,
     *          and nothing re-derives it - see the note in the definition.
     */
    void resync_to_preset() noexcept;

    /**
     * @brief Rebuild the per-slot target table from current selections and slot targets.
     *
     * @details Needs no live actor, so it can run while a new world is still under construction - which is when
     *          socket_mesh_override reads it.
     */
    void rebuild_target_table() noexcept;

    /**
     * @brief Mark ONE slot's previous target as stale, so the post-apply sweep removes it.
     *
     * @param prev_item_id The target being replaced. Zero is a no-op.
     * @details The full apply gets this for free: `notify_apply_starting` runs a deactivate cycle that parks every
     *          installed target, the rebuild re-registers only what is still selected, and the sweep detaches the
     *          difference. A single-slot apply cannot use that cycle, because a park of everything while only one
     *          slot re-installs sweeps the other slots' live targets off the body. It parks only the target it
     *          replaces.
     *
     *          Without this a single-slot target change parks nothing, the sweep finds no candidates, and the
     *          previous mesh stays on screen. It is the whole reason an Instant-Apply pick behaved differently from
     *          Apply All.
     * @warning Call BEFORE @ref ensure_armed_for_slot_apply, and only when the target actually changes. The id that
     *          is being re-installed parks the wrappers the sweep is about to see as live.
     */
    void park_slot_target_for_sweep(std::uint16_t prev_item_id) noexcept;

    /**
     * @brief Run the post-apply sweep for a single-slot apply.
     *
     * @details Same sweep `notify_apply_finished` drives, exposed for the path that does not go through it. Detaches
     *          parked wrappers that the just-completed apply did not re-install. Safe with nothing parked - it
     *          returns immediately.
     */
    void sweep_after_slot_apply() noexcept;

    /**
     * @brief Notify the module of an upcoming apply's slot itemIds.
     *
     * @param itemIds The five armor-slot item ids this apply installs.
     * @details The itemIds decide between an install pass and a cleanup-only pass. An install pass rebuilds the swap
     *          map and activates. A cleanup-only pass (every id zero) deactivates, so the tear-down calls that follow
     *          run with the hook in passthrough.
     */
    void notify_apply_starting(const std::uint16_t (&itemIds)[5]);

    /**
     * @brief Notify the module of a completed apply's slot itemIds.
     *
     * @param itemIds The five armor-slot item ids this apply installed.
     * @details Records them so the next `notify_apply_starting` can detect a switch. No-op when inactive.
     */
    void notify_apply_finished(const std::uint16_t (&itemIds)[5]);

    // Per-slot dropdown catalog
    //
    // The catalog walks StringInfo with a broad body-mesh prefix ("cd_phm_00_") and classifies each matched entry
    // into one of the 5 transmog slots by sub-prefix. Once populated, `slot_catalog(slot)` returns the per-slot
    // vector sorted alphabetically by name - ready to feed an ImGui combo.
    //
    // The dropdown state (per-slot src/tgt index) lives in this module alongside `s_swap_map_per_char` /
    // `s_target_wrappers_per_char` so `apply_selections_to_swap_map()` can rebuild the swap from selections in one
    // place. No disk persistence - selections are session-scoped only.

    /**
     * @brief One prefab the picker can select, with every wrapper instance known for its name.
     */
    struct PrefabEntry
    {
        /// Full prefab name, e.g. "cd_phm_00_hel_00_0395_c".
        std::string name;
        /**
         * @brief Every known wrapper instance for this prefab name.
         * @details The first element is the canonical asset wrapper from StringInfo's entry+0x18 (pool 0x4104E*).
         *          Later elements are parallel allocations the boot-time heap walk recovers, for example pool
         *          0x4104A* partprefabdyeslot. The vector is empty when the prefab is present in the
         *          AppearanceTableLoader catalog but not yet loaded into StringInfo, so no wrapper is resident.
         */
        std::vector<std::uintptr_t> wrappers;
        /// Prefab name hash, read from entry+0x00 / metadata+0x10.
        std::uint32_t hash{0};
        /**
         * @brief True when at least one usable wrapper is resident in StringInfo.
         * @details A resident wrapper lets the engine render this prefab now, with no async load. When false the
         *          picker can still show the entry, but a selection is best-effort and can need a force-load pass.
         */
        bool is_loaded{false};
    };

    // AppearanceTableLoader integration
    //
    // The PartPrefab table is a 252,480-entry container the engine builds at boot from .pappt parser output. Its
    // loader instance lives at `[[ResMgr+0x40]+0x88]`, captured at game-init through AnchorId::LoaderRegistry. The
    // container itself is `loader+8`.
    //
    // There is deliberately no per-name lookup against it here. A lookup backed by the engine's own primitive costs
    // an AOB cascade to maintain and produces a value nothing consumes. `for_each_loader_prefab_name` below is the
    // part with a real consumer, and the picker indexes its walk to answer per-name questions in-process. No INI keys
    // in this module.

    /**
     * @brief Walk every entry in the AppearanceTableLoader registry singleton and invoke @p cb with each entry's
     *        inline key c-string name.
     *
     * @param cb Visitor called once per entry. A null callback is a no-op.
     * @details Unfiltered: it emits all ~15k entries including non-body-mesh families (`gimmick_*`, `collection_*`,
     *          `cd_ex_*`, etc.) that the internal `enumerate_loader_registry_into_catalog` drops through its slot-tag
     *          filter. Intended for diagnostic dumps, not for the live swap pipeline. No-op when the singleton is not
     *          resolved yet.
     */
    void for_each_loader_prefab_name(const std::function<void(std::string_view)> &cb) noexcept;

    /**
     * @brief Walk StringInfo with prefix "cd_" once and copy the matching entries into every slot's catalog vector.
     *
     * @return Total entries cataloged.
     * @details Idempotent. Each catalog comes back sorted by name. Mutates no swap-map bucket and no activation
     *          state. Safe to call from the UI thread.
     */
    std::size_t populate_slot_catalogs() noexcept;

    /// True after a successful populate_slot_catalogs.
    [[nodiscard]] bool is_catalog_populated() noexcept;

    /**
     * @brief Per-slot catalog, sorted by name.
     *
     * @param slot Slot whose catalog to read.
     * @return An empty vector for a slot that is not populated yet, for example when the catalog walk did not run or
     *         the slot has no matching prefabs in StringInfo.
     */
    [[nodiscard]] const std::vector<PrefabEntry> &slot_catalog(Transmog::TransmogSlot slot) noexcept;

    /// Source selection index for @p slot. -1 means unset, 0..N-1 indexes the slot's catalog.
    [[nodiscard]] int selection_src_index(Transmog::TransmogSlot slot) noexcept;
    /// Target selection index for @p slot. -1 means unset, 0..N-1 indexes the slot's catalog.
    [[nodiscard]] int selection_tgt_index(Transmog::TransmogSlot slot) noexcept;
    /**
     * @brief Write a slot's source and target selection, mirroring it into a character's per-character row.
     *
     * @param site Short caller tag, logged with the write at trace level. Diagnostic only: a poisoned per-character
     *        row is only findable when the log names who wrote it.
     * @param char_idx_for Bucket to mirror into, 1-based. Zero means whichever character is bound right now.
     * @warning A caller that already knows which character it is writing for MUST pass @p char_idx_for. A read of the
     *          bound character inside this function re-samples a global another thread can change mid-loop, so a
     *          per-slot restore loop can start on one character's bucket and finish on another's.
     */
    void set_selection(
        Transmog::TransmogSlot slot,
        int src_idx,
        int tgt_idx,
        std::string_view site = "?",
        std::uint32_t char_idx_for = 0
    ) noexcept;

    /**
     * @brief Bind the writes done by `set_selection` to a specific protagonist row.
     *
     * @param idx 1=Kliff, 2=Damiane, 3=Oongka. Idx 0 disables per-char mirroring, used while the boot path runs
     *        before PresetManager resolves the editing character.
     * @details The same binding drives the active editing view exposed through selection_*_index.
     *          `apply_selections_to_swap_map()` rebuilds `s_swap_map_per_char[ci]` from the bound
     *          character's row alone.
     *          The other characters' buckets stay intact, so a substitution picked on Damiane stays live in her
     *          bucket while the UI shows Kliff. The natpipe-hook dispatches by `s_active_char_idx` to pick the right
     *          bucket for the body currently under assembly.
     */
    void set_active_char_idx(std::uint32_t idx) noexcept;

    /**
     * @brief Drop every buffered per-char selection row.
     *
     * @details Called from the save-load wipe paths so the next world starts with empty rows, because every fake from
     *          the prior arena is gone.
     */
    void reset_per_char_state() noexcept;

    /**
     * @brief Cross-slot adoption. Copy the PrefabEntry at @p from_slot's catalog index @p from_idx into @p into_slot's
     *        catalog and select it there.
     *
     * @return The new tgt index on success, -1 on a bad slot or index.
     * @details Deduped by name: when @p into_slot's catalog already has an entry with the same name, the existing
     *          index is reused. Source selection on @p into_slot is left untouched.
     *
     *          The overlay's "Prefabs" cross-slot browse mode uses this so the user can apply, say, a 2H weapon
     *          prefab onto the MainHand slot without an auto-route of the apply to the prefab's native slot. The
     *          engine's record-copy hook is slot-agnostic. Correctness of the resulting render is the user's concern.
     */
    [[nodiscard]] int adopt_into_slot_and_select(
        Transmog::TransmogSlot into_slot,
        Transmog::TransmogSlot from_slot,
        int from_idx
    ) noexcept;

    /**
     * @brief Rebuild `s_swap_map_per_char[active-1]` from the bound character's per-slot selections.
     *
     * @return The count of slot pairs successfully bound.
     * @details Reads pre-cached wrapper instances per name from the catalog (the boot-time heap walk merges parallel
     *          pool allocations into PrefabEntry::wrappers) and runs a fresh, small heap re-walk over only the
     *          selected source names (~1-2ms for at most 5 names) to pick up wrappers that the boot scan missed. Safe
     *          to call when inactive - this only resolves, and it does NOT toggle activation.
     */
    [[nodiscard]] std::size_t apply_selections_to_swap_map() noexcept;

    /// True if at least one slot has BOTH a src AND tgt selection set.
    [[nodiscard]] bool has_any_selection() noexcept;

    /**
     * @brief Reactivate using the current per-slot dropdown selections.
     *
     * @return The count of slots successfully bound.
     * @details When already active, it deactivates first to clean up the previous substitutions (scene-graph plus
     *          staging) before it rebuilds the swap map from the new selections and re-arms. With all selections
     *          cleared it deactivates and stays inactive.
     *
     *          The slot-row dropdowns use this for auto-apply on selection change. It mirrors the carrier-item picker
     *          flow in the overlay, where any change immediately reapplies that slot.
     */
    [[nodiscard]] std::size_t reactivate_with_selections() noexcept;

    /**
     * @brief Record a DIRECT fake - a slot where the carrier item IS the target item, so it is equipped as itself and
     *        no prefab substitution happens at all.
     *
     * @param item_id The item equipped as itself.
     * @details Such a slot installs a visual without ever passing through `on_struct_copy`, so nothing lands in this
     *          character's installed-wrapper set and the post-apply sweep has no victim to look for. The slot then
     *          keeps its mesh attached forever once cleared. Resolution of the item's prefabs here, plus their
     *          addition to the installed set, makes the existing park-then-subtract machinery cover direct fakes for
     *          free: the next apply parks them, the rebuilt set no longer lists them, so they fall out as orphans and
     *          get detached.
     */
    void register_direct_fake(std::uint16_t item_id) noexcept;
} // namespace Transmog::prefab_wrapper_swap

#endif // TRANSMOG_PREFAB_WRAPPER_SWAP_HPP
