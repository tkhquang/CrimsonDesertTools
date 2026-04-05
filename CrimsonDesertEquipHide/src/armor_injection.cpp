#include "armor_injection.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

namespace EquipHide
{
    /**
     * @brief Game's map insertion function signature.
     *
     * Entry data struct (29 bytes):
     *   +0x00  3 x byte flags + padding = 0
     *   +0x04  5 x dword socket bones   = 0
     *   +0x1C  byte Visible             = 2 (Out-only)
     */
    using MapInsertFn = __int64 *(__fastcall *)(unsigned int *map_base,
                                                int **part_hash_pp,
                                                unsigned int bucket_key,
                                                __int64 entry_data,
                                                int extra,
                                                uint8_t *out_existed,
                                                __int64 *out_hash_ptr,
                                                __int64 *out_data_ptr);

    static uint32_t compute_bucket_key(uint32_t partHash) noexcept
    {
        __try
        {
            auto globalAddr = resolved_addrs().indexedStringGlobal;
            if (!globalAddr)
                return 0;
            auto globalPtr = *reinterpret_cast<const uintptr_t *>(globalAddr);
            if (globalPtr < 0x10000)
                return 0;
            auto tablePtr = *reinterpret_cast<const uintptr_t *>(globalPtr + 0x58);
            if (tablePtr < 0x10000)
                return 0;
            return *reinterpret_cast<const uint32_t *>(
                tablePtr + 16ULL * partHash + 8);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static int inject_armor_entries_for_map(uintptr_t mapBase) noexcept
    {
        auto &mtx = vis_write_mutex();
        if (!mtx.try_lock())
            return 0;

        int result = 0;
        __try
        {
            auto &addrs = resolved_addrs();
            if (!addrs.mapInsert || !addrs.mapLookup)
            {
                mtx.unlock();
                return 0;
            }

            auto insert = reinterpret_cast<MapInsertFn>(addrs.mapInsert);
            auto lookup = reinterpret_cast<MapLookupFn>(addrs.mapLookup);

            auto &logger = DMK::Logger::get_instance();
            int injected = 0;
            int existing_set = 0;
            int skipped_key = 0;
            const bool officialActive = official_helm_active();

            for (const auto &[hash, rawMask] : get_part_map())
            {
                auto effectiveMask = rawMask;
                if (officialActive)
                    effectiveMask &= ~k_officialManagedMask;
                if (!is_any_category_hidden(effectiveMask))
                    continue;

                auto existing = lookup(mapBase, &hash);
                if (existing)
                {
                    ++existing_set;
                    continue;
                }

                auto bucketKey = compute_bucket_key(hash);
                if (bucketKey == 0)
                {
                    logger.trace("  0x{:X} — skipped (no bucket key)", hash);
                    ++skipped_key;
                    continue;
                }

                alignas(8) uint8_t entryData[32] = {};
                entryData[0x1C] = 2; // Visible = Out-only

                uint32_t hashCopy = hash;
                int *hashPtr = reinterpret_cast<int *>(&hashCopy);
                uint8_t outExisted = 0;
                __int64 outHashPtr = 0;
                __int64 outDataPtr = 0;

                auto *mapBasePtr = reinterpret_cast<unsigned int *>(mapBase);

                insert(
                    mapBasePtr,
                    &hashPtr,
                    bucketKey,
                    reinterpret_cast<__int64>(entryData),
                    0,
                    &outExisted,
                    &outHashPtr,
                    &outDataPtr);

                if (!outExisted)
                {
                    logger.debug("  0x{:X} — INJECTED new entry", hash);
                    ++injected;
                }
            }

            logger.debug("ArmorInject map: {} injected, {} existing updated, "
                         "{} skipped (no bucket key)",
                         injected, existing_set, skipped_key);
            result = injected;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            static std::atomic<bool> s_crashLogged{false};
            if (!s_crashLogged.exchange(true, std::memory_order_relaxed))
                DMK::Logger::get_instance().warning(
                    "ArmorInject: SEH caught crash during map insertion");
            result = -1;
        }
        mtx.unlock();
        return result;
    }

    void inject_armor_entries() noexcept
    {
        auto &addrs = resolved_addrs();
        if (!addrs.mapInsert || !addrs.mapLookup || !addrs.indexedStringGlobal)
            return;

        bool anyHidden = false;
        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            if (is_category_hidden(static_cast<Category>(i)))
            {
                anyHidden = true;
                break;
            }
        }

        auto &ps = player_state();
        if (!anyHidden)
        {
            /* Reset injection flags so the next hide toggle will re-update
               existing entries (setting their vis bytes back to 2). */
            for (int i = 0; i < k_maxProtagonists; ++i)
                ps.armorInjected[i].store(false, std::memory_order_relaxed);
            return;
        }

        const auto n = ps.count.load(std::memory_order_relaxed);
        if (n <= 0)
            return;

        auto &logger = DMK::Logger::get_instance();
        int totalInjected = 0;

        for (int i = 0; i < n; ++i)
        {
            if (ps.armorInjected[i].load(std::memory_order_relaxed))
                continue;

            auto vc = ps.visCtrls[i].load(std::memory_order_relaxed);
            if (!vc)
                continue;

            /* Per-player SEH so one bad pointer does not skip the rest. */
            __try
            {
                auto comp = read_ptr_unsafe(vc, 0x48);
                if (!comp)
                    continue;
                auto descNode = read_ptr_unsafe(comp, 0x218);
                if (!descNode)
                    continue;
                auto mapBase = descNode + 0x20;

                int result = inject_armor_entries_for_map(mapBase);
                if (result >= 0)
                {
                    ps.armorInjected[i].store(true, std::memory_order_relaxed);
                    totalInjected += result;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        if (totalInjected > 0)
            logger.info("ArmorInject: {} new entries injected across {} protagonists",
                        totalInjected, n);
        else
            logger.debug("ArmorInject: 0 new entries (all existed or no hidden parts)");
    }

} // namespace EquipHide
