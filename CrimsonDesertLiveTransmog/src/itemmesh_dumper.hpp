#pragma once

namespace Transmog
{
    /**
     * @brief One-shot diagnostic dump of every loaded item descriptor's
     *        icon-prefab string to a TSV next to the plugin DLL.
     *
     * Walks `iteminfo[]` and resolves `desc + 0x90 -> u16 stringSlot ->
     * stringinfo[slot]` for each entry; the resulting c-string is the
     * `ItemIcon_Prefab_<prefab>` token the engine uses to bind an item
     * to its display mesh. The dump cross-references against the
     * loader registry, the StringInfo registry, and a byte-scan of
     * MEM_PRIVATE asset bundles so every item is accounted for even
     * when its prefab string lives outside the swap-catalog's
     * vtable-filtered slice.
     *
     * @note Must be called AFTER `ItemNameTable::build()` returns Ok so
     *       the InternalName column is populated. Safe to call from
     *       the deferred nametable worker thread.
     * @note Both registry holders (`iteminfo` and `stringinfo`) are
     *       AOB-resolved per call -- no hardcoded RVAs survive a game
     *       patch.
     */
    void dump_itemmesh_tsv();
}
