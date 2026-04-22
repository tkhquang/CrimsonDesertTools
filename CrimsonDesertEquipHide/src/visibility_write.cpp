#include "visibility_write.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <algorithm>
#include <vector>

namespace EquipHide
{
    /* File-scope scratch buffer reused across apply_direct_vis_write calls.
       Populated under vis_write_mutex (held for the full duration of the
       function) so concurrent callers never race. File scope rather than a
       local inside the __try block both avoids MSVC C2712 (no
       non-trivially-destructible object sharing scope with __try) and
       keeps the stack small for game threads with tight stack reserves. */
    static std::vector<std::uintptr_t> s_touchedVisAddrs;

    void apply_direct_vis_write() noexcept
    {
        auto &addrs = resolved_addrs();
        if (!addrs.mapLookup)
            return;
        /* Manual lock/unlock: MSVC SEH does not run C++ destructors on unwind
           under /EHsc, so an RAII lock would stay held after a caught fault. */
        auto &mtx = vis_write_mutex();
        if (!mtx.try_lock())
            return;

        s_touchedVisAddrs.clear();

        __try
        {
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

                for (const auto &[hash, mask] : get_part_map())
                {
                    auto entry = lookup(mapBase, &hash);
                    if (!entry)
                        continue;

                    const auto visAddr = entry + 0x1C;
                    s_touchedVisAddrs.push_back(visAddr);
                    auto *visPtr = reinterpret_cast<uint8_t *>(visAddr);

                    if (is_any_category_hidden(mask))
                    {
                        if (origVis.find(visAddr) == origVis.end())
                            origVis[visAddr] = *visPtr;
                        *visPtr = 2;
                        ++hiddenCount;
                        logger.trace("  [{}] 0x{:04X} hidden (vis=2)",
                                     i, hash);
                    }
                    else
                    {
                        auto it = origVis.find(visAddr);
                        if (it != origVis.end())
                        {
                            const auto restored = (it->second == 2) ? 0 : it->second;
                            *visPtr = static_cast<uint8_t>(restored);
                            logger.trace("  [{}] 0x{:04X} restored (vis={})",
                                         i, hash, restored);
                            origVis.erase(it);
                            ++restoredCount;
                        }
                        else if (flag_force_show().load(std::memory_order_relaxed))
                        {
                            *visPtr = 0;
                            ++restoredCount;
                            logger.trace("  [{}] 0x{:04X} force-shown (vis=0)",
                                         i, hash);
                        }
                    }
                }
            }

            /* Orphan sweep. An entry lands here when its hash was in the
               active character's hidden category at a previous apply pass
               (so we wrote vis=2 and cached the original in origVis) but
               is no longer in the part map for the current character (or
               its category stopped being registered). The main loop above
               iterates only the active character's map, so those orphan
               entries would otherwise retain vis=2 indefinitely. Since
               part-mesh names are shared across protagonists (e.g.
               CD_Upperbody / CD_Vest exist on every humanoid), the
               leaked vis=2 write from the previous character causes the
               matching mesh on the new character to render hidden until
               shutdown triggers cleanup_vis_bytes().

               The touched-address vector is sorted once (cheap: at most a
               few hundred entries) and orphan membership is resolved via
               binary_search, giving O((|orig| + |touched|) * log(|touched|))
               for the sweep. Restored bytes use the same (origVal == 2 ?
               0 : origVal) mapping the in-map restore branch uses, so the
               resulting state is indistinguishable from the path a
               same-character hide-then-show would have taken. */
            std::sort(s_touchedVisAddrs.begin(), s_touchedVisAddrs.end());
            int orphanRestored = 0;
            for (auto it = origVis.begin(); it != origVis.end();)
            {
                const auto visAddr = it->first;
                if (std::binary_search(s_touchedVisAddrs.begin(),
                                       s_touchedVisAddrs.end(), visAddr))
                {
                    ++it;
                    continue;
                }
                auto *visPtr = reinterpret_cast<uint8_t *>(visAddr);
                const auto origVal = it->second;
                const auto restored = (origVal == 2) ? 0 : origVal;
                *visPtr = static_cast<uint8_t>(restored);
                it = origVis.erase(it);
                ++orphanRestored;
            }
            if (orphanRestored > 0)
                logger.debug(
                    "DirectWrite: {} orphan vis bytes restored "
                    "(category or character-swap sweep)",
                    orphanRestored);
            restoredCount += orphanRestored;

            logger.trace("DirectWrite: {} protagonists, {} hidden, {} restored",
                         n, hiddenCount, restoredCount);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            static std::atomic<bool> s_crashLogged{false};
            if (!s_crashLogged.exchange(true, std::memory_order_relaxed))
                DMK::Logger::get_instance().warning("DirectWrite: SEH caught crash");
        }
        mtx.unlock();
    }

    void cleanup_vis_bytes() noexcept
    {
        auto &mtx = vis_write_mutex();
        mtx.lock();

        __try
        {
            auto &origVis = original_vis_map();
            int restoredCount = 0;
            for (const auto &[visAddr, origVal] : origVis)
            {
                auto *visPtr = reinterpret_cast<uint8_t *>(visAddr);
                *visPtr = origVal;
                ++restoredCount;
            }
            origVis.clear();

            DMK::Logger::get_instance().debug(
                "Cleanup: {} vis bytes restored", restoredCount);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        mtx.unlock();
    }

} // namespace EquipHide
