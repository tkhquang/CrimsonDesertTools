#ifndef TRANSMOG_COLOR_OVERRIDE_MATINST_PROBE_HPP
#define TRANSMOG_COLOR_OVERRIDE_MATINST_PROBE_HPP

/**
 * @file matinst_probe.hpp
 * @brief Shared matInst identity probe and submesh-name reader.
 * @details The publisher hook and the setter detour both read identity fields off a live matInst (template id,
 *          stable id, content hash) and the submesh name off its wrapper. The struct offsets and the heap-floor
 *          sanity screen live here so one edit covers every reader. All readers are guarded and `noexcept`. A bad
 *          pointer or freed memory returns `false` and no exception escapes.
 */

#include <DetourModKit.hpp>

#include <cstddef>
#include <cstdint>

namespace Transmog::color_override::mat_inst_probe
{
    // Engine struct offsets. Centralized here so a future patch that shifts them needs a single edit. Every reader
    // picks up the new value at once.
    inline constexpr std::ptrdiff_t MI_OFFSET_PERMUT_TOKEN = 0x70;
    inline constexpr std::ptrdiff_t MI_OFFSET_TEMPLATE_ID = 0x48;
    inline constexpr std::ptrdiff_t MI_OFFSET_STABLE_ID = 0x80;
    inline constexpr std::ptrdiff_t MI_OFFSET_AREC_BACKREF = 0xA0;
    inline constexpr std::ptrdiff_t AREC_OFFSET_CONTENT_HASH = 0x40;

    // Material to SkinnedMeshMaterialWrapper backref, and the wrapper's `_subMeshName` string-wrapper field offsets.
    inline constexpr std::ptrdiff_t MAT_OFFSET_WRAPPER_BACKREF = 0x10;
    inline constexpr std::ptrdiff_t WRAPPER_OFFSET_SUBMESH_NAME_SW = 0x28;
    inline constexpr std::ptrdiff_t STRING_WRAPPER_OFFSET_INLINE = 0x18;

    // Address range sanity. The engine heap pool sits above `0x200000000`. This floor is deliberately stricter than
    // the generic user-space lower bound and screens out bogus-low pointers that the weaker `memory::is_plausible_ptr`
    // floor (0x10000) accepts.
    inline constexpr std::uintptr_t HEAP_FLOOR = 0x200000000ULL;
    inline constexpr std::uintptr_t HEAP_CEILING = 0x800000000000ULL;

    inline bool is_likely_heap(std::uintptr_t p) noexcept
    {
        return p >= HEAP_FLOOR && p < HEAP_CEILING;
    }

    /**
     * @brief Tests whether @p p lies inside the host EXE mapped PE range.
     * @param p Absolute address to test.
     * @return True when @p p falls inside the host image span.
     * @details Catches stale-pointer reads where freed heap memory was overwritten with non-vtable garbage that still
     *          happens to be mapped. `Region::host()` is loader-backed and re-walks the PE headers on every call, so
     *          the span is captured once into a function-local static and every later call is pure arithmetic.
     * @note Callback-safe after the first call. The first call resolves the host image and must not run under the
     *       Windows loader lock.
     */
    inline bool is_module_resident(std::uintptr_t p) noexcept
    {
        static const DMK::Region s_host_image = DMK::Region::host();
        return s_host_image.contains(DMK::Address{p});
    }

    // Identity probe

    struct MatInstFields
    {
        std::uintptr_t mi{};
        std::uintptr_t vtable{};
        std::uint32_t content_hash{};
        std::uint64_t stable_id{};
        std::uint16_t template_id{};
    };

    /**
     * @brief Probes a matInst pointer directly.
     * @param mi Absolute address of the matInst.
     * @param out Receives the identity fields on success, and is cleared first on every call.
     * @return True when every field read succeeded, false on a bad pointer or a read fault.
     * @note Callback-safe: every hop is a guarded read and nothing allocates.
     */
    bool probe_matinst(std::uintptr_t mi, MatInstFields &out) noexcept;

    /**
     * @brief Probes through a wrapper-like struct whose `+0x10` field points to the matInst.
     * @param wrapper Absolute address of the wrapper.
     * @param out Receives the identity fields on success, and is cleared first on every call.
     * @return True when every field read succeeded, false on a bad pointer or a read fault.
     * @details The setter detour holds the wrapper in `ctx.rdi`, not the matInst itself.
     * @note Callback-safe: every hop is a guarded read and nothing allocates.
     */
    bool probe_from_wrapper(std::uintptr_t wrapper, MatInstFields &out) noexcept;

    // Submesh-name reader

    /**
     * @brief Reads the `_subMeshName` ASCIIZ off the SkinnedMeshMaterialWrapper parent of @p material.
     * @param material Absolute address of the material.
     * @param out Destination buffer, always null-terminated, cleared first on every call.
     * @param out_cap Capacity of @p out in bytes, including the terminator.
     * @return True with @p out filled on success. False on a read fault, a missing back-pointer, the module-resident
     *         empty-string sentinel, or an empty name.
     * @details Defensive layers, in order:
     *          - material and wrapper pointers must be heap-resident
     *          - the wrapper vtable at +0 must be module-resident, which catches stale-pointer reads into
     *            reallocated heap garbage
     *          - the string-wrapper pointer must be heap-resident, which rejects the module-resident empty-string
     *            sentinel the engine uses for "no name set"
     *          - every character must be printable ASCII, which rejects UTF-16 and binary fragments from reallocated
     *            objects
     * @note Callback-safe: every hop is a guarded read and nothing allocates.
     */
    bool read_submesh_name(std::uintptr_t material, char *out, std::size_t out_cap) noexcept;
} // namespace Transmog::color_override::mat_inst_probe

#endif // TRANSMOG_COLOR_OVERRIDE_MATINST_PROBE_HPP
