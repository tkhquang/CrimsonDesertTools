#include "socket_mesh_override.hpp"

#include <cdcore/controlled_char.hpp>
#include "aob_resolver.hpp"
#include "dye_record_inject.hpp"
#include "prefab_wrapper_swap.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"
#include "transmog_map.hpp"
#include "transmog_worker.hpp"

#include <DetourModKit/hook_manager.hpp>
#include <DetourModKit/logger.hpp>
#include <DetourModKit/memory.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>

#include <intrin.h>

#include <windows.h>

namespace Transmog::SocketMeshOverride
{
    namespace
    {
        /// `sub_14081DD40(a1, &partId, slotTag, a4, a5, record, outList)`.
        using BuildFn = std::int64_t(__fastcall *)(std::int64_t, std::int16_t *, std::uint16_t, std::uint32_t *, char,
                                                   std::int64_t, std::uint64_t *);

        BuildFn g_orig = nullptr;
        std::atomic<bool> g_installed{false};
        std::atomic<unsigned> g_overridden{0};

        /// Appended descriptor stride, and the offset of the mesh wrapper inside it (its first field).
        constexpr std::size_t k_descriptorStride = 112;
        constexpr std::size_t k_descriptorWrapperOffset = 0x00;
        /// Refcount field on a StringInfo wrapper.
        constexpr std::size_t k_wrapperRefcountOffset = 0x10;

        /// Dye entry array on the part record the descriptor's dye is built from. Geometry and the record writer
        /// both come from DyeRecordInject -- this module is a second PRODUCER of those records, not a second
        /// definition of them.
        constexpr std::size_t k_recordDyeDataOffset =
            DyeRecordInject::k_dyeVectorOffset + DyeRecordInject::k_vecDataOffset;
        constexpr std::size_t k_recordDyeCountOffset =
            DyeRecordInject::k_dyeVectorOffset + DyeRecordInject::k_vecCountOffset;
        /// Buffer capacity in records. Sparse mode starts from the engine's own entries, which can outnumber the
        /// channel count, so this sits above it.
        constexpr std::size_t k_maxDyeRecords = 32;

