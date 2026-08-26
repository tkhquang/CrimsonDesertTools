#include "claim_walk_guard.hpp"

#include "aob_resolver.hpp"

#include <DetourModKit/hook_manager.hpp>
#include <DetourModKit/logger.hpp>
#include <DetourModKit/memory.hpp>
#include <DetourModKit/scanner.hpp>

#include <safetyhook/context.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Transmog::ClaimWalkGuard
{
    namespace
    {
        std::atomic<bool> g_installed{false};
        std::atomic<unsigned> g_patched{0};

        /// Upper bound on guarded sites. Each needs its own callback (a mid-hook callback receives only the register
        /// context, with no way to tell which site it fired for), so the count is fixed at compile time. Sized above
        /// the expected two to absorb a build that splits or duplicates a walker.
        constexpr std::size_t k_maxSites = 4;

        /// Loop-continue label per site, decoded from that site's own `jz`. Written during install and read by the
        /// callbacks; plain stores suffice because a hook cannot fire before its entry is filled in.
        std::array<std::uintptr_t, k_maxSites> g_continueAddr{};

        /**
         * @brief Skip a claim entry whose owner is mid-erase.
         *
         * RAX holds the entry's owning pointer, freshly loaded by the instruction ahead of the hook. While the erase
         * shifts the vector down, that pointer is transiently null but still inside the count, and the instruction
         * this hook precedes would dereference it at `+0x28`. Redirecting to the site's own loop-continue label makes
         * the walker treat the entry as absent -- which is what it would have observed a moment later, once the count
         * caught up.
         *
         * Runs per claim entry per walk, so it stays a single compare in the common case.
         */
        template <std::size_t Index> void on_claim_walk(safetyhook::Context &ctx) noexcept
        {
            if (ctx.rax == 0)
                ctx.rip = g_continueAddr[Index];
        }

        /// One callback per site index, so a site's continue address needs no lookup on the per-entry path.
        constexpr std::array<safetyhook::MidHookFn, k_maxSites> k_callbacks{
            &on_claim_walk<0>,
            &on_claim_walk<1>,
            &on_claim_walk<2>,
            &on_claim_walk<3>,
        };
    }

    bool install() noexcept
    {
        bool expected = false;
        if (!g_installed.compare_exchange_strong(expected, true))
            return g_patched.load(std::memory_order_acquire) != 0;

        auto &log = DMK::Logger::get_instance();

        const auto host = DetourModKit::Memory::host_module_range();
        if (!host.valid())
        {
            log.warning("[claim-guard] host module range unavailable; claim-walk guard NOT installed");
            return false;
        }

        const auto pattern = DetourModKit::Scanner::parse_aob(k_claimWalkSiteAob);
        if (!pattern.has_value())
        {
            log.warning("[claim-guard] walk-site AOB failed to parse; guard NOT installed");
            return false;
        }

        // Multi-match by design: more than one walker carries this shape and each needs guarding. See the anchor's
        // documentation in aob_resolver.hpp for why it is not an AddrCandidate cascade.
        std::vector<std::uintptr_t> sites;
        const auto *cursor = reinterpret_cast<const std::byte *>(host.base);
        const auto *hostEnd = reinterpret_cast<const std::byte *>(host.end);
        while (cursor < hostEnd)
        {
            const auto remaining = static_cast<std::size_t>(hostEnd - cursor);
            const auto *hit = DetourModKit::Scanner::find_pattern(cursor, remaining, *pattern);
            if (hit == nullptr)
                break;
            sites.push_back(reinterpret_cast<std::uintptr_t>(hit));
            cursor = hit + 1;
        }

        if (sites.empty())
        {
            // Not fatal on its own, but it means the shape drifted and the crash window is unguarded again.
            log.warning("[claim-guard] no claim-walk sites matched; guard NOT installed -- "
                        "a claim erase overlapping an engine walk can fault");
            return false;
        }

        if (sites.size() != k_claimWalkExpectedSites)
            log.warning("[claim-guard] expected {} claim-walk sites, found {} -- hooking anyway; "
                        "re-verify the walk survey against this build",
                        k_claimWalkExpectedSites, sites.size());

        auto &hookMgr = DMK::HookManager::get_instance();
        unsigned patched = 0;
        for (std::size_t i = 0; i < sites.size(); ++i)
        {
            if (i >= k_maxSites)
            {
                log.warning("[claim-guard] more than {} sites found; {} left unguarded", k_maxSites,
                            sites.size() - k_maxSites);
                break;
            }

            const auto site = sites[i] + k_claimWalkDerefOffset;

            // Decode this site's own `jz rel8` to find where the engine continues when it rejects an entry -- exactly
            // where a skipped entry should resume. Reading it out of the instruction stream keeps the guard free of
            // hardcoded continue targets.
            const auto jzOpcode = DMKMemory::seh_read<std::uint8_t>(sites[i] + 11).value_or(0);
            if (jzOpcode != 0x74)
            {
                log.warning("[claim-guard] site {:#x}: expected jz at +11, saw {:#04x} -- skipped", site, jzOpcode);
                continue;
            }
            const auto rel8 = DMKMemory::seh_read<std::int8_t>(sites[i] + 12);
            if (!rel8.has_value())
            {
                log.warning("[claim-guard] site {:#x}: jz displacement unreadable -- skipped", site);
                continue;
            }
            g_continueAddr[i] =
                static_cast<std::uintptr_t>(static_cast<std::int64_t>(sites[i] + 13) + static_cast<std::int64_t>(*rel8));

            // A managed mid-hook, so the DMK teardown removes it on unload. An earlier revision hand-wrote the stub
            // and its lifetime, which left the site jumping into freed memory across a hot reload.
            const auto name = std::string("ClaimWalkGuard_") + std::to_string(i);
            auto res = hookMgr.create_mid_hook(name, site, k_callbacks[i]);
            if (!res.has_value())
            {
                log.warning("[claim-guard] site {:#x}: mid-hook install failed: {} -- skipped", site,
                            DetourModKit::Hook::error_to_string(res.error()));
                continue;
            }

            ++patched;
            log.info("[claim-guard] guarded claim walk at {:#x} (continue {:#x})", site, g_continueAddr[i]);
        }

        g_patched.store(patched, std::memory_order_release);
        return patched != 0;
    }

    unsigned patched_site_count() noexcept
    {
        return g_patched.load(std::memory_order_acquire);
    }
}
