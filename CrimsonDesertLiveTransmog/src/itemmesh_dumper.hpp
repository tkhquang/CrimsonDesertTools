#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Transmog
{
    /**
     * @brief Runtime source-mesh solver: the distinct per-body rig mesh prefab names a loaded item emits.
     *
     * Resolves the item's descriptor from the (AOB-resolved, cached) iteminfo registry and walks its per-body
     * variant entry list at `desc+0x3E0` -- the same authoritative structure `dump_itemmesh_tsv` uses -- returning
     * every distinct rig mesh (e.g. Kliff_Mask -> {cd_phm_00_mask_00_0271_a, cd_phw_01_..., cd_pom_01_...}). Items
     * with no variant list (single-rig) fall back to their rule-chain primary body mesh, so the result is non-empty
     * for any item that owns a real mesh. Returns empty only when the registries are not resolved yet or the item
     * has no resolvable mesh. All reads are SEH-guarded; safe to call from the game or UI thread.
     *
     * Lets PrefabWrapperSwap derive the body-mesh swap SOURCE from the carrier's itemId at runtime instead of a
     * hardcoded prefab name in carrier_defaults.hpp.
     */
    [[nodiscard]] std::vector<std::string> variant_meshes_for_item(std::uint16_t itemId) noexcept;

    /**
     * @brief One-shot diagnostic dump of every loaded item descriptor's icon-prefab string to a TSV next to the plugin
     *        DLL.
     *
     * Walks `iteminfo[]` and resolves `desc + 0x90 -> u16 stringSlot -> stringinfo[slot]` for each entry; the resulting
     * c-string is the `ItemIcon_Prefab_<prefab>` token the engine uses to bind an item to its display mesh. The dump
     * cross-references against the loader registry, the StringInfo registry, and a byte-scan of MEM_PRIVATE asset
     * bundles so every item is accounted for even when its prefab string lives outside the swap-catalog's
     * vtable-filtered slice.
     *
     * @note Must be called AFTER `ItemNameTable::build()` returns Ok so the InternalName column is populated. Safe to
     *       call from the deferred nametable worker thread.
     * @note Both registry holders (`iteminfo` and `stringinfo`) are AOB-resolved per call -- no hardcoded RVAs survive
     *       a game patch.
     */
    void dump_itemmesh_tsv();
} // namespace Transmog
