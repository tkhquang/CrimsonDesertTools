#include "visibility_write.hpp"
#include "categories.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <vector>

namespace EquipHide
{
    /* File-scope scratch buffer reused across apply_direct_vis_write calls.
       Populated under vis_write_mutex (held for the full duration of the
       function) so concurrent callers never race. File scope rather than a
       local inside the __try block keeps the stack small for game threads
       with tight stack reserves and side-steps MSVC C2712.

       Stores composite (vis_ctrl, addr) keys so the orphan sweep can
       distinguish "this address is no longer in any character's part
       map" (true orphan -- restore) from "this address belongs to a
       different vis_ctrl that has already been processed in this pass"
       (not an orphan -- leave alone). Address-only keying would
       restore the previously-active character's vis bytes back to
       visible on a swap, because the new character's vis ctrl does
       not iterate the outgoing character's vis-byte addresses. */
    static std::vector<VisKey> s_touchedVisKeys;

    /* File-scope active vis-ctrl scratch for the orphan sweep. */
    static std::array<std::uintptr_t, k_maxProtagonists> s_activeVisCtrls{};

    /* Stateless less-than comparator on (vis_ctrl, addr) lexicographic
       order. Free function (not a lambda) so it can sit alongside __try
       without tripping C2712 on captured lambdas. */
    static bool vis_key_less(const VisKey &a, const VisKey &b) noexcept
    {
        if (a.visCtrl != b.visCtrl)
            return a.visCtrl < b.visCtrl;
        return a.addr < b.addr;
    }

    /* Implementation body extracted out of the SEH-wrapped public entry
       point so MSVC's C2712 ("Cannot use __try in functions that require
       object unwinding") does not fire. The new per-(vis_ctrl, addr)
       keyed map and the per-character map selection both introduce
       hidden temporaries (VisKey rvalues, conditional reference
       materialisation, structured-binding pair access on a map keyed by
       a class type) that MSVC reports as object unwinding requirements
       and refuses to combine with __try. Pattern mirrors
       equip_hide.cpp::on_vis_check_impl. */
    static void apply_direct_vis_write_impl() noexcept
    {
        auto &addrs = resolved_addrs();
        auto &logger = DMK::Logger::get_instance();
        auto lookup = reinterpret_cast<MapLookupFn>(addrs.mapLookup);
        auto &ps = player_state();
        auto &origVis = original_vis_map();
        const auto n = ps.count.load(std::memory_order_relaxed);
        int hiddenCount = 0;
        int restoredCount = 0;

        for (int i = 0; i < n; ++i)
        {
            auto vc = ps.visCtrls[i].load(std::memory_order_relaxed);
            if (!vc)
                continue;

            /* Per-slot character idx. -1 (unknown body, fallback path,
               pre-resolve) collapses to the active character's map via
               classify_part_for / is_any_category_hidden_for so
               unidentified slots preserve single-character semantics. */
            const int charIdx =
                ps.visCharIdx[i].load(std::memory_order_relaxed);

            auto comp = read_ptr_unsafe(vc, 0x58);
            if (!comp)
            {
                logger.trace("DirectWrite [{}]: vc=0x{:X} comp=NULL (+0x58)",
                             i, vc);
                continue;
            }
            auto descNode = read_ptr_unsafe(comp, 0x218);
            if (!descNode)
            {
                logger.trace("DirectWrite [{}]: vc=0x{:X} comp=0x{:X} "
                             "descNode=NULL (+0x218)", i, vc, comp);
                continue;
            }
            auto mapBase = descNode + 0x20;

            /* Choose the character-specific map for the iteration.
               charIdx == -1 routes to the active-character map so a
               slot we could not identify still cycles through the
               full active-character part list. */
            const auto *partMapPtr =
                (charIdx >= 0 && charIdx < static_cast<int>(kCharIdxCount))
                    ? &get_part_map_for(charIdx)
                    : &get_part_map();

            for (auto pmIt = partMapPtr->begin();
                 pmIt != partMapPtr->end(); ++pmIt)
            {
                const auto hash = pmIt->first;
                const auto mask = pmIt->second;

                auto entry = lookup(mapBase, &hash);
                if (!entry)
                    continue;

                const auto visAddr = entry + 0x20;
                const VisKey key{vc, visAddr};
                s_touchedVisKeys.push_back(key);
                auto *visPtr = reinterpret_cast<uint8_t *>(visAddr);

                /* Per-character hidden-state lookup: classify_part_for
                   has already produced `mask` from the per-character
                   map. is_any_category_hidden_for currently mirrors
                   the active-character helper because the Hidden /
                   Enabled toggles are global -- only the parts list
                   is per-character. Plumbed regardless so a future
                   per-character Hidden / Enabled overlay slots in
                   without re-touching this hot path. */
                if (is_any_category_hidden_for(mask, charIdx))
                {
                    if (origVis.find(key) == origVis.end())
                        origVis[key] = *visPtr;
                    *visPtr = 2;
                    ++hiddenCount;
                    logger.trace("  [{}] 0x{:04X} hidden (vis=2, char_idx={})",
                                 i, hash, charIdx);
                }
                else
                {
                    /* Always force vis=0 for visible parts. Cached
                       origVis values cannot be restored verbatim:
                       the engine writes its own state into this byte
                       (sample values 0xE6, 0xF6 observed in trace)
                       with the hidden-bit (0x02) set, so restoring
                       the cached value keeps the part hidden. */
                    *visPtr = 0;
                    auto it = origVis.find(key);
                    if (it != origVis.end())
                    {
                        logger.trace("  [{}] 0x{:04X} restored (vis=0, "
                                     "cached_orig=0x{:02X}, char_idx={})",
                                     i, hash, it->second, charIdx);
                        origVis.erase(it);
                    }
                    else
                    {
                        logger.trace("  [{}] 0x{:04X} force-shown (vis=0, char_idx={})",
                                     i, hash, charIdx);
                    }
                    ++restoredCount;
                }
            }
        }

        /* Orphan sweep, per-vis-ctrl edition.
           An entry in origVis is considered orphaned ONLY when its
           vis_ctrl matches a vis_ctrl that was processed this pass
           (i.e. that vis_ctrl is currently in ps.visCtrls) AND no
           (vis_ctrl, addr) pair we just touched matches the entry's
           key. Entries belonging to a vis_ctrl that is no longer in
           the active player set are untouched -- they will be cleaned
           up by cleanup_vis_bytes() at shutdown -- so a character swap
           does not strip the inactive character's hide state from the
           now-unwatched vis_ctrl. An unconditional sweep that ignored
           vis_ctrl identity would walk only the active character's
           part map, find every previously-active character's vis-byte
           address absent from the touched set, and restore them to
           visible -- silently undoing the prior character's hide
           state on every swap.

           Sort + binary_search on composite keys: lexicographic
           (vis_ctrl, addr) ordering keeps the active-vis-ctrl set
           clustered and matches the equality semantics of VisKey. */
        std::sort(s_touchedVisKeys.begin(), s_touchedVisKeys.end(),
                  vis_key_less);

        s_activeVisCtrls.fill(0);
        int activeCount = 0;
        for (int i = 0; i < n && activeCount < k_maxProtagonists; ++i)
        {
            auto vc = ps.visCtrls[i].load(std::memory_order_relaxed);
            if (vc)
                s_activeVisCtrls[activeCount++] = vc;
        }

        int orphanRestored = 0;
        for (auto it = origVis.begin(); it != origVis.end();)
        {
            const auto entryKey = it->first;

            bool vcIsActive = false;
            for (int j = 0; j < activeCount; ++j)
            {
                if (s_activeVisCtrls[j] == entryKey.visCtrl)
                {
                    vcIsActive = true;
                    break;
                }
            }
            if (!vcIsActive)
            {
                /* vis_ctrl is no longer tracked -- this entry's
                   character has been swapped out or the player set
                   changed shape entirely. Leave the vis byte alone so
                   the inactive character's hide state survives the
                   swap; cleanup_vis_bytes() at shutdown handles the
                   final restore. */
                ++it;
                continue;
            }
            if (std::binary_search(s_touchedVisKeys.begin(),
                                   s_touchedVisKeys.end(), entryKey,
                                   vis_key_less))
            {
                ++it;
                continue;
            }

            auto *visPtr = reinterpret_cast<uint8_t *>(entryKey.addr);
            // Cached origVis values can carry the engine's own
            // hidden-bit (0x02). Restoring verbatim keeps the part
            // hidden, so write a literal 0 (same reasoning as the
            // main restore branch above).
            *visPtr = 0;
            it = origVis.erase(it);
            ++orphanRestored;
        }
        if (orphanRestored > 0)
            logger.debug(
                "DirectWrite: {} orphan vis bytes restored "
                "(category change for active vis ctrls)",
                orphanRestored);
        restoredCount += orphanRestored;

        logger.trace("DirectWrite: {} protagonists, {} hidden, {} restored",
                     n, hiddenCount, restoredCount);
    }

