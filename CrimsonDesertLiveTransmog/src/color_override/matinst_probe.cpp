#include "matinst_probe.hpp"

#include <Windows.h>

namespace Transmog::ColorOverride::MatInstProbe
{
    bool probe_matinst(std::uintptr_t mi, MatInstFields &out) noexcept
    {
        out = {};
        if (!is_likely_heap(mi))
            return false;

        const auto vtable = DMKMemory::seh_read<std::uintptr_t>(mi);
        const auto template_id =
            DMKMemory::seh_read<std::uint16_t>(mi + k_offMi_TemplateId);
        const auto stable_id =
            DMKMemory::seh_read<std::uint64_t>(mi + k_offMi_StableId);
        if (!vtable || !template_id || !stable_id)
            return false;

        // Resolve the arec backref EXPLICITLY (not via seh_read_chain):
        // its `is_likely_heap` floor (0x200000000) is stricter than the
        // chain primitive's `plausible_userspace_ptr` floor (0x10000),
        // which would let a bogus-low arec through.
        const auto arec =
            DMKMemory::seh_read<std::uintptr_t>(mi + k_offMi_ArecBackref);
        if (!arec || !is_likely_heap(*arec))
            return false;
        const auto content_hash = DMKMemory::seh_read<std::uint32_t>(
            *arec + k_offArec_ContentHash);
        if (!content_hash)
            return false;

        out.mi = mi;
        out.vtable = *vtable;
        out.template_id = *template_id;
        out.stable_id = *stable_id;
        out.content_hash = *content_hash;
        return true;
    }

    bool probe_from_wrapper(std::uintptr_t wrapper,
                            MatInstFields &out) noexcept
    {
        out = {};
        if (wrapper == 0)
            return false;
        const auto mi = DMKMemory::seh_read<std::uintptr_t>(
                            wrapper + k_offMat_WrapperBackref)
                            .value_or(0);
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
            const auto wrapper = *reinterpret_cast<const std::uintptr_t *>(
                material + k_offMat_WrapperBackref);
            if (!is_likely_heap(wrapper))
                return false;

            // Hardening: the wrapper's vtable pointer (at +0) must be
            // module-resident. Catches the stale-pointer case where
            // wrapper memory was freed and overwritten with non-
            // vtable garbage that still happens to be mapped.
            const auto wrapper_vtbl =
                *reinterpret_cast<const std::uintptr_t *>(wrapper);
            if (!is_module_resident(wrapper_vtbl))
                return false;

            // Wrapper's _subMeshName string-wrapper field. Engine
            // uses a module-resident empty-string sentinel vtable
            // instance when unset -- reject those.
            const auto sw = *reinterpret_cast<const std::uintptr_t *>(
                wrapper + k_offWrapper_SubMeshNameSw);
            if (!is_likely_heap(sw))
                return false;

            const char *src = reinterpret_cast<const char *>(
                sw + k_offStringWrapper_Inline);
            std::size_t n = 0;
            while (n < out_cap - 1 && src[n] != '\0')
            {
                // ASCII-only sanity: submesh names are
                // `[a-zA-Z0-9_]+`. Anything else likely means we read
                // garbage from a reallocated heap object.
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
