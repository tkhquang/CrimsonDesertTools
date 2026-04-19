#include <cdcore/aob_resolver.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <cstdint>

namespace CDCore
{
    bool sanity_check_function_prologue(std::uintptr_t addr) noexcept
    {
        if (!addr)
            return false;

        std::uint8_t b0 = 0;
        __try
        {
            b0 = *reinterpret_cast<const volatile std::uint8_t *>(addr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        // Blacklist: reject known poison, accept anything else. See
        // header comment for the full rationale and history.
        if (b0 == 0x00 || b0 == 0xCC || b0 == 0xC2 || b0 == 0xC3)
            return false;

        return true;
    }

    static std::uintptr_t resolve_match(
        std::uintptr_t match, const AddrCandidate &c) noexcept
    {
        if (c.mode == ResolveMode::Direct)
            return match + c.dispOffset;

        const auto disp = *reinterpret_cast<const std::int32_t *>(
            match + c.dispOffset);
        return static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(match + c.instrEndOffset) + disp);
    }

    static std::uintptr_t scan_for_address(
        const AddrCandidate *candidates, std::size_t count,
        const char *&matchedName)
    {
        auto &logger = DMK::Logger::get_instance();

        for (std::size_t i = 0; i < count; ++i)
        {
            auto p = DMK::Scanner::parse_aob(candidates[i].pattern);
            if (!p)
            {
                logger.warning("Failed to parse address AOB '{}'",
                               candidates[i].name);
                continue;
            }

            auto *match = DMK::Scanner::scan_executable_regions(*p);
            if (match)
            {
                matchedName = candidates[i].name;
                return resolve_match(
                    reinterpret_cast<std::uintptr_t>(match), candidates[i]);
            }
        }

        return 0;
    }

    std::uintptr_t resolve_address(
        const AddrCandidate *candidates, std::size_t count,
        const char *label)
    {
        auto &logger = DMK::Logger::get_instance();
        const char *matchedName = nullptr;
        const auto result = scan_for_address(candidates, count, matchedName);

        if (result)
        {
            logger.info("{} resolved via '{}' at 0x{:X}",
                        label, matchedName, result);
            return result;
        }

        logger.warning("{} AOB scan failed — feature disabled", label);
        return 0;
    }

    std::uintptr_t scan_for_hook_target(
        const std::vector<CompiledCandidate> &candidates,
        const AobCandidate *&matchedSource)
    {
        for (const auto &c : candidates)
        {
            auto *match = DMK::Scanner::scan_executable_regions(c.compiled);
            if (match)
            {
                matchedSource = c.source;
                return reinterpret_cast<std::uintptr_t>(match) +
                       c.source->offsetToHook;
            }
        }

        return 0;
    }

} // namespace CDCore
