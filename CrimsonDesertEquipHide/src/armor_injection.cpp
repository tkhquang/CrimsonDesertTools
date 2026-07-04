#include "armor_injection.hpp"
#include "categories.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <format>
#include <string>
#include <vector>

namespace EquipHide
{
    /* File-scope scratch buffers reused across inject_armor_entries_for_map calls. Cleared at function entry.
       Serialised by vis_write_mutex which is held for the full call duration, so concurrent callers cannot race. File
       scope (rather than function-scope std::vector / std::string) is required by MSVC C2712 -- the function's
       __try/__finally cannot coexist with objects that require C++ unwinding, and that includes temporaries returned
       by-value from helpers. Mirrors the s_touchedVisKeys pattern in visibility_write.cpp. */
    static std::vector<uint32_t> s_v_injected;
    static std::vector<uint32_t> s_v_reinjected;
    static std::string s_v_joined;

    /* Writes a comma-separated "0x..., 0x..., ..." rendering of @p v into the file-scope @ref s_v_joined buffer. Used
       by the per-call summary emit; lives at file scope (not as a function-local lambda) because returning std::string
       by value would put a destructor-bearing temporary inside __try and re-trigger C2712. */
    static void build_hex_joined(const std::vector<uint32_t> &v)
    {
        s_v_joined.clear();
        s_v_joined.reserve(v.size() * 8);
        for (std::size_t k = 0; k < v.size(); ++k)
        {
            if (k > 0)
                s_v_joined += ", ";
            s_v_joined += std::format("0x{:X}", v[k]);
        }
    }

    /**
     * @brief Game's map insertion function signature.
     *
     * Entry data struct (29 bytes):
     *   +0x00  3 x byte flags + padding = 0
     *   +0x04  5 x dword socket bones   = 0
     *   +0x1C  byte Visible             = 2 (Out-only)
     */
    using MapInsertFn = __int64 *(__fastcall *)(unsigned int *map_base, int **part_hash_pp, unsigned int bucket_key,
                                                __int64 entry_data, int extra, uint8_t *out_existed,
                                                __int64 *out_hash_ptr, __int64 *out_data_ptr);

    // Reject a mapBase that passed the plausible-pointer gate but does not look like a real part-visibility hashtable.
    // A stale/reallocated vis-ctrl descriptor (e.g. a companion despawned between the resolve pass and this write) can
    // yield a garbage map -- an oversized count, a bucket pointer that lands in the image .rdata instead of the heap --
    // and inserting into it faults inside the game's MapInsert (SEH would catch it, but noisily, every frame). The game
    // reads [mapBase+0] as the bucket modulus, [mapBase+4] as capacity, [mapBase+0x10] as the bucket array (see
    // MapLookup); part-vis maps are tiny per-character tables. Introduced with the v1.13.00 vis-ctrl chain drift.
    static bool part_vis_map_looks_valid(std::uintptr_t mapBase) noexcept
    {
        // Mirror exactly the fields the game's MapInsert dereferences (verified from its disassembly): bucket modulus
        // [+0], live entry count [+4], capacity [+8], bucket array [+0x10], entry-pointer array [+0x18]. MapInsert
        // *writes* through [+0x18][entryCount], so a bad entry-pointer array (or a garbage capacity that skips the grow
        // path) faults inside the game. A stale/reallocated descriptor -- or a wrong map offset after a struct
        // re-layout -- fails one of these. Part-vis maps are tiny per-character hashtables.
        const auto count = DMKMemory::seh_read<std::uint32_t>(mapBase);
        const auto entryCount = DMKMemory::seh_read<std::uint32_t>(mapBase + 4);
        const auto cap = DMKMemory::seh_read<std::uint32_t>(mapBase + 8);
        const auto buckets = DMKMemory::seh_read<std::uintptr_t>(mapBase + 0x10);
        const auto entryPtrs = DMKMemory::seh_read<std::uintptr_t>(mapBase + 0x18);
        if (!count || !entryCount || !cap || !buckets || !entryPtrs)
            return false;
        if (*count == 0 || *count > 0x400)
            return false;
        if (*cap == 0 || *cap > 0x10000 || *entryCount > *cap)
            return false;
        if (!DMKMemory::plausible_userspace_ptr(*buckets) || !DMKMemory::plausible_userspace_ptr(*entryPtrs))
            return false;
        // Both arrays MapInsert will index/write must be readable.
        return DMKMemory::seh_read<std::uint32_t>(*buckets).has_value() &&
               DMKMemory::seh_read<std::uintptr_t>(*entryPtrs).has_value();
    }

