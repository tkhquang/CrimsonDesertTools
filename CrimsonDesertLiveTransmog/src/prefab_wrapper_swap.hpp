#pragma once

#include "shared_state.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pointer-swap body-mesh override.
//
// Hooks `sub_140352AA0` (the 96B record-copy operator). When LT is
// inside an `apply_transmog` call (`Transmog::in_transmog() == true`)
// AND the source record's intermediate-wrapper pointer matches a
// registered Source, the wrapper-ptr is swapped to a registered
// Target before the copy completes.
//
// The wrapper for each StringInfo entry lives at `*(QWORD*)(entry +
// 0x18)`. Our INI configures NAMES (e.g. "cd_phm_00_ub_00_0435"); on
// activation we walk the StringInfo registry at MEMORY[0x145EF1DE8],
// resolve each name to its entry, and read the wrapper-ptr from the
// entry's +0x18. The hook then matches by wrapper-ptr equality.
//
// Refcount handling: `Target` wrapper is `_InterlockedIncrement`'d
// before substitution (so the destination's eventual destruction-time
// decrement is balanced). `Source` wrapper's leftover +1 (from the
// pre-copy bump in `sub_14079DA20`) is left in place; net effect is a
// per-substitution +1 leak on the source -- acceptable for these small
// wrapper allocations that StringInfo keeps alive for the session.
//
// Why this doesn't break inventory (unlike the prior `sub_142719500`
// substitution): the gate is `in_transmog()`, which is set ONLY when
// LT itself is calling SlotPopulator with a fake item. The engine's
// own real-item flow (inventory checks, equip validation) runs with
// the gate FALSE so the hook is a pass-through.
//
// No INI keys. The hook installs unconditionally at boot; per-slot
// source defaults are hardcoded (see k_kliffCarriers in
// transmog_apply.cpp and k_defaultSrcByEnumSlot in this file's .cpp);
// target selection is user-driven via the overlay picker.

namespace Transmog::PrefabWrapperSwap
{
    void register_config();
    bool init();
    void shutdown();
    bool is_active();

    /// Programmatic deactivation. Clears the swap map and disarms the
    /// active flag -- equivalent to the hotkey toggle when active. Used
    /// by `Transmog::clear_all_transmog` so a "Clear" action also
    /// removes any pointer-swap effect (otherwise the re-apply-real
    /// pass would re-substitute target wrappers and the user would
    /// stay in the body-mesh override outfit instead of restoring the
    /// carrier's mesh). Idempotent.
    void deactivate_for_clear();

    /// Notify the module of an upcoming apply's slot itemIds. If swap
    /// is active AND the previous apply's recorded itemIds differ from
    /// these, treat this as a preset-switch and `deactivate_for_clear`
    /// before the new apply so substitutions don't re-bind target
    /// wrappers to the new gear. The first apply post-activation
    /// records its itemIds without deactivating.
    void notify_apply_starting(const std::uint16_t (&itemIds)[5]);

    /// Notify the module of a completed apply's slot itemIds. Records
    /// them so the next `notify_apply_starting` can detect a switch.
    /// No-op when inactive.
    void notify_apply_finished(const std::uint16_t (&itemIds)[5]);

    /// Read-only membership check against the resolved target-wrapper
    /// set (the values in s_swapMap). Used by the SMO-add block hook to
    /// decide whether to sentinel-swap an asset wrapper during the
    /// deactivate window. Lock-free read of an immutable-on-hot-path
    /// snapshot -- see s_targetWrappers comment.
    [[nodiscard]] bool is_target_wrapper(std::uintptr_t wrapperPtr) noexcept;

    /// Current count of resolved target wrappers. Used by the SMO-add
    /// block hook for diagnostic logging.
    [[nodiscard]] std::size_t target_wrappers_count() noexcept;

    // --- Per-slot dropdown catalog (Goal B) ---
    //
    // The catalog is built by walking StringInfo with a broad
    // body-mesh prefix ("cd_phm_00_") and classifying each matched
    // entry into one of the 5 transmog slots by sub-prefix. Once
    // populated, `slot_catalog(slot)` returns the per-slot vector
    // sorted alphabetically by name -- ready to feed an ImGui combo.
    //
    // The dropdown state (per-slot src/tgt index) lives in this module
    // alongside the existing s_swapMap / s_targetWrappers so
    // `apply_selections_to_swap_map()` can rebuild the swap from
    // selections in one place. No disk persistence -- selections are
    // session-scoped only.

    struct PrefabEntry
    {
        std::string                  name;      // e.g. "cd_phm_00_hel_00_0395_c"
        // ALL known wrapper instances for this prefab name. The first
        // element is the canonical "asset wrapper" from StringInfo's
        // entry+0x18 (pool 0x4104E*); subsequent elements are parallel
        // allocations recovered by the boot-time heap walk (e.g. pool
        // 0x4104A* partprefabdyeslot). Empty vector when the prefab is
        // present in the AppearanceTableLoader catalog but not yet
        // loaded into StringInfo (no wrapper resident).
        std::vector<std::uintptr_t>  wrappers;
        std::uint32_t                hash;      // entry+0x00 / metadata+0x10
        // 24-byte metadata pointer returned by the AppearanceTableLoader
        // lookup primitive (see lookup_prefab_metadata). 0 if either the
        // loader was not captured at startup or the name is absent from
        // the loader's table. Owned by the engine; never freed by us.
        std::uintptr_t               metadata{0};
        // True iff at least one usable wrapper is currently resident in
        // StringInfo (i.e. the engine can render this prefab right now
        // without an async load). When false, the picker may still show
        // the entry to the user but selecting it is a best-effort
        // operation -- a force-load pass may be required.
        bool                         is_loaded{false};
    };

