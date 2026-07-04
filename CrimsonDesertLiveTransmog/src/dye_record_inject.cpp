// Per-slot dye-record injector. See `dye_record_inject.hpp` for the architectural overview. This file installs one
// inline detour and resolves one engine function pointer:
//
//   * DyeCopier (sub_141E019E0) -- INLINE DETOUR. After the engine's natural copy completes (with empty source for
//     LT-fake), the detour synthesizes per-channel ARMOR_MOD records and APPENDS them to dst+120 by calling the DyeCopy
//     primitive directly. This is independent of any real item the user wears.
//
//   * DyeCopy (sub_140CADEF0) -- function pointer only. The engine's 16-byte record-copy primitive; the detour calls it
//     directly to append synthesized records. Resolved via AOB cascade so the call target tracks executable patches.
//
// Why an inline detour rather than a mid-hook here:
//   * The engine writes the natural record vector inside the trampoline body; we must run AFTER it to append.
//   * Substitution must be conditional on LT being active and a slot state being published. A mid-hook on entry runs
//     unconditionally.
//   * The DyeCopy primitive is reused as a function call, so we need a stable resolved address; reusing the
//     AOB-resolved value keeps hook target and call target locked together.

#include "dye_record_inject.hpp"
#include "aob_resolver.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace Transmog::DyeRecordInject
{
    // Per-slot dye injection state set by LT's dispatch loop before each apply_transmog call. Thread-local because the
    // DyeCopier detour runs on the same thread that published the state.
    //
    //   s_injectActive: true if any channel has group_hash != 0.
    //   s_injectChannels: per-channel state.
    //   s_injectConsumed: set after first inject in current slotpop;
    //     prevents double-inject from the multiple sub_141E019E0
    //     calls that fire per slot.
    static thread_local bool s_injectActive = false;
    static thread_local ChannelState s_injectChannels[k_dyeChannelCount] = {};
    static thread_local bool s_injectConsumed = false;
    // When true, the detour emits records ONLY for channels with group_hash != 0; inactive slots are skipped entirely
    // instead of being filled with the first active channel's settings. Set by callers that mirror a real auth-table
    // source (the restore path) where the original mesh never colored those channels.
    static thread_local bool s_injectSparse = false;

    // -- Counters -----------------------------------------------------
    static std::atomic<std::uint64_t> g_dyeInjectCount{0};

    // -- SEH-protected memory primitives ------------------------------
    //
    // The DyeCopy call may transiently fault on alloc or vector-header access during asset reload. Wrap it in SEH so a
    // bad state fails the call instead of crashing the game thread. The SEH __try block lives in its own function
    // because it cannot share scope with C++ object unwinding (string, etc.).

    // -- DyeCopier inline detour (the injection site) -----------------
    //
    // After the engine's own dye-copy completes (with empty source for LT-fake), we APPEND `k_dyeChannelCount` records
    // by calling the engine's record-copy primitive directly. This is INDEPENDENT of any real item the user wears.
    //
    // The detour is gated by:
    //   1. `Transmog::in_transmog()` -- only patch during LT apply
    //   2. `s_injectActive`          -- a slot state was published
    //   3. `!s_injectConsumed`       -- only inject once per slotpop
    //   4. `g_dye_copy_fn != nullptr` -- DyeCopy AOB resolved cleanly

    using DyeCopier_t = std::int64_t(__fastcall *)(std::uintptr_t a1, std::uintptr_t a2);
    using DyeCopy_t = std::int64_t(__fastcall *)(std::uintptr_t a1, std::uintptr_t a2);

    static DyeCopier_t g_dye_copier_trampoline = nullptr;
    static DyeCopy_t g_dye_copy_fn = nullptr;

    // SEH wrapper around the DyeCopy call. The engine primitive touches the target vector header and may alloc; isolate
    // faults.
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

    // Build a 16-byte ARMOR_MOD record. See ChannelState in the header for the offset map. The +13 = 0x04 marker on
    // indices 0 and 3 mirrors what natural captures show; the engine accepts records without it but matching the
    // natural shape avoids any latent shape-validation we have not RE'd yet.
    static void build_dye_record(std::uint8_t *out, std::size_t channel_idx, std::uint32_t group_hash, std::uint8_t r,
                                 std::uint8_t g, std::uint8_t b, std::uint16_t material_id,
                                 std::uint8_t repair_byte) noexcept
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
        // Snapshot a1 -- RCX may be clobbered after the trampoline.
        const auto dst_struct = a1;

        const auto result = g_dye_copier_trampoline(a1, a2);

        // Gate: skip when not in LT apply, no state published, already consumed for this slot, or DyeCopy never
        // resolved.
        if (!in_transmog().load(std::memory_order_acquire))
            return result;
        if (!s_injectActive || s_injectConsumed || g_dye_copy_fn == nullptr)
            return result;

        // Locate the first active channel. In dense mode this also serves as the fill value for inactive channels. In
        // sparse mode it is only used for diagnostics (the per-record emission loop skips inactives outright in sparse
        // mode). Either way, if no channel is active there is nothing to emit and we skip out before touching the
        // destination.
        const ChannelState *fallback = nullptr;
        for (std::size_t i = 0; i < k_dyeChannelCount; ++i)
        {
            if (s_injectChannels[i].group_hash != 0)
            {
                fallback = &s_injectChannels[i];
                break;
            }
        }
        if (fallback == nullptr)
            return result;

        // Append into the destination dye-record vector. On v1.13.00 this moved from dst+0x78 (120) to dst+0x70 (112)
        // -- the same -8 shift as the auth-table entry and its dye header. Confirmed against the engine's own DyeCopier
        // (sub_141F87550): its record-copy loop runs `lea rcx,[r14+0x70]; call <DyeCopy 0x140D1E5E0>` with a 16-byte
        // record, i.e. it appends to dst+0x70 via the exact primitive we call here. Passing dst+0x78 handed
        // g_dye_copy_fn the vector's count field instead of its base, so every append faulted (emitted=0/16, ok=false).
        const auto target_vec = dst_struct + 112;
        bool all_ok = true;
        std::size_t emitted = 0;
        const bool sparse = s_injectSparse;
        for (std::size_t i = 0; i < k_dyeChannelCount && all_ok; ++i)
        {
            const bool active = s_injectChannels[i].group_hash != 0;
            if (!active && sparse)
                continue;
            const auto &channel = active ? s_injectChannels[i] : *fallback;
            std::uint8_t record[16];
            build_dye_record(record, i, channel.group_hash, channel.r, channel.g, channel.b, channel.material_id,
                             channel.repair_byte);
            all_ok = seh_call_dye_copy(g_dye_copy_fn, target_vec, reinterpret_cast<std::uintptr_t>(record));
            if (all_ok)
                ++emitted;
        }

        s_injectConsumed = true;

        const auto inject_count = g_dyeInjectCount.fetch_add(1, std::memory_order_relaxed);
        DMK::Logger::get_instance().debug("[dye-inject] #{} dst+120=0x{:X} mode={} emitted={}/{} "
                                          "first_hash=0x{:08X} rgb=({:02X},{:02X},{:02X}) ok={}",
                                          inject_count, target_vec, sparse ? "sparse" : "dense", emitted,
                                          k_dyeChannelCount, fallback->group_hash, fallback->r, fallback->g,
                                          fallback->b, all_ok);
        return result;
    }

    // -- Public API ---------------------------------------------------

    bool init() noexcept
    {
        auto &logger = DMK::Logger::get_instance();

        // Resolve all targets via patch-proof AOB cascades before any hook is installed. resolve_address returns 0 on
        // cascade failure; a hook against address 0 would smash the PE header.
        const auto copy_target = resolve_address(k_dyeCopyCandidates, "DyeRecordInject_DyeCopy");
        if (copy_target == 0)
        {
            logger.warning("[dye-inject] AOB resolve failed for DyeCopy primitive; "
                           "dye injection disabled");
            return false;
        }

        const auto copier_target = resolve_address(k_dyeCopierCandidates, "DyeRecordInject_DyeCopier");
        if (copier_target == 0)
        {
            logger.warning("[dye-inject] AOB resolve failed for DyeCopier; "
                           "dye injection disabled");
            return false;
        }

        auto &hookMgr = DMK::HookManager::get_instance();

        // Bind the DyeCopy function pointer to the AOB-resolved address; the inline detour calls it directly to append
        // records.
        g_dye_copy_fn = reinterpret_cast<DyeCopy_t>(copy_target);

        // DyeCopier inline detour (the injection site).
        auto copier_res = hookMgr.create_inline_hook("DyeCopierInjectInline", copier_target,
                                                     reinterpret_cast<void *>(&dye_copier_inline_detour),
                                                     reinterpret_cast<void **>(&g_dye_copier_trampoline));
        if (copier_res.has_value())
            logger.info("[dye-inject] DyeCopier inline-hook installed at 0x{:X}; "
                        "DyeCopy fn at 0x{:X}",
                        copier_target, reinterpret_cast<std::uintptr_t>(g_dye_copy_fn));
        else
            logger.warning("[dye-inject] DyeCopier inline-hook FAILED: {}",
                           DetourModKit::Hook::error_to_string(copier_res.error()));

        return true;
    }

    void log_counters() noexcept
    {
        DMK::Logger::get_instance().debug("[dye-inject] counters: injects={}",
                                          g_dyeInjectCount.load(std::memory_order_relaxed));
    }

    void restore_all() noexcept
    {
        log_counters();
    }

    // Cross-thread snapshot of the first active channel's RGB. The setter-substitute hook runs on the engine's render
    // thread, which differs from the LT apply thread where the thread_local s_injectChannels is set. Atomic snapshot
    // lets the render-side detour read the user color without TLS coupling.
    static std::atomic<std::uint32_t> g_publishedRGB{0};

    void set_slot_dye_state(const ChannelState *channels, bool sparse) noexcept
    {
        bool any_active = false;
        std::uint32_t first_hash = 0;
        std::uint8_t first_r = 0, first_g = 0, first_b = 0;
        int active_count = 0;
        for (std::size_t i = 0; i < k_dyeChannelCount; ++i)
        {
            s_injectChannels[i] = channels[i];
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
        s_injectActive = any_active;
        s_injectSparse = sparse;
        s_injectConsumed = false;

        // Cross-thread snapshot for ColorOverride::SetterSubstitute. Bit-layout:
        //   bits  0..7 : R
        //   bits  8..15: G
        //   bits 16..23: B
        //   bit  24    : active flag
        std::uint32_t snap = any_active
                                 ? (static_cast<std::uint32_t>(first_r) | (static_cast<std::uint32_t>(first_g) << 8) |
                                    (static_cast<std::uint32_t>(first_b) << 16) | (1u << 24))
                                 : 0;
        g_publishedRGB.store(snap, std::memory_order_release);

        DMK::Logger::get_instance().debug("[dye-inject] state set: active_count={} firstHash=0x{:08X} "
                                          "firstRGB=({:02X},{:02X},{:02X}) snapshot=0x{:08X} "
                                          "&snap={:#x}",
                                          active_count, first_hash, first_r, first_g, first_b, snap,
                                          reinterpret_cast<std::uintptr_t>(&g_publishedRGB));
    }

    void clear_slot_dye_state() noexcept
    {
        s_injectActive = false;
        s_injectConsumed = false;
        s_injectSparse = false;
        // NOTE: deliberately do NOT clear g_publishedRGB here. clear_slot_dye_state is called immediately after each
        // apply_transmog completes -- the engine reads the dst+120 records once during slotPop and discards them, so
        // clearing the ARMOR_MOD inject state is fine. But the per-property setter (sub_140A03810) fires DURING RENDER
        // frames, long after apply_transmog returned, so the ColorOverride::SetterSubstitute hook needs the RGB
        // snapshot to persist beyond apply. The snapshot gets overwritten on the next set_slot_dye_state (preset color
        // change) so stale state self-clears on the next apply pass.
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

    void log_dye_snapshot(const char *source, const char *slotName,
                          const ChannelState (&state)[k_dyeChannelCount]) noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        std::size_t active = 0;
        for (const auto &ch : state)
            if (ch.group_hash != 0)
                ++active;
        logger.trace("[dye-snapshot] src={} slot={} active_channels={}/{}", source, slotName, active,
                     k_dyeChannelCount);
        for (std::size_t i = 0; i < k_dyeChannelCount; ++i)
        {
            const auto &ch = state[i];
            if (ch.group_hash == 0)
            {
                logger.trace("[dye-snapshot]   ch[{:02}] (empty)", i);
                continue;
            }
            logger.trace("[dye-snapshot]   ch[{:02}] hash=0x{:08X} mat=0x{:04X} "
                         "rgb=({:02X},{:02X},{:02X}) repair=0x{:02X}",
                         i, ch.group_hash, ch.material_id, ch.r, ch.g, ch.b, ch.repair_byte);
        }
    }

    std::size_t read_entry_dye_records(std::uintptr_t entryBase, ChannelState (&out)[k_dyeChannelCount]) noexcept
    {
        for (auto &c : out)
            c = ChannelState{};

        // entryBase points into the live auth/dye table, which can tear or relocate on a world reload or arena flip.
        // Both call sites invoke this outside an SEH frame, so every read here is self-guarded: a faulting header read
        // yields 0 and the entry is treated as having no records.
        //
        // The dye-vector header offsets track the auth-entry layout, which shrank 8 bytes on v1.13.00 (the entry
        // reverted to the v1.04 stride 0xC8; see k_entrySlotTagOffset in real_part_tear_down.cpp). The data ptr moved
        // +0x78 -> +0x70 and the count +0x80 -> +0x78. Verified live: a dyed entry holds a 16-byte-record array ptr at
        // +0x70 with the channel count at +0x78.
        const auto data = DMKMemory::seh_read<std::uintptr_t>(entryBase + 0x70).value_or(0);
        auto count = DMKMemory::seh_read<std::uint32_t>(entryBase + 0x78).value_or(0);
        if (data < 0x10000 || count == 0)
            return 0;
        if (count > k_dyeChannelCount)
            count = static_cast<std::uint32_t>(k_dyeChannelCount);

        std::size_t filled = 0;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto rec = data + i * 16;
            // Copy the whole 16-byte record under one fault guard, then parse from the local buffer so a torn record
            // cannot fault mid-field.
            std::uint8_t buf[16];
            if (!DMKMemory::seh_read_bytes(rec, buf, sizeof(buf)))
                continue;

            std::uint32_t group_hash = 0;
            std::memcpy(&group_hash, buf + 0, sizeof(group_hash));
            if (group_hash == 0)
                continue;
            const std::uint8_t channel_idx = buf[6];
            if (channel_idx >= k_dyeChannelCount)
                continue;

            std::uint16_t material_id = 0;
            std::memcpy(&material_id, buf + 4, sizeof(material_id));
            out[channel_idx] = ChannelState{
                group_hash,
                buf[7],      // r
                buf[8],      // g
                buf[9],      // b
                material_id, // material_id
                buf[11],     // repair_byte
            };
            ++filled;
        }
        return filled;
    }
} // namespace Transmog::DyeRecordInject
