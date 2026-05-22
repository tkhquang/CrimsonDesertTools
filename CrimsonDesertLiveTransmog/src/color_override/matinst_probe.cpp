#include "matinst_probe.hpp"

#include <DetourModKit.hpp>
#include <Windows.h>

#include <cstring>

namespace
{
    namespace DMK = DetourModKit;

    // Predicate-validated POD read (silent filter).
    //
    // Validates the source page via DMK's cached VirtualQuery probe
    // (`DMK::Memory::is_readable`) before reading. On success copies
    // `sizeof(T)` bytes via `memcpy`. The predicate filters the
    // common stale-pointer cases (NULL, low addresses, uncommitted
    // pages, PAGE_NOACCESS, PAGE_GUARD, short regions) without
    // firing an AV, so no first-chance exception lands on an
    // attached debugger for those cases.
    //
    // Deliberately contains no SEH. MSVC forbids `__try/__except`
    // in a function that has objects requiring unwind, and mixing
    // SEH with templated code is fragile under that rule. The
    // residual TOCTOU race window between predicate and `memcpy`
    // (page freed between the two, or a stale cache entry) is
    // caught by the outer `__try/__except` in each caller, so this
    // helper stays SEH-free.
    template <typename T>
    inline bool safe_read(std::uintptr_t src, T &out) noexcept
    {
        if (!DMK::Memory::is_readable(
                reinterpret_cast<const void *>(src), sizeof(T)))
            return false;
        std::memcpy(&out, reinterpret_cast<const void *>(src), sizeof(T));
        return true;
    }
}

namespace Transmog::ColorOverride::MatInstProbe
{
    bool probe_matinst(std::uintptr_t mi, MatInstFields &out) noexcept
    {
        out = {};
        if (!is_likely_heap(mi))
            return false;
        __try
        {
            // Stale-pointer gate: vtable @ +0 must be module-
            // resident. The predicate inside `safe_read` rejects
            // freed / unmapped pages silently; the surrounding SEH
            // boundary catches the residual race where the page is
            // freed between predicate and read.
            std::uintptr_t vtbl = 0;
            if (!safe_read(mi, vtbl))
                return false;
            if (!is_module_resident(vtbl))
                return false;

            out.mi     = mi;
            out.vtable = vtbl;

            if (!safe_read(mi + k_offMi_TemplateId, out.template_id))
                return false;
            if (!safe_read(mi + k_offMi_StableId, out.stable_id))
                return false;

            std::uintptr_t arec = 0;
            if (!safe_read(mi + k_offMi_ArecBackref, arec))
                return false;
            if (!is_likely_heap(arec))
                return false;
            if (!safe_read(arec + k_offArec_ContentHash, out.content_hash))
                return false;

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool probe_from_wrapper(std::uintptr_t wrapper,
                            MatInstFields &out) noexcept
    {
        out = {};
        if (wrapper == 0)
            return false;
        std::uintptr_t mi = 0;
        __try
        {
            if (!safe_read(wrapper + k_offMat_WrapperBackref, mi))
                return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return probe_matinst(mi, out);
    }

    bool read_submesh_name(std::uintptr_t material,
                           char *out, std::size_t out_cap) noexcept
    {
        if (out == nullptr || out_cap == 0)
            return false;
        out[0] = '\0';
        if (!is_likely_heap(material))
            return false;
        __try
        {
            // Back-pointer to parent SkinnedMeshMaterialWrapper.
            std::uintptr_t wrapper = 0;
            if (!safe_read(material + k_offMat_WrapperBackref, wrapper))
                return false;
            if (!is_likely_heap(wrapper))
                return false;

            // Stale-pointer gate on the wrapper: vtable @ +0 must
            // be module-resident. Catches reads into reallocated
            // heap garbage that survived the readability predicate.
            std::uintptr_t wrapperVtbl = 0;
            if (!safe_read(wrapper, wrapperVtbl))
                return false;
            if (!is_module_resident(wrapperVtbl))
                return false;

            // Wrapper's `_subMeshName` string-wrapper field. Engine
            // uses a module-resident empty-string sentinel vtable
            // instance when unset -- the inline buffer on that
            // sentinel would read into module .rdata, so require
            // the string-wrapper to be heap-resident.
            std::uintptr_t sw = 0;
            if (!safe_read(wrapper + k_offWrapper_SubMeshNameSw, sw))
                return false;
            if (!is_likely_heap(sw))
                return false;

            // Validate the inline string buffer in one shot before
            // the char-walk. `out_cap` bounds the walk, so one
            // readability check covers every read in the loop. The
            // residual race during the walk is still caught by the
            // surrounding `__try`.
            const auto str_addr = sw + k_offStringWrapper_Inline;
            if (!DMK::Memory::is_readable(
                    reinterpret_cast<const void *>(str_addr), out_cap))
                return false;

            const char *src = reinterpret_cast<const char *>(str_addr);
            std::size_t n = 0;
            while (n < out_cap - 1 && src[n] != '\0')
            {
                // Printable-ASCII sanity: submesh names are
                // `[a-zA-Z0-9_]+`. Anything else is garbage from a
                // reallocated heap object that raced past the
                // readability check.
                const auto c = static_cast<unsigned char>(src[n]);
                if (c < 0x20 || c > 0x7E)
                    break;
                out[n] = src[n];
                ++n;
            }
            out[n] = '\0';
            return n > 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out[0] = '\0';
            return false;
        }
    }
}
