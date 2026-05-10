// Per-slot dye-record injector. See `dye_record_inject.hpp` for the
// architectural overview. This file installs one inline detour and
// resolves one engine function pointer:
//
//   * DyeCopier (sub_141E019E0) -- INLINE DETOUR. After the engine's
//     natural copy completes (with empty source for LT-fake), the
//     detour synthesizes per-channel ARMOR_MOD records and APPENDS
//     them to dst+120 by calling the DyeCopy primitive directly.
//     This is independent of any real item the user wears.
//
//   * DyeCopy (sub_140CADEF0) -- function pointer only. The engine's
//     16-byte record-copy primitive; the detour calls it directly to
//     append synthesized records. Resolved via AOB cascade so the
//     call target tracks executable patches.
//
// Why an inline detour rather than a mid-hook here:
//   * The engine writes the natural record vector inside the
//     trampoline body; we must run AFTER it to append.
//   * Substitution must be conditional on LT being active and a slot
//     state being published. A mid-hook on entry runs unconditionally.
//   * The DyeCopy primitive is reused as a function call, so we need
//     a stable resolved address; reusing the AOB-resolved value keeps
//     hook target and call target locked together.

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
    // Per-slot dye injection state set by LT's dispatch loop before
    // each apply_transmog call. Thread-local because the DyeCopier
    // detour runs on the same thread that published the state.
    //
    //   s_injectActive: true if any channel has group_hash != 0.
    //   s_injectChannels: per-channel state.
    //   s_injectConsumed: set after first inject in current slotpop;
    //     prevents double-inject from the multiple sub_141E019E0
    //     calls that fire per slot.
    static thread_local bool s_injectActive = false;
    static thread_local ChannelState
        s_injectChannels[k_dyeChannelCount] = {};
    static thread_local bool s_injectConsumed = false;

    // -- Counters -----------------------------------------------------
    static std::atomic<std::uint64_t> g_dyeInjectCount{0};

    // -- SEH-protected memory primitives ------------------------------
    //
    // The DyeCopy call may transiently fault on alloc or vector-header
    // access during asset reload. Wrap it in SEH so a bad state fails
    // the call instead of crashing the game thread. The SEH __try
    // block lives in its own function because it cannot share scope
    // with C++ object unwinding (string, etc.).

    // -- DyeCopier inline detour (the injection site) -----------------
    //
    // After the engine's own dye-copy completes (with empty source for
    // LT-fake), we APPEND `k_dyeChannelCount` records by calling the
    // engine's record-copy primitive directly. This is INDEPENDENT of
    // any real item the user wears.
    //
    // The detour is gated by:
    //   1. `Transmog::in_transmog()` -- only patch during LT apply
    //   2. `s_injectActive`          -- a slot state was published
    //   3. `!s_injectConsumed`       -- only inject once per slotpop
    //   4. `g_dye_copy_fn != nullptr` -- DyeCopy AOB resolved cleanly

    using DyeCopier_t = std::int64_t(__fastcall *)(std::uintptr_t a1,
                                                   std::uintptr_t a2);
    using DyeCopy_t   = std::int64_t(__fastcall *)(std::uintptr_t a1,
                                                   std::uintptr_t a2);

    static DyeCopier_t g_dye_copier_trampoline = nullptr;
    static DyeCopy_t   g_dye_copy_fn           = nullptr;

    // SEH wrapper around the DyeCopy call. The engine primitive
    // touches the target vector header and may alloc; isolate faults.
    static bool seh_call_dye_copy(DyeCopy_t fn,
                                  std::uintptr_t target_vec,
                                  std::uintptr_t src_record) noexcept
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

    // Build a 16-byte ARMOR_MOD record. See ChannelState in the header
    // for the offset map. The +13 = 0x04 marker on indices 0 and 3
    // mirrors what natural captures show; the engine accepts records
    // without it but matching the natural shape avoids any latent
    // shape-validation we have not RE'd yet.
    static void build_dye_record(std::uint8_t *out,
                                 std::size_t channel_idx,
                                 std::uint32_t group_hash,
                                 std::uint8_t r,
                                 std::uint8_t g,
                                 std::uint8_t b,
                                 std::uint16_t material_id,
                                 std::uint8_t repair_byte) noexcept
    {
        std::memset(out, 0, 16);
        std::memcpy(out + 0, &group_hash, 4);
        std::memcpy(out + 4, &material_id, 2);
        out[6]  = static_cast<std::uint8_t>(channel_idx);
        out[7]  = r;
        out[8]  = g;
        out[9]  = b;
        out[10] = 0xFF;
        out[11] = repair_byte;
        if (channel_idx == 0 || channel_idx == 3)
            out[13] = 0x04;
    }

    static std::int64_t __fastcall dye_copier_inline_detour(
        std::uintptr_t a1, std::uintptr_t a2) noexcept
    {
        // Snapshot a1 -- RCX may be clobbered after the trampoline.
        const auto dst_struct = a1;

        const auto result = g_dye_copier_trampoline(a1, a2);

        // Gate: skip when not in LT apply, no state published, already
        // consumed for this slot, or DyeCopy never resolved.
        if (!in_transmog().load(std::memory_order_acquire))
            return result;
        if (!s_injectActive || s_injectConsumed
            || g_dye_copy_fn == nullptr)
            return result;

        // Locate the first active channel. We always emit
        // `k_dyeChannelCount` records (16); inactive channels reuse
        // the first active channel's settings. This is empirical:
        // sparse injection (skipping inactive indices) leaves the
        // engine's natural records dominant and the dye does not
        // render. Per-channel selective override (preserving the
        // engine's natural record for inactive channels) requires
        // pre-trampoline capture of the natural records and is
        // deferred.
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

        // Append into the destination vector at dst+120.
        const auto target_vec = dst_struct + 120;
        bool all_ok = true;
        for (std::size_t i = 0; i < k_dyeChannelCount && all_ok; ++i)
        {
            const auto &channel =
                s_injectChannels[i].group_hash != 0
                    ? s_injectChannels[i]
                    : *fallback;
            std::uint8_t record[16];
            build_dye_record(record, i, channel.group_hash,
                             channel.r, channel.g, channel.b,
                             channel.material_id, channel.repair_byte);
            all_ok = seh_call_dye_copy(
                g_dye_copy_fn, target_vec,
                reinterpret_cast<std::uintptr_t>(record));
        }

        s_injectConsumed = true;

        const auto inject_count = g_dyeInjectCount.fetch_add(
            1, std::memory_order_relaxed);
        DMK::Logger::get_instance().info(
            "[dye-inject] #{} dst+120=0x{:X} fallback_hash=0x{:08X} "
            "rgb=({:02X},{:02X},{:02X}) ok={}",
            inject_count, target_vec, fallback->group_hash,
            fallback->r, fallback->g, fallback->b, all_ok);
        return result;
    }

    // -- Public API ---------------------------------------------------

    bool init() noexcept
    {
        auto &logger = DMK::Logger::get_instance();

        // Resolve all targets via patch-proof AOB cascades before any
        // hook is installed. resolve_address returns 0 on cascade
        // failure; a hook against address 0 would smash the PE header.
        const auto copy_target =
            resolve_address(k_dyeCopyCandidates, "DyeRecordInject_DyeCopy");
        if (copy_target == 0)
        {
            logger.warning(
                "[dye-inject] AOB resolve failed for DyeCopy primitive; "
                "dye injection disabled");
            return false;
        }

        const auto copier_target =
            resolve_address(k_dyeCopierCandidates,
                            "DyeRecordInject_DyeCopier");
        if (copier_target == 0)
        {
            logger.warning(
                "[dye-inject] AOB resolve failed for DyeCopier; "
                "dye injection disabled");
            return false;
        }

        auto &hookMgr = DMK::HookManager::get_instance();

        // Bind the DyeCopy function pointer to the AOB-resolved address;
        // the inline detour calls it directly to append records.
        g_dye_copy_fn = reinterpret_cast<DyeCopy_t>(copy_target);

        // DyeCopier inline detour (the injection site).
        auto copier_res = hookMgr.create_inline_hook(
            "DyeCopierInjectInline", copier_target,
            reinterpret_cast<void *>(&dye_copier_inline_detour),
            reinterpret_cast<void **>(&g_dye_copier_trampoline));
        if (copier_res.has_value())
            logger.info(
                "[dye-inject] DyeCopier inline-hook installed at 0x{:X}; "
                "DyeCopy fn at 0x{:X}",
                copier_target,
                reinterpret_cast<std::uintptr_t>(g_dye_copy_fn));
        else
            logger.warning(
                "[dye-inject] DyeCopier inline-hook FAILED: {}",
                DetourModKit::Hook::error_to_string(copier_res.error()));

        return true;
    }

    void log_counters() noexcept
    {
        DMK::Logger::get_instance().info(
            "[dye-inject] counters: injects={}",
            g_dyeInjectCount.load(std::memory_order_relaxed));
    }

    void restore_all() noexcept { log_counters(); }

    void set_slot_dye_state(const ChannelState *channels) noexcept
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
        s_injectConsumed = false;
        DMK::Logger::get_instance().info(
            "[dye-inject] state set: active_count={} firstHash=0x{:08X} "
            "firstRGB=({:02X},{:02X},{:02X})",
            active_count, first_hash, first_r, first_g, first_b);
    }

    void clear_slot_dye_state() noexcept
    {
        s_injectActive = false;
        s_injectConsumed = false;
    }
}
