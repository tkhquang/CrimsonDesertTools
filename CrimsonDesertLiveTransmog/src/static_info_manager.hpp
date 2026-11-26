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
     *     manager + ENTRY_ARRAY_OFFSET                      -> row array base
     *     manager + COUNT_OFFSET                            -> row count (dword)
     *     array_base + index * ENTRY_STRIDE + DEF_OFFSET    -> definition pointer for row `index`
     *     array_base + index * ENTRY_STRIDE + NAME_OFFSET   -> name wrapper for row `index`
     *
     * The engine states the walk itself in its index lookup, one template instance per manager class,
     * `def *lookup(Manager *, u32 index)`:
     *     cmp edx,[rcx+0x20] ; jae <miss> ; ... ; lea rdi,[rbp*2] ; add rdi,rbp ; shl rdi,4 ; add rdi,[rsi+0x18]
     *     ... ; mov rax,[rdi+0x20]
     * A row is `{u32 key, u32, name wrapper*, u64, def*, u64 expiry}`. The key is the catalog key (a skill id, a
     * string hash), not the row index, so a row is always addressed by index and its key is content.
     *
     * The row owns the NAME: its wrapper is the `{char*, u32 length}` shape. A row with no name holds the engine's
     * static empty wrapper. A definition opens on `{u32 key, u32 row index}` and carries no name of its own.
     *
     * @warning A stale offset fails SILENTLY. The neighboring fields hold other heap pointers and packed counts, so
     *          the plausibility guards pass and a walk yields an empty or garbage catalog instead of a fault. Measure
     *          all five values against live memory after a patch, and read them off the lookup body above.
     * @note A row's definition pointer is null until the engine loads it on first lookup, and a manager in streaming
     *       mode can drop it again. Every reader treats a null pointer as "no row", never as the end of the array.
     */
    inline constexpr std::ptrdiff_t ENTRY_ARRAY_OFFSET = 0x18;
    inline constexpr std::ptrdiff_t COUNT_OFFSET = 0x20;

    inline constexpr std::size_t ENTRY_STRIDE = 0x30;

    /// Name-wrapper pointer inside a row.
    inline constexpr std::ptrdiff_t NAME_OFFSET = 0x08;

    /// Definition pointer inside a row.
    inline constexpr std::ptrdiff_t DEF_OFFSET = 0x20;

    /// Catalog key (u32) at the head of a definition. It repeats the row key.
    inline constexpr std::ptrdiff_t DEF_KEY_OFFSET = 0x00;

    /// Address of the definition-pointer slot of row `index`. Arithmetic only, so the caller still owns the read.
    [[nodiscard]] inline constexpr std::uintptr_t def_slot(std::uintptr_t array_base, std::size_t index) noexcept
    {
        return array_base + index * static_cast<std::uintptr_t>(ENTRY_STRIDE) + static_cast<std::uintptr_t>(DEF_OFFSET);
    }

    /// Address of the name-wrapper slot of row `index`. Arithmetic only, so the caller still owns the read.
    [[nodiscard]] inline constexpr std::uintptr_t name_slot(std::uintptr_t array_base, std::size_t index) noexcept
    {
        return array_base + index * static_cast<std::uintptr_t>(ENTRY_STRIDE) +
               static_cast<std::uintptr_t>(NAME_OFFSET);
    }
} // namespace Transmog::static_info

#endif // TRANSMOG_STATIC_INFO_MANAGER_HPP
