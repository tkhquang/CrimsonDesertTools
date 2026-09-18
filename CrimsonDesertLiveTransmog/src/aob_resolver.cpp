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

namespace Transmog
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
         * @brief Post-resolve validator for a code target: inside the image AND on a byte that can begin an
         *        instruction.
         * @details Most ladders here reach their entry through a negative walk-back measured against one build's
         *          prologue length. Nothing in the resolver re-checks that distance, so a prologue that gains or
         *          loses bytes leaves the pattern matching its unchanged body while the walk-back lands short, on an
         *          alignment pad or mid-instruction. A hook installed there writes its jump across an instruction
         *          boundary and the process dies somewhere unrelated later. The probe still returns true for the
         *          0xE9 / 0xEB / 0xFF 0x25 shapes an existing inline hook leaves behind, so a target another mod
         *          already hooked passes; only scan poison is rejected. Rejecting here turns a silent failure into a
         *          logged miss and a disabled feature.
         */
        [[nodiscard]] bool code_site(std::int64_t value, const void *context) noexcept
        {
            if (!in_host_image(value, context))
            {
                return false;
            }
            return DMK::scan::is_likely_function_prologue(DMK::Address{static_cast<std::uintptr_t>(value)});
        }

        // The registry, indexed by AnchorId. The enumerator order IS this order. Every candidate row is a byte
        // pattern authored against an instruction, so Pages::Executable narrows each sweep to code pages and a
        // signature cannot alias an identical run in .rdata or .data. That holds even for the rows whose RESULT is a
        // data slot: the match site is the referencing instruction, and only the decoded disp32 leaves code.
        // Every row sets require_validator: an anchor that reaches a backend without a post-resolve predicate then
        // fails CLOSED instead of publishing an unchecked address. It is a no-op for the rows below, which all carry
        // one; it is there so a row ADDED later cannot quietly skip verification.
        const Anchor k_anchors[] = {
            {
                .label = "SlotPopulator",
                .kind = AnchorKind::RipGlobal,
                .site = k_slotPopulatorCandidates,
                .validator = code_site,
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
                .label = "SubTranslator",
                .kind = AnchorKind::RipGlobal,
                .site = k_subTranslatorCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "SafeTearDown",
                .kind = AnchorKind::RipGlobal,
                .site = k_safeTearDownCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "InitSwapEntry",
                .kind = AnchorKind::RipGlobal,
                .site = k_initSwapEntryCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "PartSlotRefresh",
                .kind = AnchorKind::RipGlobal,
                .site = k_partSlotRefreshCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "SlotTagToHandle",
                .kind = AnchorKind::RipGlobal,
                .site = k_slotTagToHandleCandidates,
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
                .label = "WorldSystem",
                .kind = AnchorKind::RipGlobal,
                .site = CDCore::Anchors::k_worldSystemCandidates,
                // A data slot, not code: the entry-plausibility screen does not apply.
                .validator = in_host_image,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "StringInfoRegistry",
                .kind = AnchorKind::RipGlobal,
                .site = k_stringInfoRegistryCandidates,
                // A data slot, not code.
                .validator = in_host_image,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "StringInfoVtable",
                .kind = AnchorKind::RipGlobal,
                .site = k_stringInfoVtableCandidates,
                // A vtable in .rdata, not code.
                .validator = in_host_image,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "LoaderRegistry",
                .kind = AnchorKind::RipGlobal,
                .site = k_loaderRegistryCandidates,
                // A data slot, not code.
                .validator = in_host_image,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "StructCopy",
                .kind = AnchorKind::RipGlobal,
                .site = k_structCopyCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "NaturalPipeline",
                .kind = AnchorKind::RipGlobal,
                .site = k_naturalPipelineCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "UnlinkByWrapper",
                .kind = AnchorKind::RipGlobal,
                .site = k_unlinkByWrapperCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "PartListMerge",
                .kind = AnchorKind::RipGlobal,
                .site = k_partListMergeCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "PartDescriptorBuild",
                .kind = AnchorKind::RipGlobal,
                .site = k_partDescriptorBuildCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "DyeCopy",
                .kind = AnchorKind::RipGlobal,
                .site = k_dyeCopyCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "DyeCopier",
                .kind = AnchorKind::RipGlobal,
                .site = k_dyeCopierCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "ColorPublisher",
                .kind = AnchorKind::RipGlobal,
                .site = k_colorPublisherCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "HostScopeVfunc1",
                .kind = AnchorKind::RipGlobal,
                .site = k_hostScopeVfunc1Candidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "HostScopeVfunc2",
                .kind = AnchorKind::RipGlobal,
                .site = k_hostScopeVfunc2Candidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "SetterByte",
                .kind = AnchorKind::RipGlobal,
                .site = k_setterByteCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "ColorTokenInterner",
                .kind = AnchorKind::RipGlobal,
                .site = k_colorTokenInternerCandidates,
                .validator = code_site,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "GameAudioEffectVtable",
                .kind = AnchorKind::RipGlobal,
                .site = k_gameAudioEffectVtableCandidates,
                // A vtable in .rdata, not code.
                .validator = in_host_image,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "PlayerStatic",
                .kind = AnchorKind::RipGlobal,
                .site = k_playerStaticCandidates,
                // A data slot, not code.
                .validator = in_host_image,
                .validator_context = &s_hostImage,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "HelmAudioRegistrar",
                .kind = AnchorKind::RipGlobal,
                .site = k_helmAudioRegistrarCandidates,
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

    DMK::scan::Pattern claim_walk_site_pattern()
    {
        //   mov  rax, [rbx+8]        48 8B 43 08     entry's owning pointer (claim vector, `node+0x58`)
        //   mov  rcx, [rax+28h]      48 8B 48 28     faults when the owner is null
        //   test rcx, rcx            48 85 C9
        //   jz   <next entry>        74 ??           rel8 to the loop-continue label, validated at install time
        return Pattern::literal("48 8B 43 08 48 8B 48 28 48 85 C9");
    }

} // namespace Transmog
