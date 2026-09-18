/**
 * @file aob_resolver.cpp
 * @brief The declarative anchor table, its one-pass resolution, and the resolved-address store.
 *
 * The candidate ladders in aob_resolver.hpp and cdcore/anchors.hpp enter a DetourModKit anchor registry as
 * RipGlobal entries. resolve_all_anchors() grades the signatures offline, resolves the whole table in a single
 * parallel pass at startup, and records each address. anchor_address() hands the resolved address, or 0 on a ladder
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

        constexpr std::size_t ANCHOR_COUNT = static_cast<std::size_t>(AnchorId::Count);

        /// The host image every anchor resolves against, filled before the sweep and handed to each validator.
        DMK::Region s_host_image{};

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
         * @brief Post-resolve validator for a code target: inside the image AND not on a byte that can never open a
         *        function body.
         * @details Rejects a site whose first byte is zero fill, INT3 padding, or a near return. Several ladders here
         *          reach the entry through a negative walk-back measured against one build's prologue length. Nothing
         *          in the resolver re-checks that distance, so a prologue that gains or loses bytes still matches its
         *          unchanged body while the walk-back lands short, on an alignment pad or mid-instruction. A hook
         *          installed there writes its jump across an instruction boundary and the process dies somewhere
         *          unrelated later. A rejection here turns that silent failure into a logged miss and a disabled
         *          feature.
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
        // fails CLOSED and never publishes an unchecked address. It is a no-op for the rows below, which all carry
        // one. It is there so a row ADDED later cannot quietly skip verification.
        const Anchor ANCHORS[] = {
            {
                .label = "WorldSystem",
                .kind = AnchorKind::RipGlobal,
                .site = CDCore::anchors::WORLD_SYSTEM_CANDIDATES,
                .validator = in_host_image,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "ChildActorVtbl",
                .kind = AnchorKind::RipGlobal,
                .site = CHILD_ACTOR_VTBL_CANDIDATES,
                .validator = in_host_image,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "MapLookup",
                .kind = AnchorKind::RipGlobal,
                .site = CDCore::anchors::MAP_LOOKUP_CANDIDATES,
                .validator = code_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "MapInsert",
                .kind = AnchorKind::RipGlobal,
                .site = MAP_INSERT_CANDIDATES,
                .validator = code_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "EquipVisCheck",
                .kind = AnchorKind::RipGlobal,
                .site = EQUIP_VIS_CHECK_CANDIDATES,
                .validator = code_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "PartAddShow",
                .kind = AnchorKind::RipGlobal,
                .site = CDCore::anchors::PART_ADD_SHOW_CANDIDATES,
                .validator = code_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "PostfixEval",
                .kind = AnchorKind::RipGlobal,
                .site = POSTFIX_EVAL_CANDIDATES,
                .validator = code_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "NpcPfeReturnAddr",
                .kind = AnchorKind::RipGlobal,
                .site = NPC_PFE_RETURN_ADDR_CANDIDATES,
                // A return address points at the instruction AFTER a call, not at a function entry, so the
                // entry-plausibility screen does not apply. In-image is the whole contract here.
                .validator = in_host_image,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "VisualEquipChange",
                .kind = AnchorKind::RipGlobal,
                .site = CDCore::anchors::VISUAL_EQUIP_CHANGE_CANDIDATES,
                .validator = code_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "VisualEquipSwap",
                .kind = AnchorKind::RipGlobal,
                // BatchEquip is the same engine function this mod calls VisualEquipSwap.
                .site = CDCore::anchors::BATCH_EQUIP_CANDIDATES,
                .validator = code_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
        };
        static_assert(std::size(ANCHORS) == ANCHOR_COUNT, "ANCHORS must hold one entry per AnchorId.");

        // The per-anchor report, indexed by AnchorId and the one store of every resolved address. Written once by
        // resolve_all_anchors() on the init thread before any consumer reads, then read-only, so no synchronization
        // is required. The startup summary and the shutdown diagnostics snapshot roll it up, so neither recomputes
        // it.
        std::array<DMK::anchor::ResolvedAnchor, ANCHOR_COUNT> s_report{};
        std::size_t s_report_count = 0;

        /**
         * @brief Grades every candidate pattern in the table and reports the weak ones.
         * @details sighealth is offline and side-effect-free: it reads the COMPILED pattern bytes and mask and scores
         *          atom rarity, byte entropy, and expected ambiguity. It touches no process memory and never gates
         *          resolution, so it runs before the sweep and only reports. Its value is on patch day: a ladder that
         *          stops resolving is usually one whose rungs were already weakly selective, and this says which,
         *          without a disassembler. An RTTI candidate carries no byte pattern, so the walk skips it.
         */
        void report_signature_health(std::span<const Anchor> anchor_table)
        {
            auto &logger = DMK::log();
            std::size_t fragile = 0;
            std::size_t unusable = 0;

            for (const Anchor &entry : anchor_table)
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
                        "Signature health: {}/{} {} - {}",
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
                    "Signature health: {} candidate(s) grade Unusable and {} Fragile. Re-author them before the "
                    "next game patch (details at Debug level)",
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
        // whole-process walk, and a generic-shaped candidate cannot first-match inside a sibling mod or an overlay.
        // The same region validates each resolved value.
        s_host_image = DMK::Region::host();

        // The build fingerprint separates "the mod is stale for a new game build" from "a sibling mod patched the
        // site" when it is read next to each anchor's own image identity below. It reads the PE header only.
        const DMK::scan::ImageIdentity host_identity = DMK::scan::image_identity(s_host_image);
        if (host_identity.present())
        {
            logger.info(
                "Host image identity: {} (timestamp {}, size {})",
                DMK::format::format_hex(host_identity.token()),
                DMK::format::format_hex(host_identity.timestamp),
                DMK::format::format_hex(host_identity.size_of_image)
            );
        }
        else
        {
            logger.warning("Host image identity unavailable: the PE headers did not validate");
        }

        // Offline grading first: it needs no game memory and says which rungs are structurally weak BEFORE the sweep
        // says which ones missed, so the two lines read together on a patch-day log.
        report_signature_health(ANCHORS);

        s_report_count = DMK::anchor::resolve_all_parallel(ANCHORS, s_report, s_host_image);

        // resolve_all_parallel writes s_report[i] for ANCHORS[i], so the report index IS the AnchorId.
        for (std::size_t i = 0; i < s_report_count; ++i)
        {
            const DMK::anchor::ResolvedAnchor &entry = s_report[i];
            if (entry.status != DMK::anchor::AnchorStatus::Resolved)
            {
                logger.warning(
                    "Anchor {} unresolved ({})",
                    entry.label,
                    DMK::anchor::anchor_status_to_string(entry.status)
                );
                continue;
            }

            // The witness names which backend won the row, whether the sweep saw a complete authoritative view, and
            // which image owns the value. On a patch day that separates a stale signature from a patched site.
            logger.debug(
                "Anchor {} -> {} [{} via {}, image {}, {}, {} witness byte(s)]",
                entry.label,
                DMK::format::format_address(static_cast<std::uintptr_t>(entry.value)),
                DMK::anchor::result_domain_to_string(entry.domain),
                DMK::anchor::physical_source_to_string(entry.witness.source),
                DMK::format::format_hex(entry.witness.image.token()),
                entry.witness.completeness == DMK::anchor::WitnessCompleteness::Complete ? "complete" : "partial",
                entry.witness.evidence.span().size()
            );
        }

        const DMK::anchor::AnchorQuality quality = DMK::anchor::assess_quality(anchor_report());
        const DMK::anchor::GateVerdict verdict = DMK::anchor::evaluate_gate(quality);
        logger.info(
            "Anchor resolution: {}/{} resolved, {} failed, {} unsupported, gate {}",
            quality.resolved,
            quality.total,
            quality.failed,
            quality.unsupported,
            DMK::anchor::gate_verdict_to_string(verdict)
        );
        if (verdict == DMK::anchor::GateVerdict::Fail)
        {
            // B-51: drift telemetry must not be followed by an unconditional arm. The verdict rates the WHOLE table,
            // so it does not by itself disable anything. Each feature gates on its own anchor, which reads 0 when
            // that row failed, and stays off at its own install site.
            logger.warning(
                "Anchor resolution: the gate rejects this table, so every feature whose own anchor failed stays off"
            );
        }
    }

    std::uintptr_t anchor_address(AnchorId id) noexcept
    {
        const auto index = static_cast<std::size_t>(id);
        if (index >= ANCHOR_COUNT || s_report[index].status != DMK::anchor::AnchorStatus::Resolved)
        {
            return 0;
        }
        return static_cast<std::uintptr_t>(s_report[index].value);
    }

    std::span<const DMK::anchor::ResolvedAnchor> anchor_report() noexcept
    {
        return std::span<const DMK::anchor::ResolvedAnchor>(s_report.data(), s_report_count);
    }

} // namespace EquipHide
