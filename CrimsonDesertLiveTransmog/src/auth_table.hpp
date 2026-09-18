#ifndef TRANSMOG_AUTH_TABLE_HPP
#define TRANSMOG_AUTH_TABLE_HPP

#include <cstddef>
#include <cstdint>

namespace Transmog::auth_table
{
    /**
     * @brief Memory geometry of the engine's authoritative equip table, hanging off ClientEquipSlotActorComponent.
     *
     * This is the ONE place these offsets are written down, and they must stay that way. The geometry moves as a
     * UNIT on patch day, so splitting it across the files that walk it (transmog.cpp, transmog_apply.cpp,
     * real_part_tear_down.cpp) leaves a partial edit one missed grep away. A partial edit fails silently.
     *
     *     component + CONTAINER_PTR_OFFSET        -> container
     *     container + CONTAINER_ARRAY_BASE_OFFSET  -> entry array base
     *     container + CONTAINER_COUNT_OFFSET      -> live entry count (dword)
     *     array_base + index * ENTRY_STRIDE       -> entry
     *
     * The engine states the walk itself, with the component in the base register:
     *     mov rax,[<comp>+0x90] ; mov rdx,[rax+08] ; mov eax,[rax+10]
     * and the entry search loop states the stride and the tag offset literally:
     *     imul rcx,rax,0xD0 ; cmp [rdx+0xC8],r8w ; add rdx,0xD0
     *
     * @warning A stale container offset fails SILENTLY and disables the mod. The neighboring slot holds a packed
     *          scalar, not a pointer. On most component instances that scalar is small and the `< 0x10000` guard
     *          rejects it, so the apply path reports zero applied slots forever. On other instances it is large
     *          enough to PASS the guard, and the walk then reads garbage.
     * @warning A stale stride walks the array off-phase and reads garbage tags without ever faulting.
     * @note The stride and the slot-tag offset always move TOGETHER by 8. The engine alternates between two known
     *       shapes: stride 0xC8 with the tag at +0xC0, and stride 0xD0 with the tag at +0xC8. Never assume the pair
     *       only grows. A patch can revert it to the narrower shape. The static_assert below encodes that pairing, so
     *       editing one without the other fails the build instead of corrupting the walk.
     * @note The item id sits at +0x08.
     * @note Slot tag VALUES are stable (see slot_metadata.hpp). Only their POSITION within the entry shifts.
     */
    inline constexpr std::ptrdiff_t CONTAINER_PTR_OFFSET = 0x90;
    inline constexpr std::ptrdiff_t CONTAINER_ARRAY_BASE_OFFSET = 0x08;
    inline constexpr std::ptrdiff_t CONTAINER_COUNT_OFFSET = 0x10;

    inline constexpr std::ptrdiff_t ENTRY_STRIDE = 0xD0;

    /// Primary item word. 0xFFFF or 0 marks an empty entry.
    inline constexpr std::ptrdiff_t ENTRY_ITEM_ID_OFFSET = 0x08;

    /// Live-entry gate. A live entry holds a non-zero value here.
    inline constexpr std::ptrdiff_t ENTRY_GATE_OFFSET = 0x10;

    inline constexpr std::ptrdiff_t ENTRY_SLOT_TAG_OFFSET = 0xC8;

    static_assert(
        ENTRY_SLOT_TAG_OFFSET + 0x08 == ENTRY_STRIDE,
        "auth-table entry stride and slot-tag offset move together by 8. Update both, not one "
        "(0xC8/0xC0 and 0xD0/0xC8 are the two shapes the engine alternates between)."
    );

    /// Address of entry `index`. Arithmetic only, so the caller still owns the guarded read.
    [[nodiscard]] inline constexpr std::uintptr_t entry_at(std::uintptr_t array_base, std::size_t index) noexcept
    {
        return array_base + index * static_cast<std::uintptr_t>(ENTRY_STRIDE);
    }
} // namespace Transmog::auth_table

#endif // TRANSMOG_AUTH_TABLE_HPP
