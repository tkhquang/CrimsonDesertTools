#ifndef CDCORE_PROLOGUE_CHECK_HPP
#define CDCORE_PROLOGUE_CHECK_HPP

#include <cstdint>

namespace CDCore
{
    /**
     * @brief First-byte prologue sanity check for a callable function.
     *
     * Reads up to 1 byte from @p addr under SEH guard. Uses a
     * blacklist approach (reject known poison, accept everything
     * else) so the gate is patch-proof: future game updates that
     * introduce new legal prologue shapes will not trigger false
     * rejections.
     *
     * Rejected bytes (treated as scan-corruption signals):
     *   0x00       uninitialized memory / zero-fill BSS / NULL page
     *   0xCC       int3 breakpoint / alignment pad / debugger trap
     *   0xC2/0xC3  bare ret (bare-ret stub, not a callable body)
     *
     * JMP opcodes (E9/EB/FF 25) are NOT rejected: sibling mods
     * legitimately patch these prologues and the resolver must still
     * succeed so SafetyHook can layer its trampoline on top of an
     * existing one.
     */
    [[nodiscard]] bool sanity_check_function_prologue(
        std::uintptr_t addr) noexcept;

} // namespace CDCore

#endif // CDCORE_PROLOGUE_CHECK_HPP
