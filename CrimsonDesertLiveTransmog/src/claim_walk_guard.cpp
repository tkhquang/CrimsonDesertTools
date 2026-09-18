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
#include <utility>
#include <vector>

namespace Transmog::claim_walk_guard
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
        constexpr std::size_t MAX_SITES = 4;

        /// Hook name per site index, so the install loop never builds one at runtime.
        constexpr std::array<const char *, MAX_SITES> SITE_NAMES{
            "ClaimWalkGuard_0",
            "ClaimWalkGuard_1",
            "ClaimWalkGuard_2",
            "ClaimWalkGuard_3",
        };

        /**
         * @brief Loop-continue label per site, decoded from that site's own `jz`. Install writes it with a release
         * store and the callback reads it relaxed, so the value a live detour redirects RIP to rests on a language
         * guarantee rather than on the incidental fence inside the arming sequence.
         */
        std::array<std::atomic<std::uintptr_t>, MAX_SITES> g_continueAddr{};

        /**
         * @brief Skip a claim entry whose owner is mid-erase.
         *
         * RAX holds the entry's owning pointer, freshly loaded by the instruction ahead of the hook. While the erase
         * shifts the vector down, that pointer is transiently null but still inside the count, and the instruction
         * this hook precedes dereferences it at `+0x28`. A redirect to the site's own loop-continue label makes the
         * walker treat the entry as absent, which is what it observes a moment later, once the count catches up.
         *
         * Runs per claim entry per walk, so it stays a single compare in the common case.
         */
        template <std::size_t Index> void on_claim_walk(DMK::hook::MidContext &ctx) noexcept
        {
            if (DMK::hook::gpr(ctx, DMK::hook::Gpr::Rax) == 0)
                DMK::hook::instruction_pointer(ctx) = g_continueAddr[Index].load(std::memory_order_relaxed);
        }

        /// One callback per site index, so a site's continue address needs no lookup on the per-entry path.
        constexpr std::array<DMK::hook::MidHookFn, MAX_SITES> CALLBACKS{
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
        unsigned patched = 0;

        // The body allocates (the site vector, the hook stack push) and formats log records, so every step of it runs
        // inside one catch-all. An allocation failure during mod startup leaves the sites unguarded instead of
        // terminating the process.
        try
        {
            const auto host = DMK::Region::host();
            if (host.size == 0)
            {
                (void)log.try_log(
                    DMK::LogLevel::Warning,
                    "[claim-guard] host module range unavailable; claim-walk guard NOT installed"
                );
                return false;
            }

            // Multi-match by design: more than one walker carries this shape and each needs guarding. See the anchor's
            // documentation in aob_resolver.hpp for why it is not a resolution ladder. The occurrence sweep is
            // page-gated to executable pages, so an identical run in .rdata cannot be mistaken for a walker and an
            // unmapped hole in the image is skipped rather than faulted through.
            std::vector<std::uintptr_t> sites;
            for (std::size_t occurrence = 1; occurrence <= MAX_SITES + 1; ++occurrence)
            {
                const auto hit =
                    DMK::scan::scan(CLAIM_WALK_SITE_PATTERN, host, occurrence, DMK::scan::Pages::Executable);
                if (!hit)
                    break;
                sites.push_back(hit->raw());
            }

            if (sites.empty())
            {
                // Not fatal on its own, but it means the shape drifted and the crash window is unguarded again.
                (void)log.try_log(
                    DMK::LogLevel::Warning,
                    "[claim-guard] no claim-walk sites matched; guard NOT installed - "
                    "a claim erase overlapping an engine walk can fault"
                );
                return false;
            }

            if (sites.size() != CLAIM_WALK_EXPECTED_SITES)
            {
                (void)log.try_log(
                    DMK::LogLevel::Warning,
                    "[claim-guard] expected {} claim-walk sites, found {} - hooking anyway; "
                    "re-verify the walk survey against this build",
                    CLAIM_WALK_EXPECTED_SITES,
                    sites.size()
                );
            }

            for (std::size_t i = 0; i < sites.size(); ++i)
            {
                if (i >= MAX_SITES)
                {
                    (void)log.try_log(
                        DMK::LogLevel::Warning,
                        "[claim-guard] more than {} sites found; {} left unguarded",
                        MAX_SITES,
                        sites.size() - MAX_SITES
                    );
                    break;
                }

                const auto site = sites[i] + CLAIM_WALK_DEREF_OFFSET;

                // Decode this site's own `jz rel8` to find where the engine continues when it rejects an entry, which
                // is exactly where a skipped entry resumes. A read out of the instruction stream keeps the guard free
                // of hardcoded continue targets.
                //
                // The branch is NOT part of the signature (short Jcc opcodes flip encoding across builds, so an
                // anchor on one retires the row silently). It is validated here instead: a site whose branch is not
                // the expected 2-byte `jz rel8` is skipped with a log line rather than mis-decoded into a bogus
                // continue address.
                const auto jz_addr = sites[i] + CLAIM_WALK_JZ_OFFSET;
                const auto jz_opcode = DMK::memory::read<std::uint8_t>(DMK::Address{jz_addr}).value_or(0);
                if (jz_opcode != 0x74)
                {
                    (void)log.try_log(
                        DMK::LogLevel::Warning,
                        "[claim-guard] site {:#x}: expected jz at +{}, saw {:#04x} - skipped",
                        site,
                        CLAIM_WALK_JZ_OFFSET,
                        jz_opcode
                    );
                    continue;
                }
                const auto rel8 = DMK::memory::read<std::int8_t>(DMK::Address{jz_addr + 1});
                if (!rel8.has_value())
                {
                    (void)log.try_log(
                        DMK::LogLevel::Warning,
                        "[claim-guard] site {:#x}: jz displacement unreadable - skipped",
                        site
                    );
                    continue;
                }
                // rel8 is measured from the byte AFTER the 2-byte branch.
                const auto branch_end = static_cast<std::int64_t>(jz_addr + 2);
                const auto continue_addr = static_cast<std::uintptr_t>(branch_end + static_cast<std::int64_t>(*rel8));
                g_continueAddr[i].store(continue_addr, std::memory_order_release);

                // The handle goes into the module hook stack, so teardown removes it newest-first on unload. A
                // hand-written stub owns its own lifetime and leaves the site jumping into freed memory across a hot
                // reload.
                auto guard = DMK::hook::mid_at(
                    DMK::hook::MidRequest{
                        .name = SITE_NAMES[i],
                        .target = DMK::Address{site},
                    },
                    CALLBACKS[i]
                );
                if (!guard)
                {
                    (void)log.try_log(
                        DMK::LogLevel::Warning,
                        "[claim-guard] site {:#x}: mid-hook creation failed ({}) - skipped",
                        site,
                        guard.error().message()
                    );
                    continue;
                }
                if (auto armed = guard->enable(); !armed)
                {
                    (void)log.try_log(
                        DMK::LogLevel::Warning,
                        "[claim-guard] site {:#x}: mid-hook could not be armed ({}) - skipped",
                        site,
                        armed.error().message()
                    );
                    continue;
                }
                hooks.push(std::move(*guard));

                ++patched;
                (void)log.try_log(
                    DMK::LogLevel::Info,
                    "[claim-guard] guarded claim walk at {:#x} (continue {:#x})",
                    site,
                    continue_addr
                );
            }
        }
        catch (...)
        {
            (void)log.try_log(
                DMK::LogLevel::Warning,
                "[claim-guard] install stopped on an exception; {} site(s) guarded, the rest are unguarded",
                patched
            );
        }

        g_patched.store(patched, std::memory_order_release);
        return patched != 0;
    }

    unsigned patched_site_count() noexcept
    {
        return g_patched.load(std::memory_order_acquire);
    }
} // namespace Transmog::claim_walk_guard
