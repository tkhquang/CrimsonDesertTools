#ifndef TRANSMOG_COLOR_OVERRIDE_COLOR_TOKEN_DISCOVERY_HPP
#define TRANSMOG_COLOR_OVERRIDE_COLOR_TOKEN_DISCOVERY_HPP

/**
 * @file color_token_discovery.hpp
 * @brief Patch-proof token-slot discovery.
 * @details The engine string-interner re-buckets token ids for shader properties such as `_dyeingColorMaskR` on every
 *          patch. The slot ADDRESS that stores a token id comes from the compiler layout of the registrar function
 *          and moves only when the game binary is rebuilt with a different layout.
 *
 *          This module scans the host image executable regions for the adjacent-lea pattern every registrar call
 *          emits:
 *
 *              lea rcx, [rip+disp32]      ; lea rcx, [table_slot]
 *              lea rdx, [rip+disp32]      ; lea rdx, [name_string]
 *              ... mov r8d,1 ... mov r9d,196607 ... call interner
 *
 *          Every match whose `rdx` target names a known dye-property string records `(slot_addr, layer, channel)`.
 *          Classification walks that list, reads the live u32 out of each slot, and returns the layer and channel of
 *          the slot equal to the incoming token.
 *
 *          Every registrar table is picked up automatically, because each registrar emits the same pattern against a
 *          different slot target.
 */

#include <cstddef>
#include <cstdint>

namespace Transmog::color_override::token_slot_discovery
{
    /**
     * @brief Runs the one-shot module-init scan pass.
     * @details Safe to call more than once. Only the first call performs the scan.
     * @note Setup/control-plane only: the sweep walks the whole host image.
     */
    void run();

    /**
     * @brief Re-runs the scan when fewer than @p expected_min slots are known.
     * @param expected_min Slot-count baseline the capture must reach before the retry settles.
     * @details Cold start misses registrar call sites whose code pages are not committed yet, so a hot-path caller
     *          such as the setter mid-hook covers the gap. Throttled internally to roughly 1.5 s between attempts.
     *          Becomes a permanent no-op once two consecutive scans return the same slot count AND that count reaches
     *          @p expected_min. The sweep itself is dispatched to a worker, so the caller returns immediately.
     * @note Best-effort: the call is dropped while the throttle holds or a sweep is already in flight.
     * @warning The dispatch takes a mutex shared with the re-scan worker teardown path.
     */
    void retry_if_underpopulated(std::size_t expected_min) noexcept;

    /**
     * @brief Stops any re-scan worker and waits for it to exit.
     * @details Must run before the module can be unmapped. The worker walks the host image and touches this module's
     *          code and statics, so a hot reload that unmapped the logic DLL while it ran would fault. Idempotent,
     *          and safe when no worker was ever started.
     * @note Setup/control-plane only: joins a thread. Never call it from a detour or under the loader lock.
     */
    void stop_and_join_rescan() noexcept;

    /**
     * @brief Reports whether `run()` finished its scan.
     * @return True once the scan completed, whether or not it discovered any slot.
     * @note Callback-safe: one relaxed-domain atomic load.
     */
    bool is_complete() noexcept;

    /**
     * @brief Classifies a token layer by reading the discovered slots.
     * @param tok Live token id to classify.
     * @return 0 = tint, 1 = mask, 2 = detail, 3 = hair, -1 = no match.
     * @note Callback-safe, but it takes the slot-table mutex the re-scan worker also holds.
     */
    int classify_layer(std::uint32_t tok) noexcept;

    /**
     * @brief Classifies a token channel by reading the discovered slots.
     * @param tok Live token id to classify.
     * @return 0 = R, 1 = G, 2 = B, -1 = none.
     * @note Callback-safe, but it takes the slot-table mutex the re-scan worker also holds.
     */
    int classify_channel(std::uint32_t tok) noexcept;

    /**
     * @brief Reads the live token id for a property name.
     * @param name Property name to look up.
     * @return The live token id, or 0 when the name is undiscovered or its slot reads 0.
     * @details The persistence reload paths use this to rebind a saved name to the current build token id.
     * @note Callback-safe, but it takes the slot-table mutex the re-scan worker also holds.
     */
    std::uint32_t lookup_token_for_name(const char *name) noexcept;

    /**
     * @brief Finds the property NAME behind a live token id.
     * @param tok Live token id to resolve.
     * @return A static string literal, or `nullptr` when no discovered slot matches. The pointer is safe to compare
     *         with strcmp or hand to a logger.
     * @note Callback-safe, but it takes the slot-table mutex the re-scan worker also holds.
     */
    const char *name_for_token(std::uint32_t tok) noexcept;

    /**
     * @brief Counts the discovered `(slot_addr, name)` pairs.
     * @return The number of discovered slots. Diagnostic only.
     * @note Callback-safe, but it takes the slot-table mutex the re-scan worker also holds.
     */
    std::size_t slot_count() noexcept;
} // namespace Transmog::color_override::token_slot_discovery

#endif // TRANSMOG_COLOR_OVERRIDE_COLOR_TOKEN_DISCOVERY_HPP
