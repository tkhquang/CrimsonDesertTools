#pragma once

#include <cstdint>

namespace Transmog
{
    // --- Hook function typedefs ---

    using VisualEquipChangeFn = __int64(__fastcall *)(__int64, __int16, __int16, __int64);
    using BatchEquipFn = uint32_t *(__fastcall *)(__int64, uint32_t *, __int64 **, __int64 **);

    // --- Trampoline accessors ---

    VisualEquipChangeFn &orig_vec();
    BatchEquipFn &orig_batch_equip();

    // --- Hook callbacks ---

    __int64 __fastcall on_vec(__int64 a1, __int16 slotId, __int16 itemId, __int64 a4);
    uint32_t *__fastcall on_batch_equip(__int64 a1, uint32_t *a2, __int64 **a3, __int64 **a4);

    /// Player detection: *(*(actor+0x88)+1) == 1.
    [[nodiscard]] bool is_player_actor(__int64 a1) noexcept;

} // namespace Transmog
