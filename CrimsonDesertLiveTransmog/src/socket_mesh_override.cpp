#include "socket_mesh_override.hpp"

#include "aob_resolver.hpp"
#include "dye_record_inject.hpp"
#include "prefab_wrapper_swap.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"
#include "transmog_map.hpp"
#include "transmog_worker.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <intrin.h> // _InterlockedIncrement, used for the wrapper refcount bump

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Transmog::SocketMeshOverride
{
    namespace
    {
        /**
         * @brief PartDescriptorBuild: `f(a1, &partId, slotTag, a4, a5, record, outList)`, resolved through
         *        AnchorId::PartDescriptorBuild.
         */
        using BuildFn = std::int64_t(__fastcall *)(
            std::int64_t,
            std::int16_t *,
            std::uint16_t,
            std::uint32_t *,
            char,
            std::int64_t,
            std::uint64_t *
        );

        BuildFn g_orig = nullptr;
        std::atomic<bool> g_installed{false};
        std::atomic<unsigned> g_overridden{0};

        /// Stride of one appended descriptor.
        constexpr std::size_t k_descriptorStride = 112;
        /// Offset of the mesh wrapper inside a descriptor: its first field.
        constexpr std::size_t k_descriptorWrapperOffset = 0x00;
        /// Refcount field on a StringInfo wrapper.
        constexpr std::size_t k_wrapperRefcountOffset = 0x10;

        /**
         * @brief Dye entry array on the part record the descriptor's dye comes from. Geometry and the record writer
         *        both come from DyeRecordInject. This module is a second PRODUCER of those records, not a second
         *        definition of them.
         */
        constexpr std::size_t k_recordDyeDataOffset =
            DyeRecordInject::k_dyeVectorOffset + DyeRecordInject::k_vecDataOffset;
        constexpr std::size_t k_recordDyeCountOffset =
            DyeRecordInject::k_dyeVectorOffset + DyeRecordInject::k_vecCountOffset;
        /**
         * @brief Buffer capacity in records. Sparse mode starts from the engine's own entries, which can outnumber
         *        the channel count, so this sits above it.
         */
        constexpr std::size_t k_maxDyeRecords = 32;
        /// One dye record, aliased locally so the arithmetic below reads in units rather than raw 16s.
        constexpr std::size_t k_dyeRecordSize = DyeRecordInject::k_dyeRecordSize;
        /// Floor for anything treated as a live heap pointer. Below this is a packed scalar or a null.
        constexpr std::uint64_t k_minPlausiblePtr = 0x10000;
        /**
         * @brief Same floor for the values the engine passes through signed `__int64` parameters. A comparison
         *        against the unsigned form converts the operand, so a negative (garbage) value lands ABOVE the floor
         *        and passes the guard meant to reject it.
         */
        constexpr std::int64_t k_minPlausiblePtrSigned = static_cast<std::int64_t>(k_minPlausiblePtr);

        /**
         * @brief Fabricate the record's dye entries for this slot from the active preset.
         *
         * The engine reads 16-byte entries in the SAME layout DyeRecordInject already builds for the DyeCopier path
         * - `+0x00` group hash, `+0x04` material id, `+0x06` channel index, `+0x07..09` RGB, `+0x0B` repair byte -
         * confirmed against the reader (`sub_140671EB0`), which takes the color from `+7/+8/+9` and the ratio from
         * `+0x0B`.
         *
         * @return Number of entries written, or 0 when the slot has no dye.
         */
        std::uint32_t build_slot_dye_records(std::size_t slotIdx, std::uintptr_t record, std::uint8_t *out) noexcept
        {
            const Preset *preset = PresetManager::instance().active_preset();
            if (!preset || slotIdx >= preset->slots.size())
                return 0;
            const auto &dye = preset->slots[slotIdx].dye;
            if (!any_dye_active(dye))
                return 0;
            const bool sparse = preset->slots[slotIdx].dyeSparse;

            if (sparse)
            {
                // Sparse: override ONLY the channels the preset sets, and leave the rest of the item's dye alone.
                //
                // The injector achieves that with an upsert into the records the engine already produced. This path
                // replaces the array instead, so it has to start from a copy of the engine's own entries. A list of
                // just the active channels drops every channel the preset does not touch.
                const auto srcData =
                    DMK::memory::read<std::uint64_t>(DMK::Address{record + k_recordDyeDataOffset}).value_or(0);
                const auto srcCount =
                    DMK::memory::read<std::uint32_t>(DMK::Address{record + k_recordDyeCountOffset}).value_or(0);
                std::uint32_t n = 0;
                if (srcData >= k_minPlausiblePtr)
                {
                    const auto copy =
                        srcCount < k_maxDyeRecords ? srcCount : static_cast<std::uint32_t>(k_maxDyeRecords);
                    for (std::uint32_t i = 0; i < copy; ++i)
                    {
                        const auto offset = static_cast<std::size_t>(i) * k_dyeRecordSize;
                        const auto bytes = DMK::memory::read_into(
                                               DMK::Address{static_cast<std::uintptr_t>(srcData) + offset},
                                               std::span{reinterpret_cast<std::byte *>(out + offset), k_dyeRecordSize}
                        )
                                               .has_value();
                        if (!bytes)
                            break;
                        ++n;
                    }
                }

                for (std::size_t k = 0; k < DyeRecordInject::k_dyeChannelCount; ++k)
                {
                    const auto &ch = dye[k];
                    if (ch.group_hash == 0)
                        continue;

                    // Upsert by channel index, which lives at +0x06 of the record.
                    std::uint8_t *rec = nullptr;
                    for (std::uint32_t i = 0; i < n; ++i)
                        if (out[static_cast<std::size_t>(i) * k_dyeRecordSize + 6] == static_cast<std::uint8_t>(k))
                        {
                            rec = out + static_cast<std::size_t>(i) * k_dyeRecordSize;
                            break;
                        }
                    if (rec == nullptr)
                    {
                        if (n >= k_maxDyeRecords)
                            continue;
                        rec = out + static_cast<std::size_t>(n) * k_dyeRecordSize;
                        std::memset(rec, 0, k_dyeRecordSize);
                        ++n;
                    }
                    DyeRecordInject::build_dye_record(
                        rec,
                        k,
                        ch.group_hash,
                        ch.r,
                        ch.g,
                        ch.b,
                        ch.material_id,
                        ch.repair_byte
                    );
                }
                return n;
            }

            // Dense: emit EVERY channel, with the first active one as the stand-in for any the preset leaves unset -
            // exactly what the injector's dense mode does. A skip of the unset channels shortens the array, and the
            // materials bound to them then get no entry at all, so only some pieces of the part come in dyed.
            const ChannelDye *fallback = nullptr;
            for (std::size_t k = 0; k < DyeRecordInject::k_dyeChannelCount; ++k)
                if (dye[k].group_hash != 0)
                {
                    fallback = &dye[k];
                    break;
                }
            if (fallback == nullptr)
                return 0;

            std::uint32_t n = 0;
            for (std::size_t k = 0; k < DyeRecordInject::k_dyeChannelCount && n < k_maxDyeRecords; ++k)
            {
                const auto &ch = (dye[k].group_hash != 0) ? dye[k] : *fallback;
                std::uint8_t *rec = out + static_cast<std::size_t>(n) * k_dyeRecordSize;
                std::memset(rec, 0, k_dyeRecordSize);
                DyeRecordInject::build_dye_record(
                    rec,
                    k,
                    ch.group_hash,
                    ch.r,
                    ch.g,
                    ch.b,
                    ch.material_id,
                    ch.repair_byte
                );
                ++n;
            }
            return n;
        }

        /// Out-list container: `*a7` when non-null, else `a7[1]`. Data at `+0x00`, count at `+0x08`.
        [[nodiscard]] std::uintptr_t out_container(const std::uint64_t *a7) noexcept
        {
            if (a7 == nullptr)
                return 0;
            const auto primary =
                DMK::memory::read<std::uint64_t>(DMK::Address{reinterpret_cast<std::uintptr_t>(a7)}).value_or(0);
            if (primary >= k_minPlausiblePtr)
                return static_cast<std::uintptr_t>(primary);
            return static_cast<std::uintptr_t>(
                DMK::memory::read<std::uint64_t>(DMK::Address{reinterpret_cast<std::uintptr_t>(a7) + 8}).value_or(0)
            );
        }

        /**
         * @brief Take a reference on a wrapper about to go into a descriptor.
         *
         * Mirrors the engine's own guard: it only bumps when the count is non-negative. This deliberately does NOT
         * release the wrapper the descriptor held before - these are canonical interner instances that live for the
         * session, so a stray reference keeps alive something already immortal, whereas an unbalanced decrement can
         * free a wrapper still referenced elsewhere.
         */
        void addref_wrapper(std::uintptr_t wrapper) noexcept
        {
            if (wrapper < k_minPlausiblePtr)
                return;
            const auto cur = DMK::memory::read<std::int32_t>(DMK::Address{wrapper + k_wrapperRefcountOffset});
            if (!cur.has_value() || *cur < 0)
                return;
            __try
            {
                _InterlockedIncrement(reinterpret_cast<volatile long *>(wrapper + k_wrapperRefcountOffset));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        /// Put the record's own dye array back after the build.
        void restore_dye_records(std::int64_t record, std::uint64_t data, std::uint32_t count) noexcept
        {
            (void)DMK::memory::write_in_place<std::uint64_t>(
                DMK::Address{static_cast<std::uintptr_t>(record) + k_recordDyeDataOffset},
                data
            );
            (void)DMK::memory::write_in_place<std::uint32_t>(
                DMK::Address{static_cast<std::uintptr_t>(record) + k_recordDyeCountOffset},
                count
            );
        }

        std::int64_t __fastcall on_build(
            std::int64_t a1,
            std::int16_t *partId,
            std::uint16_t slotTag,
            std::uint32_t *a4,
            char a5,
            std::int64_t record,
            std::uint64_t *outList
        ) noexcept
        {
            const auto trampoline = g_orig;
            if (!trampoline)
                return 0;

            // The detour runs inside an engine call tree whose frames carry no unwind data, so one catch-all holds
            // every step that can throw and every diagnostic goes through the no-throw log verb. The state the tail
            // needs lives outside the try, so a throw still restores the record and still lets the engine build once.
            bool dyeSwapped = false;
            std::uint64_t savedDyeData = 0;
            std::uint32_t savedDyeCount = 0;
            std::int64_t ret = 0;
            bool trampolineRan = false;

            try
            {
                // Cheap rejects first: this runs for every socket of every actor the engine builds.
                if (!flag_enabled().load(std::memory_order_relaxed) || in_transmog().load(std::memory_order_relaxed) ||
                    outList == nullptr)
                    return trampoline(a1, partId, slotTag, a4, a5, record, outList);

                const auto slotOpt = slot_from_game_slot(static_cast<std::int16_t>(slotTag));
                if (!slotOpt)
                    return trampoline(a1, partId, slotTag, a4, a5, record, outList);
                const auto slotIdx = static_cast<std::size_t>(*slotOpt);
                if (slotIdx >= k_slotCount || !slot_enabled(slotIdx))
                    return trampoline(a1, partId, slotTag, a4, a5, record, outList);

                // Only a slot LT actively dresses. A slot set to "active + none" (hide) has no target and must fall
                // through - an override puts a mesh back on a socket the user asked to be empty.
                const auto &mapping = slot_mappings()[slotIdx];
                if (!mapping.active || mapping.targetItemId == 0)
                    return trampoline(a1, partId, slotTag, a4, a5, record, outList);

                // Resolve the target FIRST: this also brings the per-slot table up to date for the current world and
                // character, so the ownership test below reads a settled stamp.
                const auto target = PrefabWrapperSwap::target_wrapper_for_slot(slotIdx);
                if (target < k_minPlausiblePtr)
                    return trampoline(a1, partId, slotTag, a4, a5, record, outList);

                // The body being built must belong to the character whose targets the table holds.
                //
                // This hook fires for companions, NPCs and wildlife as well as the player, so it needs an ownership
                // test. It must be `char_idx_for_equip_slot` and NOT a comparison against `resolve_player_component()`:
                // that helper returns Kliff's component whoever is controlled, so it answers "is this Kliff's body?"
                // rather than "does this body belong to the character these targets came from" - and a table that
                // holds one protagonist's targets passes while the engine rebuilds another, and dresses the wrong
                // character.
                //
                // char_idx_for_equip_slot answers from a table the load-detect worker republishes once per tick in
                // steady state and once per retry attempt during a load. It confirms a match against the recorded
                // actor before an index comes back, so a recycled address cannot borrow a protagonist's identity. What
                // it can do is answer 0 for a short window after a body is rebuilt, which costs this pre-substitution
                // pass and nothing else: the character's own apply still dresses them. A zero on either side means
                // "not a protagonist body" or "table unbound" - both fall through rather than guess.
                const auto tableIdx = PrefabWrapperSwap::target_table_char_idx();
                const auto hostIdx = char_idx_for_equip_slot(static_cast<std::uintptr_t>(a1));
                if (tableIdx == 0 || hostIdx == 0 || hostIdx != tableIdx)
                {
                    // Name the cross-character case. A zero on either side is ordinary - every companion, NPC and
                    // wildlife socket lands here, and so does any build before the table binds - so those stay
                    // silent. Two DIFFERENT protagonists is the defect this gate exists for, and a silent refusal
                    // makes it very hard to find. Rate-limited to one line per (host, table) pair per world so a
                    // persistent mismatch reports once instead of per socket per build.
                    if (tableIdx != 0 && hostIdx != 0)
                    {
                        const auto worldGen = CDCore::world_generation();
                        const auto key = (worldGen << 8) | (static_cast<std::uint64_t>(hostIdx) << 4) | tableIdx;
                        static std::atomic<std::uint64_t> s_lastMismatchKey{0};
                        if (s_lastMismatchKey.exchange(key, std::memory_order_relaxed) != key)
                        {
                            (void)DMK::log().try_log(
                                DMK::LogLevel::Warning,
                                "[socket-override] HOST MISMATCH: body 0x{:X} belongs to charIdx {}, but the target "
                                "table holds charIdx {}'s targets - refusing to dress it. Slot={} tag={:#06x}. "
                                "Expect the character's own apply to redress it once the table rebinds.",
                                static_cast<std::uint64_t>(a1),
                                hostIdx,
                                tableIdx,
                                slot_name(*slotOpt),
                                static_cast<unsigned>(slotTag)
                            );
                        }
                    }
                    return trampoline(a1, partId, slotTag, a4, a5, record, outList);
                }

                // Point the record's dye entries at LT's colors for the duration of the build.
                //
                // A rewrite of the mesh alone leaves the descriptor carrying the REAL item's dye, because the engine
                // builds the dye object HERE from the incoming record's entry array - the transmog mesh then shows
                // undyed until LT's apply re-injects a second later. A substitution of the source array bakes the
                // right color in on the first build. A publish through DyeRecordInject does NOT work at this point:
                // its DyeCopier detour is not in this call tree, so nothing ever consumes the published state.
                //
                // The record is part state, not the authoritative equip table, and the restore below puts the
                // original pointer and count back.
                thread_local std::uint8_t s_dyeRecords[k_maxDyeRecords * k_dyeRecordSize];
                if (record >= k_minPlausiblePtrSigned)
                {
                    const auto n = build_slot_dye_records(slotIdx, static_cast<std::uintptr_t>(record), s_dyeRecords);
                    if (n != 0)
                    {
                        const auto curData = DMK::memory::read<std::uint64_t>(
                            DMK::Address{static_cast<std::uintptr_t>(record) + k_recordDyeDataOffset}
                        );
                        const auto curCount = DMK::memory::read<std::uint32_t>(
                            DMK::Address{static_cast<std::uintptr_t>(record) + k_recordDyeCountOffset}
                        );
                        if (curData.has_value() && curCount.has_value() &&
                            DMK::memory::write_in_place<std::uint64_t>(
                                DMK::Address{static_cast<std::uintptr_t>(record) + k_recordDyeDataOffset},
                                reinterpret_cast<std::uint64_t>(s_dyeRecords)
                            )
                                .has_value() &&
                            DMK::memory::write_in_place<std::uint32_t>(
                                DMK::Address{static_cast<std::uintptr_t>(record) + k_recordDyeCountOffset},
                                n
                            )
                                .has_value())
                        {
                            savedDyeData = *curData;
                            savedDyeCount = *curCount;
                            dyeSwapped = true;
                        }
                    }
                }

                // Snapshot the out-list length so only descriptors THIS call appends get rewritten.
                const auto containerBefore = out_container(outList);
                const auto countBefore =
                    containerBefore ? DMK::memory::read<std::uint32_t>(DMK::Address{containerBefore + 8}).value_or(0)
                                    : 0;

                // Marked BEFORE the call, not after. The flag answers "has the engine been given its build",
                // and a throw out of the trampoline with the flag still clear would send the tail through a
                // second one.
                trampolineRan = true;
                ret = trampoline(a1, partId, slotTag, a4, a5, record, outList);

                if (dyeSwapped)
                {
                    restore_dye_records(record, savedDyeData, savedDyeCount);
                    dyeSwapped = false;
                }

                const auto container = out_container(outList);
                if (container == 0)
                    return ret;
                const auto data = DMK::memory::read<std::uint64_t>(DMK::Address{container}).value_or(0);
                const auto countAfter = DMK::memory::read<std::uint32_t>(DMK::Address{container + 8}).value_or(0);
                if (data < k_minPlausiblePtr || countAfter <= countBefore)
                    return ret;

                // Rewrite the mesh wrapper on every descriptor appended for this socket. The engine resolved it from
                // the REAL item. A pointer to LT's target makes the transmog mesh the one the engine attaches, so the
                // real mesh is never built and there is nothing to flash or tear down afterwards.
                unsigned rewritten = 0;
                for (std::uint32_t i = countBefore; i < countAfter; ++i)
                {
                    const auto entry = static_cast<std::uintptr_t>(data) + i * k_descriptorStride;
                    const auto cur =
                        DMK::memory::read<std::uint64_t>(DMK::Address{entry + k_descriptorWrapperOffset}).value_or(0);
                    if (cur < k_minPlausiblePtr || cur == target)
                        continue;
                    // Reference AFTER the write lands. A reference taken first leaks one count per failed write, and
                    // a wrapper over-referenced this way is never released for the rest of the process.
                    if (DMK::memory::write_in_place<std::uint64_t>(
                            DMK::Address{entry + k_descriptorWrapperOffset},
                            static_cast<std::uint64_t>(target)
                        )
                            .has_value())
                    {
                        addref_wrapper(target);
                        ++rewritten;
                    }
                }

                if (rewritten != 0)
                {
                    const auto n = g_overridden.fetch_add(rewritten, std::memory_order_relaxed) + rewritten;
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Debug,
                        "[socket-override] slot={} tag={:#06x} rewrote {} descriptor(s) -> {:#x} (total {})",
                        slot_name(static_cast<TransmogSlot>(slotIdx)),
                        slotTag,
                        rewritten,
                        target,
                        n
                    );
                }
                return ret;
            }
            catch (...)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "[socket-override] descriptor build stopped on an exception tag={:#06x}",
                    slotTag
                );
            }

            // A throw between the dye swap and its restore must not leave the engine's record pointing at this
            // thread's scratch buffer.
            if (dyeSwapped)
                restore_dye_records(record, savedDyeData, savedDyeCount);
            if (!trampolineRan)
                ret = trampoline(a1, partId, slotTag, a4, a5, record, outList);
            return ret;
        }
    } // namespace

    bool install(DMK::hook::HookStack &hooks) noexcept
    {
        bool expected = false;
        if (!g_installed.compare_exchange_strong(expected, true))
            return g_orig != nullptr;

        auto &log = DMK::log();

        const auto addr = anchor_address(AnchorId::PartDescriptorBuild);
        if (addr == 0)
        {
            log.warning("[socket-override] PartDescriptorBuild AOB failed - real items will flash before tear-down");
            return false;
        }

        auto hook = DMK::hook::inline_at(
            DMK::hook::InlineRequest{.name = "PartDescriptorBuild", .target = DMK::Address{addr}},
            &on_build
        );
        if (!hook)
        {
            log.warning("[socket-override] hook creation failed at {:#x}: {}", addr, hook.error().message());
            return false;
        }

        // Publish the trampoline BEFORE enable() arms the patch, so no game thread can enter the detour while its
        // original pointer is still null.
        g_orig = hook->original<BuildFn>();
        if (auto armed = hook->enable(); !armed)
        {
            log.warning("[socket-override] hook could not be armed at {:#x}: {}", addr, armed.error().message());
            g_orig = nullptr;
            return false;
        }
        hooks.push(std::move(*hook));

        log.info("[socket-override] hooked PartDescriptorBuild at {:#x}", addr);
        return true;
    }

    unsigned overridden_count() noexcept
    {
        return g_overridden.load(std::memory_order_relaxed);
    }
} // namespace Transmog::SocketMeshOverride
