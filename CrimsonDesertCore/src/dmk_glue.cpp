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
        // PSAPI returns module paths as wide strings. Lower-case ASCII the basename and the needle so "EquipHide",
        // "equiphide", and "EQUIPHIDE" all collide. Non-ASCII bytes are passed through unchanged because module
        // filenames in PE binaries are ASCII in practice and a non-ASCII byte never collides with the ASCII range.
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

        [[nodiscard]] std::string wide_basename_to_ascii_lower(const wchar_t *path) noexcept
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

        [[nodiscard]] bool any_module_matches(std::span<const std::string> needles_lower) noexcept
        {
            const HANDLE proc = GetCurrentProcess();
            // Two-pass enumeration: first call sizes the buffer, second call fills it. PSAPI does not expose a
            // streaming API.
            DWORD bytesNeeded = 0;
            if (!EnumProcessModules(proc, nullptr, 0, &bytesNeeded) || bytesNeeded == 0)
                return false;

            std::vector<HMODULE> modules(bytesNeeded / sizeof(HMODULE));
            if (!EnumProcessModules(proc, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                                    &bytesNeeded))
                return false;

            const auto count = bytesNeeded / sizeof(HMODULE);
            std::array<wchar_t, MAX_PATH> path{};
            for (size_t i = 0; i < count; ++i)
            {
                if (!GetModuleFileNameExW(proc, modules[i], path.data(), static_cast<DWORD>(path.size())))
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

    void resolve_address_batch(std::span<const BatchRequest> requests, std::span<std::uintptr_t> out) noexcept
    {
        // Honour the address-or-zero contract even on an early bail.
        for (auto &slot : out)
            slot = 0;

        const std::size_t count = std::min(requests.size(), out.size());
        if (count == 0)
            return;

        try
        {
            // Each request resolves exactly as resolve_address would: host-EXE scope (every target lives in
            // CrimsonDesert.exe) plus prologue fallback (re-match a sibling-stomped prologue). resolve_cascade_batch
            // dispatches each request to its own worker, so the host-module scans run concurrently instead of one after
            // another.
            std::vector<DetourModKit::Scanner::CascadeRequest> batch;
            batch.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                DetourModKit::Scanner::CascadeRequest req{};
                req.candidates = requests[i].candidates;
                req.label = requests[i].label;
                req.range = DetourModKit::Memory::host_module_range();
                req.prologue_fallback = true;
                batch.push_back(req);
            }

            const auto results = DetourModKit::Scanner::resolve_cascade_batch(batch);
            for (std::size_t i = 0; i < count && i < results.size(); ++i)
            {
                if (results[i].has_value())
                {
                    out[i] = results[i]->address;
                    continue;
                }
                DetourModKit::Logger::get_instance().warning(
                    "{} resolve cascade failed: {}", requests[i].label,
                    DetourModKit::Scanner::resolve_error_to_string(results[i].error()));
            }
        }
        catch (...)
        {
            // resolve_cascade_batch can throw bad_alloc: it allocates the result vector and spins a worker pool.
            // Degrade to the serial resolver so a transient failure loses only the concurrency, never the feature.
            for (std::size_t i = 0; i < count; ++i)
                out[i] = resolve_address(requests[i].candidates, requests[i].label);
        }
    }

    bool is_sibling_mod_loaded(std::string_view needle) noexcept
    {
        if (needle.empty())
            return false;
        const std::string lowered = ascii_lower(needle);
        const std::array<std::string, 1> arr{lowered};
        return any_module_matches(arr);
    }

    bool is_sibling_mod_loaded(std::initializer_list<std::string_view> needles) noexcept
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
