#ifndef CDCORE_DMK_GLUE_HPP
#define CDCORE_DMK_GLUE_HPP

// CDCore::Glue is the thin consumer-side seam between the two Crimson Desert mods and DetourModKit. It keeps only the
// slice that DMK does not own:
//
//   * resolve_address: flattens the std::expected returned by the cascade scanner into the legacy uintptr_t-or-zero
//     shape used pervasively in CD mods. Worth keeping as a single-function call site so call sites stay focused on
//     conditional feature wiring.
//   * is_sibling_mod_loaded: cross-DLL presence check used to yield to a sibling mod that hooks the same target by a
//     name we know in advance. DMK's `is_target_already_hooked` covers managed-hook collision detection within DMK;
//     this helper still serves the case where the sibling has not yet installed (load-order races) or hooks via a
//     non-DMK route.
//   * looks_like_function_entry: pre-hook sanity check on an address a Direct-mode cascade reached through a
//     negative walk-back. Both mods share the CDCore anchor tables and both carry such walk-backs, so the guard
//     lives here rather than in either mod.

#include <DetourModKit.hpp>

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

namespace CDCore::Glue
{
    /**
     * @brief Resolves the first matching candidate from a cascade and returns the absolute address, or 0 on failure.
     * @details Thin wrapper around DetourModKit::Scanner::resolve_cascade_in_host_module_with_prologue_fallback that
     *          flattens the std::expected return into the legacy uintptr_t-or-zero shape used pervasively in CD mods.
     *          The scan is scoped to the host executable because every Crimson Desert hook/scan target lives in
     *          CrimsonDesert.exe. The underlying cascade already logs the success line; on failure this wrapper emits a
     *          single Warning so caller code can stay focused on conditional feature wiring.
     *
     *          Use this for "look up an address, install a hook if it resolved, otherwise log a warning and continue
     *          without the feature" patterns. For call sites that need the precise ResolveError (e.g. to distinguish
     *          all-patterns-failed from parse failures), call DetourModKit::Scanner::resolve_cascade directly.
     */
    [[nodiscard]] inline std::uintptr_t
    resolve_address(std::span<const DetourModKit::Scanner::AddrCandidate> candidates, std::string_view label)
    {
        // Every Crimson Desert hook/scan target lives in the host EXE, so scope the scan to that image: faster than a
        // whole-process walk and immune to a coincidental byte match in another injected module.
        auto hit = DetourModKit::Scanner::resolve_cascade_in_host_module_with_prologue_fallback(candidates, label);
        if (hit.has_value())
            return hit->address;

        DetourModKit::Logger::get_instance().warning("{} resolve cascade failed: {}", label,
                                                     DetourModKit::Scanner::resolve_error_to_string(hit.error()));
        return 0;
    }

    /**
     * @struct BatchRequest
     * @brief One target for @ref resolve_address_batch: a candidate cascade plus the label echoed in log lines.
     *        Caller-owned; the candidate span and the label must outlive the batch call.
     */
    struct BatchRequest
    {
        std::span<const DetourModKit::Scanner::AddrCandidate> candidates;
        std::string_view label;
    };

    /**
     * @brief Resolves several independent cascades concurrently, writing each absolute address (or 0 on failure) into
     *        @p out, parallel to @p requests.
     * @details Fork-join counterpart to @ref resolve_address. Builds one host-EXE-scoped, prologue-fallback
     *          DetourModKit::Scanner::CascadeRequest per entry and resolves the whole set in a single
     *          DetourModKit::Scanner::resolve_cascade_batch pass, so the wall-clock collapses from the sum of the scans
     *          to the slowest single scan. Per-request resolution is byte-identical to resolve_address: same host
     *          range, same prologue recovery for a sibling-stomped target, same require_unique discipline. Only the
     *          timing changes, so it is correct only for targets whose resolution does not depend on another request's
     *          result.
     *
     *          Setup/control-plane only. resolve_cascade_batch allocates and spins a transient worker pool, so unlike
     *          the noexcept single-shot resolvers it can throw. This wrapper is noexcept and, on any exception, falls
     *          back to a serial resolve_address per request, so a pool/allocation failure costs only the concurrency
     *          and never aborts init. Never call it under the loader lock, where worker threads cannot start.
     *
     *          Each unresolved request is reported as 0 and a single Warning, matching resolve_address, so call sites
     *          keep the same "address-or-zero, gate the feature" shape. Writes min(requests.size(), out.size())
     *          entries; any remaining @p out slots are left zeroed.
     */
    void resolve_address_batch(std::span<const BatchRequest> requests, std::span<std::uintptr_t> out) noexcept;

