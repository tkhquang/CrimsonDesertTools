#ifndef TRANSMOG_DYE_RECORD_INJECT_HPP
#define TRANSMOG_DYE_RECORD_INJECT_HPP

// Per-slot dye-record injector for fake transmog.
//
// LT-fake transmog items have no real-item backing, so the engine has no natural ARMOR_MOD records to publish for
// them. Without records, the slot renders with the engine's default palette whatever the user's preset holds.
//
// This module installs an inline detour on the engine's per-slot dye-record copier, the DyeCopier anchor.
// Post-trampoline, while LT is active and after LT publishes a per-slot dye state, the detour calls the engine's own
// 16-byte record-copy primitive, the DyeCopy anchor, to APPEND fabricated ARMOR_MOD records to the destination vector
// at `dst+120`.
//
// Each fabricated record encodes one channel's RGB at offsets +7/+8/+9. A channel with `group_hash == 0` is inactive
// and reuses the first active channel's settings, so the engine sees a contiguous record block. Sparse injection,
// which skips the inactive channels, leaves the engine's natural records dominant and the dye does not render.
//
// The anchor registry in `aob_resolver.hpp` resolves every function target at init through a patch-proof AOB cascade.
// No raw RVA serves as an anchor.

#include <DetourModKit/hook.hpp>

#include <cstddef>
#include <cstdint>

namespace Transmog::DyeRecordInject
{
    /**
     * @brief Maximum dye channels per slot.
     * @details Mirrors PresetSlot's `k_dyeChannelCount`, but stays independent so this header does not pull in the
     *          preset types.
     */
    inline constexpr std::size_t k_dyeChannelCount = 16;

    /**
     * @brief One channel's dye state.
     * @details `group_hash == 0` means "no override for this channel". The injector then substitutes the first
     *          active channel's settings, as the header preamble describes.
     *
     * Field layout in the 16-byte ARMOR_MOD record this module synthesizes:
     *   +0..+3   group_hash      (color group key)
     *   +4..+5   material_id     (dye-template variant 1..10;
     *                            0xFFFF = engine default)
     *   +6       channel index   (0..N-1; written at inject time)
     *   +7       r
     *   +8       g
     *   +9       b
     *   +10      0xFF
     *   +11      repair_byte     (0=pristine, 0x7F=max wear; the
     *                            engine still accepts 0xFF as a legacy "no override" sentinel and renders it as
     *                            pristine)
     *   +13      0x04 for indices 0 and 3 (mirrors natural records)
     */
    struct ChannelState
    {
        std::uint32_t group_hash{};
        std::uint8_t r{};
        std::uint8_t g{};
        std::uint8_t b{};
        std::uint16_t material_id{};
        std::uint8_t repair_byte{};
    };

    /**
     * @brief Dye-record vector geometry.
     *
     * @details The same {qword data, u32 count, u32 capacity} header sits at this offset on BOTH an auth-table entry
     *          and the DyeCopier destination struct - the two structures share the layout. It is published here so
     *          every consumer reads one copy: the injector, the capture path and the socket override all walk the
     *          same vector.
     * @note The offset moves with the auth-table entry geometry (see auth_table.hpp) on patch day. A stale value
     *       lands on the neighbouring field and dereferences a garbage pointer instead of failing closed.
     */
    inline constexpr std::size_t k_dyeVectorOffset = 0x78; // vector header, from the record / entry base
    inline constexpr std::size_t k_vecDataOffset = 0x00;   // within the header: qword heap ptr
    inline constexpr std::size_t k_vecCountOffset = 0x08;  // within the header: u32 valid record count
    inline constexpr std::size_t k_dyeRecordSize = 16;

    /**
     * @brief Fill one 16-byte ARMOR_MOD record.
     * @details The single writer for the field layout documented on @ref ChannelState above. Callers must not
     *          open-code it, or the layout ends up stated in as many places as there are producers.
     * @param out Destination, at least @ref k_dyeRecordSize bytes.
     * @param channel_idx Channel this record describes, below @ref k_dyeChannelCount.
     */
    void build_dye_record(
        std::uint8_t *out,
        std::size_t channel_idx,
        std::uint32_t group_hash,
        std::uint8_t r,
        std::uint8_t g,
        std::uint8_t b,
        std::uint16_t material_id,
        std::uint8_t repair_byte
    ) noexcept;

