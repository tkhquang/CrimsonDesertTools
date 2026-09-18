#ifndef TRANSMOG_COLOR_OVERRIDE_COLOR_TOKEN_INTERNER_HOOK_HPP
#define TRANSMOG_COLOR_OVERRIDE_COLOR_TOKEN_INTERNER_HOOK_HPP

/**
 * @file color_token_interner_hook.hpp
 * @brief Engine string-interner capture.
 * @details The engine master name-interning function, reached through the ColorTokenInterner anchor, has the shape
 *          `intern(u32 *dest, const char *name, int, u32 sentinel)`. It assigns a u32 token id for a shader-property
 *          name, or returns the cached one. This module captures every `(name, token_id)` pair so `name_for_token`
 *          and `token_for_name` resolve names outside the AOB-discovered static slot tables.
 *
 *          The anchor sits on the function prologue, so tokens minted by ANY engine code path are captured, not only
 *          the ones the registrar-table discovery walks.
 */

#include <cstddef>
#include <cstdint>

namespace Transmog::ColorOverride::InternerHook
{
    /**
     * @brief Runs the one-shot capture install.
     * @return True once the interner state is resolved and its entries array is walked.
     * @details Safe to call more than once. Only the first successful call captures.
     * @note Setup/control-plane only: it resolves an anchor and walks the whole entries array.
     */
    bool init() noexcept;

    /**
     * @brief Re-walks the engine interner entries array when the captured set looks stale.
     * @return The number of newly captured (name, token) pairs.
     * @details Stale means the engine holds more entries than the capture, or it reallocated the table. The walk
     *          resumes from the previous cursor when the base is unchanged. Throttled internally to roughly 1.5 s.
     * @note Best-effort: the call is dropped while the throttle holds or a walk is already in flight.
     * @warning The walk runs inline on the caller thread and touches up to 0x20000 entries.
     */
    std::size_t refresh() noexcept;

    /**
     * @brief Resolves a token id back to its property name.
     * @param tok Token id to resolve.
     * @return The engine own string, stable for the process lifetime, or nullptr when the interner never minted the
     *         token while the capture was live.
     * @note Callback-safe: a linear scan over the capture table with no lock and no allocation.
     */
    const char *name_for_token(std::uint32_t tok) noexcept;

    /**
     * @brief Resolves a property name to its live token id.
     * @param name Property name to resolve.
     * @return The live token id, or 0 when the name was never observed.
     * @note Callback-safe, but the scan runs std::strcmp over up to 65536 captures.
     */
    std::uint32_t token_for_name(const char *name) noexcept;

    /**
     * @brief Counts the captured (name, token) pairs.
     * @return The number of captured pairs. Diagnostic only.
     * @note Callback-safe: one atomic load.
     */
    std::size_t capture_count() noexcept;
} // namespace Transmog::ColorOverride::InternerHook

#endif // TRANSMOG_COLOR_OVERRIDE_COLOR_TOKEN_INTERNER_HOOK_HPP
