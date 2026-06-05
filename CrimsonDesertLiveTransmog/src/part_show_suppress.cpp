#include "part_show_suppress.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstring>

namespace Transmog::PartShowSuppress
{
    // Indexed by the low 16 bits of the IndexedStringA hash the game passes
    // to sub_14081DC20 in R8. 1 = suppress (return 0 from the show handler),
    // 0 = pass through. Mirrors EquipHide's s_hideLocked layout.
    static std::atomic<uint8_t> s_suppressedHashes[0x10000]{};

    static PartAddShowFn s_originalPartAddShow = nullptr;

    // Slot -> IndexedStringA indices. IndexedStringA buckets rebuild on
    // every game patch, so these are resolved at runtime via
    // init_slot_hashes() from a prior scan_indexed_string_table() pass.
    struct SlotHashes
    {
        std::size_t slot;
        std::array<uint32_t, 4> hashes; // zero-terminated, unused = 0
    };

    // The (TransmogSlot -> "CD_*") slot-name table sourced from
    // slot_metadata.hpp's `partShowHashKey` field. Only the 5 original
    // armor slots have CD_* slot-suffix hashes in IndexedStringA --
    // accessory and weapon slots store nullptr there and are filtered
    // out below. (Replaces a local 5-entry table; extending the
    // TransmogSlot enum is now safe -- nullptr trailing entries are
    // skipped explicitly.)

    // s_slotHashes stays sized to k_slotCount because callers index it
    // by TransmogSlot value (which can be any of the 20 entries). The
    // 15 trailing slots have all-zero hashes and act as a no-op for
    // suppression -- correct behavior since accessories don't use
    // this hash-driven path.
    static std::array<SlotHashes, k_slotCount> s_slotHashes{};
    static std::atomic<bool> s_slotHashesReady{false};

    void set_part_add_show_trampoline(PartAddShowFn original)
    {
        s_originalPartAddShow = original;
    }

    void set_hash_suppressed(uint32_t partHash, bool suppressed) noexcept
    {
        const uint32_t idx = partHash & 0xFFFF;
        s_suppressedHashes[idx].store(suppressed ? 1u : 0u,
                                      std::memory_order_relaxed);
    }

    void clear_all_suppressed() noexcept
    {
        for (std::size_t i = 0; i < 0x10000; ++i)
            s_suppressedHashes[i].store(0, std::memory_order_relaxed);
    }

    void set_mask(uint32_t categoryMask) noexcept
    {
        clear_all_suppressed();
        if (!s_slotHashesReady.load(std::memory_order_acquire))
            return;

        for (const auto &entry : s_slotHashes)
        {
            if ((categoryMask & (uint32_t{1} << entry.slot)) == 0)
                continue;
            for (uint32_t h : entry.hashes)
            {
                if (h != 0)
                    set_hash_suppressed(h, true);
            }
        }
    }

    std::size_t init_slot_hashes(
        const std::unordered_map<std::string, uint32_t> &nameToHash) noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        std::size_t resolved = 0;

        // Walk slot_metadata; only rows where partShowHashKey is set
        // (the 5 armor slots) participate in IndexedStringA-driven
        // suppression. Other rows (accessories, weapons) keep their
        // zero-initialized s_slotHashes entry as a no-op so set_mask
        // calls for those bits silently pass through.
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            s_slotHashes[i].slot = i;
            s_slotHashes[i].hashes = {0, 0, 0, 0};

            const char *partShowKey = k_slotMetadata[i].partShowHashKey;
            if (!partShowKey || partShowKey[0] == '\0')
                continue;

            auto it = nameToHash.find(partShowKey);
            if (it == nameToHash.end())
            {
                logger.warning(
                    "[dispatch] slot hash missing: '{}' not found in "
                    "IndexedStringA -- suppression for slot {} disabled",
                    partShowKey, i);
                continue;
            }

            s_slotHashes[i].hashes[0] = it->second;
            logger.info("[dispatch] slot hash resolved: {} = 0x{:X}",
                        partShowKey, it->second);
            ++resolved;
        }

        s_slotHashesReady.store(resolved > 0, std::memory_order_release);
        return resolved;
    }

    bool slot_hashes_ready() noexcept
    {
        return s_slotHashesReady.load(std::memory_order_acquire);
    }

    // Diagnostic: remember which 16-bit hashes we've already logged so each
    // unique hash shows up once per session. Uses the same 0x10000-byte
    // layout as s_suppressedHashes so lookups stay O(1) and branch-free.
    static std::atomic<uint8_t> s_hashSeen[0x10000]{};

    static uint32_t read_hash_safe(uint64_t partHashPtr) noexcept
    {
        if (partHashPtr < 0x10000)
            return 0;
        __try
        {
            return *reinterpret_cast<const uint32_t *>(partHashPtr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    __int64 __fastcall on_part_add_show(
        __int64 a1, uint8_t a2, uint64_t partHashPtr, float blend,
        __int64 a5, __int64 a6, __int64 a7, __int64 a8, __int64 a9)
    {
        // Snapshot the trampoline pointer at entry. SafetyHook drains
        // in-flight callers under its own shared lock, but the brief
        // teardown window between hook removal and the loader unmapping
        // the Logic DLL can still see this body executing with a torn
        // trampoline if a game thread re-enters after the drain. A
        // null snapshot means teardown is in progress; bailing to 0
        // (the suppressed-return convention) mirrors what an
        // early-cancelled call would have produced.
        const auto trampoline = s_originalPartAddShow;
        if (!trampoline)
            return 0;

        const uint32_t partHash = read_hash_safe(partHashPtr);
        const uint32_t idx = partHash & 0xFFFF;
        const bool enabled = flag_enabled().load(std::memory_order_relaxed);
        const bool suppress =
            enabled && partHash != 0 &&
            s_suppressedHashes[idx].load(std::memory_order_relaxed) != 0;

        if (partHash != 0 &&
            s_hashSeen[idx].exchange(1, std::memory_order_relaxed) == 0)
        {
            DMK::Logger::get_instance().trace(
                "[dispatch] PartAddShow unique hash=0x{:08X} low16=0x{:04X} "
                "a2={:#04x} enabled={} suppress={}",
                partHash, idx, static_cast<unsigned>(a2), enabled, suppress);
        }

        if (suppress)
        {
            // Per-suppress diagnostic kept at trace level -- this fires on
            // the hot PartAddShow dispatch path, so INFO would flood the log.
            DMK::Logger::get_instance().trace(
                "[dispatch] PartAddShow suppressed hash=0x{:04X}", idx);
            return 0;
        }

        return trampoline(a1, a2, partHashPtr, blend,
                          a5, a6, a7, a8, a9);
    }

} // namespace Transmog::PartShowSuppress