    void apply_direct_vis_write() noexcept
    {
        auto &addrs = resolved_addrs();
        if (!addrs.mapLookup)
            return;
        /* Manual lock/unlock: MSVC SEH does not run C++ destructors on
           unwind under /EHsc, so an RAII lock would stay held after a
           caught fault. */
        auto &mtx = vis_write_mutex();
        if (!mtx.try_lock())
        {
            /* Lost the lock race against another writer (mid-hook,
               resolve poll, or a second input-thread tick). Republish
               the work-pending signal so the mid-hook re-runs us on
               the next game frame; without this the toggle that
               triggered this call would be silently dropped and the
               user-visible vis byte would not flip. */
            needs_direct_write().store(true, std::memory_order_release);
            DMK::Logger::get_instance().trace(
                "DirectWrite: try_lock failed, deferred to mid-hook");
            return;
        }

        s_touchedVisKeys.clear();

        __try
        {
            apply_direct_vis_write_impl();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            static std::atomic<bool> s_crashLogged{false};
            if (!s_crashLogged.exchange(true, std::memory_order_relaxed))
                DMK::Logger::get_instance().warning("DirectWrite: SEH caught crash");
        }
        mtx.unlock();
    }

    /* Implementation body for cleanup_vis_bytes(): structured binding on
       map<VisKey, ...> creates the same hidden-temporary issue
       apply_direct_vis_write_impl works around. Same _impl pattern
       keeps the SEH wrapper free of unwind state. */
    static void cleanup_vis_bytes_impl() noexcept
    {
        auto &origVis = original_vis_map();
        int restoredCount = 0;
        for (auto it = origVis.begin(); it != origVis.end(); ++it)
        {
            auto *visPtr = reinterpret_cast<uint8_t *>(it->first.addr);
            *visPtr = it->second;
            ++restoredCount;
        }
        origVis.clear();

        DMK::Logger::get_instance().debug(
            "Cleanup: {} vis bytes restored", restoredCount);
    }

    void cleanup_vis_bytes() noexcept
    {
        auto &mtx = vis_write_mutex();
        mtx.lock();

        __try
        {
            cleanup_vis_bytes_impl();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        mtx.unlock();
    }

} // namespace EquipHide
