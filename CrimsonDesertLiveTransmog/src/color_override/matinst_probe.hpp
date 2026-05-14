#pragma once

// Shared matInst identity probe + submesh-name reader.
//
// Both the publisher hook (color_publisher_hook.cpp) and the setter
// detour (color_override/setter_substitute.cpp) need to read identity fields off
// a live matInst (template id, stable id, content hash) and the
// submesh name off its wrapper. Centralised here so the v1.06 struct
// offsets + heap-floor sanity live in one place.
//
// All readers are SEH-guarded and `noexcept`. Bad pointers / freed
// memory return `false`; no exception escapes.

#include <cstddef>
#include <cstdint>

namespace Transmog::ColorOverride::MatInstProbe
{
    // ---- Engine struct offsets ---------------------------------------
    //
    // Centralised here so a future patch that shifts them needs a
    // single edit; every reader picks up the new value at once.
    inline constexpr std::ptrdiff_t k_offMi_PermutTok     = 0x70;
    inline constexpr std::ptrdiff_t k_offMi_TemplateId    = 0x48;
    inline constexpr std::ptrdiff_t k_offMi_StableId      = 0x80;
    inline constexpr std::ptrdiff_t k_offMi_ArecBackref   = 0xA0;
    inline constexpr std::ptrdiff_t k_offArec_ContentHash = 0x40;

    // Material -> SkinnedMeshMaterialWrapper backref, and the
    // wrapper's `_subMeshName` string-wrapper field offsets.
    inline constexpr std::ptrdiff_t k_offMat_WrapperBackref = 0x10;
    inline constexpr std::ptrdiff_t k_offWrapper_SubMeshNameSw = 0x28;
    inline constexpr std::ptrdiff_t k_offStringWrapper_Inline = 0x18;

    // ---- Address range sanity ----------------------------------------
    //
    // Engine heap pool is typically `0x300000000`+; module image is
    // `0x140000000..0x150000000` on v1.06 (no ASLR shift observed).
    inline constexpr std::uintptr_t k_heapFloor   = 0x200000000ULL;
    inline constexpr std::uintptr_t k_heapCeiling = 0x800000000000ULL;
    inline constexpr std::uintptr_t k_moduleLow   = 0x140000000ULL;
    inline constexpr std::uintptr_t k_moduleHigh  = 0x150000000ULL;

    inline bool is_likely_heap(std::uintptr_t p) noexcept
    {
        return p >= k_heapFloor && p < k_heapCeiling;
    }

    inline bool is_module_resident(std::uintptr_t p) noexcept
    {
        return p >= k_moduleLow && p < k_moduleHigh;
    }

    // ---- Identity probe ----------------------------------------------

    struct MatInstFields
    {
        std::uintptr_t mi{};
        std::uintptr_t vtable{};
        std::uint32_t  content_hash{};
        std::uint64_t  stable_id{};
        std::uint16_t  template_id{};
    };

    /// Probe a matInst pointer directly. Returns true with `out`
    /// filled on success; false on bad pointer / SEH fault.
    bool probe_matinst(std::uintptr_t mi, MatInstFields &out) noexcept;

    /// Probe via a wrapper-like struct whose `+0x10` field points to
    /// the matInst. Used by the setter detour where ctx.rdi is the
    /// wrapper, not the matInst itself.
    bool probe_from_wrapper(std::uintptr_t wrapper,
                            MatInstFields &out) noexcept;

    // ---- Submesh-name reader -----------------------------------------

    /// Read `_subMeshName` ASCIIZ from the SkinnedMeshMaterialWrapper
    /// parent of `material`. Returns true with `out` filled (null-
    /// terminated) on success. Returns false and writes `out[0]='\0'`
    /// on SEH, missing back-pointer, module-resident empty-string
    /// sentinel, or empty string.
    ///
    /// Defensive layers:
    ///   * material / wrapper pointers must be heap-resident
    ///   * wrapper's vtable (at +0) must be module-resident -- catches
    ///     stale-pointer reads into reallocated heap garbage
    ///   * string-wrapper pointer must be heap-resident (rejects the
    ///     module-resident empty-string sentinel the engine uses for
    ///     "no name set")
    ///   * each character must be printable ASCII (rejects UTF-16 /
    ///     binary fragments from reallocated objects)
    bool read_submesh_name(std::uintptr_t material,
                           char *out, std::size_t out_cap) noexcept;
}
