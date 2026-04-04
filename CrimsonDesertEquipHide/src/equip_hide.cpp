#include "equip_hide.hpp"
#include "categories.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace EquipHide
{
    /**
     * @brief Hook target: sub_14081D3C0 — PartInOut transition function.
     *
     * Hook point: movzx eax, byte ptr [r13+1Ch]; cmp al, 3
     *
     * Register layout at hook point:
     *   R10 = pointer to part hash DWORD (IndexedStringA ID)
     *   R13 = pointer to PartInOutSocket struct (Visible byte at +0x1C)
     *   R8B = exclusion-list flag
     *   [RBP+0x67] = a4 (transition type byte, saved from R9B at prologue)
     *
     * Visibility byte values: 0=both, 1=In-only, 2=Out-only, 3=skip.
     * To hide: set [R13+0x1C]=2 and force a4=2 when a4==1.
     *
     * Transition types (a4): 0=stow, 1=draw, 2=skip.
     *
     * Cascading AOB patterns (tried in order until one matches):
     *   P1: Direct hook-site pattern (most precise, least resilient)
     *   P2: Wider context pattern (survives register reallocation, +0x36 offset)
     *   P3: Short core pattern (last resort, 7 bytes)
     *
     * See .idea/research/update_resilience.md for full analysis.
     */

    static std::atomic<bool> s_playerOnly{true};
    static std::atomic<bool> s_forceShow{false};
    static std::atomic<bool> s_baldFix{true};
    static std::atomic<bool> s_glidingFix{true};

    static constexpr int k_maxProtagonists = 8;
    static std::atomic<uintptr_t> s_playerVisCtrls[k_maxProtagonists]{};
    static std::atomic<int> s_playerCount{0};

    static std::atomic<uint32_t> s_resolveCounter{0};
    static constexpr uint32_t k_resolveInterval = 512;

    /* Cached primary player vis_ctrl for fast single-compare.
       For the common single-protagonist case, replaces the slot array loop. */
    static std::atomic<uintptr_t> s_primaryPlayerVisCtrl{0};

    static uintptr_t s_worldSystemPtr = 0;
    static uintptr_t s_childActorVtbl = 0;
    static uintptr_t s_mapLookupAddr = 0;

    static uintptr_t s_mapInsertAddr = 0;                          // sub_1408228C0
    static uintptr_t s_indexedStringGlobalAddr = 0;                // qword_145BBAED8
    static std::atomic<bool> s_armorInjected[k_maxProtagonists]{}; // reset on vis ctrl change

    /* Inline hook trampoline for sub_14081DC20 — PartInOut direct-show.
       This function bypasses the vis check and calls sub_1425EB1E0 directly,
       causing hidden parts to flash during state transitions (e.g. gliding exit). */
    using PartAddShowFn = __int64(__fastcall *)(__int64, uint8_t, uint64_t,
                                                float, __int64, __int64,
                                                __int64, __int64, __int64);
    static PartAddShowFn s_originalPartAddShow = nullptr;

    /* Inline hook trampoline for sub_1423FDEB0 — Postfix rule evaluator.
       Evaluates whether a conditional part prefab postfix rule matches the
       currently equipped items. When Helm/Cloak is hidden by our mod, we
       suppress hair-hiding rules to prevent baldness. */
    using PostfixEvalFn = __int64(__fastcall *)(__int64, __int64);
    static PostfixEvalFn s_originalPostfixEval = nullptr;

    static std::atomic<bool> s_needsDirectWrite{false};

    /* Guarded by s_directWriteMtx — accessed from hook thread (s_needsDirectWrite)
       and input thread (flush_visibility). */
    static std::mutex s_directWriteMtx;
    static std::unordered_map<uintptr_t, uint8_t> s_originalVis;

    static DMK::Config::KeyComboList s_showAllCombos;
    static DMK::Config::KeyComboList s_hideAllCombos;

    static std::atomic<bool> s_fallbackMode{false};        // d8-based fallback when global chain AOBs fail
    static std::atomic<bool> s_shutdownRequested{false};   // signals all background threads to exit
    static std::atomic<bool> s_deferredScanPending{false}; // set when IndexedStringA table not ready at init
    static std::atomic<bool> s_lazyProbePending{false};    // set when deferred scan finishes with unresolved parts
    static std::atomic<int64_t> s_lazyProbeSignal{0};

    static std::mutex s_bgThreadMtx;
    static std::thread s_deferredScanThread;
    static std::thread s_lazyProbeThread;

    static uintptr_t s_prevVisCtrls[k_maxProtagonists]{};
    static int s_prevCount = 0;

    static int64_t steady_ms() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    /** @brief Unsafe pointer read — use ONLY inside SEH-protected hot paths. */
    static uintptr_t read_ptr_unsafe(uintptr_t base, ptrdiff_t off) noexcept
    {
        auto addr = *reinterpret_cast<const uintptr_t *>(base + off);
        return (addr > 0x10000) ? addr : 0;
    }

    // --- AOB-based address resolution ---
    enum class ResolveMode : uint8_t
    {
        Direct,
        RipRelative
    };

    struct AddrCandidate
    {
        const char *name;
        const char *pattern;
        ResolveMode mode;
        ptrdiff_t dispOffset;
        ptrdiff_t instrEndOffset;
    };

    static uintptr_t resolve_match(uintptr_t match, const AddrCandidate &c) noexcept
    {
        if (c.mode == ResolveMode::Direct)
            return match + c.dispOffset;
        auto disp = *reinterpret_cast<const int32_t *>(match + c.dispOffset);
        return static_cast<uintptr_t>(
            static_cast<int64_t>(match + c.instrEndOffset) + disp);
    }

    static uintptr_t scan_for_address(
        const AddrCandidate *candidates, std::size_t count,
        const char *&matchedName)
    {
        auto &logger = DMK::Logger::get_instance();

        struct Compiled
        {
            const AddrCandidate *src;
            DMK::Scanner::CompiledPattern pat;
        };
        std::vector<Compiled> compiled;

        for (std::size_t i = 0; i < count; ++i)
        {
            auto p = DMK::Scanner::parse_aob(candidates[i].pattern);
            if (p)
                compiled.push_back({&candidates[i], std::move(*p)});
            else
                logger.warning("Failed to parse address AOB '{}'", candidates[i].name);
        }

        if (compiled.empty())
            return 0;

        const uint8_t *addr = nullptr;
        MEMORY_BASIC_INFORMATION mbi;

        while (VirtualQuery(addr, &mbi, sizeof(mbi)))
        {
            if (mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY)))
            {
                const auto regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);

                for (const auto &c : compiled)
                {
                    const auto *match = DMK::Scanner::find_pattern(
                        reinterpret_cast<const std::byte *>(regionBase),
                        mbi.RegionSize, c.pat);
                    if (match)
                    {
                        matchedName = c.src->name;
                        return resolve_match(
                            reinterpret_cast<uintptr_t>(match), *c.src);
                    }
                }
            }

            addr = static_cast<const uint8_t *>(mbi.BaseAddress) + mbi.RegionSize;
        }

        return 0;
    }

    static constexpr AddrCandidate k_worldSystemCandidates[] = {
        {"WS_P1_SmallFunc",
         "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 8B 49 50 E8 ?? ?? ?? ?? 84 C0 0F 94 C0 48 83 C4 28 C3",
         ResolveMode::RipRelative, 7, 11},

        {"WS_P2_StructField",
         "80 B8 49 01 00 00 00 75 ?? 48 8B 05 ?? ?? ?? ?? 48 8B 88 D8 00 00 00",
         ResolveMode::RipRelative, 12, 16},

        {"WS_P3_InnerLoad",
         "48 8B 0D ?? ?? ?? ?? 48 8B 49 50 E8 ?? ?? ?? ?? 84 C0 0F 94 C0",
         ResolveMode::RipRelative, 3, 7},
    };

    static constexpr AddrCandidate k_childActorVtblCandidates[] = {
        {"VT_P1_AllocCtor",
         "48 8B 55 08 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06 EB ??",
         ResolveMode::RipRelative, 16, 20},

        {"VT_P2_CtorStore",
         "48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06 EB ?? 4C",
         ResolveMode::RipRelative, 12, 16},

        {"VT_P3_WiderCtorStore",
         "45 31 ED 48 85 F6 74 ?? 48 8B 55 08 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05",
         ResolveMode::RipRelative, 24, 28},
    };

    static constexpr AddrCandidate k_mapLookupCandidates[] = {
        {"ML_P1_FullPrologue",
         "48 83 EC 08 83 79 04 00 4C 8B C1 75 ?? 33 C0 48 83 C4 08 C3 48 8B 05 ?? ?? ?? ?? 48 89 1C 24 8B 1A",
         ResolveMode::Direct, 0, 0},

        {"ML_P2_HashBody",
         "8B 48 58 48 03 D2 44 8B 5C D1 08 41 8B 08 85 C9 74 ?? 33 D2 41 8B C3 F7 F1",
         ResolveMode::Direct, -0x24, 0},

        {"ML_P3_HashLoop",
         "44 8B CA 33 D2 49 C1 E1 08 4D 03 48 10 45 8B 11 45 85 D2",
         ResolveMode::Direct, -0x3D, 0},
    };

    static constexpr AddrCandidate k_mapInsertCandidates[] = {
        {"MI_P1_FullPrologue",
         "4C 89 4C 24 20 53 55 56 57 41 54 41 55 48 83 EC 28 44 8B 11 48 8B D9 4D 8B E1 41 8B F0 4C 8B EA",
         ResolveMode::Direct, 0, 0},

        {"MI_P2_InnerBody",
         "44 8B 11 48 8B D9 4D 8B E1 41 8B F0 4C 8B EA",
         ResolveMode::Direct, -0x11, 0},

        {"MI_P3_PrologueBody",
         "53 55 56 57 41 54 41 55 48 83 EC 28 44 8B 11 48 8B D9 4D 8B E1 41 8B F0 4C 8B EA 41 8B CA 45 85 D2",
         ResolveMode::Direct, -5, 0},
    };

    static uintptr_t resolve_address(
        const AddrCandidate *candidates, std::size_t count,
        const char *label)
    {
        auto &logger = DMK::Logger::get_instance();
        const char *matchedName = nullptr;
        auto result = scan_for_address(candidates, count, matchedName);

        if (result)
        {
            logger.info("{} resolved via '{}' at 0x{:X}", label, matchedName, result);
            return result;
        }

        logger.warning("{} AOB scan failed — feature disabled", label);
        return 0;
    }

    /**
     * @brief IndexedStringA table scan.
     *
     * Table layout:
     *   globalPtr   = *(qword*)(mapLookupFunc + 20 + rip_disp)
     *   tableArray  = *(qword*)(globalPtr + 0x58)
     *   entry[hash] = tableArray + hash * 16
     *   entry[hash]+0 = pointer to null-terminated string (or 0)
     */
    static constexpr uint32_t k_tableScanMin = 0xAC00;
    static constexpr uint32_t k_tableScanMax = 0xCFFF;

    static constexpr std::size_t k_maxStringLen = 64;

    static std::size_t read_table_entry(uintptr_t tableArray, uint32_t hash,
                                        char *buf, std::size_t bufSize) noexcept
    {
        __try
        {
            const auto entryAddr = tableArray + static_cast<uintptr_t>(hash) * 16;
            const auto strPtr = *reinterpret_cast<const uintptr_t *>(entryAddr);
            if (strPtr < 0x10000)
                return 0;

            const auto *src = reinterpret_cast<const char *>(strPtr);
            std::size_t len = 0;
            while (len < bufSize - 1 && src[len] != '\0')
            {
                buf[len] = src[len];
                ++len;
            }
            buf[len] = '\0';
            return len;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static std::unordered_map<std::string, uint32_t> scan_indexed_string_table(
        uintptr_t mapLookupFunc)
    {
        auto &logger = DMK::Logger::get_instance();
        std::unordered_map<std::string, uint32_t> nameToHash;

        const auto ripInstr = mapLookupFunc + 20;
        const auto *instrBytes = reinterpret_cast<const uint8_t *>(ripInstr);

        if (instrBytes[0] != 0x48 || instrBytes[1] != 0x8B || instrBytes[2] != 0x05)
        {
            logger.warning("IndexedStringA scan: unexpected opcode at +20 "
                           "(expected 48 8B 05, got {:02X} {:02X} {:02X})",
                           instrBytes[0], instrBytes[1], instrBytes[2]);
            return nameToHash;
        }

        int32_t disp = 0;
        std::memcpy(&disp, instrBytes + 3, sizeof(int32_t));
        const auto instrEnd = ripInstr + 7;
        const auto globalPtrAddr = static_cast<uintptr_t>(
            static_cast<int64_t>(instrEnd) + disp);

        const auto globalPtr = *reinterpret_cast<const uintptr_t *>(globalPtrAddr);
        if (globalPtr < 0x10000)
        {
            logger.trace("IndexedStringA scan: global pointer not yet initialized (0x{:X})",
                         globalPtr);
            return nameToHash;
        }

        const auto tableArray = *reinterpret_cast<const uintptr_t *>(globalPtr + 0x58);
        if (tableArray < 0x10000)
        {
            logger.warning("IndexedStringA scan: tableArray is null/invalid (0x{:X})",
                           tableArray);
            return nameToHash;
        }

        logger.trace("IndexedStringA scan: globalPtr=0x{:X} tableArray=0x{:X} "
                     "range=0x{:X}-0x{:X}",
                     globalPtr, tableArray, k_tableScanMin, k_tableScanMax);

        const auto t0 = std::chrono::steady_clock::now();
        uint32_t cdEntries = 0;
        uint32_t probeHits = 0;

        char buf[k_maxStringLen + 1];

        for (uint32_t hash = k_tableScanMin; hash <= k_tableScanMax; ++hash)
        {
            auto len = read_table_entry(tableArray, hash, buf, sizeof(buf));
            if (len == 0 || len >= k_maxStringLen)
                continue;

            if (buf[0] != 'C' || buf[1] != 'D' || buf[2] != '_')
                continue;

            nameToHash[std::string(buf, len)] = hash;
            ++cdEntries;
        }

        auto unresolvedNames = get_unresolved_parts(nameToHash);
        if (!unresolvedNames.empty())
        {
            constexpr uint32_t k_wideScanMax = 0x20000;
            for (uint32_t h = 1; h < k_wideScanMax && !unresolvedNames.empty(); ++h)
            {
                if (h >= k_tableScanMin && h <= k_tableScanMax)
                    continue;

                auto len = read_table_entry(tableArray, h, buf, sizeof(buf));
                if (len == 0 || len >= k_maxStringLen)
                    continue;
                if (buf[0] != 'C' || buf[1] != 'D' || buf[2] != '_')
                    continue;

                for (auto it = unresolvedNames.begin();
                     it != unresolvedNames.end(); ++it)
                {
                    if (len == it->size() &&
                        std::memcmp(buf, it->c_str(), len) == 0)
                    {
                        nameToHash[*it] = h;
                        ++probeHits;
                        logger.trace("Wide scan: {} found at 0x{:X}", *it, h);
                        unresolvedNames.erase(it);
                        break;
                    }
                }
            }
        }

        const auto t1 = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        logger.trace("IndexedStringA scan complete: {} CD_ entries ({} range, {} probed) in {}ms",
                     cdEntries + probeHits, cdEntries, probeHits, ms);

        return nameToHash;
    }

    /** @brief Traverse body -> vis_ctrl pointer chain. Caller MUST be SEH-protected. */
    static uintptr_t body_to_vis_ctrl(uintptr_t body) noexcept
    {
        if (!body)
            return 0;
        auto inner = read_ptr_unsafe(body, 0x68);
        if (!inner)
            return 0;
        auto sub = read_ptr_unsafe(inner, 0x40);
        if (!sub)
            return 0;
        return read_ptr_unsafe(sub, 0xE8);
    }

    static void resolve_player_vis_ctrls() noexcept
    {
        if (!s_worldSystemPtr || !s_childActorVtbl)
            return;

        __try
        {
            /* read_ptr_unsafe: outer __try makes is_readable() redundant. */
            auto ws = read_ptr_unsafe(s_worldSystemPtr, 0);
            if (!ws)
                return;
            auto am = read_ptr_unsafe(ws, 0x30);
            if (!am)
                return;
            auto user = read_ptr_unsafe(am, 0x28);
            if (!user)
                return;

            static constexpr ptrdiff_t k_bodyOffsets[] = {
                0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8, 0x100, 0x108};

            int count = 0;
            for (auto off : k_bodyOffsets)
            {
                if (count >= k_maxProtagonists)
                    break;

                /* Per-slot SEH so one bad body pointer does not abort the loop. */
                __try
                {
                    auto candidate = read_ptr_unsafe(user, off);
                    if (!candidate)
                        continue;

                    auto vt = read_ptr_unsafe(candidate, 0);
                    if (vt != s_childActorVtbl)
                        continue;

                    auto vc = body_to_vis_ctrl(candidate);
                    if (vc)
                        s_playerVisCtrls[count++] = vc;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }

            for (int i = count; i < k_maxProtagonists; ++i)
                s_playerVisCtrls[i].store(0, std::memory_order_relaxed);

            s_primaryPlayerVisCtrl.store(
                count > 0 ? s_playerVisCtrls[0].load(std::memory_order_relaxed) : 0,
                std::memory_order_relaxed);

            for (int i = 0; i < k_maxProtagonists; ++i)
                s_armorInjected[i].store(false, std::memory_order_relaxed);

            s_playerCount.store(count, std::memory_order_relaxed);

            {
                static std::atomic<bool> s_logged{false};
                if (!s_logged.exchange(true, std::memory_order_relaxed))
                    DMK::Logger::get_instance().debug(
                        "Resolve: ws=0x{:X} am=0x{:X} user=0x{:X} count={}",
                        ws, am, user, count);
            }
            if (count > 0)
            {
                /* Non-blocking: skip if the input thread holds the mutex;
                   the next resolve cycle will catch any change. */
                if (s_directWriteMtx.try_lock())
                {
                    bool changed = (count != s_prevCount);
                    if (!changed)
                    {
                        for (int j = 0; j < count; ++j)
                        {
                            if (s_playerVisCtrls[j].load(std::memory_order_relaxed) != s_prevVisCtrls[j])
                            {
                                changed = true;
                                break;
                            }
                        }
                    }
                    if (changed)
                    {
                        s_prevCount = count;
                        for (int j = 0; j < count; ++j)
                            s_prevVisCtrls[j] = s_playerVisCtrls[j].load(std::memory_order_relaxed);
                        for (int j = 0; j < k_maxProtagonists; ++j)
                            s_armorInjected[j].store(false, std::memory_order_relaxed);
                        s_needsDirectWrite.store(true, std::memory_order_relaxed);
                        DMK::Logger::get_instance().debug(
                            "Player set changed — scheduling injection + direct write");
                    }
                    s_directWriteMtx.unlock();
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            static std::atomic<bool> s_crashLogged{false};
            if (!s_crashLogged.exchange(true, std::memory_order_relaxed))
                DMK::Logger::get_instance().warning("Resolve: SEH caught crash");
        }
    }

    struct AobCandidate
    {
        const char *name;
        const char *pattern;
        ptrdiff_t offsetToHook; // bytes from AOB match to the actual hook point
    };

    using MapLookupFn = uintptr_t(__fastcall *)(uintptr_t map_base, const uint32_t *key);

    static void apply_direct_vis_write() noexcept
    {
        if (!s_mapLookupAddr)
            return;
        /* Manual lock/unlock: MSVC SEH does not run C++ destructors on unwind
           under /EHsc, so an RAII lock would stay held after a caught fault. */
        if (!s_directWriteMtx.try_lock())
            return;

        __try
        {
            auto &logger = DMK::Logger::get_instance();
            auto lookup = reinterpret_cast<MapLookupFn>(s_mapLookupAddr);
            const auto n = s_playerCount.load(std::memory_order_relaxed);
            int hiddenCount = 0;
            int restoredCount = 0;

            for (int i = 0; i < n; ++i)
            {
                auto vc = s_playerVisCtrls[i].load(std::memory_order_relaxed);
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
                        if (s_originalVis.find(visAddr) == s_originalVis.end())
                            s_originalVis[visAddr] = *visPtr;
                        *visPtr = 2;
                        ++hiddenCount;
                        logger.trace("  [{}] 0x{:04X} hidden (vis=2)",
                                     i, hash);
                    }
                    else
                    {
                        auto it = s_originalVis.find(visAddr);
                        if (it != s_originalVis.end())
                        {
                            const auto restored = (it->second == 2) ? 0 : it->second;
                            *visPtr = static_cast<uint8_t>(restored);
                            logger.trace("  [{}] 0x{:04X} restored (vis={})",
                                         i, hash, restored);
                            s_originalVis.erase(it);
                            ++restoredCount;
                        }
                        else if (s_forceShow.load(std::memory_order_relaxed))
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
        s_directWriteMtx.unlock();
    }

    /**
     * @brief Armor entry injection into the PartInOutSocket hash map.
     *
     * Armor parts have no PartInOutSocket entries in vanilla. To hide them,
     * inject new entries with Visible=2 via the game's map insertion function.
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
            if (!s_indexedStringGlobalAddr)
                return 0;
            auto globalPtr = *reinterpret_cast<const uintptr_t *>(s_indexedStringGlobalAddr);
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
        if (!s_directWriteMtx.try_lock())
            return 0;

        int result = 0;
        __try
        {
            if (!s_mapInsertAddr || !s_mapLookupAddr)
            {
                s_directWriteMtx.unlock();
                return 0;
            }

            auto insert = reinterpret_cast<MapInsertFn>(s_mapInsertAddr);
            auto lookup = reinterpret_cast<MapLookupFn>(s_mapLookupAddr);

            auto &logger = DMK::Logger::get_instance();
            int injected = 0;
            int existing_set = 0;
            int skipped_key = 0;

            for (const auto &[hash, mask] : get_part_map())
            {
                if (!is_any_category_hidden(mask))
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
        s_directWriteMtx.unlock();
        return result;
    }

    static void inject_armor_entries() noexcept
    {
        if (!s_mapInsertAddr || !s_mapLookupAddr || !s_indexedStringGlobalAddr)
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
        if (!anyHidden)
        {
            /* Reset injection flags so the next hide toggle will re-update
               existing entries (setting their vis bytes back to 2). */
            for (int i = 0; i < k_maxProtagonists; ++i)
                s_armorInjected[i].store(false, std::memory_order_relaxed);
            return;
        }

        const auto n = s_playerCount.load(std::memory_order_relaxed);
        if (n <= 0)
            return;

        auto &logger = DMK::Logger::get_instance();
        int totalInjected = 0;

        for (int i = 0; i < n; ++i)
        {
            if (s_armorInjected[i].load(std::memory_order_relaxed))
                continue;

            auto vc = s_playerVisCtrls[i].load(std::memory_order_relaxed);
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
                    s_armorInjected[i].store(true, std::memory_order_relaxed);
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

    static constexpr AobCandidate k_aobCandidates[] = {
        {"P1_DirectSite",
         "41 0F B6 45 1C 3C 03 74 ?? 45 84 C0 75 ?? 84 C0",
         0},

        {"P2_WiderContext",
         "45 32 C0 48 8B 4D ?? 48 8B 41 38 8B 49 40 48 C1 E1 04 48 03 C8 48 3B C1 74 ?? 41 8B 12",
         0x36},

        {"P3_ShortCore",
         "41 0F B6 45 1C 3C 03",
         0},
    };

    /**
     * @brief Inline hook: PartInOut direct-show bypass (sub_14081DC20).
     *
     * Signature (x64 __fastcall):
     *   __int64 sub_14081DC20(
     *       __int64 a1,           // RCX  descriptor context
     *       char    a2,           // DL   transition flag
     *       uint64_t partHashPtr, // R8   pointer to DWORD part hash
     *       float   blend,        // XMM3 animation blend
     *       __int64 a5..a9)       // stack params
     */
    static constexpr AddrCandidate k_partAddShowCandidates[] = {
        {"PAS_P1_Prologue",
         "40 55 56 57 41 55 48 83 EC 48 48 8B 79 38",
         ResolveMode::Direct, 0, 0},

        // Post-prologue: sub rsp,48; mov rdi,[rcx+38]; mov r13,r8; mov r9d,[rcx+40]
        {"PAS_P2_PostPrologue",
         "48 83 EC 48 48 8B 79 38 4D 8B E8 44 8B 49 40",
         ResolveMode::Direct, -6, 0},

        // SIMD save + shift: movaps [rsp+30],xmm6; shl rax,04; movaps xmm6,xmm3; add rax,rdi
        {"PAS_P3_SimdBody",
         "0F 29 74 24 30 48 C1 E0 04 0F 28 F3 48 03 C7",
         ResolveMode::Direct, -0x1B, 0},
    };

    /**
     * @brief AOB candidates for sub_1423FDEB0 — Postfix rule evaluator.
     *
     * Virtual function at vtable[4] of objects with vtable 0x144CC8248.
     * Evaluates whether a postfix rule matches currently equipped items.
     * Returns 1 = rule matches (hide hair), 0 = no match (keep hair).
     *
     * Prologue: mov [rsp+8],rbx; mov [rsp+10],rbp; mov [rsp+18],rsi;
     *           push rdi; push r14; push r15
     */
    static constexpr AddrCandidate k_postfixEvalCandidates[] = {
        {"PFE_P1_PrologueAndBody",
         "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 "
         "48 83 EC 50 4C 8B FA 48 8B 5A 58 8B 42 60 48 8D 3C C3",
         ResolveMode::Direct, 0, 0},

        {"PFE_P2_UniqueBody",
         "48 83 EC 50 4C 8B FA 48 8B 5A 58 8B 42 60 48 8D 3C C3 48 3B",
         ResolveMode::Direct, -0x14, 0},

        {"PFE_P3_LoopInit",
         "45 33 F6 44 89 74 24 28 C7 44 24 2C 08 00 00 00 49 8B 5F 58 41 8B 47 60",
         ResolveMode::Direct, -0x6F, 0},
    };

    /**
     * @brief Hair-hiding suffix check for the BaldFix hook.
     *
     * ruleObj + 0x18 is a string handle (char**). Double-deref to get char*.
     * Hair-hiding suffixes: _a, _c, _d, _f, _i, _q, _v
     */
    static bool is_hair_hiding_rule(__int64 ruleObj)
    {
        auto handleAddr = *reinterpret_cast<uintptr_t *>(ruleObj + 0x18);
        if (handleAddr < 0x10000)
            return false;
        auto suffixAddr = *reinterpret_cast<uintptr_t *>(handleAddr);
        if (suffixAddr < 0x10000)
            return false;
        const auto *suffix = reinterpret_cast<const char *>(suffixAddr);

        if (suffix[0] != '_' || suffix[2] != '\0')
            return false;

        switch (suffix[1])
        {
        case 'a':
        case 'c':
        case 'd':
        case 'f':
        case 'i':
        case 'q':
        case 'v':
            return true;
        default:
            return false;
        }
    }

    static bool should_suppress_hair_hiding(__int64 ruleObj) noexcept
    {
        __try
        {
            if (!is_hair_hiding_rule(ruleObj))
                return false;
            return is_category_hidden(Category::Helm) ||
                   is_category_hidden(Category::Cloak);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static __int64 __fastcall on_postfix_eval(__int64 ruleObj, __int64 context)
    {
        if (s_baldFix.load(std::memory_order_relaxed) &&
            should_suppress_hair_hiding(ruleObj))
        {
            static std::atomic<bool> s_loggedBaldFix{false};
            if (!s_loggedBaldFix.exchange(true, std::memory_order_relaxed))
                DMK::Logger::get_instance().debug(
                    "BaldFix: suppressed hair-hiding rule (suffix at ruleObj 0x{:X})",
                    ruleObj);
            return 0;
        }
        return s_originalPostfixEval(ruleObj, context);
    }

    static bool check_part_hidden(uint64_t partHashPtr)
    {
        if (partHashPtr < 0x10000)
            return false;
        auto partHash = *reinterpret_cast<const uint32_t *>(partHashPtr);
        if (!needs_classification(partHash))
            return false;
        auto mask = classify_part(partHash);
        return mask != 0 && is_any_category_hidden(mask);
    }

    static bool should_skip_part_add_show(uint64_t partHashPtr) noexcept
    {
        __try
        {
            return check_part_hidden(partHashPtr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static __int64 __fastcall on_part_add_show(
        __int64 a1, uint8_t a2, uint64_t partHashPtr, float blend,
        __int64 a5, __int64 a6, __int64 a7, __int64 a8, __int64 a9)
    {
        if (s_glidingFix.load(std::memory_order_relaxed) &&
            should_skip_part_add_show(partHashPtr))
            return 0;
        return s_originalPartAddShow(a1, a2, partHashPtr, blend,
                                     a5, a6, a7, a8, a9);
    }

    static void cleanup_vis_bytes() noexcept;

    // --- Deferred IndexedStringA scan ---
    static constexpr int k_maxScanAttempts = 10;
    static constexpr int k_scanRetryMs = 2000;
    static constexpr int k_scanIdleRetryMs = 10000;
    static constexpr int k_scanInitialDelayMs = 8000;

    static void deferred_scan_thread() noexcept
    {
        auto &logger = DMK::Logger::get_instance();

        int realAttempts = 0;
        bool tableReady = false;
        for (;;)
        {
            if (s_shutdownRequested.load(std::memory_order_relaxed))
                return;

            int sleepMs;
            if (realAttempts == 0 && !tableReady)
                sleepMs = k_scanInitialDelayMs;
            else if (!tableReady)
                sleepMs = k_scanIdleRetryMs;
            else
                sleepMs = k_scanRetryMs;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));

            auto runtimeHashes = scan_indexed_string_table(s_mapLookupAddr);
            if (runtimeHashes.empty())
                continue;

            const auto totalParts = total_part_count();
            auto unresolved = get_unresolved_parts(runtimeHashes);
            const auto resolvedCount = totalParts - unresolved.size();

            // Table not meaningfully populated yet — keep waiting without
            // burning attempt budget.
            if (resolvedCount < 10)
                continue;

            tableReady = true;

            ++realAttempts;

            // Accept results when >=90% resolved or attempt budget spent.
            constexpr auto k_minResolvePct = 90;
            const bool enough = (resolvedCount * 100 / totalParts) >= k_minResolvePct;

            if (enough || realAttempts >= k_maxScanAttempts)
            {
                set_runtime_hashes(std::move(runtimeHashes));
                rebuild_part_lookup();
                s_deferredScanPending.store(false, std::memory_order_relaxed);

                // Restore any vis bytes set during a prior scan phase so the
                // rebuilt part map can re-apply with correct hashes.
                cleanup_vis_bytes();

                for (int j = 0; j < k_maxProtagonists; ++j)
                    s_armorInjected[j].store(false, std::memory_order_relaxed);
                s_needsDirectWrite.store(true, std::memory_order_relaxed);

                logger.info("Deferred scan complete: {}/{} resolved ({} attempts)",
                            resolvedCount, totalParts, realAttempts);

                if (!unresolved.empty())
                    s_lazyProbePending.store(true, std::memory_order_relaxed);
                return;
            }

            logger.trace("Deferred scan attempt {}: {}/{} resolved, retrying",
                         realAttempts, resolvedCount, totalParts);
        }
    }

    static void launch_deferred_scan() noexcept
    {
        if (!s_deferredScanPending.load(std::memory_order_relaxed))
            return;
        if (!s_mapLookupAddr)
            return;

        static std::atomic<bool> s_launched{false};
        if (s_launched.exchange(true, std::memory_order_relaxed))
            return;

        {
            std::lock_guard<std::mutex> lk(s_bgThreadMtx);
            s_deferredScanThread = std::thread(deferred_scan_thread);
        }
    }

    // --- Lazy re-probe for demand-loaded IndexedStringA entries ---
    static constexpr int64_t k_lazyProbeIntervalMs = 60'000;

    static void lazy_probe_thread() noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        int probeCount = 0;

        logger.info("Lazy probe started for demand-loaded parts "
                    "(interval: {}s)",
                    k_lazyProbeIntervalMs / 1000);

        while (s_lazyProbePending.load(std::memory_order_relaxed))
        {
            if (s_shutdownRequested.load(std::memory_order_relaxed))
                return;
            std::this_thread::sleep_for(std::chrono::seconds(5));

            auto signal = s_lazyProbeSignal.load(std::memory_order_relaxed);
            if (signal == 0)
                continue;

            ++probeCount;
            logger.trace("Lazy probe #{}: scanning IndexedStringA table", probeCount);

            auto runtimeHashes = scan_indexed_string_table(s_mapLookupAddr);
            if (runtimeHashes.empty())
                continue;

            auto unresolved = get_unresolved_parts(runtimeHashes);
            if (unresolved.empty())
            {
                set_runtime_hashes(std::move(runtimeHashes));
                rebuild_part_lookup();
                cleanup_vis_bytes();
                s_lazyProbePending.store(false, std::memory_order_relaxed);
                for (int j = 0; j < k_maxProtagonists; ++j)
                    s_armorInjected[j].store(false, std::memory_order_relaxed);
                s_needsDirectWrite.store(true, std::memory_order_relaxed);
                logger.info("Lazy probe resolved all remaining parts "
                            "({} probes)",
                            probeCount);
                return;
            }

            logger.trace("Lazy probe #{}: {} parts still unresolved",
                         probeCount, unresolved.size());

            s_lazyProbeSignal.store(0, std::memory_order_relaxed);
        }
    }

    static void launch_lazy_probe() noexcept
    {
        if (!s_lazyProbePending.load(std::memory_order_relaxed))
            return;

        static std::atomic<bool> s_launched{false};
        if (s_launched.exchange(true, std::memory_order_relaxed))
            return;

        {
            std::lock_guard<std::mutex> lk(s_bgThreadMtx);
            s_lazyProbeThread = std::thread(lazy_probe_thread);
        }
    }

    // --- Player-only filter ---

    /** @brief Returns true if the current actor is a known protagonist. */
    static bool is_player_vis_ctrl(uintptr_t a1) noexcept
    {
        if (a1 == s_primaryPlayerVisCtrl.load(std::memory_order_relaxed))
            return true;

        const auto n = s_playerCount.load(std::memory_order_relaxed);
        for (int i = 1; i < n; ++i)
        {
            if (s_playerVisCtrls[i].load(std::memory_order_relaxed) == a1)
                return true;
        }
        return false;
    }

    /**
     * @brief Check player-only filter. Returns false (reject) if the actor
     *        is not a known protagonist when PlayerOnly mode is active.
     *
     * Side effects: triggers resolve cycle (global chain) or caches new
     * vis ctrls (fallback mode).
     */
    static bool check_player_filter(uintptr_t a1) noexcept
    {
        if (!s_playerOnly.load(std::memory_order_relaxed))
            return true;

        if (a1 < 0x10000)
            return false;

        if (s_fallbackMode.load(std::memory_order_relaxed))
        {
            // d8-based fallback: a1->+0x48->+0x08->+0xD8.
            // Uses read_ptr_unsafe — SEH-protected via on_vis_check.
            auto comp = read_ptr_unsafe(a1, 0x48);
            if (comp)
            {
                auto actor = read_ptr_unsafe(comp, 0x08);
                if (actor)
                {
                    auto d8Val = *reinterpret_cast<const int32_t *>(actor + 0xD8);
                    if (d8Val >= 2)
                    {
                        const auto n = s_playerCount.load(std::memory_order_relaxed);
                        bool alreadyCached = false;
                        for (int i = 0; i < n; ++i)
                        {
                            if (s_playerVisCtrls[i].load(std::memory_order_relaxed) == a1)
                            {
                                alreadyCached = true;
                                break;
                            }
                        }
                        if (!alreadyCached && n < k_maxProtagonists)
                        {
                            int expected = n;
                            if (s_playerCount.compare_exchange_weak(
                                    expected, n + 1, std::memory_order_relaxed))
                            {
                                s_playerVisCtrls[n].store(a1, std::memory_order_relaxed);
                                s_primaryPlayerVisCtrl.store(
                                    s_playerVisCtrls[0].load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
                                s_needsDirectWrite.store(true, std::memory_order_relaxed);
                                DMK::Logger::get_instance().debug(
                                    "Fallback: new player vis ctrl cached at slot {} (0x{:X})",
                                    n, a1);
                            }
                        }
                    }
                }
            }

            return s_playerCount.load(std::memory_order_relaxed) <= 0 ||
                   is_player_vis_ctrl(a1);
        }

        // Global pointer chain mode.
        auto cnt = s_resolveCounter.fetch_add(1, std::memory_order_relaxed);
        if (s_playerCount.load(std::memory_order_relaxed) == 0 ||
            (cnt & (k_resolveInterval - 1)) == 0)
            resolve_player_vis_ctrls();

        return s_playerCount.load(std::memory_order_relaxed) <= 0 ||
               is_player_vis_ctrl(a1);
    }

    // --- Mid-hook callback ---
    static void on_vis_check_impl(SafetyHookContext &ctx)
    {
        if (s_needsDirectWrite.load(std::memory_order_relaxed) &&
            s_needsDirectWrite.exchange(false, std::memory_order_relaxed))
        {
            inject_armor_entries();
            apply_direct_vis_write();
        }

        if (s_deferredScanPending.load(std::memory_order_relaxed))
            launch_deferred_scan();

        if (s_lazyProbePending.load(std::memory_order_relaxed))
        {
            launch_lazy_probe();
            static std::atomic<uint32_t> s_probeCounter{0};
            if ((s_probeCounter.fetch_add(1, std::memory_order_relaxed) & 0xFFF) == 0)
            {
                const auto now = steady_ms();
                auto prev = s_lazyProbeSignal.load(std::memory_order_relaxed);
                if (prev == 0 || (now - prev) >= k_lazyProbeIntervalMs)
                    s_lazyProbeSignal.store(now, std::memory_order_relaxed);
            }
        }

        auto r10 = ctx.r10;
        if (r10 < 0x10000)
            return;

        auto partHash = *reinterpret_cast<const uint32_t *>(r10);

        if (!needs_classification(partHash))
            return;

        const auto mask = classify_part(partHash);
        if (mask == 0)
            return;

        auto r13 = ctx.r13;
        if (r13 < 0x10000)
            return;

        auto a1 = *reinterpret_cast<uintptr_t *>(ctx.rbp + 0x4F);
        if (!check_player_filter(a1))
            return;

        if (is_any_category_hidden(mask))
        {
            auto *visPtr = reinterpret_cast<uint8_t *>(r13 + 0x1C);
            *visPtr = 2;
        }
        else if (s_forceShow.load(std::memory_order_relaxed))
        {
            auto *visPtr = reinterpret_cast<uint8_t *>(r13 + 0x1C);
            *visPtr = 0;
        }
    }

    /* SEH wrapper: separate function because MSVC SEH cannot coexist with
       C++ destructors in the same frame. Swallows faults if the mod is
       outdated and register layout has changed — don't crash the game. */
    static int seh_filter(unsigned int /*code*/) { return EXCEPTION_EXECUTE_HANDLER; }

    static void on_vis_check(SafetyHookContext &ctx)
    {
        __try
        {
            on_vis_check_impl(ctx);
        }
        __except (seh_filter(GetExceptionCode()))
        {
        }
    }

    /* AOB scan — scans all executable regions, not just the main module,
       because packed/protected binaries may decompress code elsewhere. */
    struct CompiledCandidate
    {
        const AobCandidate *source;
        DMK::Scanner::CompiledPattern compiled;
    };

    static uintptr_t scan_for_hook_target(
        const std::vector<CompiledCandidate> &candidates,
        const AobCandidate *&matchedSource)
    {
        const uint8_t *addr = nullptr;
        MEMORY_BASIC_INFORMATION mbi;

        while (VirtualQuery(addr, &mbi, sizeof(mbi)))
        {
            if (mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY)))
            {
                const auto regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);

                for (const auto &c : candidates)
                {
                    const auto *match = DMK::Scanner::find_pattern(
                        reinterpret_cast<const std::byte *>(regionBase),
                        mbi.RegionSize, c.compiled);
                    if (match)
                    {
                        matchedSource = c.source;
                        return reinterpret_cast<uintptr_t>(match) + c.source->offsetToHook;
                    }
                }
            }

            addr = static_cast<const uint8_t *>(mbi.BaseAddress) + mbi.RegionSize;
        }

        return 0;
    }

    // --- Config ---
    static void load_config()
    {
        DMK::Config::register_string("General", "LogLevel", "Log Level", [](const std::string &val)
                                     {
                auto& logger = DMK::Logger::get_instance();
                logger.set_log_level(DMK::Logger::string_to_log_level(val)); }, "Info");

        DMK::Config::register_bool("General", "PlayerOnly", "Player Only", [](bool val)
                                   { s_playerOnly.store(val, std::memory_order_relaxed); }, true);

        DMK::Config::register_bool("General", "ForceShow", "Force Show", [](bool val)
                                   { s_forceShow.store(val, std::memory_order_relaxed); }, false);

        DMK::Config::register_bool("General", "BaldFix", "Bald Fix", [](bool val)
                                   { s_baldFix.store(val, std::memory_order_relaxed); }, true);

        DMK::Config::register_bool("General", "GlidingFix", "Gliding Fix", [](bool val)
                                   { s_glidingFix.store(val, std::memory_order_relaxed); }, true);

        DMK::Config::register_key_combo("General", "ShowAllHotkey", "Show All Hotkey", [](const DMK::Config::KeyComboList &combos)
                                        { s_showAllCombos = combos; }, "");

        DMK::Config::register_key_combo("General", "HideAllHotkey", "Hide All Hotkey", [](const DMK::Config::KeyComboList &combos)
                                        { s_hideAllCombos = combos; }, "");

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            const auto cat = static_cast<Category>(i);
            const std::string section{category_section(cat)};

            const bool active = (cat == Category::Shields ||
                                 cat == Category::Helm ||
                                 cat == Category::Mask);
            const char *defaultToggle = active ? "V" : "";

            DMK::Config::register_bool(section, "Enabled", section + " Enabled", [i](bool val)
                                       { category_states()[i].enabled.store(val, std::memory_order_relaxed); }, active);

            DMK::Config::register_key_combo(section, "ToggleHotkey", section + " Toggle Hotkey", [i](const DMK::Config::KeyComboList &combos)
                                            { category_states()[i].toggleHotkeyCombos = combos; }, defaultToggle);

            DMK::Config::register_key_combo(section, "ShowHotkey", section + " Show Hotkey", [i](const DMK::Config::KeyComboList &combos)
                                            { category_states()[i].showHotkeyCombos = combos; }, "");

            DMK::Config::register_key_combo(section, "HideHotkey", section + " Hide Hotkey", [i](const DMK::Config::KeyComboList &combos)
                                            { category_states()[i].hideHotkeyCombos = combos; }, "");

            DMK::Config::register_bool(section, "DefaultHidden", section + " Default Hidden", [i](bool val)
                                       { category_states()[i].hidden.store(val, std::memory_order_relaxed); }, active);

            DMK::Config::register_string(section, "Parts", section + " Parts", [cat](const std::string &val)
                                         { register_parts(cat, val); }, default_parts_string(cat));
        }

        DMK::Config::load(INI_FILE);
        DMK::Config::log_all();
        build_part_lookup();

        {
            auto &logger = DMK::Logger::get_instance();
            if (logger.get_log_level() <= DMK::LogLevel::Trace)
            {
                const auto &states = category_states();
                const auto &partMap = get_part_map();

                for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
                {
                    const auto cat = static_cast<Category>(i);
                    const auto bit = category_bit(cat);
                    int count = 0;
                    for (const auto &[hash, mask] : partMap)
                    {
                        if (mask & bit)
                            ++count;
                    }

                    const bool enabled = states[i].enabled.load(std::memory_order_relaxed);
                    const bool hidden = states[i].hidden.load(std::memory_order_relaxed);

                    if (!enabled)
                        logger.trace("Category {}: disabled ({} parts registered)",
                                     category_section(cat), count);
                    else
                        logger.trace("Category {}: enabled, default={} ({} parts)",
                                     category_section(cat),
                                     hidden ? "hidden" : "visible", count);
                }
            }
        }
    }

    // --- Hotkey helpers ---
    static std::string combo_key(const DMK::Config::KeyComboList &combos)
    {
        std::string key;
        for (const auto &combo : combos)
        {
            for (const auto &mod : combo.modifiers)
                key += std::to_string(static_cast<int>(mod.source)) + ":" + std::to_string(mod.code) + "+";
            for (const auto &k : combo.keys)
                key += std::to_string(static_cast<int>(k.source)) + ":" + std::to_string(k.code) + ",";
            key += "|";
        }
        return key;
    }

    static void flush_visibility() noexcept
    {
        if (!s_fallbackMode.load(std::memory_order_relaxed))
            resolve_player_vis_ctrls();

        // Reset injection flags so newly-toggled categories get entries created.
        for (int i = 0; i < k_maxProtagonists; ++i)
            s_armorInjected[i].store(false, std::memory_order_relaxed);

        inject_armor_entries();
        apply_direct_vis_write();
    }

    // --- Hotkey registration ---
    static void register_hotkeys()
    {
        auto &inputMgr = DMK::InputManager::get_instance();
        auto &states = category_states();
        auto &logger = DMK::Logger::get_instance();

        int toggleCount = 0;
        int showCount = 0;
        int hideCount = 0;
        int globalCount = 0;

        if (!s_showAllCombos.empty())
        {
            inputMgr.register_press(
                "ShowAll",
                s_showAllCombos,
                [&logger]()
                {
                    auto &st = category_states();
                    for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
                        st[i].hidden.store(false, std::memory_order_relaxed);

                    logger.info("Equip hide: all categories VISIBLE");
                    flush_visibility();
                });
            ++globalCount;
        }

        if (!s_hideAllCombos.empty())
        {
            inputMgr.register_press(
                "HideAll",
                s_hideAllCombos,
                [&logger]()
                {
                    auto &st = category_states();
                    for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
                        st[i].hidden.store(true, std::memory_order_relaxed);

                    logger.info("Equip hide: all categories HIDDEN");
                    flush_visibility();
                });
            ++globalCount;
        }

        struct HotkeyGroup
        {
            DMK::Config::KeyComboList combos;
            std::vector<std::size_t> categoryIndices;
        };

        std::unordered_map<std::string, HotkeyGroup> groups;

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            if (!states[i].enabled.load(std::memory_order_relaxed))
                continue;
            if (states[i].toggleHotkeyCombos.empty())
                continue;

            auto &group = groups[combo_key(states[i].toggleHotkeyCombos)];
            if (group.combos.empty())
                group.combos = states[i].toggleHotkeyCombos;
            group.categoryIndices.push_back(i);
        }

        for (auto &[key, group] : groups)
        {
            std::string bindingName = "ToggleEquip";
            for (auto idx : group.categoryIndices)
                bindingName += std::string("_") + std::string(category_section(static_cast<Category>(idx)));

            auto indices = group.categoryIndices;

            inputMgr.register_press(
                bindingName,
                group.combos,
                [indices, &logger]()
                {
                    auto &st = category_states();
                    const bool newHidden = !st[indices[0]].hidden.load(std::memory_order_relaxed);
                    for (auto idx : indices)
                        st[idx].hidden.store(newHidden, std::memory_order_relaxed);

                    std::string catNames;
                    for (auto idx : indices)
                    {
                        if (!catNames.empty())
                            catNames += ", ";
                        catNames += category_section(static_cast<Category>(idx));
                    }
                    logger.info("Equip hide toggled [{}]: {}", catNames, newHidden ? "HIDDEN" : "VISIBLE");
                    flush_visibility();
                });

            ++toggleCount;
        }

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            if (!states[i].enabled.load(std::memory_order_relaxed))
                continue;

            const auto section = std::string(category_section(static_cast<Category>(i)));

            if (!states[i].showHotkeyCombos.empty())
            {
                inputMgr.register_press(
                    "ShowEquip_" + section,
                    states[i].showHotkeyCombos,
                    [i, &logger, section]()
                    {
                        category_states()[i].hidden.store(false, std::memory_order_relaxed);
                        logger.info("Equip hide [{}]: VISIBLE", section);
                        flush_visibility();
                    });
                ++showCount;
            }

            if (!states[i].hideHotkeyCombos.empty())
            {
                inputMgr.register_press(
                    "HideEquip_" + section,
                    states[i].hideHotkeyCombos,
                    [i, &logger, section]()
                    {
                        category_states()[i].hidden.store(true, std::memory_order_relaxed);
                        logger.info("Equip hide [{}]: HIDDEN", section);
                        flush_visibility();
                    });
                ++hideCount;
            }
        }

        const int total = globalCount + toggleCount + showCount + hideCount;
        logger.info("Hotkeys registered: {} binding(s) "
                    "({} toggle, {} show, {} hide, {} global)",
                    total, toggleCount, showCount, hideCount, globalCount);
    }

    // --- Public interface ---
    bool init()
    {
        auto &logger = DMK::Logger::get_instance();

        logger.info("{} v{}", MOD_NAME, MOD_VERSION);
        logger.info("By {}", MOD_AUTHOR);
        logger.info("Source: {}", MOD_SOURCE);
        logger.info("Nexus:  {}", MOD_NEXUS);
        logger.debug("Built on " __DATE__ " at " __TIME__);

        DMK::Memory::init_cache();

        s_worldSystemPtr = resolve_address(
            k_worldSystemCandidates, std::size(k_worldSystemCandidates),
            "WorldSystem");

        s_childActorVtbl = resolve_address(
            k_childActorVtblCandidates, std::size(k_childActorVtblCandidates),
            "ChildActorVtbl");

        s_mapLookupAddr = resolve_address(
            k_mapLookupCandidates, std::size(k_mapLookupCandidates),
            "MapLookup");

        s_mapInsertAddr = resolve_address(
            k_mapInsertCandidates, std::size(k_mapInsertCandidates),
            "MapInsert");

        // Resolve IndexedStringA global from MapLookup: mov rax, [rip+disp] at +20
        // (opcode 48 8B 05 xx xx xx xx → loads qword_145BBAED8).
        if (s_mapLookupAddr)
        {
            const auto ripInstr = s_mapLookupAddr + 20;
            const auto *instrBytes = reinterpret_cast<const uint8_t *>(ripInstr);
            if (instrBytes[0] == 0x48 && instrBytes[1] == 0x8B && instrBytes[2] == 0x05)
            {
                int32_t disp = 0;
                std::memcpy(&disp, instrBytes + 3, sizeof(int32_t));
                s_indexedStringGlobalAddr = static_cast<uintptr_t>(
                    static_cast<int64_t>(ripInstr + 7) + disp);
                logger.info("IndexedStringA global resolved at 0x{:X}",
                            s_indexedStringGlobalAddr);
            }
            else
            {
                logger.warning("IndexedStringA global: unexpected opcode at MapLookup+20 "
                               "({:02X} {:02X} {:02X}), armor injection disabled",
                               instrBytes[0], instrBytes[1], instrBytes[2]);
            }
        }

        if (!s_worldSystemPtr || !s_childActorVtbl)
        {
            s_fallbackMode.store(true, std::memory_order_relaxed);
            logger.info("Player identification: d8-based fallback (global chain AOB unavailable)");
        }
        else
        {
            logger.info("Player identification: global pointer chain");
        }

        if (s_mapLookupAddr)
        {
            auto runtimeHashes = scan_indexed_string_table(s_mapLookupAddr);
            if (!runtimeHashes.empty())
            {
                logger.info("IndexedStringA scan: {} entries resolved at init",
                            runtimeHashes.size());
                set_runtime_hashes(std::move(runtimeHashes));
            }
            else
            {
                logger.info("IndexedStringA table not ready at init, "
                            "starting deferred scan thread");
                s_deferredScanPending.store(true, std::memory_order_relaxed);
                launch_deferred_scan();
            }
        }
        else
        {
            logger.warning("MapLookup not resolved, cannot scan IndexedStringA table");
        }

        load_config();

        std::vector<CompiledCandidate> compiledCandidates;
        for (const auto &candidate : k_aobCandidates)
        {
            auto compiled = DMK::Scanner::parse_aob(candidate.pattern);
            if (compiled)
                compiledCandidates.push_back({&candidate, std::move(*compiled)});
            else
                logger.warning("Failed to parse AOB pattern '{}'", candidate.name);
        }

        if (compiledCandidates.empty())
        {
            logger.error("No valid AOB patterns available.");
            return false;
        }

        const AobCandidate *matchedSource = nullptr;
        uintptr_t hookAddr = scan_for_hook_target(compiledCandidates, matchedSource);

        if (hookAddr == 0)
        {
            logger.error("No AOB pattern matched. The mod may be outdated for this game version.");
            return false;
        }

        auto &hookMgr = DMK::HookManager::get_instance();
        auto hookResult = hookMgr.create_mid_hook("EquipVisCheck", hookAddr, on_vis_check);

        if (!hookResult.has_value())
        {
            logger.error("Hook creation failed at 0x{:X}: {}",
                         hookAddr, DetourModKit::Hook::error_to_string(hookResult.error()));
            return false;
        }

        logger.info("Hook installed via pattern '{}' at 0x{:X}",
                    matchedSource->name, hookAddr);

        // Prevents hidden parts from flashing during state transitions (gliding exit).
        if (s_glidingFix.load(std::memory_order_relaxed))
        {
            auto partAddShowAddr = resolve_address(
                k_partAddShowCandidates, std::size(k_partAddShowCandidates),
                "PartAddShow");

            if (partAddShowAddr)
            {
                auto result = hookMgr.create_inline_hook(
                    "PartAddShow", partAddShowAddr,
                    reinterpret_cast<void *>(on_part_add_show),
                    reinterpret_cast<void **>(&s_originalPartAddShow));

                if (result.has_value())
                    logger.info("PartAddShow inline hook installed at 0x{:X}",
                                partAddShowAddr);
                else
                    logger.warning("PartAddShow hook failed: {} — gliding flash fix disabled",
                                   DetourModKit::Hook::error_to_string(result.error()));
            }
            else
            {
                logger.warning("PartAddShow AOB scan failed — gliding flash fix disabled");
            }
        }

        // Prevents baldness when hiding helmets/cloaks by suppressing
        // the game's postfix rules that hide hair based on equipped gear.
        if (s_baldFix.load(std::memory_order_relaxed))
        {
            auto postfixEvalAddr = resolve_address(
                k_postfixEvalCandidates, std::size(k_postfixEvalCandidates),
                "PostfixEval");

            if (postfixEvalAddr)
            {
                auto result = hookMgr.create_inline_hook(
                    "PostfixEval", postfixEvalAddr,
                    reinterpret_cast<void *>(on_postfix_eval),
                    reinterpret_cast<void **>(&s_originalPostfixEval));

                if (result.has_value())
                    logger.info("PostfixEval inline hook installed at 0x{:X} — bald fix active",
                                postfixEvalAddr);
                else
                    logger.warning("PostfixEval hook failed: {} — bald fix disabled",
                                   DetourModKit::Hook::error_to_string(result.error()));
            }
            else
            {
                logger.warning("PostfixEval AOB scan failed — bald fix disabled");
            }
        }
        else
        {
            logger.info("BaldFix disabled in config — hair-hiding rules will apply normally");
        }

        register_hotkeys();

        auto &inputMgr = DMK::InputManager::get_instance();
        inputMgr.start();

        if (s_deferredScanPending.load(std::memory_order_relaxed))
            logger.info("Hooks installed, part hashes pending (deferred scan active)");
        else
            logger.info("Equip hide fully initialized ({} parts resolved)",
                        total_part_count());
        return true;
    }

    /**
     * @brief Undo all visibility modifications before unload.
     * @details Restores only entries tracked in s_originalVis. Unmodified
     *          entries (including injected zero-bone armor entries) are left as-is.
     */
    static void cleanup_vis_bytes() noexcept
    {
        s_directWriteMtx.lock();

        __try
        {
            int restoredCount = 0;
            for (const auto &[visAddr, origVis] : s_originalVis)
            {
                auto *visPtr = reinterpret_cast<uint8_t *>(visAddr);
                *visPtr = origVis;
                ++restoredCount;
            }
            s_originalVis.clear();

            DMK::Logger::get_instance().debug(
                "Cleanup: {} vis bytes restored", restoredCount);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        s_directWriteMtx.unlock();
    }

    void shutdown()
    {
        DMK::Logger::get_instance().info("{} shutting down...", MOD_NAME);
        s_shutdownRequested.store(true, std::memory_order_relaxed);
        s_lazyProbePending.store(false, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lk(s_bgThreadMtx);
            if (s_deferredScanThread.joinable())
                s_deferredScanThread.join();
            if (s_lazyProbeThread.joinable())
                s_lazyProbeThread.join();
        }

        cleanup_vis_bytes();
        DMK_Shutdown();
    }

} // namespace EquipHide
