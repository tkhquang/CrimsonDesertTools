#ifndef CDCORE_AOB_RESOLVER_HPP
#define CDCORE_AOB_RESOLVER_HPP

#include <DetourModKit.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Cascading AOB-candidate address and hook resolver (game-agnostic).
//
// Each mod defines its own candidate tables (AddrCandidate / AobCandidate
// arrays) and passes them to resolve_address() / scan_for_hook_target().
// The helpers here own only the framework — parse, scan, RIP-resolve.
// ---------------------------------------------------------------------------

namespace CDCore
{
    enum class ResolveMode : std::uint8_t
    {
        Direct,
        RipRelative
    };

    struct AddrCandidate
    {
        const char *name;
        const char *pattern;
        ResolveMode mode;
        std::ptrdiff_t dispOffset;
        std::ptrdiff_t instrEndOffset;
    };

    struct AobCandidate
    {
        const char *name;
        const char *pattern;
        std::ptrdiff_t offsetToHook;
    };

    struct CompiledCandidate
    {
        const AobCandidate *source;
        DMK::Scanner::CompiledPattern compiled;
    };

    /**
     * @brief Resolve an address via cascading AOB patterns.
     *
     * Tries each candidate in order (priority: P1 most precise, later
     * candidates are fallback anchors). Logs match-name on success, a
     * warning on full failure. Returns 0 on failure.
     */
    [[nodiscard]] std::uintptr_t resolve_address(
        const AddrCandidate *candidates, std::size_t count,
        const char *label);

    /**
     * @brief Scan all executable regions for a hook target from a list of
     *        precompiled AOB candidates.
     *
     * On success, @p matchedSource is set to the winning candidate's
     * source entry; returns resolved-address + source->offsetToHook.
     * Returns 0 on full failure.
     */
    [[nodiscard]] std::uintptr_t scan_for_hook_target(
        const std::vector<CompiledCandidate> &candidates,
        const AobCandidate *&matchedSource);

    /**
     * @brief First-byte prologue sanity check for a callable function.
     *
     * Reads up to 1 byte from @p addr under SEH guard. Uses a
     * **blacklist** approach (reject known poison, accept everything
     * else) so the gate is patch-proof: future game updates that
     * introduce new legal prologue shapes won't trigger false
     * rejections.
     *
     * Rejected bytes (treated as scan-corruption signals):
     *   0x00       — uninitialized memory / zero-fill BSS / NULL page
     *   0xCC       — int3 breakpoint / alignment pad / debugger trap
     *   0xC2/0xC3  — bare `ret` (bare-ret stub, not a callable body)
     *
     * Accepted: every other legal x86-64 first-instruction byte
     * (REX prefixes 0x40-0x4F, push/pop 0x50-0x5F, mov/lea/sub/xor/
     * test/cmp opcodes, 0x0F two-byte prefix, 0x66 operand-size
     * prefix, 0xE8/0xE9/0xEB jmp/call, etc.).
     *
     * History:
     *   2026-04-11 initial narrow whitelist falsely rejected
     *   SlotPopulator (0x14076C960) because its real prologue starts
     *   with 0x4C (mov [rsp+18h], r8). Flipped to blacklist approach
     *   the same day.
     */
    [[nodiscard]] bool sanity_check_function_prologue(
        std::uintptr_t addr) noexcept;

} // namespace CDCore

#endif // CDCORE_AOB_RESOLVER_HPP
