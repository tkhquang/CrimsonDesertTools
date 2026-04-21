#include "part_show_suppress.hpp"
#include "shared_state.hpp"

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

    // Slot name -> TransmogSlot index mapping. Kept separate from
    // s_slotHashes so init can look up each slot by its CD_* string.
    struct SlotNameEntry
    {
        std::size_t slot;
        const char *name;
    };

    static constexpr std::array<SlotNameEntry, k_slotCount> k_slotNames = {{
        {static_cast<std::size_t>(TransmogSlot::Helm),   "CD_Helm"},
        {static_cast<std::size_t>(TransmogSlot::Chest),  "CD_Upperbody"},
        {static_cast<std::size_t>(TransmogSlot::Cloak),  "CD_Cloak"},
        {static_cast<std::size_t>(TransmogSlot::Gloves), "CD_Hand"},
        {static_cast<std::size_t>(TransmogSlot::Boots),  "CD_Foot"},
    }};

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

        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            s_slotHashes[i].slot = k_slotNames[i].slot;
            s_slotHashes[i].hashes = {0, 0, 0, 0};

            auto it = nameToHash.find(k_slotNames[i].name);
            if (it == nameToHash.end())
            {
                logger.warning(
                    "[dispatch] slot hash missing: '{}' not found in "
                    "IndexedStringA -- suppression for slot {} disabled",
                    k_slotNames[i].name, k_slotNames[i].slot);
                continue;
            }

            s_slotHashes[i].hashes[0] = it->second;
            logger.info("[dispatch] slot hash resolved: {} = 0x{:X}",
                        k_slotNames[i].name, it->second);
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
            DMK::Logger::get_instance().info(
                "[dispatch] PartAddShow suppressed hash=0x{:04X}", idx);
            return 0;
        }

        return s_originalPartAddShow(a1, a2, partHashPtr, blend,
                                     a5, a6, a7, a8, a9);
    }

} // namespace Transmog::PartShowSuppress