    /**
     * @brief Resolves the target functions through AOB cascades and installs the hooks.
     * @param hooks The stack that takes ownership of every installed hook.
     * @return True on success.
     * @details A target that fails to resolve disables injection and logs a warning. The rest of LT keeps working.
     */
    bool init(DetourModKit::hook::HookStack &hooks) noexcept;

    /// Logs the injection counters, a diagnostic.
    void log_counters() noexcept;

    /**
     * @brief Dumps the injection counters. This is the deactivation-path stat hook.
     * @details `prefab_wrapper_swap.cpp` calls it when LT deactivates. It restores nothing.
     */
    void restore_all() noexcept;

    /**
     * @brief Publishes per-slot dye state for the next DyeCopier invocation.
     * @param channels Exactly `k_dyeChannelCount` entries.
     * @param sparse Selects the per-record emission strategy.
     * @details The storage is thread-local, because the detour runs on the thread that publishes.
     *
     *          `sparse == false`, the default and the fake-transmog path, always emits `k_dyeChannelCount` records.
     *          A channel with `group_hash == 0` reuses the first active channel's settings, so the engine sees a
     *          contiguous block. LT-fake meshes require this, because their natural source has zero records and a
     *          sparse vector leaves the engine on its default palette.
     *
     *          `sparse == true`, the restore path, emits ONLY records whose source had `group_hash != 0`. An
     *          inactive channel stays absent from the destination vector, so the engine does not paint mesh parts
     *          that the real item never colored. Use it when the `ChannelState` array came from a real auth-table
     *          entry's dye vector and the render side must mirror exactly the same indices.
     */
    void set_slot_dye_state(const ChannelState *channels, bool sparse = false) noexcept;

    /**
     * @brief Clears the published state.
     * @details Call it once the apply path completes, so a later natural equip event goes through unmodified.
     */
    void clear_slot_dye_state() noexcept;

    /**
     * @brief Reads the live dye-record vector from an auth-table entry.
     * @param entryBase Base address of the auth-table entry.
     * @param out Destination array, indexed by the in-record channel index (0..N-1). An inactive or out-of-range
     *        channel stays zero-initialized.
     * @return The number of channels populated. `0` means nothing usable: an empty vector, an invalid data pointer,
     *         or every record carrying `group_hash == 0`.
     * @details Source layout:
     *          - entryBase + 0x78, qword, data_ptr, contiguous 16-byte records
     *          - entryBase + 0x80, dword, count
     *          - entryBase + 0x84, dword, capacity, ignored
     *
     *          Each record has the shape this header documents above, the engine's ARMOR_MOD format that DyeCopy
     *          emits.
     * @note Every read routes through the guarded memory primitives, so the caller needs no SEH of its own. An
     *       unreadable entry yields 0 populated channels.
     */
    std::size_t read_entry_dye_records(std::uintptr_t entryBase, ChannelState (&out)[k_dyeChannelCount]) noexcept;

    /**
     * @brief Trace-logs a ChannelState array in a stable per-channel format.
     * @param source Short tag such as "capture" or "restore".
     * @param slotName The LT slot the records belong to.
     * @param state The channel array to log.
     * @details The stable format lets a reader diff a capture-time snapshot against an apply-time one in the log. A
     *          channel with `group_hash == 0` still prints, so the diff catches a missing channel too.
     */
    void
    log_dye_snapshot(const char *source, const char *slotName, const ChannelState (&state)[k_dyeChannelCount]) noexcept;

    /**
     * @brief Reads the first ACTIVE channel's RGB from the currently published slot dye state.
     * @param r Receives the red component.
     * @param g Receives the green component.
     * @param b Receives the blue component.
     * @return True when the slot holds an active channel and the three outputs are filled. False when no inject is
     *         active or every channel is zero.
     * @details `ColorOverride::SetterSubstitute` calls it to learn which color to redirect the engine's per-property
     *          setter to.
     */
    bool get_published_first_active_rgb(std::uint8_t *r, std::uint8_t *g, std::uint8_t *b) noexcept;
} // namespace Transmog::DyeRecordInject

#endif // TRANSMOG_DYE_RECORD_INJECT_HPP
