#include "aob_resolver.hpp"

#include <DetourModKit.hpp>

namespace EquipHide
{
    static uintptr_t resolve_match(uintptr_t match, const AddrCandidate &c) noexcept
    {
        if (c.mode == ResolveMode::Direct)
            return match + c.dispOffset;
        auto disp = *reinterpret_cast<const int32_t *>(match + c.dispOffset);
        return static_cast<uintptr_t>(
            static_cast<int64_t>(match + c.instrEndOffset) + disp);
    }

    /** @brief Try each candidate pattern across all executable regions.
     *  Candidate order is priority order (P1 most precise, P3 last resort). */
    static uintptr_t scan_for_address(
        const AddrCandidate *candidates, std::size_t count,
        const char *&matchedName)
    {
        auto &logger = DMK::Logger::get_instance();

        for (std::size_t i = 0; i < count; ++i)
        {
            auto p = DMK::Scanner::parse_aob(candidates[i].pattern);
            if (!p)
            {
                logger.warning("Failed to parse address AOB '{}'", candidates[i].name);
                continue;
            }

            auto *match = DMK::Scanner::scan_executable_regions(*p);
            if (match)
            {
                matchedName = candidates[i].name;
                return resolve_match(reinterpret_cast<uintptr_t>(match), candidates[i]);
            }
        }

        return 0;
    }

    uintptr_t resolve_address(
        const AddrCandidate *candidates, std::size_t count,
        const char *label)
    {
        auto &logger = DMK::Logger::get_instance();
        const char *matchedName = nullptr;
        auto result = scan_for_address(candidates, count, matchedName);

        if (result)
        {
            logger.info("{} resolved via '{}' at 0x{:X}", label, matchedName, result);
            return result;
        }

        logger.warning("{} AOB scan failed — feature disabled", label);
        return 0;
    }

    uintptr_t scan_for_hook_target(
        const std::vector<CompiledCandidate> &candidates,
        const AobCandidate *&matchedSource)
    {
        for (const auto &c : candidates)
        {
            auto *match = DMK::Scanner::scan_executable_regions(c.compiled);
            if (match)
            {
                matchedSource = c.source;
                return reinterpret_cast<uintptr_t>(match) + c.source->offsetToHook;
            }
        }

        return 0;
    }

} // namespace EquipHide
