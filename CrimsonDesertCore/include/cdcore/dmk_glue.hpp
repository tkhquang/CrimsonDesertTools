#ifndef CDCORE_DMK_GLUE_HPP
#define CDCORE_DMK_GLUE_HPP

// CDCore::Glue is the consumer-side seam between the two Crimson Desert mods
// and DetourModKit. The bulk of the historical wrappers (atomic-bool register,
// log-level register, expected<>-bail logger) now live upstream in DMK; the
// surface kept here is the slice that DMK does not own:
//
//   * resolve_address: flattens the std::expected returned by the cascade
//     scanner into the legacy uintptr_t-or-zero shape used pervasively in CD
//     mods. Worth keeping as a single-function call site so call sites stay
//     focused on conditional feature wiring.
//   * is_sibling_mod_loaded: cross-DLL presence check used to yield to a
//     sibling mod that hooks the same target by a name we know in advance.
//     DMK's `is_target_already_hooked` covers managed-hook collision detection
//     within DMK; this helper still serves the case where the sibling has not
//     yet installed (load-order races) or hooks via a non-DMK route.

#include <DetourModKit.hpp>

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

namespace CDCore::Glue
{
    /**
     * @brief Resolves the first matching candidate from a cascade and
     *        returns the absolute address, or 0 on failure.
     * @details Thin wrapper around
     *          DetourModKit::Scanner::resolve_cascade_with_prologue_fallback
     *          that flattens the std::expected return into the legacy
     *          uintptr_t-or-zero shape used pervasively in CD mods. The
     *          underlying cascade already logs the success line; on
     *          failure this wrapper emits a single Warning so caller code
     *          can stay focused on conditional feature wiring.
     *
     *          Use this for "look up an address, install a hook if it
     *          resolved, otherwise log a warning and continue without the
     *          feature" patterns. For call sites that need the precise
     *          ResolveError (e.g. to distinguish all-patterns-failed from
     *          parse failures), call DetourModKit::Scanner::resolve_cascade
     *          directly.
     */
    [[nodiscard]] inline std::uintptr_t resolve_address(
        std::span<const DetourModKit::Scanner::AddrCandidate> candidates,
        std::string_view label)
    {
        auto hit = DetourModKit::Scanner::resolve_cascade_with_prologue_fallback(
            candidates, label);
        if (hit.has_value())
            return hit->address;

        DetourModKit::Logger::get_instance().warning(
            "{} resolve cascade failed: {}",
            label,
            DetourModKit::Scanner::resolve_error_to_string(hit.error()));
        return 0;
    }

    /**
     * @brief Returns true if any module whose name contains @p needle is
     *        currently mapped into the process.
     * @details Walks the loaded-module list via PSAPI and compares each
     *          basename case-insensitively against @p needle. Used to
     *          detect sibling mods that hook the same target so we can
     *          yield rather than dual-install.
     *
     *          DMK 3.2.2 ships HookManager::is_target_already_hooked() and
     *          HookConfig::fail_if_already_hooked, which detect collisions
     *          between managed hooks at install time. This helper covers
     *          the orthogonal case where the sibling mod is identified by
     *          a known module-name substring (load-order races, mods that
     *          hook via non-DMK routes). When both signals exist the
     *          consumer prefers the upstream check; this remains the
     *          fallback for known-named siblings.
     * @param needle Substring to match (e.g. "CrimsonDesertEquipHide"). The
     *               check is case-insensitive on the basename only.
     * @return true if at least one loaded module's basename contains the
     *         needle, false otherwise (including PSAPI failure).
     */
    [[nodiscard]] bool is_sibling_mod_loaded(std::string_view needle) noexcept;

    /**
     * @brief Returns true if any of @p needles matches a loaded module.
     * @details Convenience overload that short-circuits on the first match.
     */
    [[nodiscard]] bool is_sibling_mod_loaded(
        std::initializer_list<std::string_view> needles) noexcept;

} // namespace CDCore::Glue

#endif // CDCORE_DMK_GLUE_HPP
