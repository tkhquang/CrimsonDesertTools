#include "claim_walk_guard.hpp"

#include "aob_resolver.hpp"

#include <DetourModKit/hook.hpp>
#include <DetourModKit/logger.hpp>
#include <DetourModKit/memory.hpp>
#include <DetourModKit/scan.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Transmog::ClaimWalkGuard
{
    namespace
    {
        std::atomic<bool> g_installed{false};
        std::atomic<unsigned> g_patched{0};

        /**
         * @brief Upper bound on guarded sites. Each needs its own callback (a mid-hook callback receives only the
         * register context, with no way to tell which site it fired for), so the count is fixed at compile time. Sized
         * above the expected two to absorb a build that splits or duplicates a walker.
         */
        constexpr std::size_t k_maxSites = 4;

        /**
         * @brief Loop-continue label per site, decoded from that site's own `jz`. Written during install and read by
         * the callbacks; plain stores suffice because a hook cannot fire before its entry is filled in.
         */
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
        template <std::size_t Index> void on_claim_walk(DMK::hook::MidContext &ctx) noexcept
        {
            if (DMK::hook::gpr(ctx, DMK::hook::Gpr::Rax) == 0)
                DMK::hook::instruction_pointer(ctx) = g_continueAddr[Index];
        }

        /// One callback per site index, so a site's continue address needs no lookup on the per-entry path.
        constexpr std::array<DMK::hook::MidHookFn, k_maxSites> k_callbacks{
            &on_claim_walk<0>,
            &on_claim_walk<1>,
            &on_claim_walk<2>,
            &on_claim_walk<3>,
        };
    } // namespace

    bool install(DMK::hook::HookStack &hooks) noexcept
    {
        bool expected = false;
        if (!g_installed.compare_exchange_strong(expected, true))
            return g_patched.load(std::memory_order_acquire) != 0;

        auto &log = DMK::log();

        const auto host = DMK::Region::host();
        if (host.size == 0)
        {
            log.warning("[claim-guard] host module range unavailable; claim-walk guard NOT installed");
            return false;
        }

        const auto pattern = claim_walk_site_pattern();

        // Multi-match by design: more than one walker carries this shape and each needs guarding. See the anchor's
        // documentation in aob_resolver.hpp for why it is not a resolution ladder. The occurrence sweep is page-gated
        // to executable pages, so an identical run in .rdata cannot be mistaken for a walker and an unmapped hole in
        // the image is skipped rather than faulted through.
        std::vector<std::uintptr_t> sites;
        for (std::size_t occurrence = 1; occurrence <= k_maxSites + 1; ++occurrence)
        {
            const auto hit = DMK::scan::scan(pattern, host, occurrence, DMK::scan::Pages::Executable);
            if (!hit)
                break;
            sites.push_back(hit->raw());
        }

        if (sites.empty())
        {
            // Not fatal on its own, but it means the shape drifted and the crash window is unguarded again.
            log.warning(
                "[claim-guard] no claim-walk sites matched; guard NOT installed -- "
                "a claim erase overlapping an engine walk can fault"
            );
            return false;
        }

        if (sites.size() != k_claimWalkExpectedSites)
            log.warning(
                "[claim-guard] expected {} claim-walk sites, found {} -- hooking anyway; "
                "re-verify the walk survey against this build",
                k_claimWalkExpectedSites,
                sites.size()
            );

        unsigned patched = 0;
        for (std::size_t i = 0; i < sites.size(); ++i)
        {
            if (i >= k_maxSites)
            {
                log.warning(
                    "[claim-guard] more than {} sites found; {} left unguarded",
                    k_maxSites,
                    sites.size() - k_maxSites
                );
                break;
            }

            const auto site = sites[i] + k_claimWalkDerefOffset;

            // Decode this site's own `jz rel8` to find where the engine continues when it rejects an entry -- exactly
            // where a skipped entry should resume. Reading it out of the instruction stream keeps the guard free of
            // hardcoded continue targets.
            //
            // The branch is NOT part of the signature (short Jcc opcodes flip encoding across builds, so anchoring on
            // one retires the row silently). It is validated here instead: a site whose branch is not the expected
            // 2-byte `jz rel8` is skipped with a log line rather than mis-decoded into a bogus continue address.
            const auto jzAddr = sites[i] + k_claimWalkJzOffset;
            const auto jzOpcode = DMK::memory::read<std::uint8_t>(DMK::Address{jzAddr}).value_or(0);
            if (jzOpcode != 0x74)
            {
                log.warning(
                    "[claim-guard] site {:#x}: expected jz at +{}, saw {:#04x} -- skipped",
                    site,
                    k_claimWalkJzOffset,
                    jzOpcode
                );
                continue;
            }
            const auto rel8 = DMK::memory::read<std::int8_t>(DMK::Address{jzAddr + 1});
            if (!rel8.has_value())
            {
                log.warning("[claim-guard] site {:#x}: jz displacement unreadable -- skipped", site);
                continue;
            }
            // rel8 is measured from the byte AFTER the 2-byte branch.
            g_continueAddr[i] =
                static_cast<std::uintptr_t>(static_cast<std::int64_t>(jzAddr + 2) + static_cast<std::int64_t>(*rel8));

            // The handle goes into the module hook stack, so teardown removes it newest-first on unload. A
            // hand-written stub would own its own lifetime and leave the site jumping into freed memory across a hot
            // reload.
            const auto name = std::string("ClaimWalkGuard_") + std::to_string(i);
            auto guard =
                DMK::hook::mid_at(DMK::hook::MidRequest{.name = name, .target = DMK::Address{site}}, k_callbacks[i]);
            if (!guard)
            {
                log.warning(
                    "[claim-guard] site {:#x}: mid-hook creation failed ({}) -- skipped",
                    site,
                    guard.error().message()
                );
                continue;
            }
            if (auto armed = guard->enable(); !armed)
            {
                log.warning(
                    "[claim-guard] site {:#x}: mid-hook could not be armed ({}) -- skipped",
                    site,
                    armed.error().message()
                );
                continue;
            }
            hooks.push(std::move(*guard));

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
} // namespace Transmog::ClaimWalkGuard