    // --- AppearanceTableLoader integration (Goal A #5) ---
    //
    // The PartPrefab table is a 252,480-entry container the engine
    // builds at boot from .pappt parser output. Its loader instance
    // lives at `[[ResMgr+0x40]+0x88]` -- captured by hooking the first
    // call to `sub_1408AF8F0` (game-init). The container itself is
    // `loader+8`, vtable `0x144D24308`.
    //
    // We expose a read-only lookup against this container so the picker
    // can surface prefabs that exist in the engine's catalog but are
    // not yet StringInfo-resident (i.e. have no live wrapper). No INI
    // keys in this module.

    /// Returns the 24-byte AppearanceTableLoader metadata pointer for
    /// `name` if present in the boot-loaded PartPrefab container.
    /// Returns 0 on miss OR if the loader was not captured at startup.
    /// Pure read-only -- safe to call from the UI thread.
    [[nodiscard]] std::uintptr_t lookup_prefab_metadata(
        const char *name) noexcept;

    /// True if the AppearanceTableLoader hook fired and we hold valid
    /// snapshots of partPrefabContainer.
    [[nodiscard]] bool is_loader_ready() noexcept;

    /// Idempotent. Walks StringInfo with prefix "cd_phm_00_" once and
    /// classifies entries into per-slot vectors (sorted by name).
    /// Returns total entries cataloged. Does NOT mutate s_swapMap or
    /// activation state. Safe to call from the UI thread.
    std::size_t populate_slot_catalogs() noexcept;

    /// True after a successful populate_slot_catalogs.
    [[nodiscard]] bool is_catalog_populated() noexcept;

    /// Slot-prefix string used for catalog classification (also
    /// useful for the UI to strip the prefix from dropdown labels).
    [[nodiscard]] const char *slot_prefix_str(
        Transmog::TransmogSlot slot) noexcept;

    /// Per-slot catalog (sorted by name). Returns an empty vector for
    /// slots that have not been populated yet (e.g. catalog walk hasn't
    /// run, or the slot has no matching prefabs in StringInfo).
    [[nodiscard]] const std::vector<PrefabEntry> &slot_catalog(
        Transmog::TransmogSlot slot) noexcept;

    /// Selection indices per slot. -1 = unset; 0..N-1 = index into the
    /// slot's catalog. UI reads/writes these.
    [[nodiscard]] int selection_src_index(
        Transmog::TransmogSlot slot) noexcept;
    [[nodiscard]] int selection_tgt_index(
        Transmog::TransmogSlot slot) noexcept;
    void set_selection(Transmog::TransmogSlot slot,
                       int srcIdx, int tgtIdx) noexcept;

    /// Cross-slot adoption. Copies the PrefabEntry at
    /// `fromSlot`'s catalog index `fromIdx` into `intoSlot`'s catalog
    /// (deduped by name -- if `intoSlot`'s catalog already has an
    /// entry with the same name, the existing index is reused). Sets
    /// `intoSlot`'s tgt selection to the resulting index. Source
    /// selection on `intoSlot` is left untouched.
    ///
    /// Used by the overlay's "Prefabs" cross-slot browse mode so the
    /// user can apply, say, a 2H weapon prefab onto the MainHand slot
    /// without auto-routing the apply to the prefab's native slot.
    /// The engine's record-copy hook is slot-agnostic; correctness of
    /// the resulting render is the user's concern.
    ///
    /// Returns the new tgt index on success, -1 on bad slot/index.
    int adopt_into_slot_and_select(
        Transmog::TransmogSlot intoSlot,
        Transmog::TransmogSlot fromSlot,
        int fromIdx) noexcept;

    /// Rebuilds s_swapMap from the current per-slot selections. Reads
    /// every pre-cached wrapper instance per name from the catalog
    /// (the boot-time heap walk merges parallel pool allocations into
    /// PrefabEntry::wrappers); no I/O or heap walk on this hot path.
    /// Returns the count of slot pairs successfully bound. Safe to
    /// call when inactive -- this only resolves; it does NOT toggle
    /// activation.
    std::size_t apply_selections_to_swap_map() noexcept;

    /// True if at least one slot has BOTH a src AND tgt selection set.
    [[nodiscard]] bool has_any_selection() noexcept;

    /// Reactivate using the current per-slot dropdown selections. If
    /// already active, deactivates first to clean up the previous
    /// substitutions (scene-graph + staging) before rebuilding the swap
    /// map from the new selections and re-arming. When all selections
    /// are cleared, deactivates and stays inactive. Returns the count
    /// of slots successfully bound.
    ///
    /// Used by the slot-row dropdowns for auto-apply on selection
    /// change (no F10 required) -- mirrors the carrier-item picker
    /// flow in the overlay where any change immediately reapplies that
    /// slot.
    std::size_t reactivate_with_selections() noexcept;
} // namespace Transmog::PrefabWrapperSwap
