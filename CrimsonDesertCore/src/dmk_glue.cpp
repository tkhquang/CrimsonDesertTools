#include <cdcore/dmk_glue.hpp>

#include <Windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <span>
#include <string>
#include <vector>

namespace CDCore::Glue
{
    namespace
    {
        // PSAPI returns module paths as wide strings. Lower-case ASCII the
        // basename and the needle so "EquipHide", "equiphide", and
        // "EQUIPHIDE" all collide. Non-ASCII bytes are passed through
        // unchanged because module filenames in PE binaries are ASCII in
        // practice and a non-ASCII byte never collides with the ASCII range.
        [[nodiscard]] std::string ascii_lower(std::string_view in)
        {
            std::string out;
            out.reserve(in.size());
            for (char c : in)
            {
                const auto u = static_cast<unsigned char>(c);
                if (u >= 'A' && u <= 'Z')
                    out.push_back(static_cast<char>(u + ('a' - 'A')));
                else
                    out.push_back(c);
            }
            return out;
        }

        [[nodiscard]] std::string wide_basename_to_ascii_lower(
            const wchar_t *path) noexcept
        {
            const wchar_t *basename = path;
            for (const wchar_t *p = path; *p; ++p)
            {
                if (*p == L'\\' || *p == L'/')
                    basename = p + 1;
            }

            std::string out;
            for (const wchar_t *p = basename; *p; ++p)
            {
                wchar_t wc = *p;
                if (wc >= L'A' && wc <= L'Z')
                    wc = static_cast<wchar_t>(wc + (L'a' - L'A'));
                if (wc < 0x80)
                    out.push_back(static_cast<char>(wc));
                else
                    out.push_back('?');
            }
            return out;
        }

        [[nodiscard]] bool any_module_matches(
            std::span<const std::string> needles_lower) noexcept
        {
            const HANDLE proc = GetCurrentProcess();
            // Two-pass enumeration: first call sizes the buffer, second
            // call fills it. PSAPI does not expose a streaming API.
            DWORD bytesNeeded = 0;
            if (!EnumProcessModules(proc, nullptr, 0, &bytesNeeded) ||
                bytesNeeded == 0)
                return false;

            std::vector<HMODULE> modules(bytesNeeded / sizeof(HMODULE));
            if (!EnumProcessModules(
                    proc, modules.data(),
                    static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                    &bytesNeeded))
                return false;

            const auto count = bytesNeeded / sizeof(HMODULE);
            std::array<wchar_t, MAX_PATH> path{};
            for (size_t i = 0; i < count; ++i)
            {
                if (!GetModuleFileNameExW(proc, modules[i], path.data(),
                                          static_cast<DWORD>(path.size())))
                    continue;

                const auto base = wide_basename_to_ascii_lower(path.data());
                for (const auto &needle : needles_lower)
                {
                    if (base.find(needle) != std::string::npos)
                        return true;
                }
            }
            return false;
        }
    } // namespace

    bool is_sibling_mod_loaded(std::string_view needle) noexcept
    {
        if (needle.empty())
            return false;
        const std::string lowered = ascii_lower(needle);
        const std::array<std::string, 1> arr{lowered};
        return any_module_matches(arr);
    }

    bool is_sibling_mod_loaded(
        std::initializer_list<std::string_view> needles) noexcept
    {
        std::vector<std::string> lowered;
        lowered.reserve(needles.size());
        for (auto n : needles)
        {
            if (!n.empty())
                lowered.push_back(ascii_lower(n));
        }
        if (lowered.empty())
            return false;
        return any_module_matches(lowered);
    }

} // namespace CDCore::Glue
