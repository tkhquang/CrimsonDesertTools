#include "aob_resolver.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <cstdint>

namespace Transmog
{
    bool sanity_check_function_prologue(uintptr_t addr) noexcept
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

        // Blacklist-only sanity check. Any byte that isn't explicitly
        // rejected is accepted. This is deliberate: whitelisting the
        // "known function-entry bytes" is fragile across game patches
        // (see the 2026-04-11 SlotPopulator incident where 0x4C
        // wasn't in the original whitelist and the gate falsely
        // rejected a clean address). A blacklist catches the common
        // scan-corruption signals without risking future false
        // negatives on legal prologue shapes we didn't predict.
        //
        // Rejected bytes:
        //   0x00       — uninitialized memory / zero-fill BSS / NULL
        //                page. A resolved address that reads as 0x00
        //                almost certainly points outside executable
        //                memory.
        //   0xCC       — int3 debugger breakpoint / alignment padding
        //                between functions / SOFT_BREAKPOINT poison.
        //                Could be a leftover CE/x64dbg BP at a stale
        //                address.
        //   0xC2/0xC3  — bare `ret` / `ret imm16`. A "function" that
        //                is nothing but ret isn't a realistic callable
        //                target for any of our hook/inject sites —
        //                more likely a scan landed on a jump-table
        //                entry or alignment byte.
        //
        // Everything else (REX prefixes 0x40-0x4F, push/pop 0x50-0x5F,
        // mov/lea/sub/xor/test/cmp opcodes, two-byte 0x0F prefix,
        // operand-size 0x66 prefix, jmp/call 0xE8/0xE9/0xEB, etc.) is
        // accepted — they're all legitimate first-instruction bytes.
        if (b0 == 0x00 || b0 == 0xCC || b0 == 0xC2 || b0 == 0xC3)
            return false;

        return true;
    }

    static uintptr_t resolve_match(uintptr_t match, const AddrCandidate &c) noexcept
    {
        if (c.mode == ResolveMode::Direct)
            return match + c.dispOffset;
        auto disp = *reinterpret_cast<const int32_t *>(match + c.dispOffset);
        return static_cast<uintptr_t>(
            static_cast<int64_t>(match + c.instrEndOffset) + disp);
    }

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

} // namespace Transmog
