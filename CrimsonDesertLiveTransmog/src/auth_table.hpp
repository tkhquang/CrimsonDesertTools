#pragma once

#include <cstddef>
#include <cstdint>

namespace Transmog::AuthTable
{
    /**
     * Memory geometry of the engine's authoritative equip table, hanging off ClientEquipSlotActorComponent.
     *
     * This is the ONE place these offsets are written down. They used to be duplicated across transmog.cpp,
     * transmog_apply.cpp and real_part_tear_down.cpp under two different naming schemes, with the array-base and count
     * offsets left as bare literals at six call sites. Every copy carried a comment pointing at the others, which is
     * the tell: the geometry moves as a UNIT on patch day, so splitting it across files means a partial edit is one
     * missed grep away, and a partial edit fails silently.
     *
     *     component + k_containerPtrOffset        -> container
     *     container + k_containerArrayBaseOffset  -> entry array base
     *     container + k_containerCountOffset      -> live entry count (dword)
     *     arrayBase + index * k_entryStride       -> entry
     *
     * The engine states the walk itself, with the component in the base register:
     *     mov rax,[<comp>+0x80] ; mov rdx,[rax+08] ; mov eax,[rax+10]
     * and the entry search loop states the stride and the tag offset literally:
     *     imul rcx,rax,0xD0 ; cmp [rdx+0xC8],r8w ; add rdx,0xD0
     *
     * Patch-day notes, kept here because they apply to the whole struct rather than to any one field:
     *
     * - A stale container offset fails SILENTLY and disables the mod. The neighbouring slot holds a packed scalar, not
     *   a pointer. On most component instances that scalar is small and the `< 0x10000` guard rejects it, so the apply
     *   path reports zero applied slots forever. On other instances it is large enough to PASS the guard, and the walk
     *   then reads garbage.
     * - A stale stride walks the array off-phase and reads garbage tags without ever faulting.
     * - The stride and the slot-tag offset always move TOGETHER by 8. The engine alternates between two known shapes:
     *   stride 0xC8 with the tag at +0xC0, and stride 0xD0 with the tag at +0xC8. Never assume the pair only grows --
     *   a patch can revert it to the narrower shape. The static_assert below encodes that pairing, so editing one
     *   without the other fails the build instead of corrupting the walk.
     * - The item id at +0x08 has not moved across any version this mod has shipped against.
     * - Slot tag VALUES are stable (see slot_metadata.hpp). Only their POSITION within the entry shifts.
     */
    inline constexpr std::ptrdiff_t k_containerPtrOffset = 0x80;
    inline constexpr std::ptrdiff_t k_containerArrayBaseOffset = 0x08;
    inline constexpr std::ptrdiff_t k_containerCountOffset = 0x10;

    inline constexpr std::ptrdiff_t k_entryStride = 0xD0;
    inline constexpr std::ptrdiff_t k_entryItemIdOffset = 0x08; // primary item word; 0xFFFF or 0 == empty
    inline constexpr std::ptrdiff_t k_entryGateOffset = 0x10;   // must be non-zero for a live entry
    inline constexpr std::ptrdiff_t k_entrySlotTagOffset = 0xC8;

    static_assert(k_entrySlotTagOffset + 0x08 == k_entryStride,
                  "auth-table entry stride and slot-tag offset move together by 8 -- update both, not one "
                  "(0xC8/0xC0 and 0xD0/0xC8 are the two shapes the engine alternates between).");

    /// Address of entry `index`. Arithmetic only -- the caller still owns the guarded read.
    inline constexpr std::uintptr_t entry_at(std::uintptr_t arrayBase, std::size_t index) noexcept
    {
        return arrayBase + index * static_cast<std::uintptr_t>(k_entryStride);
    }
} // namespace Transmog::AuthTable
