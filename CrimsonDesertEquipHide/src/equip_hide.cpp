#include "equip_hide.hpp"
#include "categories.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace EquipHide
{
    // =========================================================================
    // Hook target: sub_140818340 — visibility decision function
    //
    // Hook point: movzx eax, byte ptr [r13+1Ch]; cmp al, 3
    //
    // Register layout at hook point:
    //   R10 = pointer to part hash DWORD (IndexedStringA ID)
    //   R13 = pointer to PartInOutSocket struct (Visible byte at +0x1C)
    //   R8B = exclusion-list flag
    //
    // To hide a part: set [R13+0x1C] = 2 (Out-only visibility)
    //
    // Cascading AOB patterns (tried in order until one matches):
    //   P1: Direct hook-site pattern (most precise, least resilient)
    //   P2: Wider context pattern encoding array iteration structure
    //       (survives register reallocation, needs +0x36 offset)
    //   P3: Short core pattern (last resort, 7 bytes)
    //
    // See .idea/research/update_resilience.md for full analysis.
    // =========================================================================

    static std::atomic<bool> s_playerOnly{true};

    static constexpr int k_maxProtagonists = 8;
    static std::atomic<uintptr_t> s_playerVisCtrls[k_maxProtagonists]{};
    static std::atomic<int> s_playerCount{0};
    static std::atomic<uint32_t> s_resolveCounter{0};
    static constexpr uint32_t k_resolveInterval = 512;

    static uintptr_t s_worldSystemPtr = 0;
    static uintptr_t s_childActorVtbl = 0;
    static uintptr_t s_mapLookupAddr  = 0;

    static std::atomic<bool> s_needsDirectWrite{false};

    static DMK::Config::KeyComboList s_showAllCombos;
    static DMK::Config::KeyComboList s_hideAllCombos;

    // d8-based fallback: activates when global pointer chain AOBs fail
    static std::atomic<bool> s_fallbackMode{false};

    // Deferred IndexedStringA scan: set when the table isn't ready at init
    static std::atomic<bool> s_deferredScanPending{false};

    static uintptr_t read_ptr(uintptr_t base, ptrdiff_t off) noexcept
    {
        const auto *addr = reinterpret_cast<const uintptr_t *>(base + off);
        if (!DMK::Memory::is_readable(addr, sizeof(uintptr_t)))
            return 0;
        return (*addr > 0x10000) ? *addr : 0;
    }

    // =========================================================================
    // AOB-based address resolution
    //
    // Each target address (global pointer, vtable, function) is resolved at
    // init via cascading AOB patterns.  Two resolution modes:
    //   Direct:      target = match_addr + offset
    //   RipRelative: target = match_addr + instrEnd + *(int32*)(match + dispOffset)
    // =========================================================================
    enum class ResolveMode : uint8_t { Direct, RipRelative };

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
    };

    static constexpr AddrCandidate k_childActorVtblCandidates[] = {
        {"VT_P1_DtorConst",
         "48 8D 05 ?? ?? ?? ?? 48 89 01 E8 ?? ?? ?? ?? F6 C3 01 74 ?? BA 87 EE 08 4C",
         ResolveMode::RipRelative, 3, 7},

        {"VT_P2_CtorStore",
         "48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06 EB 03 4C",
         ResolveMode::RipRelative, 12, 16},
    };

    static constexpr AddrCandidate k_mapLookupCandidates[] = {
        {"ML_P1_FullPrologue",
         "48 83 EC 08 83 79 04 00 4C 8B C1 75 07 33 C0 48 83 C4 08 C3 48 8B 05 ?? ?? ?? ?? 48 89 1C 24 8B 1A",
         ResolveMode::Direct, 0, 0},

        {"ML_P2_HashBody",
         "8B 48 58 48 03 D2 44 8B 5C D1 08 41 8B 08 85 C9 74 ?? 33 D2 41 8B C3 F7 F1",
         ResolveMode::Direct, -0x24, 0},
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
            logger.info("{} resolved via '{}' at {:#x}", label, matchedName, result);
            return result;
        }

        logger.warning("{} AOB scan failed — feature disabled", label);
        return 0;
    }

    // =========================================================================
    // IndexedStringA table scan
    //
    // Reads the game's global IndexedStringA table to build a name->hash map
    // at init time. This makes hash IDs resilient to table reordering across
    // game patches.
    //
    // Table layout (verified via CE):
    //   globalPtr      = *(qword*)(mapLookupFunc + 20 + rip_disp)
    //   tableArray     = *(qword*)(globalPtr + 0x58)
    //   entry[hash]    = tableArray + hash * 16
    //   entry[hash]+0  = pointer to null-terminated string (or 0)
    // =========================================================================
    // Scan only the range where equipment parts are known to cluster.
    // Outlier hashes (e.g. CD_Tool_FishingRod, CD_Tool_Book) are resolved
    // via targeted probes around their fallback hashes.
    static constexpr uint32_t k_tableScanMin = 0xAD00;
    static constexpr uint32_t k_tableScanMax = 0xBFFF;

    // SEH-protected table entry reader.  Returns the string length (excluding
    // null terminator) or 0 on any access fault.  Separate function so that
    // __try/__except does not conflict with C++ objects in the caller.
    static constexpr std::size_t k_maxStringLen = 64;

