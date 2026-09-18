// Per-slot dye-record injector. See `dye_record_inject.hpp` for the architectural overview. This file installs one
// inline detour and resolves one engine function pointer:
//
//   * DyeCopier - INLINE DETOUR. After the engine's natural copy completes (with empty source for LT-fake), the
//     detour synthesizes per-channel ARMOR_MOD records and writes them into the record vector at dst+120 through the
//     DyeCopy primitive. This is independent of any real item the user wears.
//
//   * DyeCopy - function pointer only. The engine's 16-byte record-copy primitive. The detour calls it directly to
//     append synthesized records. Resolved through an AOB cascade so the call target tracks executable patches.
//
// Why an inline detour rather than a mid-hook here:
//   * The engine writes the natural record vector inside the trampoline body. The injection must run AFTER it.
//   * Substitution must be conditional on LT being active and a slot state being published. A mid-hook on entry runs
//     unconditionally.
//   * The DyeCopy primitive is reused as a function call, so the resolved address must stay stable. Reusing the
//     AOB-resolved value keeps hook target and call target locked together.

#include "dye_record_inject.hpp"
#include "aob_resolver.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace Transmog::dye_record_inject
{
    // Per-slot dye injection state set by LT's dispatch loop before each apply_transmog call. Thread-local because the
    // DyeCopier detour runs on the same thread that published the state.
    //
    //   s_inject_active: true if any channel has group_hash != 0.
    //   s_inject_channels: per-channel state.
    //   s_inject_consumed: marks that at least one inject ran for the currently published state. It does NOT gate
    //     injection. DyeCopier fires several times per slot, and the call whose destination reaches the renderer is
    //     not always the first one, so the detour injects on every call while a state is published. The per-channel
    //     upsert in the detour makes that idempotent. This flag only keeps the diagnostic log to one line per slot.
    static thread_local bool s_inject_active = false;
    static thread_local ChannelState s_inject_channels[DYE_CHANNEL_COUNT] = {};
    static thread_local bool s_inject_consumed = false;
    // When true, the detour emits records ONLY for channels with group_hash != 0. It skips inactive channels entirely
    // instead of filling them with the first active channel's settings. Callers that mirror a real auth-table source
    // (the restore path) set this, because the original mesh never colored those channels.
    static thread_local bool s_inject_sparse = false;

    // - Counters
    static std::atomic<std::uint64_t> g_dyeInjectCount{0};

    // - SEH-protected memory primitives
    //
    // The DyeCopy call can transiently fault on alloc or vector-header access during asset reload. Wrap it in SEH so a
    // bad state fails the call instead of crashing the game thread. The SEH __try block lives in its own function
    // because it cannot share scope with C++ object unwinding (string, etc.).

    // - DyeCopier inline detour (the injection site)
    //
    // After the engine's own dye-copy completes (with empty source for LT-fake), the detour writes up to
    // `DYE_CHANNEL_COUNT` records into the destination vector through the engine's record-copy primitive. Dense mode
    // emits every channel. Sparse mode emits only the channels the slot overrides. This is INDEPENDENT of any real
    // item the user wears.
    //
    // The detour is gated by:
    //   1. `Transmog::in_transmog()`  - only patch during LT apply
    //   2. `s_inject_active`           - a slot state was published
    //   3. `g_dye_copy_fn != nullptr` - DyeCopy AOB resolved cleanly

    using DyeCopier_t = std::int64_t(__fastcall *)(std::uintptr_t a1, std::uintptr_t a2);
    using DyeCopy_t = std::int64_t(__fastcall *)(std::uintptr_t a1, std::uintptr_t a2);

    static DyeCopier_t g_dye_copier_trampoline = nullptr;
    static DyeCopy_t g_dye_copy_fn = nullptr;

    // SEH wrapper around the DyeCopy call. The engine primitive touches the target vector header and can allocate.
    // Isolate the faults.
    static bool seh_call_dye_copy(DyeCopy_t fn, std::uintptr_t target_vec, std::uintptr_t src_record) noexcept
    {
        __try
        {
            fn(target_vec, src_record);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Engine layout constants
    //
    // The dye-record vector geometry (header offsets and record size) is published in dye_record_inject.hpp, because
    // the socket-mesh override produces the same records and must agree with this file byte for byte. What stays
    // here is what only this file needs.

    // One ARMOR_MOD record is 16 bytes, but the engine primitive only copies the first 13 (+0x00 through +0x0C) and
    // leaves the rest of the stride untouched. An in-place overwrite must write the same span to match its semantics.
    static constexpr std::size_t DYE_RECORD_COPY_SPAN = 13;

    // Channel index inside one record, and the sentinel used when the read fails. NO_CHANNEL is out of range for a
    // valid channel, so a failed read never matches the channel the loop is looking for.
    static constexpr std::uintptr_t RECORD_CHANNEL_OFFSET = 6;
    static constexpr std::uint8_t NO_CHANNEL = 0xFF;

    // Upper bound on the existing-record scan in the upsert path. The engine's copy leaves at most one record per
    // channel, which is what lets the upsert below stop at the first match, so a sane vector never holds more than a
    // handful of records and this ceiling is generous rather than tight. Its job is to reject a torn or relocated
    // vector whose count field reads as a huge value, before that value drives a long scan over unmapped memory.
    static constexpr std::uint32_t MAX_SCAN_RECORDS = 256;
    static constexpr std::uint32_t INVALID_COUNT = 0xFFFFFFFFu;

    // Build a 16-byte ARMOR_MOD record. See ChannelState in the header for the offset map. The +13 = 0x04 marker on
    // indices 0 and 3 mirrors what natural captures show. The engine accepts records without it, but matching the
    // natural shape avoids any shape validation that is not yet mapped.
    void build_dye_record(
        std::uint8_t *out,
        std::size_t channel_idx,
        std::uint32_t group_hash,
        std::uint8_t r,
        std::uint8_t g,
        std::uint8_t b,
        std::uint16_t material_id,
        std::uint8_t repair_byte
    ) noexcept
    {
        std::memset(out, 0, 16);
        std::memcpy(out + 0, &group_hash, 4);
        std::memcpy(out + 4, &material_id, 2);
        out[6] = static_cast<std::uint8_t>(channel_idx);
        out[7] = r;
        out[8] = g;
        out[9] = b;
        out[10] = 0xFF;
        out[11] = repair_byte;
        if (channel_idx == 0 || channel_idx == 3)
            out[13] = 0x04;
    }

    static std::int64_t __fastcall dye_copier_inline_detour(std::uintptr_t a1, std::uintptr_t a2) noexcept
    {
        // Snapshot a1 - RCX can be clobbered after the trampoline.
        const auto dst_struct = a1;

        const auto result = g_dye_copier_trampoline(a1, a2);

        // Gate: skip when not in LT apply, no state published, or DyeCopy never resolved.
        //
        // There is deliberately NO one-shot gate here. DyeCopier fires several times per slot (see the
        // s_inject_consumed comment above), and the destination of the FIRST call is not the one that reaches the
        // renderer. That first destination is a throwaway buffer that the engine tears down right after the apply, so
        // a one-shot inject writes correct records into memory nobody reads.
        //
        // That failure mode is deceptive, so recognize it by its signature: the diagnostic below reports a fully
        // correct destination - full record count, correct group hash and RGB, zero leftover real-item records after
        // the upsert - and a read-back at that same vector address returns all zeroes seconds later, because the
        // buffer is freed. Correcting WHAT the detour writes to that buffer never fixes the render.
        //
        // Injecting on every call while the state is published means whichever destination is the live one receives
        // the records. This is only safe because the per-channel upsert below is idempotent: a repeat inject
        // overwrites the record for the same channel instead of appending a duplicate.
        if (!in_transmog().load(std::memory_order_acquire))
            return result;
        if (!s_inject_active || g_dye_copy_fn == nullptr)
            return result;

        // Locate the first active channel. In dense mode this also serves as the fill value for inactive channels. In
        // sparse mode it is only used for diagnostics, because the per-record emission loop skips inactive channels
        // outright. Either way, when no channel is active, there is nothing to emit. The detour returns before it
        // touches the destination.
        const ChannelState *fallback = nullptr;
        for (std::size_t i = 0; i < DYE_CHANNEL_COUNT; ++i)
        {
            if (s_inject_channels[i].group_hash != 0)
            {
                fallback = &s_inject_channels[i];
                break;
            }
        }
        if (fallback == nullptr)
            return result;

        // Destination dye-record vector. The offset tracks the auth-table entry layout and moves with the entry
        // stride: an entry stride of 0xC8 puts the vector at dst+0x70, an entry stride of 0xD0 puts it at dst+0x78. A
        // stale value hands g_dye_copy_fn the vector's count field instead of its base, so every append faults.
        //
        // Vector header, relative to target_vec: +0x00 data pointer, +0x08 count, +0x0C capacity. That layout comes
        // straight from the engine primitive: it reads capacity at +0x0C, count at +0x08, grows when capacity is not
        // greater than count, writes the record at data + count * 16, then increments count.
        const auto target_vec = dst_struct + DYE_VECTOR_OFFSET;

        // Dedup precondition for the per-channel upsert below.
        //
        // The detour runs on every DyeCopier call while a slot state is published, because the call whose destination
        // reaches the renderer is not always the first one. Repeated injection is only safe while it stays idempotent,
        // which requires reading the existing records to find the one that already owns each channel. If the header is
        // unreadable or the count is implausible, that scan cannot prove a channel is absent. A blind append then
        // duplicates records on every call and grows the vector without bound. When the scan is not possible, skip
        // this call instead. Another DyeCopier call follows.
        const auto scan_data =
            DMK::memory::read<std::uintptr_t>(DMK::Address{target_vec + VEC_DATA_OFFSET}).value_or(0);
        const auto scan_count =
            DMK::memory::read<std::uint32_t>(DMK::Address{target_vec + VEC_COUNT_OFFSET}).value_or(INVALID_COUNT);
        const bool scannable = (scan_count == 0) || (DMK::memory::is_plausible_ptr(DMK::Address{scan_data}) &&
                                                     scan_count <= MAX_SCAN_RECORDS);
        if (!scannable)
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "[dye-inject] skipped: destination vector not scannable (data=0x{:X} count={})",
                scan_data,
                scan_count
            );
            return result;
        }

        bool all_ok = true;
        std::size_t emitted = 0;
        const bool sparse = s_inject_sparse;
        for (std::size_t i = 0; i < DYE_CHANNEL_COUNT && all_ok; ++i)
        {
            const bool active = s_inject_channels[i].group_hash != 0;
            if (!active && sparse)
                continue;
            const auto &channel = active ? s_inject_channels[i] : *fallback;
            std::uint8_t record[DYE_RECORD_SIZE];
            build_dye_record(
                record,
                i,
                channel.group_hash,
                channel.r,
                channel.g,
                channel.b,
                channel.material_id,
                channel.repair_byte
            );

            // Upsert this channel instead of appending it.
            //
            // The engine's own copy loop runs inside the trampoline, before this code, and fills the destination with
            // the records of the real item underneath the transmog. A plain append then leaves two records for the
            // same channel. The consumer resolves the first match per channel, so the real item's record wins and the
            // override never renders. Overwriting the record that already owns the channel is correct whichever way
            // the consumer resolves. It also keeps the channels this slot does not override. A bulk reset of the count
            // discards those channels in sparse mode.
            //
            // The header is re-read every iteration because an append can grow and relocate the array.
            bool replaced = false;
            const auto vec_data =
                DMK::memory::read<std::uintptr_t>(DMK::Address{target_vec + VEC_DATA_OFFSET}).value_or(0);
            const auto vec_count =
                DMK::memory::read<std::uint32_t>(DMK::Address{target_vec + VEC_COUNT_OFFSET}).value_or(INVALID_COUNT);
            if (vec_count > MAX_SCAN_RECORDS ||
                (vec_count > 0 && !DMK::memory::is_plausible_ptr(DMK::Address{vec_data})))
            {
                // Dedup is no longer possible mid-loop. Stop rather than append blind.
                all_ok = false;
                break;
            }
            if (DMK::memory::is_plausible_ptr(DMK::Address{vec_data}) && vec_count > 0)
            {
                for (std::uint32_t k = 0; k < vec_count; ++k)
                {
                    const auto rec_addr = vec_data + static_cast<std::uintptr_t>(k) * DYE_RECORD_SIZE;
                    const auto existing_channel =
                        DMK::memory::read<std::uint8_t>(DMK::Address{rec_addr + RECORD_CHANNEL_OFFSET})
                            .value_or(NO_CHANNEL);
                    if (existing_channel != static_cast<std::uint8_t>(i))
                        continue;
                    // write_in_place never changes page protection: the dye vector is already writable engine
                    // data, so a target that is not writable is a sign the pointer is wrong, and failing closed
                    // there is the correct outcome.
                    replaced = DMK::memory::write_in_place(
                                   DMK::Address{rec_addr},
                                   std::span{reinterpret_cast<const std::byte *>(record), DYE_RECORD_COPY_SPAN}
                    )
                                   .has_value();
                    break;
                }
            }

            all_ok = replaced || seh_call_dye_copy(g_dye_copy_fn, target_vec, reinterpret_cast<std::uintptr_t>(record));
            if (all_ok)
                ++emitted;
        }

        // Marks that at least one inject ran for the published state. This flag does not gate injection. It keeps the
        // summary below to one line per slot instead of one line per DyeCopier call.
        const bool first_inject_for_slot = !s_inject_consumed;
        s_inject_consumed = true;
        if (!first_inject_for_slot)
            return result;

        const auto inject_count = g_dyeInjectCount.fetch_add(1, std::memory_order_relaxed);
        (void)DMK::log().try_log(
            DMK::LogLevel::Debug,
            "[dye-inject] #{} vec=0x{:X} mode={} emitted={}/{} first_hash=0x{:08X} rgb=({:02X},{:02X},{:02X}) ok={}",
            inject_count,
            target_vec,
            sparse ? "sparse" : "dense",
            emitted,
            DYE_CHANNEL_COUNT,
            fallback->group_hash,
            fallback->r,
            fallback->g,
            fallback->b,
            all_ok
        );
        return result;
    }

    // - Public API

    bool init(DMK::hook::HookStack &hooks) noexcept
    {
        auto &logger = DMK::log();

        // Both addresses come from the anchor registry, which already rejected a value outside the host image and,
        // for these code entries, a first byte that cannot begin an instruction. That validator is what used to be an
        // explicit entry check here: both DyeCopier rows below P1 reach the entry through a negative walk-back, so a
        // prologue that changes length resolves them short WITHOUT failing the pattern match, and an inline detour
        // written into the middle of a live instruction is a delayed crash. A rejected anchor arrives as 0.
        const auto copy_target = anchor_address(AnchorId::DyeCopy);
        if (copy_target == 0)
        {
            (void)logger.try_log(
                DMK::LogLevel::Warning,
                "[dye-inject] AOB resolve failed for DyeCopy primitive; dye injection disabled"
            );
            return false;
        }

        const auto copier_target = anchor_address(AnchorId::DyeCopier);
        if (copier_target == 0)
        {
            (void)logger.try_log(
                DMK::LogLevel::Warning,
                "[dye-inject] AOB resolve failed for DyeCopier; dye injection disabled"
            );
            return false;
        }

        // Bind the DyeCopy function pointer to the AOB-resolved address. The inline detour calls it directly to
        // append records.
        g_dye_copy_fn = reinterpret_cast<DyeCopy_t>(copy_target);

        // DyeCopier inline detour (the injection site).
        auto copier = DMK::hook::inline_at(
            DMK::hook::InlineRequest{.name = "DyeCopierInjectInline", .target = DMK::Address{copier_target}},
            &dye_copier_inline_detour
        );
        if (!copier)
        {
            (void)logger.try_log(
                DMK::LogLevel::Warning,
                "[dye-inject] DyeCopier inline-hook creation failed at 0x{:X}: {}",
                copier_target,
                copier.error().message()
            );
            return false;
        }

        // Publish the trampoline BEFORE enable() arms the patch, so no game thread can enter the detour while its
        // original pointer is still null.
        g_dye_copier_trampoline = copier->original<DyeCopier_t>();
        if (auto armed = copier->enable(); !armed)
        {
            (void)logger.try_log(
                DMK::LogLevel::Warning,
                "[dye-inject] DyeCopier inline-hook could not be armed at 0x{:X}: {}",
                copier_target,
                armed.error().message()
            );
            g_dye_copier_trampoline = nullptr;
            return false;
        }
        hooks.push(std::move(*copier));

        (void)logger.try_log(
            DMK::LogLevel::Info,
            "[dye-inject] DyeCopier inline-hook installed at 0x{:X}; DyeCopy fn at 0x{:X}",
            copier_target,
            reinterpret_cast<std::uintptr_t>(g_dye_copy_fn)
        );

        return true;
    }

    void log_counters() noexcept
    {
        (void)DMK::log().try_log(
            DMK::LogLevel::Debug,
            "[dye-inject] counters: injects={}",
            g_dyeInjectCount.load(std::memory_order_relaxed)
        );
    }

    void restore_all() noexcept
    {
        log_counters();
    }

    // Cross-thread snapshot of the first active channel's RGB. The setter-substitute hook runs on the engine's render
    // thread, which differs from the LT apply thread where the thread_local s_inject_channels is set. Atomic snapshot
    // lets the render-side detour read the user color without TLS coupling.
    static std::atomic<std::uint32_t> g_publishedRGB{0};

    void set_slot_dye_state(const ChannelState *channels, bool sparse) noexcept
    {
        bool any_active = false;
        std::uint32_t first_hash = 0;
        std::uint8_t first_r = 0, first_g = 0, first_b = 0;
        int active_count = 0;
        for (std::size_t i = 0; i < DYE_CHANNEL_COUNT; ++i)
        {
            s_inject_channels[i] = channels[i];
            if (channels[i].group_hash != 0)
            {
                if (!any_active)
                {
                    first_hash = channels[i].group_hash;
                    first_r = channels[i].r;
                    first_g = channels[i].g;
                    first_b = channels[i].b;
                }
                any_active = true;
                ++active_count;
            }
        }
        s_inject_active = any_active;
        s_inject_sparse = sparse;
        s_inject_consumed = false;

        // Cross-thread snapshot for color_override::setter_substitute. Bit-layout:
        //   bits  0..7 : R
        //   bits  8..15: G
        //   bits 16..23: B
        //   bit  24    : active flag
        std::uint32_t snap = any_active
                                 ? (static_cast<std::uint32_t>(first_r) | (static_cast<std::uint32_t>(first_g) << 8) |
                                    (static_cast<std::uint32_t>(first_b) << 16) | (1u << 24))
                                 : 0;
        g_publishedRGB.store(snap, std::memory_order_release);

        (void)DMK::log().try_log(
            DMK::LogLevel::Debug,
            "[dye-inject] state set: active_count={} firstHash=0x{:08X} "
            "firstRGB=({:02X},{:02X},{:02X}) snapshot=0x{:08X} &snap={:#x}",
            active_count,
            first_hash,
            first_r,
            first_g,
            first_b,
            snap,
            reinterpret_cast<std::uintptr_t>(&g_publishedRGB)
        );
    }

    void clear_slot_dye_state() noexcept
    {
        s_inject_active = false;
        s_inject_consumed = false;
        s_inject_sparse = false;
        // NOTE: deliberately do NOT clear g_publishedRGB here. LT calls clear_slot_dye_state immediately after each
        // apply_transmog completes. The engine reads the dst+120 records once during slot_pop and discards them, so
        // clearing the ARMOR_MOD inject state is fine. But the engine's per-property color setter fires DURING RENDER
        // frames, long after apply_transmog returns, so the color_override::setter_substitute hook needs the RGB
        // snapshot to persist beyond apply. The next set_slot_dye_state (a preset color change) overwrites the
        // snapshot, so stale state self-clears on the next apply pass.
    }

    bool get_published_first_active_rgb(std::uint8_t *r, std::uint8_t *g, std::uint8_t *b) noexcept
    {
        auto snap = g_publishedRGB.load(std::memory_order_acquire);
        if ((snap & (1u << 24)) == 0)
            return false;
        if (r)
            *r = static_cast<std::uint8_t>(snap & 0xFF);
        if (g)
            *g = static_cast<std::uint8_t>((snap >> 8) & 0xFF);
        if (b)
            *b = static_cast<std::uint8_t>((snap >> 16) & 0xFF);
        return true;
    }

    void
    log_dye_snapshot(const char *source, const char *slot_name, const ChannelState (&state)[DYE_CHANNEL_COUNT]) noexcept
    {
        auto &logger = DMK::log();
        std::size_t active = 0;
        for (const auto &ch : state)
            if (ch.group_hash != 0)
                ++active;
        (void)logger.try_log(
            DMK::LogLevel::Trace,
            "[dye-snapshot] src={} slot={} active_channels={}/{}",
            source,
            slot_name,
            active,
            DYE_CHANNEL_COUNT
        );
        for (std::size_t i = 0; i < DYE_CHANNEL_COUNT; ++i)
        {
            const auto &ch = state[i];
            if (ch.group_hash == 0)
            {
                (void)logger.try_log(DMK::LogLevel::Trace, "[dye-snapshot]   ch[{:02}] (empty)", i);
                continue;
            }
            (void)logger.try_log(
                DMK::LogLevel::Trace,
                "[dye-snapshot]   ch[{:02}] hash=0x{:08X} mat=0x{:04X} rgb=({:02X},{:02X},{:02X}) repair=0x{:02X}",
                i,
                ch.group_hash,
                ch.material_id,
                ch.r,
                ch.g,
                ch.b,
                ch.repair_byte
            );
        }
    }

    std::size_t read_entry_dye_records(std::uintptr_t entry_base, ChannelState (&out)[DYE_CHANNEL_COUNT]) noexcept
    {
        for (auto &c : out)
            c = ChannelState{};

        // entry_base points into the live auth/dye table, which can tear or relocate on a world reload or arena flip.
        // Both call sites invoke this outside an SEH frame, so every read here is self-guarded: a faulting header read
        // yields 0 and the entry is treated as having no records.
        //
        // The dye vector sits at the same offset inside an auth-table entry as it does inside the DyeCopier
        // destination struct, so both paths read it through DYE_VECTOR_OFFSET. That offset tracks the auth-entry
        // stride and MUST move with it: see ENTRY_STRIDE and ENTRY_SLOT_TAG_OFFSET in real_part_tear_down.cpp. A
        // stale value lands on the neighboring field and dereferences a garbage pointer instead of failing closed.
        const auto data =
            DMK::memory::read<std::uintptr_t>(DMK::Address{entry_base + DYE_VECTOR_OFFSET + VEC_DATA_OFFSET})
                .value_or(0);
        auto count = DMK::memory::read<std::uint32_t>(DMK::Address{entry_base + DYE_VECTOR_OFFSET + VEC_COUNT_OFFSET})
                         .value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{data}) || count == 0)
            return 0;
        if (count > DYE_CHANNEL_COUNT)
            count = static_cast<std::uint32_t>(DYE_CHANNEL_COUNT);

        std::size_t filled = 0;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto rec = data + i * DYE_RECORD_SIZE;
            // Copy the whole record under one fault guard, then parse from the local buffer so a torn record cannot
            // fault mid-field.
            std::uint8_t buf[DYE_RECORD_SIZE];
            if (!DMK::memory::read_into(DMK::Address{rec}, std::span{reinterpret_cast<std::byte *>(buf), sizeof(buf)}))
                continue;

            std::uint32_t group_hash = 0;
            std::memcpy(&group_hash, buf + 0, sizeof(group_hash));
            if (group_hash == 0)
                continue;
            const std::uint8_t channel_idx = buf[6];
            if (channel_idx >= DYE_CHANNEL_COUNT)
                continue;

            std::uint16_t material_id = 0;
            std::memcpy(&material_id, buf + 4, sizeof(material_id));
            out[channel_idx] = ChannelState{
                .group_hash = group_hash,
                .r = buf[7],
                .g = buf[8],
                .b = buf[9],
                .material_id = material_id,
                .repair_byte = buf[11],
            };
            ++filled;
        }
        return filled;
    }
} // namespace Transmog::dye_record_inject
