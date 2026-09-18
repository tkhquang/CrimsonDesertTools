#include "armor_injection.hpp"
#include "categories.hpp"
#include "shared_state.hpp"
#include "visibility_write.hpp" // vis_byte_offset: the live decode every vis-byte write shares

#include <DetourModKit/logger.hpp>
#include <DetourModKit/memory.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <vector>

namespace EquipHide
{
    // File-scope scratch buffers reused across inject_armor_entries_for_map calls. Cleared at function entry.
    // Serialized by vis_write_mutex, which stays held for the full call duration, so concurrent callers cannot race.
    // MSVC C2712 requires file scope rather than a function-scope std::vector or std::string: the function's
    // __try/__finally cannot coexist with objects that require C++ unwinding, and that includes a temporary returned
    // by value from a helper. Mirrors the s_touchedVisKeys pattern in visibility_write.cpp.
    static std::vector<uint32_t> s_v_injected;
    static std::vector<uint32_t> s_v_reinjected;
    static std::string s_v_joined;

    /**
     * @brief Renders @p v as a comma-joined hex list into @ref s_v_joined.
     * @details The per-call summary emit uses it. It lives at file scope rather than as a function-local lambda
     *          because a std::string returned by value puts a destructor-bearing temporary inside __try and
     *          re-triggers C2712.
     */
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
     * The map holds TWO payload shapes, keyed by part hash and discriminated by the part's own type. Do not model
     * them as one struct.
     *
     * An ARMOR entry (built on the PartInOut path) ends at its visible byte. Read that byte's position from
     * vis_byte_offset(), never a literal, so this file agrees with the direct-write path and the mid-hook.
     *
     * A SOCKET entry (built on the PartInOutSocket path, weapons and shields) is longer. The engine lays it out as
     * flags, three zeroed qword socket-bone slots, a zeroed dword, then a socket-state WORD at +0x20. It seeds that
     * word with 3 and later reads the byte at +0x21 back, comparing it against 1. The payload therefore runs to at
     * least +0x21.
     *
     * Consequences for anything inserting here. A fabricated all-zero payload is valid only for the armor shape. On
     * a socket entry it wipes the state word, the part attaches to nothing, and it renders at the actor origin: the
     * mesh lies at the character's feet and other socket parts disappear. An undersized buffer is worse, because the
     * engine then reads whatever follows it on the stack. Prefer copying the live entry and editing one byte.
     */
    using MapInsertFn = __int64 *(__fastcall *)(unsigned int *map_base,
                                                int **part_hash_pp,
                                                unsigned int bucket_key,
                                                __int64 entry_data,
                                                int extra,
                                                uint8_t *out_existed,
                                                __int64 *out_hash_ptr,
                                                __int64 *out_data_ptr);

    // Reject a mapBase that passed the plausible-pointer gate but does not look like a real part-visibility
    // hashtable. A stale or reallocated vis-ctrl descriptor (a companion despawned between the resolve pass and this
    // write) yields a garbage map: an oversized count, or a bucket pointer that lands in the image .rdata instead of
    // the heap. An insert into that map faults inside the game's MapInsert, and the SEH frame catches it noisily on
    // every frame.
    static bool part_vis_map_looks_valid(std::uintptr_t mapBase) noexcept
    {
        // Exactly the fields the game's MapInsert dereferences, verified from its disassembly: bucket modulus [+0],
        // live entry count [+4], capacity [+8], bucket array [+0x10], entry-pointer array [+0x18]. MapInsert *writes*
        // through [+0x18][entryCount], so a bad entry-pointer array, or a garbage capacity that skips the grow path,
        // faults inside the game. A stale or reallocated descriptor, or a wrong map offset after a struct re-layout,
        // fails one of these. Part-vis maps are tiny per-character hashtables.
        const auto count = DMK::memory::read<std::uint32_t>(DMK::Address{mapBase});
        const auto entryCount = DMK::memory::read<std::uint32_t>(DMK::Address{mapBase + 4});
        const auto cap = DMK::memory::read<std::uint32_t>(DMK::Address{mapBase + 8});
        const auto buckets = DMK::memory::read<std::uintptr_t>(DMK::Address{mapBase + 0x10});
        const auto entryPtrs = DMK::memory::read<std::uintptr_t>(DMK::Address{mapBase + 0x18});
        if (!count || !entryCount || !cap || !buckets || !entryPtrs)
            return false;
        if (*count == 0 || *count > 0x400)
            return false;
        if (*cap == 0 || *cap > 0x10000 || *entryCount > *cap)
            return false;
        if (!DMK::memory::is_plausible_ptr(DMK::Address{*buckets}) ||
            !DMK::memory::is_plausible_ptr(DMK::Address{*entryPtrs}))
            return false;
        // Both arrays MapInsert will index/write must be readable.
        return DMK::memory::read<std::uint32_t>(DMK::Address{*buckets}).has_value() &&
               DMK::memory::read<std::uintptr_t>(DMK::Address{*entryPtrs}).has_value();
    }

