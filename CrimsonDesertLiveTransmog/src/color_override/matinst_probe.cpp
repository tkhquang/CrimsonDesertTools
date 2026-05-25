#include "matinst_probe.hpp"

#include <Windows.h>

namespace Transmog::ColorOverride::MatInstProbe
{
    bool probe_matinst(std::uintptr_t mi, MatInstFields &out) noexcept
    {
        out = {};
        if (!is_likely_heap(mi))
            return false;
        __try
        {
            out.mi = mi;
            out.vtable = *reinterpret_cast<const std::uintptr_t *>(mi);
            out.template_id = *reinterpret_cast<const std::uint16_t *>(
                mi + k_offMi_TemplateId);
            out.stable_id = *reinterpret_cast<const std::uint64_t *>(
                mi + k_offMi_StableId);
            const auto arec = *reinterpret_cast<const std::uintptr_t *>(
                mi + k_offMi_ArecBackref);
            if (!is_likely_heap(arec))
                return false;
            out.content_hash = *reinterpret_cast<const std::uint32_t *>(
                arec + k_offArec_ContentHash);
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
            mi = *reinterpret_cast<const std::uintptr_t *>(
                wrapper + k_offMat_WrapperBackref);
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
