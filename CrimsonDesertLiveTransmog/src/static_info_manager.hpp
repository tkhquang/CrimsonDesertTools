#ifndef TRANSMOG_STATIC_INFO_MANAGER_HPP
#define TRANSMOG_STATIC_INFO_MANAGER_HPP

#include <cstddef>
#include <cstdint>

namespace Transmog::static_info
{
    /**
     * @brief Memory geometry of the engine's pa::StaticInfoManager2 base, shared by every pa::*InfoManager.
     *
     * iteminfo, ItemGroupInfo, StringInfo and SkillInfo all derive from this base, so this is the ONE place their
     * container offsets are written down. The geometry moves as a UNIT on patch day, and a partial edit across the
     * files that walk it (item_name_table.cpp, prefab_wrapper_swap.cpp, itemmesh_dumper.cpp) fails silently.
     *
     *     manager + ENTRY_ARRAY_OFFSET                      -> entry array base
     *     manager + COUNT_OFFSET                            -> entry count (dword)
     *     array_base + index * ENTRY_STRIDE + DEF_OFFSET    -> definition pointer for row `index`
     *
     * The engine states the walk itself in every member of its u16-tag resolver family:
     *     movzx r8d,word [rcx] ; mov rcx,[<manager global>] ; cmp r8d,[rcx+0x28]
     *     mov rax,[rcx+0x20] ; lea rdx,[r8+r8*2] ; cmp qword [rax+rdx*8+8],0
     * A row is `{u32 key, u32, def*, u64}`. The key half is the catalog key (a skill id, a string hash), not the row
     * index, so a row is always addressed by index and its key is content.
     *
     * @warning A stale offset fails SILENTLY. The neighboring fields hold other heap pointers and packed counts, so
     *          the plausibility guards pass and a walk yields an empty or garbage catalog instead of a fault. Measure
     *          all four values against live memory after a patch, and read them off the resolver body above.
     * @note A row's definition pointer is null until the engine loads it on first lookup. Every reader treats a null
     *       pointer as "no row", never as the end of the array.
     */
    inline constexpr std::ptrdiff_t ENTRY_ARRAY_OFFSET = 0x20;
    inline constexpr std::ptrdiff_t COUNT_OFFSET = 0x28;

    inline constexpr std::size_t ENTRY_STRIDE = 0x18;

    /// Definition pointer inside a row.
    inline constexpr std::ptrdiff_t DEF_OFFSET = 0x08;

    /// Address of the definition-pointer slot of row `index`. Arithmetic only, so the caller still owns the read.
    [[nodiscard]] inline constexpr std::uintptr_t def_slot(std::uintptr_t array_base, std::size_t index) noexcept
    {
        return array_base + index * static_cast<std::uintptr_t>(ENTRY_STRIDE) + static_cast<std::uintptr_t>(DEF_OFFSET);
    }
} // namespace Transmog::static_info

#endif // TRANSMOG_STATIC_INFO_MANAGER_HPP