    // The body performs only guarded memory:: calls, which are noexcept and carry their own fault guard, so this
    // function needs no SEH frame of its own.
    static uint32_t compute_bucket_key(uint32_t partHash) noexcept
    {
        const auto globalAddr = resolved_addrs().indexedStringGlobal;
        if (!globalAddr)
            return 0;

        // Walk globalAddr -> [+0] -> [+0x58] to the bucket table.
        // The trailing 0 dereferences the +0x58 link so the result is the table pointer itself. Without it the walk
        // stops at the slot address and corrupts every bucket key.
        const auto tbl = DMK::memory::walk(DMK::Address{globalAddr}, std::array<std::ptrdiff_t, 3>{0x00, 0x58, 0x00});
        if (!tbl)
        {
            static std::atomic<bool> s_logOnce{false};
            if (!s_logOnce.exchange(true, std::memory_order_relaxed))
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "compute_bucket_key: tablePtr=NULL (globalAddr=0x{:X} +0x58)",
                    globalAddr
                );
            return 0;
        }

        // 16-byte stride table indexed by part hash. The bucket key lives at +8 within the entry. Kept as a separate
        // typed read (not a chain offset) so the indexed arithmetic stays explicit.
        return DMK::memory::read<uint32_t>(tbl->offset(static_cast<std::ptrdiff_t>(16ULL * partHash + 8))).value_or(0);
    }

    static int inject_armor_entries_for_map(uintptr_t mapBase, int charIdx) noexcept
    {
        auto &mtx = vis_write_mutex();
        if (!mtx.try_lock())
            return 0;

        // __try/__finally guarantees mtx.unlock() on every exit path, including an SEH raised by mtx.unlock()
        // itself. Note the limit: this function is noexcept, so a C++ throw inside it terminates the process at the
        // throw point and no handler here runs. Every logging step therefore goes through try_log. __leave skips to
        // __finally when the addrs guard short-circuits.
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

                const auto insert = reinterpret_cast<MapInsertFn>(addrs.mapInsert);
                const auto lookup = reinterpret_cast<MapLookupFn>(addrs.mapLookup);

                auto &logger = DMK::log();
                int injected = 0;
                int existing_set = 0;
                int skipped_key = 0;

                const bool cascadeOn = flag_cascade_fix().load(std::memory_order_relaxed);

                // Legs, gloves and boots share a ConditionalPartPrefab cascade with chest. A hidden chest deletes
                // their map entries, and an injected vis=0 for a visible body part keeps them alive.
                constexpr CategoryMask k_cascadeBodyMask =
                    category_bit(Category::Legs) | category_bit(Category::Gloves) | category_bit(Category::Boots);

                // The engine builds these parts through the PartInOutSocket path. A visible socket entry does not
                // need the body-armor cache flush, and a re-insert makes the engine re-process attachment state that
                // this mod does not own. That disturbs the game's attachment state for a drawn weapon. The normal
                // direct-write hide path stays intact. This guard applies only to a visible-entry re-insert.
                constexpr CategoryMask k_socketPartMask =
                    category_bit(Category::OneHandWeapons) | category_bit(Category::TwoHandWeapons) |
                    category_bit(Category::Shields) | category_bit(Category::Bows) |
                    category_bit(Category::SpecialWeapons) | category_bit(Category::Tools) |
                    category_bit(Category::Lanterns);

                // The per-character map drives both the part list AND the hide-mask classification. charIdx=-1
                // (unknown body or fallback path) collapses to the active-character map through get_part_map_for and
                // is_any_category_hidden_for, so an unidentified slot keeps single-character behavior.
                const auto &partMap = (charIdx >= 0 && charIdx < static_cast<int>(k_charIdxCount))
                                          ? get_part_map_for(charIdx)
                                          : get_part_map();

                int reinjected = 0;
                int socket_reinjection_skipped = 0;

                for (const auto &[hash, mask] : partMap)
                {
                    const bool hidden = is_any_category_hidden_for(mask, charIdx);
                    const auto existing = lookup(mapBase, &hash);

                    if (!hidden)
                    {
                        // Toggle-off cache flush. The engine caches a hidden render-state when this pass inserts an
                        // entry with vis=2. That cache lives in a struct field no direct vis-byte write reaches, so
                        // body armor stays visually hidden after toggle-off unless the entry is re-inserted with
                        // vis=0. The re-insert path below forces the engine to re-process the entry and clear its
                        // cached hidden state. A visible part with no existing entry has nothing to flush, so skip it
                        // unless the cascade-fix path needs it.
                        if (existing && (mask & k_socketPartMask) != 0)
                        {
                            ++socket_reinjection_skipped;
                            continue;
                        }

                        if (!existing)
                        {
                            if (!cascadeOn || (mask & k_cascadeBodyMask) == 0)
                                continue;
                        }
                    }
                    else if (existing)
                    {
                        // Hidden category with the entry already present. The direct-write path updates the vis byte
                        // in place, so this pass needs no re-insert.
                        ++existing_set;
                        continue;
                    }

                    const auto bucketKey = compute_bucket_key(hash);
                    if (bucketKey == 0)
                    {
                        (void)logger.try_log(DMK::LogLevel::Trace, "  0x{:X} - skipped (no bucket key)", hash);
                        ++skipped_key;
                        continue;
                    }

                    // Payload for the insert. PRESERVE an existing entry rather than fabricate one. The map holds
                    // two different payload shapes. An armor entry ends at its visible byte, which this module models
                    // correctly. A PartInOutSocket entry is longer: it carries socket state in a word at +0x20 whose
                    // high byte the engine reads back and compares against 1, and the engine seeds that word with 3.
                    //
                    // A fabricated all-zero payload therefore destroys a socket entry. The part loses its socket
                    // state, attaches to nothing, and renders at the actor origin. In game that reads as weapons on
                    // the ground at the character's feet, with other socket parts gone. Every registered part reaches
                    // this loop, parts of a DISABLED category included, so socket entries arrive here routinely.
                    //
                    // A copy of the live payload keeps the flush to its one job: re-insert the same bytes so the
                    // engine re-processes the entry and drops its cached hidden state. Only the hide path writes a
                    // byte, and only into an entry this module hides.
                    constexpr std::size_t k_entryDataSize = 64;
                    alignas(8) uint8_t entryData[k_entryDataSize] = {};
                    const auto visOff = vis_byte_offset();

                    const bool preserved =
                        existing &&
                        DMK::memory::read_into(DMK::Address{existing}, std::as_writable_bytes(std::span{entryData}))
                            .has_value();

                    if (!preserved)
                    {
                        // No live payload to copy. This is a fresh armor entry (a hidden part with no map row, or the
                        // cascade-fix body row), so the armor shape is the right model.
                        if (visOff < k_entryDataSize)
                            entryData[visOff] = hidden ? 2 : 0;
                    }
                    else if (hidden && visOff < k_entryDataSize)
                    {
                        entryData[visOff] = 2;
                    }

                    uint32_t hashCopy = hash;
                    int *hashPtr = reinterpret_cast<int *>(&hashCopy);
                    uint8_t outExisted = 0;
                    __int64 outHashPtr = 0;
                    __int64 outDataPtr = 0;

                    auto *const mapBasePtr = reinterpret_cast<unsigned int *>(mapBase);

                    insert(
                        mapBasePtr,
                        &hashPtr,
                        bucketKey,
                        reinterpret_cast<__int64>(entryData),
                        0,
                        &outExisted,
                        &outHashPtr,
                        &outDataPtr
                    );

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
                    (
                        void
                    )logger.try_log(DMK::LogLevel::Debug, "  injected new ({}): {}", s_v_injected.size(), s_v_joined);
                }
                if (!s_v_reinjected.empty() && logger.is_enabled(DMK::LogLevel::Trace))
                {
                    build_hex_joined(s_v_reinjected);
                    (void)logger.try_log(
                        DMK::LogLevel::Trace,
                        "  re-injected visible cache-flush ({}): {}",
                        s_v_reinjected.size(),
                        s_v_joined
                    );
                }

                (void)logger.try_log(
                    DMK::LogLevel::Debug,
                    "ArmorInject map: {} injected, {} existing updated, "
                    "{} re-injected, {} socket re-injections skipped, {} skipped (no bucket key)",
                    injected,
                    existing_set,
                    reinjected,
                    socket_reinjection_skipped,
                    skipped_key
                );
                result = injected;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                static std::atomic<bool> s_crashLogged{false};
                if (!s_crashLogged.exchange(true, std::memory_order_relaxed))
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Warning,
                        "ArmorInject: SEH caught crash during map insertion"
                    );
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

        // Hidden-state masks (read by is_any_category_hidden_for) are global, so the global anyHidden short-circuit
        // stays correct: when no category is hidden anywhere, every per-character query also returns false. The
        // cascade-fix path also writes vis=0 with no category hidden. This pass skips it, because it re-fires on
        // every tick in that case.
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
            // Reset the injection flags so the next hide toggle re-updates every existing entry and sets its vis
            // byte back to 2.
            for (int i = 0; i < k_maxProtagonists; ++i)
                ps.armorInjected[i].store(false, std::memory_order_relaxed);
            return;
        }

        const auto n = ps.count.load(std::memory_order_relaxed);
        if (n <= 0)
            return;

        auto &logger = DMK::log();
        int totalInjected = 0;

        for (int i = 0; i < n; ++i)
        {
            if (ps.armorInjected[i].load(std::memory_order_relaxed))
                continue;

            const auto vc = ps.visCtrls[i].load(std::memory_order_relaxed);
            if (!vc)
                continue;

            // Per-slot character idx. -1 (unknown or fallback path) routes the injection through the
            // active-character map, so an unidentified slot keeps single-character behavior.
            const int charIdx = ps.visCharIdx[i].load(std::memory_order_relaxed);

            // Per-player SEH so one bad pointer does not skip the rest.
            __try
            {
                // Resolve the part-info descriptor and its part-visibility map. Offsets and the re-verification
                // recipe live on the k_visCtrl* constants in visibility_write.hpp. The direct-write pass walks the
                // same three. The walk stops at the descriptor SLOT, so the trailing read is what yields the
                // descriptor pointer itself.
                const auto desc =
                    DMK::memory::walk(
                        DMK::Address{vc},
                        std::array<std::ptrdiff_t, 2>{k_visCtrlToCccOffset, k_cccToDescriptorOffset}
                    )
                        .and_then([](DMK::Address leaf) { return DMK::memory::read<std::uintptr_t>(leaf); });
                if (!desc)
                {
                    (void)logger.try_log(
                        DMK::LogLevel::Trace,
                        "ArmorInject [{}]: vc=0x{:X} descriptor=NULL (+{:#x} -> +{:#x})",
                        i,
                        vc,
                        k_visCtrlToCccOffset,
                        k_cccToDescriptorOffset
                    );
                    continue;
                }
                const auto mapBase = *desc + k_descriptorToPartVisMapOffset;

                // Reject a non-faulting garbage mapBase from a drifted chain. The guarded read traps an actual
                // fault, not a wrong-but-mapped pointer. Skip this vis-controller rather than inject into a wrong
                // map.
                if (!DMK::memory::is_plausible_ptr(DMK::Address{mapBase}))
                {
                    (void)logger.try_log(
                        DMK::LogLevel::Trace,
                        "ArmorInject [{}]: vc=0x{:X} implausible mapBase=0x{:X} (+{:#x} -> +{:#x} -> +{:#x})",
                        i,
                        vc,
                        mapBase,
                        k_visCtrlToCccOffset,
                        k_cccToDescriptorOffset,
                        k_descriptorToPartVisMapOffset
                    );
                    continue;
                }

                if (!part_vis_map_looks_valid(mapBase))
                {
                    (void)logger.try_log(
                        DMK::LogLevel::Trace,
                        "ArmorInject [{}]: vc=0x{:X} mapBase=0x{:X} not a valid part-vis map "
                        "(stale/reallocated descriptor) - skipping",
                        i,
                        vc,
                        mapBase
                    );
                    continue;
                }

                (void)logger.try_log(
                    DMK::LogLevel::Trace,
                    "ArmorInject [{}]: vc=0x{:X} descriptor=0x{:X} mapBase=0x{:X} char_idx={}",
                    i,
                    vc,
                    *desc,
                    mapBase,
                    charIdx
                );

                const int result = inject_armor_entries_for_map(mapBase, charIdx);
                if (result >= 0)
                {
                    ps.armorInjected[i].store(true, std::memory_order_relaxed);
                    totalInjected += result;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // Fail closed and loud. A silent swallow makes a per-player fault invisible while the pass still
                // reports success for every other protagonist.
                static std::atomic<bool> s_playerFaultLogged{false};
                if (!s_playerFaultLogged.exchange(true, std::memory_order_relaxed))
                    (void)logger.try_log(DMK::LogLevel::Warning, "ArmorInject: SEH caught crash on a protagonist");
            }
        }

        if (totalInjected > 0)
            (void)logger.try_log(
                DMK::LogLevel::Info,
                "ArmorInject: {} new entries injected across {} protagonists",
                totalInjected,
                n
            );
        else
            (void)logger.try_log(DMK::LogLevel::Debug, "ArmorInject: 0 new entries (all existed or no hidden parts)");
    }

} // namespace EquipHide