        /**
         * @brief Fabricate the record's dye entries for this slot from the active preset.
         *
         * The engine reads 16-byte entries in the SAME layout DyeRecordInject already builds for the DyeCopier path
         * -- `+0x00` group hash, `+0x04` material id, `+0x06` channel index, `+0x07..09` RGB, `+0x0B` repair byte --
         * confirmed against the reader (`sub_140671EB0`), which takes the colour from `+7/+8/+9` and the ratio from
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
                // The injector achieves that by upserting into the records the engine already produced. This path
                // replaces the array instead, so it has to start from a copy of the engine's own entries -- emitting
                // just the active channels would drop every channel the preset does not touch.
                const auto srcData = DMKMemory::seh_read<std::uint64_t>(record + k_recordDyeDataOffset).value_or(0);
                const auto srcCount = DMKMemory::seh_read<std::uint32_t>(record + k_recordDyeCountOffset).value_or(0);
                std::uint32_t n = 0;
                if (srcData >= 0x10000)
                {
                    const auto copy = srcCount < k_maxDyeRecords ? srcCount : static_cast<std::uint32_t>(k_maxDyeRecords);
                    for (std::uint32_t i = 0; i < copy; ++i)
                    {
                        const auto bytes = DMKMemory::seh_read_bytes(static_cast<std::uintptr_t>(srcData) + i * 16,
                                                                     out + static_cast<std::size_t>(i) * 16, 16);
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
                        if (out[static_cast<std::size_t>(i) * 16 + 6] == static_cast<std::uint8_t>(k))
                        {
                            rec = out + static_cast<std::size_t>(i) * DyeRecordInject::k_dyeRecordSize;
                            break;
                        }
                    if (rec == nullptr)
                    {
                        if (n >= k_maxDyeRecords)
                            continue;
                        rec = out + static_cast<std::size_t>(n) * DyeRecordInject::k_dyeRecordSize;
                        std::memset(rec, 0, DyeRecordInject::k_dyeRecordSize);
                        ++n;
                    }
                    DyeRecordInject::build_dye_record(rec, k, ch.group_hash, ch.r, ch.g, ch.b, ch.material_id,
                                                     ch.repair_byte);
                }
                return n;
            }

            // Dense: emit EVERY channel, with the first active one standing in for any the preset leaves unset --
            // exactly what the injector's dense mode does. Skipping unset channels shortens the array, and the
            // materials bound to them then get no entry at all, so only some pieces of the part come in dyed.
            const auto *fallback = decltype(&dye[0]){nullptr};
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
                std::uint8_t *rec = out + static_cast<std::size_t>(n) * DyeRecordInject::k_dyeRecordSize;
                std::memset(rec, 0, DyeRecordInject::k_dyeRecordSize);
                DyeRecordInject::build_dye_record(rec, k, ch.group_hash, ch.r, ch.g, ch.b, ch.material_id,
                                                 ch.repair_byte);
                ++n;
            }
            return n;
        }

        /// Out-list container: `*a7` when non-null, else `a7[1]`. Data at `+0x00`, count at `+0x08`.
        [[nodiscard]] std::uintptr_t out_container(const std::uint64_t *a7) noexcept
        {
            if (a7 == nullptr)
                return 0;
            const auto primary = DMKMemory::seh_read<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a7)).value_or(0);
            if (primary >= 0x10000)
                return static_cast<std::uintptr_t>(primary);
            return static_cast<std::uintptr_t>(
                DMKMemory::seh_read<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a7) + 8).value_or(0));
        }

        /**
         * @brief Take a reference on a wrapper we are about to store in a descriptor.
         *
         * Mirrors the engine's own guard: it only bumps when the count is non-negative. The wrapper the descriptor
         * previously held is deliberately NOT released -- these are canonical interner instances that live for the
         * session, so a stray reference keeps alive something already immortal, whereas an unbalanced decrement could
         * free a wrapper still referenced elsewhere.
         */
        void addref_wrapper(std::uintptr_t wrapper) noexcept
        {
            if (wrapper < 0x10000)
                return;
            const auto cur = DMKMemory::seh_read<std::int32_t>(wrapper + k_wrapperRefcountOffset);
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

        std::int64_t __fastcall on_build(std::int64_t a1, std::int16_t *partId, std::uint16_t slotTag,
                                         std::uint32_t *a4, char a5, std::int64_t record, std::uint64_t *outList)
        {
            const auto trampoline = g_orig;
            if (!trampoline)
                return 0;

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

            // Only a slot LT is actively dressing. A slot set to "active + none" (hide) has no target and must fall
            // through -- overriding it would put a mesh back on a socket the user asked to be empty.
            const auto &mapping = slot_mappings()[slotIdx];
            if (!mapping.active || mapping.targetItemId == 0)
                return trampoline(a1, partId, slotTag, a4, a5, record, outList);

            // Resolve the target FIRST: this also brings the per-slot table up to date for the current world and
            // character, so the ownership test below reads a settled stamp.
            const auto target = PrefabWrapperSwap::target_wrapper_for_slot(slotIdx);
            if (target < 0x10000)
                return trampoline(a1, partId, slotTag, a4, a5, record, outList);

            // The body being built must belong to the character whose targets the table holds.
            //
            // This hook fires for companions, NPCs and wildlife as well as the player, so it needs an ownership test.
            // The previous one compared a1 against resolve_player_component() -- which always returns KLIFF's
            // component regardless of who is controlled. It therefore asked "is this Kliff's body?" and never "does
            // this body belong to the character these targets came from". With the table holding Oongka's targets and
            // the engine rebuilding Kliff (a save-load forcing a character switch, or a hot reload while controlling
            // Oongka), it passed and dressed Kliff in Oongka's meshes.
            //
            // char_idx_for_equip_slot resolves the body through the live actor chain, so it cannot be fooled by state
            // a save-load or hot reload left stale. A zero on either side means "not a protagonist body" or "table
            // unbound" -- both fall through rather than guess.
            const auto tableIdx = PrefabWrapperSwap::target_table_char_idx();
            const auto hostIdx = char_idx_for_equip_slot(static_cast<std::uintptr_t>(a1));
            if (tableIdx == 0 || hostIdx == 0 || hostIdx != tableIdx)
            {
                // Name the cross-character case. A zero on either side is ordinary -- every companion, NPC and
                // wildlife socket lands here, and so does any build before the table binds -- so those stay silent.
                // Two DIFFERENT protagonists is the bug this gate exists for, and it is silent refusal that made it
                // cost three rounds of log-reading to find. Rate-limited to one line per (host, table) pair per world
                // so a persistent mismatch reports once instead of per socket per build.
                if (tableIdx != 0 && hostIdx != 0)
                {
                    const auto worldGen = CDCore::world_generation();
                    const auto key = (worldGen << 8) | (static_cast<std::uint64_t>(hostIdx) << 4) | tableIdx;
                    static std::atomic<std::uint64_t> s_lastMismatchKey{0};
                    if (s_lastMismatchKey.exchange(key, std::memory_order_relaxed) != key)
                    {
                        DMK::Logger::get_instance().warning(
                            "[socket-override] HOST MISMATCH: body 0x{:X} belongs to charIdx {}, but the target "
                            "table holds charIdx {}'s targets -- refusing to dress it. Slot={} tag={:#06x}. "
                            "Expect the character's own apply to redress it once the table rebinds.",
                            static_cast<std::uint64_t>(a1), hostIdx, tableIdx, slot_name(*slotOpt),
                            static_cast<unsigned>(slotTag));
                    }
                }
                return trampoline(a1, partId, slotTag, a4, a5, record, outList);
            }

            // Point the record's dye entries at LT's colours for the duration of the build.
            //
            // Rewriting the mesh alone leaves the descriptor carrying the REAL item's dye, because the dye object is
            // built HERE from the incoming record's entry array -- so the transmog mesh appeared undyed until LT's
            // apply re-injected a second later. Substituting the source array bakes the right colour in on the first
            // build. Publishing through DyeRecordInject does NOT work at this point: its DyeCopier detour is not in
            // this call tree (verified -- state was published and never consumed).
            //
            // The record is part state, not the authoritative equip table, and the original pointer and count are put
            // back before returning.
            thread_local std::uint8_t s_dyeRecords[k_maxDyeRecords * 16];
            bool dyeSwapped = false;
            std::uint64_t savedDyeData = 0;
            std::uint32_t savedDyeCount = 0;
            if (record >= 0x10000)
            {
                const auto n = build_slot_dye_records(slotIdx, static_cast<std::uintptr_t>(record), s_dyeRecords);
                if (n != 0)
                {
                    const auto curData = DMKMemory::seh_read<std::uint64_t>(
                        static_cast<std::uintptr_t>(record) + k_recordDyeDataOffset);
                    const auto curCount = DMKMemory::seh_read<std::uint32_t>(
                        static_cast<std::uintptr_t>(record) + k_recordDyeCountOffset);
                    if (curData.has_value() && curCount.has_value() &&
                        DMKMemory::seh_write<std::uint64_t>(static_cast<std::uintptr_t>(record) +
                                                                k_recordDyeDataOffset,
                                                            reinterpret_cast<std::uint64_t>(s_dyeRecords)) &&
                        DMKMemory::seh_write<std::uint32_t>(
                            static_cast<std::uintptr_t>(record) + k_recordDyeCountOffset, n))
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
                containerBefore ? DMKMemory::seh_read<std::uint32_t>(containerBefore + 8).value_or(0) : 0;

            const auto ret = trampoline(a1, partId, slotTag, a4, a5, record, outList);

            if (dyeSwapped)
            {
                (void)DMKMemory::seh_write<std::uint64_t>(
                    static_cast<std::uintptr_t>(record) + k_recordDyeDataOffset, savedDyeData);
                (void)DMKMemory::seh_write<std::uint32_t>(
                    static_cast<std::uintptr_t>(record) + k_recordDyeCountOffset, savedDyeCount);
            }

            const auto container = out_container(outList);
            if (container == 0)
                return ret;
            const auto data = DMKMemory::seh_read<std::uint64_t>(container).value_or(0);
            const auto countAfter = DMKMemory::seh_read<std::uint32_t>(container + 8).value_or(0);
            if (data < 0x10000 || countAfter <= countBefore)
                return ret;

            // Rewrite the mesh wrapper on every descriptor appended for this socket. The engine resolved it from the
            // REAL item; pointing it at LT's target means the transmog mesh is what gets attached, so the real mesh is
            // never built and there is nothing to flash or tear down afterwards.
            unsigned rewritten = 0;
            for (std::uint32_t i = countBefore; i < countAfter; ++i)
            {
                const auto entry = static_cast<std::uintptr_t>(data) + i * k_descriptorStride;
                const auto cur = DMKMemory::seh_read<std::uint64_t>(entry + k_descriptorWrapperOffset).value_or(0);
                if (cur < 0x10000 || cur == target)
                    continue;
                addref_wrapper(target);
                if (DMKMemory::seh_write<std::uint64_t>(entry + k_descriptorWrapperOffset,
                                                        static_cast<std::uint64_t>(target)))
                    ++rewritten;
            }

            if (rewritten != 0)
            {
                const auto n = g_overridden.fetch_add(rewritten, std::memory_order_relaxed) + rewritten;
                DMK::Logger::get_instance().debug(
                    "[socket-override] slot={} tag={:#06x} rewrote {} descriptor(s) -> {:#x} (total {})",
                    slot_name(static_cast<TransmogSlot>(slotIdx)), slotTag, rewritten, target, n);
            }
            return ret;
        }
    }

    bool install() noexcept
    {
        bool expected = false;
        if (!g_installed.compare_exchange_strong(expected, true))
            return g_orig != nullptr;

        auto &log = DMK::Logger::get_instance();

        const auto addr = resolve_address(k_partDescriptorBuildCandidates,
                                          std::size(k_partDescriptorBuildCandidates), "PartDescriptorBuild");
        if (addr == 0)
        {
            log.warning("[socket-override] PartDescriptorBuild AOB failed -- real items will flash before tear-down");
            return false;
        }

        auto &hookMgr = DMK::HookManager::get_instance();
        BuildFn tramp = nullptr;
        auto res = hookMgr.create_inline_hook("PartDescriptorBuild", addr, reinterpret_cast<void *>(&on_build),
                                              reinterpret_cast<void **>(&tramp));
        if (!res.has_value())
        {
            log.warning("[socket-override] hook install failed at {:#x}: {}", addr,
                        DetourModKit::Hook::error_to_string(res.error()));
            return false;
        }

        g_orig = tramp;
        log.info("[socket-override] hooked PartDescriptorBuild at {:#x}", addr);
        return true;
    }

    unsigned overridden_count() noexcept
    {
        return g_overridden.load(std::memory_order_relaxed);
    }
}
