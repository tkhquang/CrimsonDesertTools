#ifndef TRANSMOG_PART_SHOW_SUPPRESS_HPP
#define TRANSMOG_PART_SHOW_SUPPRESS_HPP

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Transmog::PartShowSuppress
{
    /**
     * @brief Callback signature for the PartInOut direct-show entry point the PartAddShow anchor resolves.
     *
     * The game routes transition-time visual adds through this function, which bypasses the RealPartTearDown
     * scene-graph removal. A hook on it stops stale real-helm frames from flashing through during glide exits,
     * landings, and effect spawns.
     *
     * Signature matches EquipHide's gliding_fix port (x64 __fastcall, 9 args).
     */
    using PartAddShowFn = __int64(__fastcall *)(
        __int64 a1,
        std::uint8_t a2,
        std::uint64_t partHashPtr,
        float blend,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8,
        __int64 a9
    );

    /** @brief Inline hook callback. Short-circuits to 0 when the part is suppressed. */
    __int64 __fastcall on_part_add_show(
        __int64 a1,
        std::uint8_t a2,
        std::uint64_t partHashPtr,
        float blend,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8,
        __int64 a9
    );

    /**
     * @brief Publish the original function trampoline.
     * @param original The trampoline the hook backend hands back, or nullptr to retract it during teardown.
     * @note The store is a release, so a detour running on another thread reads either the old value or a fully
     *       published one.
     */
    void set_part_add_show_trampoline(PartAddShowFn original);

    /**
     * @brief Flag an IndexedStringA hash as suppressed.
     * @param partHash 32-bit IndexedStringA index (e.g. 0xADE8 for CD_Helm).
     * @param suppressed true adds the hash to the table, false removes it.
     * @note The table matches the full 32-bit hash, so two hashes that share a low half stay independent.
     */
    void set_hash_suppressed(std::uint32_t partHash, bool suppressed) noexcept;

    /**
     * @brief Wipe the entire suppression table.
     * @note The wipe publishes a single count of zero. A detour that already read a larger count can still observe a
     *       stale entry for the length of that one scan.
     */
    void clear_all_suppressed() noexcept;

    /**
     * @brief Populate the suppression table from a slot category mask. Bit N marks TransmogSlot::N for suppression.
     * @details Clears the table first, then enables suppression for every part hash mapped to each set bit.
     */
    void set_mask(std::uint32_t categoryMask) noexcept;

    /**
     * @brief Populate the slot to IndexedStringA hash table at runtime.
     *
     * @param nameToHash Map from CD_* part name to IndexedStringA bucket index (produced by
     *                   scan_indexed_string_table()).
     *
     * @details IndexedStringA buckets are volatile across game patches, so the slot hashes cannot be hardcoded. Call
     *          this before the first set_mask() or on_part_add_show() call. When the map is missing a slot name, that
     *          slot becomes unsuppressable for this session and the function logs a warning.
     *
     * @return Number of slots resolved (out of k_slotCount).
     */
    std::size_t init_slot_hashes(const std::unordered_map<std::string, std::uint32_t> &nameToHash) noexcept;

    /** @brief True if init_slot_hashes has populated at least one slot. */
    bool slot_hashes_ready() noexcept;

} // namespace Transmog::PartShowSuppress

#endif // TRANSMOG_PART_SHOW_SUPPRESS_HPP
