#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Transmog::PartShowSuppress
{
    /**
     * @brief PartInOut direct-show bypass hook callback signature (sub_14081DC20).
     *
     * The game routes transition-time visual adds through this function,
     * which bypasses the RealPartTearDown scene-graph removal. Hooking it
     * lets us stop stale real-helm frames from flashing through during
     * glide exits, landings, and effect spawns.
     *
     * Signature matches EquipHide's gliding_fix port (x64 __fastcall, 9 args).
     */
    using PartAddShowFn = __int64(__fastcall *)(
        __int64 a1, uint8_t a2, uint64_t partHashPtr, float blend,
        __int64 a5, __int64 a6, __int64 a7, __int64 a8, __int64 a9);

    /** @brief Inline hook callback. Short-circuits to 0 when the part is suppressed. */
    __int64 __fastcall on_part_add_show(
        __int64 a1, uint8_t a2, uint64_t partHashPtr, float blend,
        __int64 a5, __int64 a6, __int64 a7, __int64 a8, __int64 a9);

    /** @brief Store the original function trampoline after hook installation. */
    void set_part_add_show_trampoline(PartAddShowFn original);

    /**
     * @brief Flag an IndexedStringA hash as suppressed. Indexed by low 16 bits.
     * @param partHash 32-bit IndexedStringA index (e.g. 0xADE8 for CD_Helm).
     */
    void set_hash_suppressed(uint32_t partHash, bool suppressed) noexcept;

    /** @brief Wipe the entire suppression table (called by clear_all_transmog). */
    void clear_all_suppressed() noexcept;

    /**
     * @brief Populate the suppression table from a slot category mask.
     *        Bit N = TransmogSlot::N should be suppressed.
     * @details Clears table first, then enables suppression for every part
     *          hash mapped to each set bit.
     */
    void set_mask(uint32_t categoryMask) noexcept;

    /**
     * @brief Populate the slot -> IndexedStringA hash table at runtime.
     *
     * @param nameToHash Map from CD_* part name to IndexedStringA bucket
     *                   index (produced by scan_indexed_string_table()).
     *
     * @details IndexedStringA buckets are volatile across game patches, so
     *          the slot hashes can't be hardcoded. This must be called
     *          before the first set_mask() / on_part_add_show() call. If
     *          the map is missing a slot name, that slot silently becomes
     *          unsuppressable for this session and a warning is logged.
     *
     * @return Number of slots successfully resolved (out of k_slotCount).
     */
    std::size_t init_slot_hashes(
        const std::unordered_map<std::string, uint32_t> &nameToHash) noexcept;

    /** @brief True if init_slot_hashes has populated at least one slot. */
    bool slot_hashes_ready() noexcept;

} // namespace Transmog::PartShowSuppress
