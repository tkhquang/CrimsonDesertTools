/**
 * @file aob_resolver.cpp
 * @brief The declarative anchor table, its one-pass resolution, and the resolved-address store.
 *
 * The candidate ladders in aob_resolver.hpp and cdcore/anchors.hpp enter a DetourModKit anchor registry as
 * RipGlobal entries. resolve_all_anchors() grades the signatures offline, resolves the whole table in a single
 * parallel pass at startup, and records each address; anchor_address() hands the resolved address, or 0 on a ladder
 * miss, to the call sites.
 */

#include "aob_resolver.hpp"

#include <DetourModKit/anchor.hpp>
#include <DetourModKit/format.hpp>
#include <DetourModKit/logger.hpp>
#include <DetourModKit/region.hpp>
#include <DetourModKit/scan.hpp>
#include <DetourModKit/sighealth.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace EquipHide
{
    namespace
    {
        using DMK::anchor::Anchor;
        using DMK::anchor::AnchorKind;
        using DMK::scan::Pages;

        constexpr std::size_t k_anchorCount = static_cast<std::size_t>(AnchorId::Count);

        /// The host image every anchor resolves against, filled before the sweep and handed to each validator.
        DMK::Region s_hostImage{};

        /**
         * @brief Post-resolve validator: the resolved value must land inside CrimsonDesert.exe.
         * @details The scan SCOPE constrains where a candidate's bytes are FOUND, not where the value it decodes
         *          POINTS. A RipRelative candidate resolves an absolute address from a disp32 and a Direct candidate
         *          applies a signed walk-back, so either can compute a target outside the image from a freak match.
         *          Every anchor here names something inside the game image, so a target outside it is proof the match
         *          was wrong. Returning false resets the value to 0 and reports Failed, exactly like a backend miss.
         */
        [[nodiscard]] bool in_host_image(std::int64_t value, const void *context) noexcept
        {
            const auto *image = static_cast<const DMK::Region *>(context);
            if (image == nullptr || image->size == 0 || value <= 0)
            {
                return false;
            }
            return image->contains(DMK::Address{static_cast<std::uintptr_t>(value)});
        }

        /**
         * @brief Post-resolve validator for a code target: inside the image AND on a byte that can begin a function.
         * @details Several ladders here reach the entry through a negative walk-back measured against one build's
         *          prologue length. Nothing in the resolver re-checks that distance, so a prologue that gains or
         *          loses bytes leaves the pattern matching its unchanged body while the walk-back lands short, on an
         *          alignment pad or mid-instruction. A hook installed there writes its jump across an instruction
         *          boundary and the process dies somewhere unrelated later. Rejecting here turns that silent failure
         *          into a logged miss and a disabled feature.
         */
        [[nodiscard]] bool code_site(std::int64_t value, const void *context) noexcept
        {
            if (!in_host_image(value, context))
            {
                return false;
            }
            return DMK::scan::is_likely_function_prologue(DMK::Address{static_cast<std::uintptr_t>(value)});
        }

        // The registry, indexed by AnchorId. The enumerator order IS this order. Every target is code: a function
        // entry, a mid-body instruction, or the instruction whose disp32 names a data slot. Pages::Executable narrows
        // each byte sweep to code pages so a signature that must land on an instruction cannot alias an identical run
        // in .rdata or .data.
        // Every row sets require_validator: an anchor that reaches a backend without a post-resolve predicate then
        // fails CLOSED instead of publishing an unchecked address. It is a no-op for the rows below, which all carry
        // one; it is there so a row ADDED later cannot quietly skip verification.
        const Anchor k_anchors[] = {
            {
                .label = "WorldSystem",
                .kind = AnchorKind::RipGlobal,
                .site = CDCore::Anchors::k_worldSystemCandidates,
                .validator = in_host_image,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "ChildActorVtbl",
                .kind = AnchorKind::RipGlobal,
                .site = k_childActorVtblCandidates,
                .validator = in_host_image,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "MapLookup",
                .kind = AnchorKind::RipGlobal,
                .site = CDCore::Anchors::k_mapLookupCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "MapInsert",
                .kind = AnchorKind::RipGlobal,
                .site = k_mapInsertCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "EquipVisCheck",
                .kind = AnchorKind::RipGlobal,
                .site = k_equipVisCheckCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "PartAddShow",
                .kind = AnchorKind::RipGlobal,
                .site = CDCore::Anchors::k_partAddShowCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "PostfixEval",
                .kind = AnchorKind::RipGlobal,
                .site = k_postfixEvalCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "NpcPfeReturnAddr",
                .kind = AnchorKind::RipGlobal,
                .site = k_npcPfeReturnAddrCandidates,
                // A return address points at the instruction AFTER a call, not at a function entry, so the
                // entry-plausibility screen does not apply. In-image is the whole contract here.
                .validator = in_host_image,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "VisualEquipChange",
                .kind = AnchorKind::RipGlobal,
                .site = CDCore::Anchors::k_visualEquipChangeCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "VisualEquipSwap",
                .kind = AnchorKind::RipGlobal,
                // BatchEquip is the same engine function this mod calls VisualEquipSwap.
                .site = CDCore::Anchors::k_batchEquipCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
        };
        static_assert(std::size(k_anchors) == k_anchorCount, "k_anchors must hold one entry per AnchorId.");

        // Resolved absolute addresses, indexed by AnchorId; 0 means unresolved. Written once by resolve_all_anchors()
        // on the init thread before any consumer reads, then read-only, so no synchronization is required.
        std::array<std::uintptr_t, k_anchorCount> s_resolved{};

        // The per-anchor report, kept under the same write-once discipline so the startup summary and the shutdown
        // diagnostics snapshot can roll it up instead of recomputing it.
        std::array<DMK::anchor::ResolvedAnchor, k_anchorCount> s_report{};
        std::size_t s_reportCount = 0;

        /**
         * @brief Grades every candidate pattern in the table and reports the weak ones.
         * @details sighealth is offline and side-effect-free: it reads the COMPILED pattern bytes and mask and scores
         *          atom rarity, byte entropy, and expected ambiguity. It touches no process memory and never gates
         *          resolution, so it runs before the sweep and only reports. Its value is on patch day: a ladder that
         *          stops resolving is usually one whose rungs were already weakly selective, and this says which,
         *          without a disassembler. RTTI candidates carry no byte pattern and are skipped.
         */
        void report_signature_health(std::span<const Anchor> anchors)
        {
            auto &logger = DMK::log();
            std::size_t fragile = 0;
            std::size_t unusable = 0;

            for (const Anchor &entry : anchors)
            {
                for (const DMK::scan::Candidate &candidate : entry.site)
                {
                    const DMK::scan::Pattern *pattern = nullptr;
                    if (const auto *direct = candidate.as_direct())
                    {
                        pattern = &direct->pattern;
                    }
                    else if (const auto *rip = candidate.as_rip_relative())
                    {
                        pattern = &rip->pattern;
                    }
                    if (pattern == nullptr)
                    {
                        continue;
                    }

                    const DMK::sighealth::PatternHealth health = DMK::sighealth::analyze_pattern(*pattern);
                    if (health.grade == DMK::sighealth::Grade::Robust)
                    {
                        logger.trace("Signature health: {}/{} Robust", entry.label, candidate.name());
                        continue;
                    }
                    (health.grade == DMK::sighealth::Grade::Unusable ? ++unusable : ++fragile);
                    logger.debug(
                        "Signature health: {}/{} {} -- {}",
                        entry.label,
                        candidate.name(),
                        DMK::sighealth::to_string(health.grade),
                        DMK::sighealth::format_report(health, candidate.name())
                    );
                }
            }

            if (unusable > 0)
            {
                logger.warning(
                    "Signature health: {} candidate(s) grade Unusable and {} Fragile; re-author them before the next "
                    "game patch (details at Debug level)",
                    unusable,
                    fragile
                );
            }
            else
            {
                logger.info("Signature health: {} fragile candidate(s), 0 unusable", fragile);
            }
        }
    } // namespace

    void resolve_all_anchors()
    {
        auto &logger = DMK::log();

        // Every Crimson Desert target lives in the host EXE, so the scan scope is that image: faster than a
        // whole-process walk, and immune to a generic-shaped candidate first-matching inside a sibling mod or an
        // overlay. The same region validates each resolved value.
        s_hostImage = DMK::Region::host();

        // Offline grading first: it needs no game memory and says which rungs are structurally weak BEFORE the sweep
        // says which ones missed, so the two lines read together on a patch-day log.
        report_signature_health(k_anchors);

        s_reportCount = DMK::anchor::resolve_all_parallel(k_anchors, s_report, s_hostImage);

        // resolve_all_parallel writes s_report[i] for k_anchors[i], so the report index IS the AnchorId.
        for (std::size_t i = 0; i < s_reportCount; ++i)
        {
            const DMK::anchor::ResolvedAnchor &entry = s_report[i];
            if (entry.status == DMK::anchor::AnchorStatus::Resolved)
            {
                s_resolved[i] = static_cast<std::uintptr_t>(entry.value);
                logger.debug("Anchor {} -> {}", entry.label, DMK::format::format_address(s_resolved[i]));
            }
            else
            {
                s_resolved[i] = 0;
                logger.warning(
                    "Anchor {} unresolved ({})",
                    entry.label,
                    DMK::anchor::anchor_status_to_string(entry.status)
                );
            }
        }

        const DMK::anchor::AnchorQuality quality = DMK::anchor::assess_quality(anchor_report());
        logger.info(
            "Anchor resolution: {}/{} resolved, {} failed, {} unsupported",
            quality.resolved,
            quality.total,
            quality.failed,
            quality.unsupported
        );
    }

    std::uintptr_t anchor_address(AnchorId id) noexcept
    {
        const auto index = static_cast<std::size_t>(id);
        if (index >= k_anchorCount)
        {
            return 0;
        }
        return s_resolved[index];
    }

    std::span<const DMK::anchor::ResolvedAnchor> anchor_report() noexcept
    {
        return std::span<const DMK::anchor::ResolvedAnchor>(s_report.data(), s_reportCount);
    }

} // namespace EquipHide
