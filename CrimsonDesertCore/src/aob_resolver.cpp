#include <cdcore/aob_resolver.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <cstdint>
#include <string>

namespace CDCore
{
    // ---------------------------------------------------------------
    // Inline-hook tolerance for already-hooked function prologues.
    //
    // When two mods share a function target and use the same AOB, the
    // first mod installs its SafetyHook-style inline hook and
    // overwrites the first 5 bytes of the function with
    // `E9 xx xx xx xx` (near-relative JMP). The second mod's AOB
    // scan fails because the prologue pattern no longer matches.
    //
    // Build a fallback pattern by replacing the first 5 bytes with
    // `E9 ?? ?? ?? ??`: the literal `E9` opcode plus four wildcard
    // displacement bytes. If the original pattern was long enough
    // (more than 5 bytes total) the remaining tail is still very
    // distinctive, so the fallback finds exactly the hooked target
    // and nothing else. Returns an empty string for patterns shorter
    // than 5 bytes (no room to host the JMP prefix).
    //
    // Tokens are whitespace-separated; each byte is one token (either
    // `XX` hex or `??` wildcard or partial `X?`/`?X`). A pipe `|`
    // marker is sometimes used to tag a RIP offset inside a longer
    // pattern; we preserve anything past the 5th byte verbatim.
    static std::string build_hooked_prologue_pattern(
        const char *orig) noexcept
    {
        if (!orig)
            return {};
        const std::string s(orig);
        std::size_t i = 0;
        int byteTokens = 0;
        while (i < s.size() && byteTokens < 5)
        {
            // Skip whitespace.
            while (i < s.size() &&
                   (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                    s[i] == '\r'))
                ++i;
            if (i >= s.size())
                break;
            // Skip the pipe marker without counting as a byte.
            if (s[i] == '|')
            {
                ++i;
                continue;
            }
            // Consume one token.
            while (i < s.size() && s[i] != ' ' && s[i] != '\t' &&
                   s[i] != '\n' && s[i] != '\r')
                ++i;
            ++byteTokens;
        }
        if (byteTokens < 5)
            return {};

        std::string out = "E9 ?? ?? ?? ??";
        out += s.substr(i); // whatever remained, including leading space
        return out;
    }

    static std::uintptr_t resolve_match(
        std::uintptr_t match, const AddrCandidate &c) noexcept;
    // forward decl -- definition lives below scan_for_address.

    static std::uintptr_t scan_for_hooked_address(
        const AddrCandidate *candidates, std::size_t count,
        const char *&matchedName) noexcept
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            // Only attempt the inline-hook fallback for direct-mode
            // candidates (the ones that hook a function entry). RIP-
            // relative candidates typically target instructions deeper
            // in the body and are unaffected by another mod's 5-byte
            // JMP rewrite at the prologue.
            if (candidates[i].mode != ResolveMode::Direct)
                continue;

            auto hooked = build_hooked_prologue_pattern(
                candidates[i].pattern);
            if (hooked.empty())
                continue;
            auto p = DMK::Scanner::parse_aob(hooked.c_str());
            if (!p)
                continue;
            auto *match = DMK::Scanner::scan_executable_regions(*p);
            if (match)
            {
                matchedName = candidates[i].name;
                return resolve_match(
                    reinterpret_cast<std::uintptr_t>(match),
                    candidates[i]);
            }
        }
        return 0;
    }

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
        //
        // JMP opcodes (E9/EB/FF 25) are NOT rejected: sibling mods
        // (EquipHide + LiveTransmog both hook VEC/BatchEquip) legitimately
        // patch these prologues and the resolver must still succeed so
        // SafetyHook can layer its trampoline on top of the existing one.
        // SafetyHook handles pre-hooked targets by decoding the existing
        // JMP into the new trampoline; refusing here would break the
        // second mod's init entirely.
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

        // Fallback: the function may already have been inline-hooked
        // by another mod that loaded before us. SafetyHook rewrites
        // the first 5 bytes of the prologue with a relative JMP, so
        // our normal AOB scan finds zero matches. Retry with the
        // first 5 bytes replaced by `E9 ?? ?? ?? ??` -- if the tail
        // of the pattern is specific enough we still locate the
        // function, and SafetyHook chains our hook behind theirs
        // cleanly.
        const char *hookedMatch = nullptr;
        const auto hookedResult = scan_for_hooked_address(
            candidates, count, hookedMatch);
        if (hookedResult)
        {
            logger.info(
                "{} resolved via '{}' at 0x{:X} "
                "(already inline-hooked by another mod; reusing target)",
                label, hookedMatch, hookedResult);
            return hookedResult;
        }

        logger.warning("{} AOB scan failed -- feature disabled", label);
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
