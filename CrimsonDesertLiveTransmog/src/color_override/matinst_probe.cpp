#include "matinst_probe.hpp"

#include <DetourModKit/memory.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace Transmog::ColorOverride::MatInstProbe
{
    bool probe_matinst(std::uintptr_t mi, MatInstFields &out) noexcept
    {
        out = {};
        if (!is_likely_heap(mi))
            return false;

        const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{mi});
        const auto template_id = DMK::memory::read<std::uint16_t>(DMK::Address{mi + k_offMi_TemplateId});
        const auto stable_id = DMK::memory::read<std::uint64_t>(DMK::Address{mi + k_offMi_StableId});
        if (!vtable || !template_id || !stable_id)
            return false;

        // Resolve the arec backref EXPLICITLY, not through memory::walk. The `is_likely_heap` floor (0x200000000) is
        // stricter than the chain primitive default `min_valid` floor (0x10000), which lets a bogus-low arec through.
        const auto arec = DMK::memory::read<std::uintptr_t>(DMK::Address{mi + k_offMi_ArecBackref});
        if (!arec || !is_likely_heap(*arec))
            return false;
        const auto content_hash = DMK::memory::read<std::uint32_t>(DMK::Address{*arec + k_offArec_ContentHash});
        if (!content_hash)
            return false;

        out.mi = mi;
        out.vtable = *vtable;
        out.template_id = *template_id;
        out.stable_id = *stable_id;
        out.content_hash = *content_hash;
        return true;
    }

    bool probe_from_wrapper(std::uintptr_t wrapper, MatInstFields &out) noexcept
    {
        out = {};
        if (wrapper == 0)
            return false;
        const auto mi = DMK::memory::read<std::uintptr_t>(DMK::Address{wrapper + k_offMat_WrapperBackref}).value_or(0);
        return probe_matinst(mi, out);
    }

    bool read_submesh_name(std::uintptr_t material, char *out, std::size_t out_cap) noexcept
    {
        if (out == nullptr || out_cap == 0)
            return false;
        out[0] = '\0';
        if (!is_likely_heap(material))
            return false;

        // Back-pointer to parent SkinnedMeshMaterialWrapper.
        const auto wrapper = DMK::memory::read<std::uintptr_t>(DMK::Address{material + k_offMat_WrapperBackref});
        if (!wrapper || !is_likely_heap(*wrapper))
            return false;

        // Hardening: the wrapper vtable pointer (at +0) must be module-resident. Catches the stale-pointer case where
        // wrapper memory was freed and overwritten with non-vtable garbage that still happens to be mapped.
        const auto wrapper_vtbl = DMK::memory::read<std::uintptr_t>(DMK::Address{*wrapper});
        if (!wrapper_vtbl || !is_module_resident(*wrapper_vtbl))
            return false;

        // Wrapper `_subMeshName` string-wrapper field. The engine parks a module-resident empty-string sentinel vtable
        // instance here when the name is unset, so the heap floor rejects those.
        const auto sw = DMK::memory::read<std::uintptr_t>(DMK::Address{*wrapper + k_offWrapper_SubMeshNameSw});
        if (!sw || !is_likely_heap(*sw))
            return false;

        // One guarded copy of the inline character window, then the printable-ASCII screen runs over the copy.
        //
        // read_into fails the WHOLE span on a fault anywhere inside it, so a short name whose buffer ends within the
        // window of an unmapped page would read back as nothing. The retry walks the window down to the readable
        // prefix, which is what the byte-at-a-time predecessor returned. The full window succeeds on every normal
        // wrapper, so the loop costs one guarded read in the common case.
        constexpr std::size_t k_name_window = 64;
        const std::size_t window = (out_cap - 1 < k_name_window) ? out_cap - 1 : k_name_window;
        std::array<std::byte, k_name_window> raw{};
        const DMK::Address inlineStart{*sw + k_offStringWrapper_Inline};
        std::size_t readable = window;
        while (readable > 0 && !DMK::memory::read_into(inlineStart, std::span{raw.data(), readable}))
            readable /= 2;
        if (readable == 0)
            return false;

        std::size_t n = 0;
        while (n < readable)
        {
            const auto c = std::to_integer<unsigned char>(raw[n]);
            if (c == 0)
                break;
            // ASCII-only sanity: submesh names are `[a-zA-Z0-9_]+`. Anything else means a garbage read off a
            // reallocated heap object.
            if (c < 0x20 || c > 0x7E)
                break;
            out[n] = static_cast<char>(c);
            ++n;
        }
        out[n] = '\0';
        return n > 0;
    }
} // namespace Transmog::ColorOverride::MatInstProbe
