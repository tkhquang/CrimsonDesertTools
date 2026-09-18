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
#include <DetourModKit/memory.hpp>
#include <DetourModKit/region.hpp>
#include <DetourModKit/scan.hpp>
#include <DetourModKit/sighealth.hpp>

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Transmog
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
         * @brief Post-resolve validator for a code target: inside the image AND on a byte that can begin an
         *        instruction.
         * @details Most ladders here reach their entry through a negative walk-back measured against one build's
         *          prologue length. Nothing in the resolver re-checks that distance, so a prologue that gains or
         *          loses bytes leaves the pattern matching its unchanged body while the walk-back lands short, on an
         *          alignment pad or mid-instruction. A hook installed there writes its jump across an instruction
         *          boundary and the process dies somewhere unrelated later. The probe still returns true for the
         *          0xE9 / 0xEB / 0xFF 0x25 shapes an existing inline hook leaves behind, so a target another mod
         *          already hooked passes. Only scan poison is rejected. Rejecting here turns a silent failure into a
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

        /**
         * @brief Post-resolve validator for a function ENTRY: @ref code_site, plus the image's own exception table has
         *        to agree that the value begins a function.
         * @details Most entry ladders here reach the entry through a negative walk-back measured against one build's
         *          prologue length. The byte probe in code_site rejects only padding and a bare return, so a walk-back
         *          that lands a few bytes short or long, mid-instruction on a plausible opcode, still passes it. The
         *          x64 exception directory records the exact begin address of every function that touches the stack,
         *          and RtlLookupFunctionEntry answers for any address inside one. A value with unwind data whose
         *          recorded begin is not the value itself is therefore a stale walk-back, and it fails closed here. A
         *          value with no unwind data at all (a leaf that touches no stack, SetterByte is one) keeps the byte
         *          probe as its only evidence, so a leaf target is never rejected for lacking a table entry.
         */
        [[nodiscard]] bool function_entry_site(std::int64_t value, const void *context) noexcept
        {
            if (!code_site(value, context))
            {
                return false;
            }
            DWORD64 image_base = 0;
            const RUNTIME_FUNCTION *entry = RtlLookupFunctionEntry(static_cast<DWORD64>(value), &image_base, nullptr);
            if (entry == nullptr)
            {
                return true;
            }
            return image_base + entry->BeginAddress == static_cast<DWORD64>(value);
        }

        // The registry, indexed by AnchorId. The enumerator order IS this order. Every candidate row is a byte
        // pattern authored against an instruction, so Pages::Executable narrows each sweep to code pages and a
        // signature cannot alias an identical run in .rdata or .data. That holds even for the rows whose RESULT is a
        // data slot: the match site is the referencing instruction, and only the decoded disp32 leaves code. Every
        // code target here is a function entry, so each takes function_entry_site. The three VtableIdentity rows at
        // the end are the RTTI witnesses resolve_all_anchors() corroborates the mid-hook ladders against.
        // Every row sets require_validator: an anchor that reaches a backend without a post-resolve predicate then
        // fails CLOSED instead of publishing an unchecked address. It is a no-op for the rows below, which all carry
        // one. It is there so a row ADDED later cannot quietly skip verification.
        const Anchor ANCHORS[] = {
            {
                .label = "SlotPopulator",
                .kind = AnchorKind::RipGlobal,
                .site = SLOT_POPULATOR_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "MapLookup",
                .kind = AnchorKind::RipGlobal,
                .site = CDCore::anchors::MAP_LOOKUP_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "SubTranslator",
                .kind = AnchorKind::RipGlobal,
                .site = SUB_TRANSLATOR_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "SafeTearDown",
                .kind = AnchorKind::RipGlobal,
                .site = SAFE_TEAR_DOWN_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "InitSwapEntry",
                .kind = AnchorKind::RipGlobal,
                .site = INIT_SWAP_ENTRY_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "PartSlotRefresh",
                .kind = AnchorKind::RipGlobal,
                .site = PART_SLOT_REFRESH_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "SlotTagToHandle",
                .kind = AnchorKind::RipGlobal,
                .site = SLOT_TAG_TO_HANDLE_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "PartAddShow",
                .kind = AnchorKind::RipGlobal,
                .site = CDCore::anchors::PART_ADD_SHOW_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "WorldSystem",
                .kind = AnchorKind::RipGlobal,
                .site = CDCore::anchors::WORLD_SYSTEM_CANDIDATES,
                // A data slot, not code: the entry-plausibility screen does not apply.
                .validator = in_host_image,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "StringInfoRegistry",
                .kind = AnchorKind::RipGlobal,
                .site = STRING_INFO_REGISTRY_CANDIDATES,
                .validator = in_host_image,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "StringInfoVtable",
                .kind = AnchorKind::RipGlobal,
                .site = STRING_INFO_VTABLE_CANDIDATES,
                // A vtable in .rdata, not code.
                .validator = in_host_image,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "LoaderRegistry",
                .kind = AnchorKind::RipGlobal,
                .site = LOADER_REGISTRY_CANDIDATES,
                .validator = in_host_image,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "StructCopy",
                .kind = AnchorKind::RipGlobal,
                .site = STRUCT_COPY_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "NaturalPipeline",
                .kind = AnchorKind::RipGlobal,
                .site = NATURAL_PIPELINE_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "UnlinkByWrapper",
                .kind = AnchorKind::RipGlobal,
                .site = UNLINK_BY_WRAPPER_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "PartListMerge",
                .kind = AnchorKind::RipGlobal,
                .site = PART_LIST_MERGE_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "PartDescriptorBuild",
                .kind = AnchorKind::RipGlobal,
                .site = PART_DESCRIPTOR_BUILD_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "DyeCopy",
                .kind = AnchorKind::RipGlobal,
                .site = DYE_COPY_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "DyeCopier",
                .kind = AnchorKind::RipGlobal,
                .site = DYE_COPIER_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "ColorPublisher",
                .kind = AnchorKind::RipGlobal,
                .site = COLOR_PUBLISHER_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "HostScopeVfunc1",
                .kind = AnchorKind::RipGlobal,
                .site = HOST_SCOPE_VFUNC1_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "HostScopeVfunc2",
                .kind = AnchorKind::RipGlobal,
                .site = HOST_SCOPE_VFUNC2_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "SetterByte",
                .kind = AnchorKind::RipGlobal,
                .site = SETTER_BYTE_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "ColorTokenInterner",
                .kind = AnchorKind::RipGlobal,
                .site = COLOR_TOKEN_INTERNER_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "GameAudioEffectVtable",
                .kind = AnchorKind::RipGlobal,
                .site = GAME_AUDIO_EFFECT_VTABLE_CANDIDATES,
                // A vtable in .rdata, not code.
                .validator = in_host_image,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "PlayerStatic",
                .kind = AnchorKind::RipGlobal,
                .site = PLAYER_STATIC_CANDIDATES,
                .validator = in_host_image,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "HelmAudioRegistrar",
                .kind = AnchorKind::RipGlobal,
                .site = HELM_AUDIO_REGISTRAR_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "FrameUpdate",
                .kind = AnchorKind::RipGlobal,
                .site = FRAME_UPDATE_CANDIDATES,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
                .pages = Pages::Executable,
            },
            {
                .label = "FrameUpdateXref",
                .kind = AnchorKind::StringXref,
                // The update step names itself through the HUD format literal it formats: one copy in the image, one
                // referencing instruction, and the enclosing function of that instruction is the entry. No byte
                // pattern is involved, so the witness survives the code motion a byte row is sensitive to.
                .xref_text = FRAME_UPDATE_HUD_FORMAT,
                .xref_return = DMK::scan::XrefReturn::EnclosingFunction,
                .validator = function_entry_site,
                .validator_context = &s_host_image,
                .require_validator = true,
            },
            // RTTI witnesses: class vtables resolved by mangled name through the RTTI records in .rdata. Each one
            // holds a mid-hook target in a known slot, and corroborate_vtable_slots() reads that slot after the sweep.
            // A vtable is data, so in_host_image is the whole contract.
            {
                .label = "HostScopeVfunc1Vtable",
                .kind = AnchorKind::VtableIdentity,
                .mangled = HOST_SCOPE_VFUNC1_BIND_TYPE,
                .validator = in_host_image,
                .validator_context = &s_host_image,
                .require_validator = true,
            },
            {
                .label = "HostScopeVfunc2Vtable",
                .kind = AnchorKind::VtableIdentity,
                .mangled = HOST_SCOPE_VFUNC2_BIND_TYPE,
                .validator = in_host_image,
                .validator_context = &s_host_image,
                .require_validator = true,
            },
            {
                .label = "SetterByteVtable",
                .kind = AnchorKind::VtableIdentity,
                .mangled = SETTER_BYTE_BIND_TYPE,
                .validator = in_host_image,
                .validator_context = &s_host_image,
                .require_validator = true,
            },
        };
        static_assert(std::size(ANCHORS) == ANCHOR_COUNT, "ANCHORS must hold one entry per AnchorId.");

        // The per-anchor report and the ONLY store of a resolved address. anchor_address() serves out of it, and the
        // startup summary and the shutdown diagnostics snapshot roll it up instead of recomputing it. Written once by
        // resolve_all_anchors() on the init thread before any consumer reads, then read-only, so no synchronization
        // is required. An index past s_report_count stays default-constructed, so its status reads Unresolved.
        std::array<DMK::anchor::ResolvedAnchor, ANCHOR_COUNT> s_report{};
        std::size_t s_report_count = 0;

        /**
         * @brief Grades every candidate pattern in the table and reports the weak ones.
         * @details sighealth is offline and side-effect-free: it reads the COMPILED pattern bytes and mask and scores
         *          atom rarity, byte entropy, and expected ambiguity. It touches no process memory and never gates
         *          resolution, so it runs before the sweep and only reports. Its value is on patch day: a ladder that
         *          stops resolving is usually one whose rungs were already weakly selective, and this says which,
         *          without a disassembler. RTTI candidates carry no byte pattern and are skipped.
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

        /**
         * @brief One mid-hook entry paired with the class vtable slot that holds the same function.
         * @details The RTTI witness for a byte ladder. The registry resolves the class vtable by mangled name (a
         *          VtableIdentity anchor), and the slot content is the function the ladder should have found. The two
         *          are independent evidence: the vtable comes from the RTTI records in .rdata, the ladder from the
         *          instruction bytes in the code section.
         */
        struct VtableSlotWitness
        {
            /// The byte-ladder anchor the slot corroborates.
            AnchorId code;
            /// The VtableIdentity anchor whose resolved vtable holds the function.
            AnchorId vtable;
            /// Zero-based slot index in that vtable.
            std::size_t slot;
            /**
             * @brief Rows that have to resolve to the slot's function, inside that function, before the slot may
             *        stand in for a missed ladder. Empty when the slot may stand in on its own.
             * @details A mid-hook that only reads `this` at entry is safe on any virtual of the same class, so the
             *          slot alone is enough evidence for it. A mid-hook that rewrites an argument register is not: a
             *          drifted slot index would hand it an unrelated function, so a body row has to confirm the
             *          function before the hook is allowed to arm.
             */
            std::span<const DMK::scan::Candidate> contract;
        };

        const VtableSlotWitness VTABLE_SLOT_WITNESSES[] = {
            {AnchorId::HostScopeVfunc1, AnchorId::HostScopeVfunc1Vtable, HOST_SCOPE_VFUNC1_SLOT, {}},
            {AnchorId::HostScopeVfunc2, AnchorId::HostScopeVfunc2Vtable, HOST_SCOPE_VFUNC2_SLOT, {}},
            {AnchorId::SetterByte, AnchorId::SetterByteVtable, SETTER_BYTE_SLOT, SETTER_BYTE_CANDIDATES},
        };

        /// The content of the witnessed vtable slot, or 0 when the vtable anchor is unresolved or the slot unreadable.
        [[nodiscard]] std::uintptr_t vtable_slot_target(const VtableSlotWitness &witness) noexcept
        {
            const auto vtable_index = static_cast<std::size_t>(witness.vtable);
            if (vtable_index >= s_report_count || s_report[vtable_index].status != DMK::anchor::AnchorStatus::Resolved)
            {
                return 0;
            }
            const auto vtable = static_cast<std::uintptr_t>(s_report[vtable_index].value);
            const auto slot_address =
                DMK::Address{vtable}.offset(static_cast<std::ptrdiff_t>(witness.slot * sizeof(std::uintptr_t)));
            const auto target = DMK::memory::read<std::uintptr_t>(slot_address);
            return target ? *target : 0;
        }

        /**
         * @brief True when the contract rows resolve to @p target inside the function that begins at @p target.
         * @details The extent comes from the exception table when the function has unwind data. A leaf without it
         *          (SetterByte is one) gets a fixed window, which is enough for rows that anchor at the entry.
         *          require_unique stays on: inside one function each contract row matches once.
         */
        [[nodiscard]] bool slot_target_confirmed(std::uintptr_t target, std::span<const DMK::scan::Candidate> contract)
        {
            if (contract.empty())
            {
                return true;
            }
            std::size_t extent = 0x100;
            DWORD64 image_base = 0;
            const RUNTIME_FUNCTION *entry = RtlLookupFunctionEntry(static_cast<DWORD64>(target), &image_base, nullptr);
            if (entry != nullptr && image_base + entry->BeginAddress == target &&
                entry->EndAddress > entry->BeginAddress)
            {
                extent = entry->EndAddress - entry->BeginAddress;
            }
            const auto hit = DMK::scan::resolve(
                DMK::scan::ScanRequest{
                    .ladder = contract,
                    .label = "VtableSlotContract",
                    .scope = DMK::Region{DMK::Address{target}, extent},
                    .pages = Pages::Executable,
                }
            );
            return hit && hit->address.raw() == target;
        }

        /**
         * @brief Corroborates each witnessed byte ladder against its class vtable slot, and adopts the slot when the
         *        ladder missed.
         * @details Runs after the parallel sweep, on the init thread, before the quality summary. Agreement is logged
         *          once at info. A disagreement keeps the ladder value and warns: one of the two is wrong and only a
         *          human with the new build can say which. A missed ladder whose slot names a plausible code entry
         *          adopts the slot, subject to the witness contract, and warns, so the feature runs and the log still
         *          says the byte row needs re-deriving. The adopted entry carries the vtable anchor's image identity
         *          and a TypeIdentity source, so the report says where the value came from.
         */
        void corroborate_vtable_slots()
        {
            auto &logger = DMK::log();
            for (const VtableSlotWitness &witness : VTABLE_SLOT_WITNESSES)
            {
                const auto code_index = static_cast<std::size_t>(witness.code);
                const auto vtable_index = static_cast<std::size_t>(witness.vtable);
                if (code_index >= s_report_count || vtable_index >= s_report_count)
                {
                    continue;
                }
                DMK::anchor::ResolvedAnchor &code = s_report[code_index];
                const std::string_view vtable_label = ANCHORS[vtable_index].label;

                const std::uintptr_t slot_target = vtable_slot_target(witness);
                if (slot_target == 0)
                {
                    logger.warning(
                        "Anchor {}: no RTTI witness ({} unresolved), the byte ladder stands alone",
                        code.label,
                        vtable_label
                    );
                    continue;
                }
                if (!code_site(static_cast<std::int64_t>(slot_target), &s_host_image))
                {
                    logger.warning(
                        "Anchor {}: {} slot {} holds {:#x}, which is not a code entry. Slot index drift?",
                        code.label,
                        vtable_label,
                        witness.slot,
                        slot_target
                    );
                    continue;
                }

                if (code.status == DMK::anchor::AnchorStatus::Resolved)
                {
                    const auto ladder = static_cast<std::uintptr_t>(code.value);
                    if (ladder == slot_target)
                    {
                        logger.info(
                            "Anchor {} corroborated: byte ladder and {} slot {} agree at {:#x}",
                            code.label,
                            vtable_label,
                            witness.slot,
                            ladder
                        );
                    }
                    else
                    {
                        logger.warning(
                            "Anchor {} DISAGREES with its RTTI witness: ladder {:#x}, {} slot {} {:#x}. Keeping the "
                            "ladder. Re-derive both on this build",
                            code.label,
                            ladder,
                            vtable_label,
                            witness.slot,
                            slot_target
                        );
                    }
                    continue;
                }

                if (!slot_target_confirmed(slot_target, witness.contract))
                {
                    logger.warning(
                        "Anchor {} unresolved. {} slot {} names {:#x} but no contract row confirms that body, so it "
                        "is not adopted",
                        code.label,
                        vtable_label,
                        witness.slot,
                        slot_target
                    );
                    continue;
                }
                code.status = DMK::anchor::AnchorStatus::Resolved;
                code.value = static_cast<std::int64_t>(slot_target);
                code.domain = DMK::anchor::ResultDomain::CodeSite;
                code.witness = DMK::anchor::ResolvedWitness{};
                code.witness.image = s_report[vtable_index].witness.image;
                code.witness.source = DMK::anchor::PhysicalSource::TypeIdentity;
                code.witness.completeness = s_report[vtable_index].witness.completeness;
                logger.warning(
                    "Anchor {} self-healed from its RTTI witness: {} slot {} -> {:#x}. Re-derive the byte ladder for "
                    "this build",
                    code.label,
                    vtable_label,
                    witness.slot,
                    slot_target
                );
            }
        }

        /**
         * @brief Holds the FrameUpdate ladder against its string-xref witness.
         * @details Runs after the parallel sweep, on the init thread, before the quality summary. The anchor gates a
         *          hook that runs engine calls, so it is the one anchor where a wrong value costs more than a missing
         *          one. Agreement is logged at info. A disagreement fails the anchor closed: the frame hook stays off
         *          and the apply runs inline, as it does without the hook. A ladder miss adopts the witness, which
         *          function_entry_site already validated, and warns that the byte rows need re-deriving.
         */
        void corroborate_frame_update()
        {
            auto &logger = DMK::log();
            const auto code_index = static_cast<std::size_t>(AnchorId::FrameUpdate);
            const auto xref_index = static_cast<std::size_t>(AnchorId::FrameUpdateXref);
            if (code_index >= s_report_count || xref_index >= s_report_count)
            {
                return;
            }
            DMK::anchor::ResolvedAnchor &code = s_report[code_index];
            const DMK::anchor::ResolvedAnchor &xref = s_report[xref_index];

            if (xref.status != DMK::anchor::AnchorStatus::Resolved)
            {
                logger.warning(
                    "Anchor {}: no string-xref witness ({} unresolved), the byte ladder stands alone",
                    code.label,
                    xref.label
                );
                return;
            }
            const auto witness = static_cast<std::uintptr_t>(xref.value);

            if (code.status == DMK::anchor::AnchorStatus::Resolved)
            {
                const auto ladder = static_cast<std::uintptr_t>(code.value);
                if (ladder == witness)
                {
                    logger.info(
                        "Anchor {} corroborated: byte ladder and string-xref witness agree at {:#x}",
                        code.label,
                        ladder
                    );
                    return;
                }
                code.status = DMK::anchor::AnchorStatus::Failed;
                code.value = 0;
                logger.warning(
                    "Anchor {} DISAGREES with its string-xref witness: ladder {:#x}, witness {:#x}. Failing it closed, "
                    "the frame hook stays off. Re-derive both on this build",
                    code.label,
                    ladder,
                    witness
                );
                return;
            }

            code.status = DMK::anchor::AnchorStatus::Resolved;
            code.value = static_cast<std::int64_t>(witness);
            code.domain = DMK::anchor::ResultDomain::CodeSite;
            code.witness = DMK::anchor::ResolvedWitness{};
            code.witness.image = xref.witness.image;
            code.witness.source = xref.witness.source;
            code.witness.completeness = xref.witness.completeness;
            logger.warning(
                "Anchor {} self-healed from its string-xref witness -> {:#x}. Re-derive the byte ladder for this build",
                code.label,
                witness
            );
        }
    } // namespace

    void resolve_all_anchors()
    {
        auto &logger = DMK::log();

        // Every Crimson Desert target lives in the host EXE, so the scan scope is that image: faster than a
        // whole-process walk, and immune to a generic-shaped candidate first-matching inside a sibling mod or an
        // overlay. The same region validates each resolved value.
        s_host_image = DMK::Region::host();

        // Offline grading first: it needs no game memory and says which rungs are structurally weak BEFORE the sweep
        // says which ones missed, so the two lines read together on a patch-day log.
        report_signature_health(ANCHORS);

        s_report_count = DMK::anchor::resolve_all_parallel(ANCHORS, s_report, s_host_image);

        // The LAYOUT witness for the whole sweep. It folds the PE timestamp, SizeOfImage, and section table, so it
        // moves on a game patch and stays put under ASLR. It is what makes the per-anchor trust keys below comparable
        // between two launches.
        const DMK::scan::ImageIdentity host_identity = DMK::scan::image_identity(s_host_image);
        logger.info(
            "Host image identity: timestamp {:#010x}, size {:#x}, sections {:#x}, token {:#x}",
            host_identity.timestamp,
            host_identity.size_of_image,
            host_identity.section_digest,
            host_identity.token()
        );

        // resolve_all_parallel writes s_report[i] for ANCHORS[i], so the report index IS the AnchorId.
        for (std::size_t i = 0; i < s_report_count; ++i)
        {
            const DMK::anchor::ResolvedAnchor &entry = s_report[i];
            if (entry.status == DMK::anchor::AnchorStatus::Resolved)
            {
                // The witness source names the ladder rung that actually WON, which the anchor kind does not say. The
                // trust key folds the anchor's own declaration together with the live image identity, so it shifts on
                // a patch even when the address holds. A moved address under an unchanged key is self-healed drift.
                logger.debug(
                    "Anchor {} -> {} [{} via {}, trust {:#x}]",
                    entry.label,
                    DMK::format::format_address(static_cast<std::uintptr_t>(entry.value)),
                    DMK::anchor::result_domain_to_string(entry.domain),
                    DMK::anchor::physical_source_to_string(entry.witness.source),
                    DMK::anchor::anchor_trust_fingerprint(ANCHORS[i], host_identity)
                );
            }
            else
            {
                logger.warning(
                    "Anchor {} unresolved ({})",
                    entry.label,
                    DMK::anchor::anchor_status_to_string(entry.status)
                );
            }
        }

        // Second, independent evidence for the three mid-hook entries: the class vtable slot that holds each one.
        // Runs after the sweep so it can compare against (or stand in for) the ladder result, and before the summary
        // so an adopted value counts as resolved.
        corroborate_vtable_slots();
        // The frame hook's entry gets the same treatment from its string-xref witness, and fails closed on a
        // disagreement, because a hook that runs engine calls must not arm on a coincidental match.
        corroborate_frame_update();

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
        if (index >= ANCHOR_COUNT)
        {
            return 0;
        }
        const DMK::anchor::ResolvedAnchor &entry = s_report[index];
        if (entry.status != DMK::anchor::AnchorStatus::Resolved)
        {
            return 0;
        }
        return static_cast<std::uintptr_t>(entry.value);
    }

    std::span<const DMK::anchor::ResolvedAnchor> anchor_report() noexcept
    {
        return std::span<const DMK::anchor::ResolvedAnchor>(s_report.data(), s_report_count);
    }

} // namespace Transmog