#ifdef _MSC_VER
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
#else
    static std::size_t read_table_entry(uintptr_t tableArray, uint32_t hash,
                                        char *buf, std::size_t bufSize) noexcept
    {
        const auto entryAddr = tableArray + static_cast<uintptr_t>(hash) * 16;
        if (!DMK::Memory::is_readable(reinterpret_cast<const void *>(entryAddr),
                                       sizeof(uintptr_t)))
            return 0;

        const auto strPtr = *reinterpret_cast<const uintptr_t *>(entryAddr);
        if (strPtr < 0x10000)
            return 0;

        if (!DMK::Memory::is_readable(reinterpret_cast<const void *>(strPtr), bufSize))
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
#endif

    static std::unordered_map<std::string, uint32_t> scan_indexed_string_table(
        uintptr_t mapLookupFunc)
    {
        auto& logger = DMK::Logger::get_instance();
        std::unordered_map<std::string, uint32_t> nameToHash;

        // The MapLookup function contains: mov rax, [rip+disp] at offset +20
        // (opcode 48 8B 05 xx xx xx xx). Extract the RIP-relative displacement.
        const auto ripInstr = mapLookupFunc + 20;
        const auto* instrBytes = reinterpret_cast<const uint8_t*>(ripInstr);

        // Verify REX.W MOV RAX opcode: 48 8B 05
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

        const auto globalPtr = *reinterpret_cast<const uintptr_t*>(globalPtrAddr);
        if (globalPtr < 0x10000)
        {
            logger.debug("IndexedStringA scan: global pointer not yet initialized ({:#x})",
                         globalPtr);
            return nameToHash;
        }

        const auto tableArray = *reinterpret_cast<const uintptr_t*>(globalPtr + 0x58);
        if (tableArray < 0x10000)
        {
            logger.warning("IndexedStringA scan: tableArray is null/invalid ({:#x})",
                           tableArray);
            return nameToHash;
        }

        logger.debug("IndexedStringA scan: globalPtr={:#x} tableArray={:#x}",
                      globalPtr, tableArray);

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

        // Targeted probe for known parts not found in the linear sweep.
        // Outlier hashes can shift between game launches, so we search a
        // small window around each fallback hash.
        const auto unresolved = get_unresolved_fallbacks(nameToHash);
        if (!unresolved.empty())
        {
            constexpr uint32_t k_probeRadius = 0x100;
            for (const auto& [name, fallback] : unresolved)
            {
                const uint32_t lo = (fallback > k_probeRadius)
                                        ? fallback - k_probeRadius : 1;
                const uint32_t hi = fallback + k_probeRadius;

                for (uint32_t h = lo; h <= hi; ++h)
                {
                    auto len = read_table_entry(tableArray, h, buf, sizeof(buf));
                    if (len != name.size())
                        continue;

                    if (std::memcmp(buf, name.c_str(), len) == 0)
                    {
                        nameToHash[name] = h;
                        ++probeHits;
                        logger.debug("Outlier probe: {} found at 0x{:X} "
                                     "(fallback was 0x{:X})", name, h, fallback);
                        break;
                    }
                }
            }
        }

        const auto t1 = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        logger.info("IndexedStringA scan complete: {} CD_ entries ({} range, {} probed) in {}ms",
                     cdEntries + probeHits, cdEntries, probeHits, ms);

        return nameToHash;
    }

    static uintptr_t body_to_vis_ctrl(uintptr_t body) noexcept
    {
        if (!body)
            return 0;
        auto inner = read_ptr(body, 0x68);
        if (!inner)
            return 0;
        auto sub = read_ptr(inner, 0x40);
        if (!sub)
            return 0;
        return read_ptr(sub, 0xE8);
    }

    static void resolve_player_vis_ctrls() noexcept
    {
        if (!s_worldSystemPtr || !s_childActorVtbl)
            return;

#ifdef _MSC_VER
        __try
        {
#endif
            auto ws = read_ptr(s_worldSystemPtr, 0);
            if (!ws) return;
            auto am = read_ptr(ws, 0x30);
            if (!am) return;
            auto user = read_ptr(am, 0x28);
            if (!user) return;

            static constexpr ptrdiff_t k_bodyOffsets[] = {
                0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8, 0x100, 0x108};

            int count = 0;
            for (auto off : k_bodyOffsets)
            {
                if (count >= k_maxProtagonists)
                    break;

                auto candidate = read_ptr(user, off);
                if (!candidate)
                    continue;

                auto vt = read_ptr(candidate, 0);
                if (vt != s_childActorVtbl)
                    continue;

                auto vc = body_to_vis_ctrl(candidate);
                if (vc)
                    s_playerVisCtrls[count++] = vc;
            }

            for (int i = count; i < k_maxProtagonists; ++i)
                s_playerVisCtrls[i].store(0, std::memory_order_relaxed);
            s_playerCount.store(count, std::memory_order_relaxed);

            {
                static std::atomic<bool> s_logged{false};
                if (!s_logged.exchange(true, std::memory_order_relaxed))
                    DMK::Logger::get_instance().debug(
                        "Resolve: ws={:#x} am={:#x} user={:#x} count={}",
                        ws, am, user, count);
            }
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            static std::atomic<bool> s_crashLogged{false};
            if (!s_crashLogged.exchange(true, std::memory_order_relaxed))
                DMK::Logger::get_instance().warning("Resolve: SEH caught crash");
        }
#endif
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

#ifdef _MSC_VER
        __try
        {
#endif
            auto lookup = reinterpret_cast<MapLookupFn>(s_mapLookupAddr);
            const auto n = s_playerCount.load(std::memory_order_relaxed);
            int modified = 0;

            for (int i = 0; i < n; ++i)
            {
                auto vc = s_playerVisCtrls[i].load(std::memory_order_relaxed);
                if (!vc)
                    continue;

                auto comp = read_ptr(vc, 0x48);
                if (!comp)
                    continue;
                auto descNode = read_ptr(comp, 0x218);
                if (!descNode)
                    continue;
                auto mapBase = descNode + 0x20;

                for (const auto &[hash, cat] : get_part_map())
                {
                    auto entry = lookup(mapBase, &hash);
                    if (!entry)
                        continue;

                    auto *visPtr = reinterpret_cast<uint8_t *>(entry + 0x1C);
                    *visPtr = is_category_hidden(cat) ? 2 : 0;
                    ++modified;
                }
            }

            DMK::Logger::get_instance().trace(
                "DirectWrite: {} protagonists, {} entries modified",
                n, modified);
#ifdef _MSC_VER
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            static std::atomic<bool> s_crashLogged{false};
            if (!s_crashLogged.exchange(true, std::memory_order_relaxed))
                DMK::Logger::get_instance().warning("DirectWrite: SEH caught crash");
        }
#endif
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

    // =========================================================================
    // Mid-hook callback (SEH-protected)
    //
    // Read part hash from [R10], classify by range, and force Visible=2
    // for hidden categories.  Wrapped in SEH to prevent game crash if the
    // mod is outdated and register layout has changed.
    // =========================================================================
    /// Launch a background thread to scan the IndexedStringA table.
    /// Retries with increasing delays until all parts resolve or limit is hit.
    static constexpr int k_maxScanAttempts = 10;

    static void deferred_scan_thread() noexcept
    {
        auto& logger = DMK::Logger::get_instance();

        for (int attempt = 0; attempt < k_maxScanAttempts; ++attempt)
        {
            // Wait before retrying (2s, 4s, 6s, ...)
            if (attempt > 0)
                std::this_thread::sleep_for(std::chrono::seconds(2 * attempt));

            auto runtimeHashes = scan_indexed_string_table(s_mapLookupAddr);
            if (runtimeHashes.empty())
                continue;

            auto unresolved = get_unresolved_fallbacks(runtimeHashes);

            if (unresolved.empty() || attempt == k_maxScanAttempts - 1)
            {
                set_runtime_hashes(std::move(runtimeHashes));
                rebuild_part_lookup();
                s_deferredScanPending.store(false, std::memory_order_relaxed);

                if (!unresolved.empty())
                {
                    for (const auto& [name, fallback] : unresolved)
                        logger.warning("Part '{}' unresolved after {} scan attempts, "
                                       "using fallback 0x{:X}",
                                       name, attempt + 1, fallback);
                }
                return;
            }

            logger.debug("Deferred scan attempt {}: {} parts unresolved, retrying",
                          attempt + 1, unresolved.size());
        }

        logger.warning("IndexedStringA deferred scan exhausted {} attempts",
                         k_maxScanAttempts);
        s_deferredScanPending.store(false, std::memory_order_relaxed);
    }

    static void launch_deferred_scan() noexcept
    {
        if (!s_deferredScanPending.load(std::memory_order_relaxed))
            return;
        if (!s_mapLookupAddr)
            return;

        // Launch once — the flag prevents re-entry
        static std::atomic<bool> s_launched{false};
        if (s_launched.exchange(true, std::memory_order_relaxed))
            return;

        std::thread(deferred_scan_thread).detach();
    }

    /// Core logic — no SEH here so C++ objects (SafetyHookContext&) are fine.
    static void on_vis_check_impl(SafetyHookContext &ctx)
    {
        if (s_needsDirectWrite.exchange(false, std::memory_order_relaxed) &&
            !s_fallbackMode.load(std::memory_order_relaxed))
            apply_direct_vis_write();

        if (s_deferredScanPending.load(std::memory_order_relaxed))
            launch_deferred_scan();

        auto r10 = ctx.r10;
        if (r10 < 0x10000)
            return;

        auto partHash = *reinterpret_cast<const uint32_t *>(r10);

        // Quick range check before map lookup — skip obviously out-of-range hashes.
        // Bounds cover the contiguous block; outliers are checked separately.
        const auto rangeMin = hash_range_min();
        const auto rangeMax = hash_range_max();
        if (rangeMin != 0 && (partHash < rangeMin || partHash > rangeMax)
            && !is_outlier_hash(partHash))
            return;

        const auto cat = classify_part(partHash);
        if (!cat)
            return;

        auto r13 = ctx.r13;
        if (r13 < 0x10000)
            return;

        if (s_playerOnly.load(std::memory_order_relaxed))
        {
            auto a1 = *reinterpret_cast<uintptr_t *>(ctx.rbp + 0x4F);
            if (a1 < 0x10000)
                return;

            if (s_fallbackMode.load(std::memory_order_relaxed))
            {
                // d8-based fallback: read actor from vis_ctrl chain and check d8 field
                auto comp = read_ptr(a1, 0x48);
                if (comp)
                {
                    auto actor = read_ptr(comp, 0x08);
                    if (actor)
                    {
                        auto *d8Ptr = reinterpret_cast<const int32_t *>(actor + 0xD8);
                        if (DMK::Memory::is_readable(d8Ptr, sizeof(int32_t)) && *d8Ptr >= 2)
                        {
                            // Player party member detected — cache a1 into slot array
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
                                s_playerVisCtrls[n].store(a1, std::memory_order_relaxed);
                                s_playerCount.store(n + 1, std::memory_order_relaxed);
                            }
                        }
                    }
                }

                // Filter using cached slots (same logic as global-chain path)
                const auto n = s_playerCount.load(std::memory_order_relaxed);
                if (n > 0)
                {
                    bool isPlayer = false;
                    for (int i = 0; i < n; ++i)
                    {
                        if (a1 == s_playerVisCtrls[i].load(std::memory_order_relaxed))
                        {
                            isPlayer = true;
                            break;
                        }
                    }
                    if (!isPlayer)
                        return;
                }
                // n == 0: cache empty, allow all (DefaultHidden still works)
            }
            else
            {
                auto cnt = s_resolveCounter.fetch_add(1, std::memory_order_relaxed);
                if (s_playerCount.load(std::memory_order_relaxed) == 0 ||
                    (cnt % k_resolveInterval) == 0)
                    resolve_player_vis_ctrls();

                const auto n = s_playerCount.load(std::memory_order_relaxed);
                if (n > 0)
                {
                    bool isPlayer = false;
                    for (int i = 0; i < n; ++i)
                    {
                        if (a1 == s_playerVisCtrls[i].load(std::memory_order_relaxed))
                        {
                            isPlayer = true;
                            break;
                        }
                    }

                    if (!isPlayer)
                        return;
                }
            }
        }

        if (is_category_hidden(*cat))
        {
            auto *visPtr = reinterpret_cast<uint8_t *>(r13 + 0x1C);
            *visPtr = 2; // Force Out-only visibility
        }
    }

    /// SEH wrapper — catches access violations if mod is outdated and
    /// register layout has changed.  Separate function because MSVC SEH
    /// cannot coexist with C++ destructors in the same frame.
#ifdef _MSC_VER
    static int seh_filter(unsigned int /*code*/) { return EXCEPTION_EXECUTE_HANDLER; }

    static void on_vis_check(SafetyHookContext &ctx)
    {
        __try
        {
            on_vis_check_impl(ctx);
        }
        __except (seh_filter(GetExceptionCode()))
        {
            // Silently swallow — mod is likely outdated, don't crash the game
        }
    }
#else
    // MinGW/GCC does not support MSVC-style SEH (__try/__except).
    // Fall back to an unprotected call — the pointer checks in
    // on_vis_check_impl already guard against the most common faults.
    static void on_vis_check(SafetyHookContext &ctx)
    {
        on_vis_check_impl(ctx);
    }
#endif

    // =========================================================================
    // AOB scan with unpack retry
    //
    // Packed/protected binaries may decompress code into dynamically
    // allocated memory outside the main module.  Scan all readable-
    // executable regions in the process to find the hook target
    // regardless of where the unpacker places the code.
    // =========================================================================

    struct CompiledCandidate
    {
        const AobCandidate *source;
        DMK::Scanner::CompiledPattern compiled;
    };

    /// Scan all executable committed memory regions for the AOB patterns.
    /// Returns the resolved hook address and sets matchedSource on success.
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

    // =========================================================================
    // Config
    // =========================================================================
    static void load_config()
    {
        DMK::Config::register_string("General", "LogLevel", "Log Level", [](const std::string &val)
                                     {
                auto& logger = DMK::Logger::get_instance();
                logger.set_log_level(DMK::Logger::string_to_log_level(val)); }, "Info");

        DMK::Config::register_bool("General", "PlayerOnly", "Player Only", [](bool val)
                                   { s_playerOnly.store(val, std::memory_order_relaxed); }, true);

        DMK::Config::register_key_combo("General", "ShowAllHotkey", "Show All Hotkey", [](const DMK::Config::KeyComboList &combos)
                                        { s_showAllCombos = combos; }, "");

        DMK::Config::register_key_combo("General", "HideAllHotkey", "Hide All Hotkey", [](const DMK::Config::KeyComboList &combos)
                                        { s_hideAllCombos = combos; }, "");

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            const auto cat = static_cast<Category>(i);
            const std::string section{category_section(cat)};

            DMK::Config::register_bool(section, "Enabled", section + " Enabled", [i](bool val)
                                       { category_states()[i].enabled.store(val, std::memory_order_relaxed); }, true);

            DMK::Config::register_key_combo(section, "ToggleHotkey", section + " Toggle Hotkey", [i](const DMK::Config::KeyComboList &combos)
                                            { category_states()[i].toggleHotkeyCombos = combos; }, "V");

            DMK::Config::register_key_combo(section, "ShowHotkey", section + " Show Hotkey", [i](const DMK::Config::KeyComboList &combos)
                                            { category_states()[i].showHotkeyCombos = combos; }, "");

            DMK::Config::register_key_combo(section, "HideHotkey", section + " Hide Hotkey", [i](const DMK::Config::KeyComboList &combos)
                                            { category_states()[i].hideHotkeyCombos = combos; }, "");

            DMK::Config::register_bool(section, "DefaultHidden", section + " Default Hidden", [i](bool val)
                                       { category_states()[i].hidden.store(val, std::memory_order_relaxed); }, false);

            DMK::Config::register_string(section, "Parts", section + " Parts", [cat](const std::string &val)
                                         { register_parts(cat, val); }, "");
        }

        DMK::Config::load(INI_FILE);
        DMK::Config::log_all();
        build_part_lookup();
    }

    // =========================================================================
    // Hotkey helpers
    // =========================================================================

    /// Serialise a KeyComboList into a stable string key for deduplication.
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

    /// Trigger direct-write update after visibility state change.
    static void flush_visibility() noexcept
    {
        if (!s_fallbackMode.load(std::memory_order_relaxed))
        {
            resolve_player_vis_ctrls();
            apply_direct_vis_write();
        }
        s_needsDirectWrite.store(true, std::memory_order_relaxed);
    }

    // =========================================================================
    // Hotkey registration — categories sharing the same key toggle together
    // =========================================================================
    static void register_hotkeys()
    {
        auto &inputMgr = DMK::InputManager::get_instance();
        auto &states = category_states();
        auto &logger = DMK::Logger::get_instance();

        // --- Global Show All / Hide All ---
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
            logger.info("Registered hotkey binding 'ShowAll'");
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
            logger.info("Registered hotkey binding 'HideAll'");
        }

        // --- Per-category toggle (grouped by shared combo) ---
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

            logger.info("Registered hotkey binding '{}' for {} categories",
                        bindingName, indices.size());
        }

        // --- Per-category force show / force hide ---
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
                logger.info("Registered hotkey binding 'ShowEquip_{}'", section);
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
                logger.info("Registered hotkey binding 'HideEquip_{}'", section);
            }
        }
    }

    // =========================================================================
    // Public interface
    // =========================================================================
    bool init()
    {
        auto &logger = DMK::Logger::get_instance();

        logger.info("=== {} v{} ===", MOD_NAME, MOD_VERSION);

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

        if (!s_worldSystemPtr || !s_childActorVtbl)
        {
            s_fallbackMode.store(true, std::memory_order_relaxed);
            logger.info("Player identification: d8-based fallback (global chain AOB unavailable)");
        }
        else
        {
            logger.info("Player identification: global pointer chain");
        }

        // Attempt IndexedStringA table scan for runtime hash resolution.
        // The game's global pointer may not be populated yet during DllMain,
        // so we also schedule a deferred retry in the hook callback.
        if (s_mapLookupAddr)
        {
            auto runtimeHashes = scan_indexed_string_table(s_mapLookupAddr);
            if (!runtimeHashes.empty())
            {
                set_runtime_hashes(std::move(runtimeHashes));
            }
            else
            {
                logger.info("IndexedStringA table not ready at init, "
                            "deferring scan to first hook invocation");
                s_deferredScanPending.store(true, std::memory_order_relaxed);
            }
        }
        else
        {
            logger.warning("MapLookup not resolved, cannot scan IndexedStringA table");
        }

        load_config();

        // Pre-compile AOB patterns once
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

        // Scan all executable memory regions for the hook target.
        // The process gate in dllmain ensures we run after the protector
        // has finished unpacking, so a single scan pass is sufficient.
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

        register_hotkeys();

        auto &inputMgr = DMK::InputManager::get_instance();
        inputMgr.start();

        logger.info("Equip hide system initialized");
        return true;
    }

    void shutdown()
    {
        DMK::Logger::get_instance().info("{} shutting down...", MOD_NAME);
        DMK_Shutdown();
    }

} // namespace EquipHide
