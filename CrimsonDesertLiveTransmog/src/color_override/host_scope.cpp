#include "host_scope.hpp"

#include "../aob_resolver.hpp"
#include "../shared_state.hpp"

#include <DetourModKit/hook.hpp>
#include <DetourModKit/logger.hpp>
#include <DetourModKit/memory.hpp>
#include <DetourModKit/region.hpp>

#include <array>
#include <atomic>
#include <cstdint>

namespace Transmog::ColorOverride::HostScope
{
    namespace
    {
        // Per-host owner-container vfuncs. Each is a vtable slot on the per-host matInst-container; at entry RCX IS the
        // owner container (no `[+0x10]` deref). They internally call the matInst-list copy loop, which
        // dispatches the publisher, which eventually invokes the 4-byte property setter that
        // ColorOverride::SetterSubstitute hooks. Both targets are resolved live via the AOB cascade in
        // `aob_resolver.hpp` so they survive function relocation across patches.

        // Per-thread RSP-tagged owner state. x64 RSP grows DOWNWARD, so setter_rsp < tl_ownerRsp puts the setter
        // inside the iter and names a live owner. setter_rsp >= tl_ownerRsp means the iter returned and the tag is
        // stale, so the gate falls back to permissive because no iter covers the setter call frame at all.
        thread_local std::uintptr_t tl_ownerParent = 0;
        thread_local std::uintptr_t tl_ownerRsp = 0;

        // Cluster histogram: bounded lock-free open-address table. Writes are from the engine render thread only, so
        // the atomics handle the rare publisher-side reader cleanly.
        struct ClusterEntry
        {
            std::atomic<std::uintptr_t> parent{0};
            std::atomic<std::uint32_t> hits{0};
        };
        constexpr std::size_t k_clusterCap = 64;
        constexpr std::uint32_t k_electionFloorHits = 5;
        std::array<ClusterEntry, k_clusterCap> g_cluster{};

        // Diagnostic counters.
        std::atomic<std::uint64_t> g_dbgEntered{0};
        std::atomic<std::uint64_t> g_dbgPlayer{0};
        std::atomic<std::uint64_t> g_dbgNpc{0};
        std::atomic<std::uint64_t> g_dbgFreed{0};
        std::atomic<std::uint64_t> g_dbgStale{0};

        std::atomic<bool> g_initDone{false};

        // Tests whether @p p lies inside the host EXE mapped range. `Region::host()` is loader-backed and re-walks
        // the PE headers on every call, so the span is captured once and `contains` is then pure constexpr
        // arithmetic. This runs once per owner-container vfunc fire.
        bool ptr_in_text_or_rdata(std::uintptr_t p) noexcept
        {
            static const DMK::Region s_hostImage = DMK::Region::host();
            return s_hostImage.contains(DMK::Address{p});
        }

        bool looks_like_live_host(std::uintptr_t parent) noexcept
        {
            if (parent == 0)
                return false;
            // Heap range sanity: container parents land in 0x4xxxxxxxxx on this engine. Anything below the 4 GiB
            // boundary is either a small int or stack junk, so reject it. This floor is deliberately stricter than
            // `memory::is_plausible_ptr`, whose 0x10000 floor lets four orders of magnitude more through.
            if (parent < 0x100000000ull)
                return false;
            const auto vtbl = DMK::memory::read<std::uintptr_t>(DMK::Address{parent}).value_or(0);
            return vtbl != 0 && ptr_in_text_or_rdata(vtbl);
        }

        void cluster_reset() noexcept
        {
            for (auto &e : g_cluster)
            {
                e.parent.store(0, std::memory_order_relaxed);
                e.hits.store(0, std::memory_order_relaxed);
            }
        }