    /**
     * @brief Returns true if any module whose name contains @p needle is currently mapped into the process.
     * @details Walks the loaded-module list via PSAPI and compares each basename case-insensitively against @p needle.
     *          Used to detect sibling mods that hook the same target so we can yield rather than dual-install.
     *
     *          DMK 3.2.2 ships HookManager::is_target_already_hooked() and HookConfig::fail_if_already_hooked, which
     *          detect collisions between managed hooks at install time. This helper covers the orthogonal case where
     *          the sibling mod is identified by a known module-name substring (load-order races, mods that hook via
     *          non-DMK routes). When both signals exist the consumer prefers the upstream check; this remains the
     *          fallback for known-named siblings.
     * @param needle Substring to match (e.g. "CrimsonDesertEquipHide"). The check is case-insensitive on the basename
     *               only.
     * @return true if at least one loaded module's basename contains the needle, false otherwise (including PSAPI
     *         failure).
     */
    [[nodiscard]] bool is_sibling_mod_loaded(std::string_view needle) noexcept;

    /**
     * @brief Returns true if any of @p needles matches a loaded module.
     * @details Convenience overload that short-circuits on the first match.
     */
    [[nodiscard]] bool is_sibling_mod_loaded(std::initializer_list<std::string_view> needles) noexcept;

    /**
     * @brief Rejects an address that is not plausibly the first byte of a function.
     *
     * @details Call this before installing an INLINE hook on anything a Direct-mode cascade resolved through a
     *          negative disp_offset. A walk-back is measured against one build's prologue length and nothing in the
     *          resolver re-checks it, so when a prologue gains or loses bytes the pattern still matches its
     *          (unchanged) body while the walk-back lands short. The hook then writes its jump across an instruction
     *          boundary, or several bytes into a live function, and the process dies somewhere unrelated later. That
     *          is a silent failure the cascade itself cannot detect, which is what this guard is for.
     *
     *          The test is a byte-level heuristic, not a disassembly: MSVC precedes a function entry with `int3`
     *          alignment padding (0xCC), or the entry follows the previous function's terminator directly, so a
     *          `ret` (0xC3) or a single-byte `nop` (0x90) also reads as a boundary. A tail `jmp` ends in an
     *          arbitrary displacement byte and cannot be recognized this way, so a function reached that way is
     *          rejected. Rejection is the safe direction: the feature disables itself and says so, which is what
     *          would have happened anyway once the stale hook took the process down.
     *
     *          Complements DetourModKit::Scanner::is_likely_function_prologue, which inspects the bytes AT the
     *          address. This one inspects the byte BEFORE it. Neither subsumes the other.
     *
     * @param addr Candidate entry address, as resolved by the cascade.
     * @param label Cascade label, used only in the rejection log line.
     * @return true if @p addr is preceded by a function boundary byte, false if it is not or is unreadable.
     * @note Best-effort, and it never throws. Mid-function hook sites chosen deliberately (MidHook targets such as
     *       LiveTransmog's claim-walk guards) must NOT be passed through this.
     */
    [[nodiscard]] bool looks_like_function_entry(std::uintptr_t addr, std::string_view label) noexcept;

} // namespace CDCore::Glue

#endif // CDCORE_DMK_GLUE_HPP
