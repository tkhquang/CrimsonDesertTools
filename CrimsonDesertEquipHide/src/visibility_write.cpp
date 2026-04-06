#include "visibility_write.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

namespace EquipHide
{
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

                auto comp = read_ptr_unsafe(vc, 0x48);
                if (!comp)
                    continue;
                auto descNode = read_ptr_unsafe(comp, 0x218);
                if (!descNode)
                    continue;
                auto mapBase = descNode + 0x20;

                for (const auto &[hash, mask] : get_part_map())
                {
                    auto entry = lookup(mapBase, &hash);
                    if (!entry)
                        continue;

                    const auto visAddr = entry + 0x1C;
                    auto *visPtr = reinterpret_cast<uint8_t *>(visAddr);

                    if (is_any_category_hidden(mask))
                    {
                        if (origVis.find(visAddr) == origVis.end())
                            origVis[visAddr] = *visPtr;
                        // Don't overwrite vis=3 lock — cascade workaround
                        // already holds this part in the skipped state.
                        if (!(flag_cascade_fix().load(std::memory_order_relaxed) &&
                              *visPtr == 3))
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