        // O(k_clusterCap) insert/increment.
        void cluster_record(std::uintptr_t parent) noexcept
        {
            if (parent == 0)
                return;
            for (auto &e : g_cluster)
            {
                auto cur = e.parent.load(std::memory_order_acquire);
                if (cur == parent)
                {
                    e.hits.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (cur == 0)
                {
                    std::uintptr_t expected = 0;
                    if (e.parent.compare_exchange_strong(expected, parent, std::memory_order_acq_rel))
                    {
                        e.hits.store(1, std::memory_order_relaxed);
                        return;
                    }
                    if (e.parent.load(std::memory_order_acquire) == parent)
                    {
                        e.hits.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                }
            }
        }

        // True when the cluster has enough hits for an election to be meaningful. Below floor the gate stays permissive
        // so the first ~5 vfunc calls of an apply do not lock the substitute out before the cluster has any data.
        bool election_ready() noexcept
        {
            std::uint32_t topHits = 0;
            for (auto &e : g_cluster)
            {
                const auto h = e.hits.load(std::memory_order_relaxed);
                if (h > topHits)
                    topHits = h;
            }
            return topHits >= k_electionFloorHits;
        }

        // True if `parent` is in the elected player set:
        // `hits >= top * 0.1`. The 10 % threshold admits dual-parent player + LOD-cull variance while still cleanly
        // rejecting NPCs whose hits sit below ~1 % of the leader.
        bool is_player_parent(std::uintptr_t parent) noexcept
        {
            if (parent == 0)
                return false;
            std::uint32_t topHits = 0;
            std::uint32_t parentHits = 0;
            for (auto &e : g_cluster)
            {
                const auto p = e.parent.load(std::memory_order_acquire);
                const auto h = e.hits.load(std::memory_order_relaxed);
                if (p == 0)
                    continue;
                if (h > topHits)
                    topHits = h;
                if (p == parent)
                    parentHits = h;
            }
            if (topHits < k_electionFloorHits)
                return false;
            // parentHits * 10 >= topHits avoids float division.
            return parentHits * 10 >= topHits;
        }

        // Mid-hook on the per-host owner-container vfuncs. RCX at entry IS the owner container; the iter that
        // dispatches the publisher (and ultimately the setter) lives below this frame on the stack, so the setter's RSP
        // < tl_ownerRsp for live captures.
        void on_iter_entry(DMK::hook::MidContext &ctx) noexcept
        {
            g_dbgEntered.fetch_add(1, std::memory_order_relaxed);

            const auto iter_rsp = DMK::hook::stack_pointer(ctx);
            const auto rcx = DMK::hook::gpr(ctx, DMK::hook::Gpr::Rcx);

            if (!looks_like_live_host(rcx))
            {
                g_dbgFreed.fetch_add(1, std::memory_order_relaxed);
                tl_ownerParent = 0;
                tl_ownerRsp = iter_rsp;
                return;
            }

            tl_ownerParent = rcx;
            tl_ownerRsp = iter_rsp;
            cluster_record(rcx);

            if (is_player_parent(rcx))
                g_dbgPlayer.fetch_add(1, std::memory_order_relaxed);
            else
                g_dbgNpc.fetch_add(1, std::memory_order_relaxed);
        }
    } // namespace

    bool is_current_host_player_owned(std::uintptr_t setter_rsp) noexcept
    {
        // Cold start: no iter has ever pushed an owner. Permissive so DLL-load and pre-first-apply renders are not
        // dropped.
        if (tl_ownerRsp == 0)
            return true;

        // The iter has returned: the setter call frame is no longer nested inside any owner-vfunc, so we have no host
        // scope to gate by. Permissive in this case (stale -> permissive is the documented outcome) and counted
        // separately for visibility.
        if (setter_rsp >= tl_ownerRsp)
        {
            g_dbgStale.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // Inside an iter, but the vfunc's RCX was junk.
        if (tl_ownerParent == 0)
            return false;

        // Inside an iter with a live host, but the cluster has not built up enough signal yet. Permissive so we do not
        // reject legitimate first-apply writes that fire before any NPC host has been seen.
        if (!election_ready())
            return true;

        return is_player_parent(tl_ownerParent);
    }

    void begin_apply_window() noexcept
    {
        if (!::Transmog::flag_color_override().load(std::memory_order_acquire))
            return;
        cluster_reset();
    }

    namespace
    {
        /**
         * @brief Installs one owner-container mid-hook and pushes it onto @p hooks.
         * @return true when the site resolved, the hook was created, and it armed.
         * @details Both vfuncs share one callback and one failure shape, so they share one installer. A missing
         *          address is a resolution miss the caller already logged through the anchor report, so it degrades
         *          quietly here; a creation or arm failure is a live-code failure and says so.
         */
        [[nodiscard]] bool install_iter_hook(DMK::hook::HookStack &hooks, std::string name, std::uintptr_t addr)
        {
            auto &log = DMK::log();
            if (addr == 0)
                return false;

            auto hook = DMK::hook::mid_at(
                DMK::hook::MidRequest{
                    .name = name,
                    .target = DMK::Address{addr},
                },
                &on_iter_entry
            );
            if (!hook)
            {
                log.warning("[dye-host-scope] {} hook FAILED at {:#x}: {}", name, addr, hook.error().message());
                return false;
            }
            if (auto armed = hook->enable(); !armed)
            {
                log.warning(
                    "[dye-host-scope] {} hook could not be armed at {:#x}: {}",
                    name,
                    addr,
                    armed.error().message()
                );
                return false;
            }
            hooks.push(std::move(*hook));
            return true;
        }
    } // namespace

    bool init(DMK::hook::HookStack &hooks)
    {
        if (g_initDone.load(std::memory_order_acquire))
            return true;

        const auto addr1 = anchor_address(AnchorId::HostScopeVfunc1);
        const auto addr2 = anchor_address(AnchorId::HostScopeVfunc2);
        if (addr1 == 0 && addr2 == 0)
            return false;

        // Deliberately not short-circuiting: both sites are installed even when the first fails, because the cluster
        // reset only sees a complete iteration when every owner-container entry point reports.
        const bool ok1 = install_iter_hook(hooks, "HostScopeVfunc1", addr1);
        const bool ok2 = install_iter_hook(hooks, "HostScopeVfunc2", addr2);
        const bool ok = ok1 && ok2;

        g_initDone.store(ok, std::memory_order_release);
        return ok;
    }

    Stats snapshot_stats() noexcept
    {
        return Stats{
            g_dbgEntered.load(std::memory_order_relaxed),
            g_dbgPlayer.load(std::memory_order_relaxed),
            g_dbgNpc.load(std::memory_order_relaxed),
            g_dbgFreed.load(std::memory_order_relaxed),
            g_dbgStale.load(std::memory_order_relaxed),
        };
    }

} // namespace Transmog::ColorOverride::HostScope