    static uint32_t compute_bucket_key(uint32_t partHash) noexcept
    {
        __try
        {
            auto globalAddr = resolved_addrs().indexedStringGlobal;
            if (!globalAddr)
                return 0;

            // Walk globalAddr -> [+0] -> [+0x58] to the bucket table.
            // The trailing 0 dereferences the +0x58 link so the result is the table pointer itself; without it the
            // chain would stop at the slot address and corrupt every bucket key.
            auto tbl = DMKMemory::seh_resolve_chain(globalAddr, {0x00, 0x58, 0x00});
            if (!tbl)
            {
                static std::atomic<bool> s_logOnce{false};
                if (!s_logOnce.exchange(true, std::memory_order_relaxed))
                    DMK::Logger::get_instance().warning("compute_bucket_key: tablePtr=NULL "
                                                        "(globalAddr=0x{:X} +0x58)",
                                                        globalAddr);
                return 0;
            }

            // 16-byte stride table indexed by part hash; the bucket key lives at +8 within the entry. Kept as a
            // separate typed read (not a chain offset) so the indexed arithmetic stays explicit.
            return DMKMemory::seh_read<uint32_t>(*tbl + 16ULL * partHash + 8).value_or(0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            static std::atomic<bool> s_logOnce{false};
            if (!s_logOnce.exchange(true, std::memory_order_relaxed))
                DMK::Logger::get_instance().warning("compute_bucket_key: SEH fault for hash 0x{:04X}", partHash);
            return 0;
        }
    }

    static int inject_armor_entries_for_map(uintptr_t mapBase, int charIdx) noexcept
    {
        auto &mtx = vis_write_mutex();
        if (!mtx.try_lock())
            return 0;

        /* __try/__finally guarantees mtx.unlock() on every exit path.
           Required because MSVC /EHsc does NOT route C++ exceptions
           (logger format error, std::bad_alloc, etc.) through the SEH
           __except below, so without __finally a thrown C++ exception
           would leak the lock and stall every subsequent ArmorInject
           + DirectWrite call. __leave skips to __finally when the
           addrs guard short-circuits. */
        int result = 0;
        // Accumulators are the file-scope statics declared near the top of this TU (see the rationale comment there for
        // why they cannot be function-locals). Cleared on entry under the lock.
        s_v_injected.clear();
        s_v_reinjected.clear();
        __try
        {
            __try
            {
                auto &addrs = resolved_addrs();
                if (!addrs.mapInsert || !addrs.mapLookup)
                    __leave;

                auto insert = reinterpret_cast<MapInsertFn>(addrs.mapInsert);
                auto lookup = reinterpret_cast<MapLookupFn>(addrs.mapLookup);

                auto &logger = DMK::Logger::get_instance();
                int injected = 0;
                int existing_set = 0;
                int skipped_key = 0;

                const bool cascadeOn = flag_cascade_fix().load(std::memory_order_relaxed);

                // Legs, gloves, boots share a ConditionalPartPrefab cascade with chest -- hiding chest deletes their
                // map entries. Injecting vis=0 for visible body parts keeps them alive.
                constexpr CategoryMask k_cascadeBodyMask =
                    category_bit(Category::Legs) | category_bit(Category::Gloves) | category_bit(Category::Boots);

                // Per-character map drives both the part list AND the hide-mask classification: charIdx=-1 (unknown
                // body or fallback path) collapses to the active-character map through get_part_map_for /
                // is_any_category_hidden_for so single-character behaviour is preserved for unidentified slots.
                const auto &partMap = (charIdx >= 0 && charIdx < static_cast<int>(k_charIdxCount))
                                          ? get_part_map_for(charIdx)
                                          : get_part_map();

                int reinjected = 0;

                for (const auto &[hash, mask] : partMap)
                {
                    const bool hidden = is_any_category_hidden_for(mask, charIdx);
                    auto existing = lookup(mapBase, &hash);

                    if (!hidden)
                    {
                        /* Toggle-off cache flush. The engine caches a hidden render-state when our inject inserts an
                           entry with vis=2; the cache is read from a struct field that direct vis-byte writes do not
                           reach, so body armor stays visually hidden after toggle-off unless we re-insert with vis=0.
                           The re-insert path below forces the engine to re-process the entry and clear its cached
                           hidden state. Visible parts with no existing entry have nothing to flush, so skip unless the
                           cascade-fix path needs them. */
                        if (!existing)
                        {
                            if (!cascadeOn || (mask & k_cascadeBodyMask) == 0)
                                continue;
                        }
                    }
                    else if (existing)
                    {
                        /* Hidden category, entry already present: vis byte is updated in-place by the direct-write
                           path; no re-insert needed. */
                        ++existing_set;
                        continue;
                    }

                    auto bucketKey = compute_bucket_key(hash);
                    if (bucketKey == 0)
                    {
                        logger.trace("  0x{:X} -- skipped (no bucket key)", hash);
                        ++skipped_key;
                        continue;
                    }

                    alignas(8) uint8_t entryData[32] = {};
                    entryData[0x1C] = hidden ? 2 : 0;

                    uint32_t hashCopy = hash;
                    int *hashPtr = reinterpret_cast<int *>(&hashCopy);
                    uint8_t outExisted = 0;
                    __int64 outHashPtr = 0;
                    __int64 outDataPtr = 0;

                    auto *mapBasePtr = reinterpret_cast<unsigned int *>(mapBase);

                    insert(mapBasePtr, &hashPtr, bucketKey, reinterpret_cast<__int64>(entryData), 0, &outExisted,
                           &outHashPtr, &outDataPtr);

                    if (!outExisted)
                    {
                        s_v_injected.push_back(hash);
                        ++injected;
                    }
                    else if (!hidden)
                    {
                        s_v_reinjected.push_back(hash);
                        ++reinjected;
                    }
                }

                if (!s_v_injected.empty() && logger.is_enabled(DMK::LogLevel::Debug))
                {
                    build_hex_joined(s_v_injected);
                    logger.debug("  injected new ({}): {}", s_v_injected.size(), s_v_joined);
                }
                if (!s_v_reinjected.empty() && logger.is_enabled(DMK::LogLevel::Trace))
                {
                    build_hex_joined(s_v_reinjected);
                    logger.trace("  re-injected visible cache-flush ({}): {}", s_v_reinjected.size(), s_v_joined);
                }

                logger.debug("ArmorInject map: {} injected, {} existing updated, "
                             "{} re-injected, {} skipped (no bucket key)",
                             injected, existing_set, reinjected, skipped_key);
                result = injected;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                static std::atomic<bool> s_crashLogged{false};
                if (!s_crashLogged.exchange(true, std::memory_order_relaxed))
                    DMK::Logger::get_instance().warning("ArmorInject: SEH caught crash during map insertion");
                result = -1;
            }
        }
        __finally
        {
            mtx.unlock();
        }
        return result;
    }

    void inject_armor_entries() noexcept
    {
        auto &addrs = resolved_addrs();
        if (!addrs.mapInsert || !addrs.mapLookup || !addrs.indexedStringGlobal)
            return;

        // Hidden-state masks (used by is_any_category_hidden_for) are global, so the global anyHidden short-circuit is
        // still correct: if no category is hidden anywhere, every per-character query also returns false. The
        // cascade-fix injection path also writes vis=0 when no category is hidden, which we intentionally still skip
        // here -- it would re-fire on every tick in the no-hidden case.
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
            /* Reset injection flags so the next hide toggle will re-update existing entries (setting their vis bytes
               back to 2). */
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

            // Per-slot character idx. -1 (unknown / fallback path) routes injection through the active-character map so
            // unidentified slots preserve single-character behaviour.
            const int charIdx = ps.visCharIdx[i].load(std::memory_order_relaxed);

            /* Per-player SEH so one bad pointer does not skip the rest. */
            __try
            {
                // Resolve the part-info descriptor and its part-visibility map. Two v1.13.00 drifts, both live-verified
                // against two protagonists: (1) the vis-ctrl -> ClientCharacterControlActorComponent (CCC) link moved
                // +0x30 (0x58 -> 0x88, RTTI-confirmed); (2) the descriptor's part-vis map moved descriptor+0x20 ->
                // descriptor+0x28 (the game's "PartInOutSocket" parser, sub_141FAA040 equiv, now emits
                // `lea rcx,[r14+0x28]` at its MapInsert call site vs `a1+0x20` on v1.05). The CCC -> descriptor link is
                // UNCHANGED at CCC+0x218. So: desc = *(*(vc+0x88)+0x218); mapBase = desc + 0x28. seh_read_chain derefs
                // the terminal +0x218 link, so `*desc` (the optional unwrap) IS the descriptor pointer.
                auto desc = DMKMemory::seh_read_chain<std::uintptr_t>(vc, {0x88, 0x218});
                if (!desc)
                {
                    logger.trace("ArmorInject [{}]: vc=0x{:X} descriptor=NULL "
                                 "(+0x88 -> +0x218)",
                                 i, vc);
                    continue;
                }
                auto mapBase = *desc + 0x28;

                // Reject a non-faulting garbage mapBase from a drifted +0x88 / +0x218 chain (the SEH read only traps an
                // actual fault, not a wrong-but-mapped pointer). Skip this vis-controller rather than inject into a
                // wrong map.
                if (!DMKMemory::plausible_userspace_ptr(mapBase))
                {
                    logger.trace("ArmorInject [{}]: vc=0x{:X} implausible mapBase=0x{:X} "
                                 "(+0x88 -> +0x218 -> +0x28)",
                                 i, vc, mapBase);
                    continue;
                }

                if (!part_vis_map_looks_valid(mapBase))
                {
                    logger.trace("ArmorInject [{}]: vc=0x{:X} mapBase=0x{:X} not a valid part-vis map "
                                 "(stale/reallocated descriptor) -- skipping",
                                 i, vc, mapBase);
                    continue;
                }

                logger.trace("ArmorInject [{}]: vc=0x{:X} descriptor=0x{:X} "
                             "mapBase=0x{:X} char_idx={}",
                             i, vc, *desc, mapBase, charIdx);

                int result = inject_armor_entries_for_map(mapBase, charIdx);
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
            logger.info("ArmorInject: {} new entries injected across {} protagonists", totalInjected, n);
        else
            logger.debug("ArmorInject: 0 new entries (all existed or no hidden parts)");
    }

} // namespace EquipHide
