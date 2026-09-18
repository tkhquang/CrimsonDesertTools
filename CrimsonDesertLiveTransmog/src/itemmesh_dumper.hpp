#ifndef TRANSMOG_ITEMMESH_DUMPER_HPP
#define TRANSMOG_ITEMMESH_DUMPER_HPP

#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

namespace Transmog
{
    /**
     * @brief Runtime source-mesh solver: the distinct per-body rig mesh prefab names a loaded item emits.
     *
     * It resolves the item's descriptor from the cached iteminfo registry and walks its per-body variant entry list,
     * the same authoritative structure `dump_itemmesh_tsv` uses. The result holds every distinct rig mesh (e.g.
     * Kliff_Mask -> {cd_phm_00_mask_00_0271_a, cd_phw_01_..., cd_pom_01_...}). An item with no variant list
     * (single-rig) falls back to its rule-chain primary body mesh, so the result is non-empty for any item that owns
     * a real mesh. It returns empty only when the registries have not resolved yet, or when the item owns no
     * resolvable mesh. Every read runs under a fault guard, and the call is safe from the game thread and from the UI
     * thread.
     *
     * It lets prefab_wrapper_swap derive the body-mesh swap SOURCE from the carrier's item_id at runtime instead of a
     * hardcoded prefab name in carrier_defaults.hpp.
     */
    [[nodiscard]] std::vector<std::string> variant_meshes_for_item(std::uint16_t item_id) noexcept;

    /**
     * @brief Write the item-prefab TSV next to the plugin DLL, once the swap catalog exists.
     *
     * The dump walks `iteminfo[]` and resolves `desc + 0x90 -> u16 stringSlot -> stringinfo[slot]` for each entry.
     * That c-string is the `ItemIcon_Prefab_<prefab>` token the engine uses to bind an item to its display mesh. The
     * dump cross-references the loader registry, the StringInfo registry, and a byte-scan of committed asset-bundle
     * memory, so every item gets a row even when its prefab string lives outside the swap-catalog's vtable-filtered
     * slice.
     *
     * @param stop Cooperative stop signal. The catalog wait polls it and abandons the dump on a stop request, which
     *             is what lets teardown join the owning worker.
     * @note Call it AFTER `ItemNameTable::build()` returns Ok, so the InternalName column carries data. The deferred
     *       nametable worker thread can call it safely.
     * @note Neither registry holder is hardcoded. The iteminfo holder comes from ItemNameTable's bounded call-graph
     *       walk and the stringinfo holder from the StringInfoRegistry anchor, so no RVA has to survive a patch.
     * @note Setup/control-plane only: it walks registries and writes a file. It runs on a worker, never a detour.
     */
    void dump_itemmesh_tsv(std::stop_token stop);

    /**
     * @brief Start the TSV dump on a DMK worker, or do nothing when one already runs.
     * @note The worker holds a counted module reference, so the pin ledger shows it while it lives.
     */
    void launch_itemmesh_dump();

    /// Requests stop and joins the dump worker. Idempotent, and safe when no worker started.
    void join_itemmesh_dump() noexcept;
} // namespace Transmog

#endif // TRANSMOG_ITEMMESH_DUMPER_HPP
