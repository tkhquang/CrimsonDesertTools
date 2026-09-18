#include "color_publisher_hook.hpp"
#include "color_carrier_set.hpp"
#include "color_matinst_owner.hpp"
#include "color_state.hpp"
#include "color_token_table.hpp"
#include "matinst_probe.hpp"

#include "../aob_resolver.hpp"
#include "../shared_state.hpp"

#include <DetourModKit.hpp>
#include <DetourModKit/hook.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Transmog::color_override::publisher_hook
{
    namespace
    {
        // Per-(dst, src) matInst hook invoked from the matInst-list copy loop. Signature: `__fastcall(matInst* dst
        // [rcx], matInst* src [rdx], ... [r8])`. Both rcx and rdx are captured as candidate carrier matInsts. Target
        // resolution lives in `color_publisher()` (aob_resolver.hpp).

        std::atomic<bool> g_initDone{false};

        std::atomic<std::uint64_t> g_entries{0};
        std::atomic<std::uint64_t> g_inserts{0};
        std::atomic<std::uint64_t> g_batchRejects{0};
        std::atomic<std::uint64_t> g_windowRejects{0};
        std::atomic<std::uint64_t> g_hostRejects{0};
        std::atomic<std::uint64_t> g_arecRejects{0};

        // SEH-guarded read of the permutations-token field (matInst+0x70). The guard is required, not optional:
        // probe_matinst never validates +0x70. Returns 0 on fault.
        std::uint16_t read_permut_token(std::uintptr_t mi) noexcept
        {
            return DMK::memory::read<std::uint16_t>(DMK::Address{mi + mat_inst_probe::MI_OFFSET_PERMUT_TOKEN})
                .value_or(0);
        }

        // Per-matInst validate + capture. Identity is established via the apply-window-scoped slot + non-zero
        // content_hash; the permutations-token resync runs as an opportunistic sanity refresh, not a gate.
        void try_insert(std::uintptr_t mi, int slot) noexcept
        {
            mat_inst_probe::MatInstFields mf{};
            if (!mat_inst_probe::probe_matinst(mi, mf))
            {
                g_arecRejects.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (mf.content_hash == 0)
            {
                g_arecRejects.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            // Refresh the cached permutations-token id. The engine's interner may hand out a different id this session;
            // we snapshot the live value so downstream consumers stay in sync. Not a gate.
            const auto perm_tok_expected = token_table::permutations_token();
            const auto perm_tok_live = read_permut_token(mi);
            if (perm_tok_expected != 0 && perm_tok_live != 0 && perm_tok_live != perm_tok_expected)
            {
                token_table::set_permutations_token(perm_tok_live);
            }

            // Insert into both the per-matInst-pointer set and the content-hash set. The setter substitute may gate by
            // either; the hash survives pool recycle of the pointer.
            carrier_set::add_hash(slot, mf.content_hash);
            if (carrier_set::add_matinst(slot, mi))
            {
                mat_inst_owner::set(mi, mf.content_hash, slot);
                g_inserts.fetch_add(1, std::memory_order_relaxed);
            }
        }

        void on_publisher_mid(DMK::hook::MidContext &ctx) noexcept
        {
            g_entries.fetch_add(1, std::memory_order_relaxed);

            // The apply slot is asserted by mark_apply_begin and remains set even after mark_apply_end - the publisher
            // fires per-frame asynchronously after the synchronous slot_pop returns, so a 3-second deadline gates the
            // tail rather than clearing the slot.
            const int slot = state::active_apply_slot().load(std::memory_order_acquire);
            if (slot < 0 || static_cast<std::size_t>(slot) >= ::Transmog::SLOT_COUNT)
                return;

            const auto deadline = state::active_apply_valid_until_ms().load(std::memory_order_acquire);
            if (deadline == 0 || state::now_ms() >= deadline)
            {
                g_windowRejects.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            // Publisher-insert block (wipe_slot sets a ~500 ms cool-down so the new slot's first fires do not race the
            // wipe).
            const auto block = state::block_publisher_inserts_until_ms().load(std::memory_order_acquire);
            if (block != 0 && state::now_ms() < block)
            {
                g_batchRejects.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            // Capture both dst and src matInsts. No host-scope gate here - host scope is only meaningful for the
            // setter call frame; the publisher fires per-matInst inside the matInst-list copy loop, not from an actor's
            // render frame.
            try_insert(DMK::hook::gpr(ctx, DMK::hook::Gpr::Rcx), slot);
            try_insert(DMK::hook::gpr(ctx, DMK::hook::Gpr::Rdx), slot);
        }
    } // namespace

    bool init(DMK::hook::HookStack &hooks)
    {
        if (g_initDone.load(std::memory_order_acquire))
            return true;

        auto &log = DMK::log();
        const auto addr = anchor_address(AnchorId::ColorPublisher);
        if (addr == 0)
            return false;

        auto hook = DMK::hook::mid_at(
            DMK::hook::MidRequest{
                .name = "ColorPublisher",
                .target = DMK::Address{addr},
            },
            &on_publisher_mid
        );
        if (!hook)
        {
            log.warning("[color-publisher] hook FAILED at {:#x}: {}", addr, hook.error().message());
            return false;
        }
        if (auto armed = hook->enable(); !armed)
        {
            log.warning("[color-publisher] hook could not be armed at {:#x}: {}", addr, armed.error().message());
            return false;
        }
        hooks.push(std::move(*hook));
        g_initDone.store(true, std::memory_order_release);
        return true;
    }

    Stats snapshot_stats() noexcept
    {
        return Stats{
            g_entries.load(std::memory_order_relaxed),
            g_inserts.load(std::memory_order_relaxed),
            g_batchRejects.load(std::memory_order_relaxed),
            g_windowRejects.load(std::memory_order_relaxed),
            g_hostRejects.load(std::memory_order_relaxed),
            g_arecRejects.load(std::memory_order_relaxed),
        };
    }

} // namespace Transmog::color_override::publisher_hook
