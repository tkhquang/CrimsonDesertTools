#include "visibility_write.hpp"
#include "aob_resolver.hpp"
#include "categories.hpp"
#include "shared_state.hpp"

#include <DetourModKit/logger.hpp>
#include <DetourModKit/memory.hpp>
#include <DetourModKit/scan.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace EquipHide
{
    // File-scope scratch buffer reused across apply_direct_vis_write calls. It is populated under vis_write_mutex,
    // held for the full duration of the function, so concurrent callers never race. File scope rather than a local
    // inside the __try block keeps the stack small for game threads with tight stack reserves and side-steps MSVC
    // C2712.
    //
    // It stores composite (vis_ctrl, addr) keys so the orphan sweep can tell "this address is no longer in any
    // character's part map" (a true orphan, restore it) from "this address belongs to a different vis_ctrl that this
    // pass already processed" (not an orphan, leave it alone). Address-only keying restores the previously-active
    // character's vis bytes back to visible on a swap, because the new character's vis ctrl does not iterate the
    // outgoing character's vis-byte addresses.
    static std::vector<VisKey> s_touchedVisKeys;

    // File-scope active vis-ctrl scratch for the orphan sweep.
    static std::array<std::uintptr_t, k_maxProtagonists> s_activeVisCtrls{};

    /**
     * @brief Stateless less-than comparator on (vis_ctrl, addr) lexicographic order.
     * @details A free function rather than a lambda, so it can sit alongside __try without tripping C2712 on a
     *          captured lambda.
     */
    static bool vis_key_less(const VisKey &a, const VisKey &b) noexcept
    {
        if (a.visCtrl != b.visCtrl)
            return a.visCtrl < b.visCtrl;
        return a.addr < b.addr;
    }

    std::size_t vis_byte_offset() noexcept
    {
        static const std::size_t value = []() noexcept -> std::size_t
        {
            constexpr std::size_t k_nominal = 0x20;
            try
            {
                DMK::scan::CodeConstant cc{};
                cc.site = k_equipVisCheckCandidates;
                cc.kind = DMK::scan::OperandKind::MemoryDisplacement;
                cc.operand_index = 1; // movzx eax, byte [r12+disp]: the memory operand
                cc.nominal = static_cast<std::int64_t>(k_nominal);
                cc.has_nominal = true;
                const auto decoded = DMK::scan::read_code_constant(cc);
                if (decoded.has_value() && *decoded >= 0x10 && *decoded <= 0x40)
                {
                    const auto off = static_cast<std::size_t>(*decoded);
                    // A live value != nominal means the PartInOut layout drifted on a patch. The decode self-heals
                    // it, but surfaces a WARNING so the offset change is easy to spot in the log.
                    if (off != k_nominal)
                        (void)DMK::log().try_log(
                            DMK::LogLevel::Warning,
                            "PartInOut vis-byte offset DRIFTED: live={:#x} nominal={:#x} - self-healed "
                            "(engine layout changed)",
                            off,
                            k_nominal
                        );
                    else
                        (void)DMK::log().try_log(
                            DMK::LogLevel::Info,
                            "PartInOut vis-byte offset decoded live: {:#x} (matches nominal)",
                            off
                        );
                    return off;
                }
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "PartInOut vis-byte offset live-decode out of range/unavailable; using nominal {:#x}",
                    k_nominal
                );
            }
            catch (...)
            {
                // Fail closed and loud. A silent swallow makes a decode that never ran look like a clean match on
                // the nominal.
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "PartInOut vis-byte offset live-decode raised an exception. Using nominal {:#x}",
                    k_nominal
                );
            }
            return k_nominal;
        }();
        return value;
    }

    // Reject a mapBase that passed the plausible-pointer gate but does not look like a real part-visibility hashtable
    // (a stale or reallocated descriptor yields an oversized count, or a bucket pointer into the image instead of the
    // heap). Field list: see the map-primitive notes in armor_injection.cpp, which walks the same five fields.
    static bool part_vis_map_looks_valid(std::uintptr_t mapBase) noexcept
    {
        const auto count = DMK::memory::read<std::uint32_t>(DMK::Address{mapBase});
        const auto entryCount = DMK::memory::read<std::uint32_t>(DMK::Address{mapBase + 4});
        const auto cap = DMK::memory::read<std::uint32_t>(DMK::Address{mapBase + 8});
        const auto buckets = DMK::memory::read<std::uintptr_t>(DMK::Address{mapBase + 0x10});
        const auto entryPtrs = DMK::memory::read<std::uintptr_t>(DMK::Address{mapBase + 0x18});
        if (!count || !entryCount || !cap || !buckets || !entryPtrs)
            return false;
        if (*count == 0 || *count > 0x400)
            return false;
        if (*cap == 0 || *cap > 0x10000 || *entryCount > *cap)
            return false;
        if (!DMK::memory::is_plausible_ptr(DMK::Address{*buckets}) ||
            !DMK::memory::is_plausible_ptr(DMK::Address{*entryPtrs}))
            return false;
        return DMK::memory::read<std::uint32_t>(DMK::Address{*buckets}).has_value() &&
               DMK::memory::read<std::uintptr_t>(DMK::Address{*entryPtrs}).has_value();
    }

    /**
     * @brief Implementation body extracted out of the SEH-wrapped public entry point.
     * @details MSVC rejects __try in a frame that requires object unwinding (C2712). The per-(vis_ctrl, addr) keyed
     *          map and the per-character map selection both introduce hidden temporaries that MSVC counts as unwind
     *          requirements. Pattern mirrors equip_hide.cpp::on_vis_check_impl.
     */
    static void apply_direct_vis_write_impl() noexcept
    {
        auto &addrs = resolved_addrs();
        auto &logger = DMK::log();
        const auto lookup = reinterpret_cast<MapLookupFn>(addrs.mapLookup);
        auto &ps = player_state();
        auto &origVis = original_vis_map();
        const auto n = ps.count.load(std::memory_order_relaxed);
        int hiddenCount = 0;
        int restoredCount = 0;

        for (int i = 0; i < n; ++i)
        {
            const auto vc = ps.visCtrls[i].load(std::memory_order_relaxed);
            if (!vc)
                continue;

            // Per-slot character idx. -1 (unknown body, fallback path, pre-resolve) collapses to the active
            // character's map via classify_part_for / is_any_category_hidden_for, so an unidentified slot keeps
            // single-character semantics.
            const int charIdx = ps.visCharIdx[i].load(std::memory_order_relaxed);

            // Resolve the part-info descriptor and its part-visibility map. Offsets and the re-verification recipe
            // live on the k_visCtrl* constants in visibility_write.hpp. armor_injection.cpp walks the same three.
            // The walk stops at the descriptor SLOT, so the trailing read is what yields the descriptor pointer
            // itself.
            const auto desc = DMK::memory::walk(
                                  DMK::Address{vc},
                                  std::array<std::ptrdiff_t, 2>{k_visCtrlToCccOffset, k_cccToDescriptorOffset}
            )
                                  .and_then([](DMK::Address leaf) { return DMK::memory::read<std::uintptr_t>(leaf); });
            if (!desc)
            {
                (void)logger.try_log(
                    DMK::LogLevel::Trace,
                    "DirectWrite [{}]: vc=0x{:X} descriptor=NULL (+{:#x} -> +{:#x})",
                    i,
                    vc,
                    k_visCtrlToCccOffset,
                    k_cccToDescriptorOffset
                );
                continue;
            }
            const auto mapBase = *desc + k_descriptorToPartVisMapOffset;

            // A drifted chain offset can yield a non-faulting garbage mapBase. The guarded walk traps an actual
            // fault, not a wrong-but-mapped pointer. Reject an implausible base so a future layout shift skips this
            // vis-controller instead of a per-part lookup through a wrong map, or a fault that aborts the whole pass
            // for every character.
            if (!DMK::memory::is_plausible_ptr(DMK::Address{mapBase}))
            {
                (void)logger.try_log(
                    DMK::LogLevel::Trace,
                    "DirectWrite [{}]: vc=0x{:X} implausible mapBase=0x{:X} (+{:#x} -> +{:#x} -> +{:#x})",
                    i,
                    vc,
                    mapBase,
                    k_visCtrlToCccOffset,
                    k_cccToDescriptorOffset,
                    k_descriptorToPartVisMapOffset
                );
                continue;
            }

            // A stale or reallocated descriptor (a companion despawned between resolve passes) can pass the
            // plausible-ptr gate yet point at a non-map. A walk of it in lookup() below faults. Skip it.
            if (!part_vis_map_looks_valid(mapBase))
            {
                (void)logger.try_log(
                    DMK::LogLevel::Trace,
                    "DirectWrite [{}]: vc=0x{:X} mapBase=0x{:X} not a valid part-vis map "
                    "(stale/reallocated descriptor) - skipping",
                    i,
                    vc,
                    mapBase
                );
                continue;
            }

            // Choose the character-specific map for the iteration. charIdx == -1 routes to the active-character map,
            // so a slot the mod cannot identify still cycles through the full active-character part list.
            const auto *partMapPtr = (charIdx >= 0 && charIdx < static_cast<int>(k_charIdxCount))
                                         ? &get_part_map_for(charIdx)
                                         : &get_part_map();

            // Per-protagonist TRACE accumulators, emitted as one comma-joined line per state at the end of this outer
            // iteration. Cheap to maintain even when TRACE is off. Only the join and emit at the bottom is gated.
            std::vector<uint32_t> v_hidden;
            std::vector<uint32_t> v_force_shown;
            std::vector<std::pair<uint32_t, uint8_t>> v_restored;

            for (const auto &[hash, mask] : *partMapPtr)
            {
                const auto entry = lookup(mapBase, &hash);
                if (!entry)
                    continue;

                const auto visAddr = entry + vis_byte_offset();
                const VisKey key{vc, visAddr};
                s_touchedVisKeys.push_back(key);

                // Per-character hidden-state lookup: classify_part_for already produced `mask` from the per-character
                // map. is_any_category_hidden_for currently mirrors the active-character helper because the Hidden
                // and Enabled toggles are global, and only the parts list is per-character. It is plumbed regardless
                // so a future per-character overlay slots in without another touch of this hot path.
                if (is_any_category_hidden_for(mask, charIdx))
                {
                    if (origVis.find(key) == origVis.end())
                    {
                        // Cache the engine's own byte before the first hide. An entry freed between the lookup and
                        // this read fails closed and leaves the cache untouched.
                        const auto current = DMK::memory::read<std::uint8_t>(DMK::Address{visAddr});
                        if (!current)
                            continue;
                        origVis[key] = *current;
                    }
                    if (!DMK::memory::write_in_place<std::uint8_t>(DMK::Address{visAddr}, std::uint8_t{2}))
                        continue;
                    ++hiddenCount;
                    v_hidden.push_back(hash);
                }
                else
                {
                    // Always force vis=0 for a visible part. A cached origVis value cannot be restored verbatim: the
                    // engine writes its own state into this byte (sample values 0xE6, 0xF6 observed in trace) with
                    // the hidden bit (0x02) set, so a restore of the cached value keeps the part hidden.
                    if (!DMK::memory::write_in_place<std::uint8_t>(DMK::Address{visAddr}, std::uint8_t{0}))
                        continue;
                    auto it = origVis.find(key);
                    if (it != origVis.end())
                    {
                        v_restored.push_back({hash, it->second});
                        origVis.erase(it);
                    }
                    else
                    {
                        v_force_shown.push_back(hash);
                    }
                    ++restoredCount;
                }
            }

            // Per-protagonist compact TRACE summary. One line per state keeps a direct-write tick to three lines.
            if (logger.is_enabled(DMK::LogLevel::Trace))
            {
                auto join_hex = [](const std::vector<uint32_t> &v)
                {
                    std::string s;
                    s.reserve(v.size() * 8);
                    for (std::size_t k = 0; k < v.size(); ++k)
                    {
                        if (k > 0)
                            s += ", ";
                        s += std::format("0x{:04X}", v[k]);
                    }
                    return s;
                };
                if (!v_hidden.empty())
                    (void)logger.try_log(
                        DMK::LogLevel::Trace,
                        "  [{}] hidden char_idx={} ({}): {}",
                        i,
                        charIdx,
                        v_hidden.size(),
                        join_hex(v_hidden)
                    );
                if (!v_force_shown.empty())
                    (void)logger.try_log(
                        DMK::LogLevel::Trace,
                        "  [{}] force-shown char_idx={} ({}): {}",
                        i,
                        charIdx,
                        v_force_shown.size(),
                        join_hex(v_force_shown)
                    );
                if (!v_restored.empty())
                {
                    std::string s;
                    s.reserve(v_restored.size() * 14);
                    for (std::size_t k = 0; k < v_restored.size(); ++k)
                    {
                        if (k > 0)
                            s += ", ";
                        s += std::format("0x{:04X}(orig=0x{:02X})", v_restored[k].first, v_restored[k].second);
                    }
                    (void)logger.try_log(
                        DMK::LogLevel::Trace,
                        "  [{}] restored char_idx={} ({}): {}",
                        i,
                        charIdx,
                        v_restored.size(),
                        s
                    );
                }
            }
        }

        // Orphan sweep, per-vis-ctrl edition. An entry in origVis counts as orphaned ONLY when its vis_ctrl matches a
        // vis_ctrl this pass processed (that vis_ctrl is currently in ps.visCtrls) AND no (vis_ctrl, addr) pair this
        // pass touched matches the entry's key. An entry whose vis_ctrl left the active player set stays untouched,
        // and cleanup_vis_bytes() restores it at shutdown, so a character swap does not strip the inactive
        // character's hide state from the now-unwatched vis_ctrl. An unconditional sweep that ignores vis_ctrl
        // identity walks only the active character's part map, finds every previously-active character's vis-byte
        // address absent from the touched set, and restores them to visible, which silently undoes the prior
        // character's hide state on every swap.
        //
        // Sort plus binary_search on composite keys: lexicographic (vis_ctrl, addr) ordering keeps the
        // active-vis-ctrl set clustered and matches the equality semantics of VisKey.
        std::sort(s_touchedVisKeys.begin(), s_touchedVisKeys.end(), vis_key_less);

        s_activeVisCtrls.fill(0);
        int activeCount = 0;
        for (int i = 0; i < n && activeCount < k_maxProtagonists; ++i)
        {
            const auto vc = ps.visCtrls[i].load(std::memory_order_relaxed);
            if (vc)
                s_activeVisCtrls[activeCount++] = vc;
        }

        int orphanRestored = 0;
        for (auto it = origVis.begin(); it != origVis.end();)
        {
            const auto entryKey = it->first;

            bool vcIsActive = false;
            for (int j = 0; j < activeCount; ++j)
            {
                if (s_activeVisCtrls[j] == entryKey.visCtrl)
                {
                    vcIsActive = true;
                    break;
                }
            }
            if (!vcIsActive)
            {
                // The vis_ctrl is no longer tracked, so this entry's character swapped out or the player set changed
                // shape entirely. Leave the vis byte alone so the inactive character's hide state survives the swap.
                // cleanup_vis_bytes() at shutdown handles the final restore.
                ++it;
                continue;
            }
            if (std::binary_search(s_touchedVisKeys.begin(), s_touchedVisKeys.end(), entryKey, vis_key_less))
            {
                ++it;
                continue;
            }

            // A cached origVis value can carry the engine's own hidden bit (0x02). A verbatim restore keeps the part
            // hidden, so write a literal 0 (same reasoning as the main restore branch above).
            (void)DMK::memory::write_in_place<std::uint8_t>(DMK::Address{entryKey.addr}, std::uint8_t{0});
            it = origVis.erase(it);
            ++orphanRestored;
        }
        if (orphanRestored > 0)
            (void)logger.try_log(
                DMK::LogLevel::Debug,
                "DirectWrite: {} orphan vis bytes restored (category change for active vis ctrls)",
                orphanRestored
            );
        restoredCount += orphanRestored;

        (void)logger.try_log(
            DMK::LogLevel::Info,
            "DirectWrite: {} protagonists, {} hidden, {} restored",
            n,
            hiddenCount,
            restoredCount
        );
    }

    void apply_direct_vis_write() noexcept
    {
        auto &addrs = resolved_addrs();
        if (!addrs.mapLookup)
            return;
        // Manual lock and unlock: MSVC SEH does not run C++ destructors on unwind under /EHsc, so an RAII lock stays
        // held after a caught fault. The __try/__finally wrapper below gives the same guarantee in SEH terms.
        // __finally runs on normal return, on SEH propagation, and when mtx.unlock() itself raises SEH, so the mutex
        // is always released. Note the limit: apply_direct_vis_write_impl is noexcept, so a C++ throw inside it
        // terminates the process at the throw point and no handler here runs. Every throwing step in that body goes
        // through try_log for that reason. Without the __finally the mid-hook re-fires the work on every frame and
        // produces a tight try_lock failure spam loop.
        auto &mtx = vis_write_mutex();
        if (!mtx.try_lock())
        {
            // Lost the lock race against another writer (mid-hook, resolve poll, or a second input-thread tick).
            // Republish the work-pending signal so the mid-hook re-runs this pass on the next game frame. Without it
            // the toggle that triggered this call is silently dropped and the user-visible vis byte never flips.
            needs_direct_write().store(true, std::memory_order_release);
            (void)DMK::log().try_log(DMK::LogLevel::Trace, "DirectWrite: try_lock failed, deferred to mid-hook");
            return;
        }

        s_touchedVisKeys.clear();

        __try
        {
            __try
            {
                apply_direct_vis_write_impl();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                static std::atomic<bool> s_crashLogged{false};
                if (!s_crashLogged.exchange(true, std::memory_order_relaxed))
                    (void)DMK::log().try_log(DMK::LogLevel::Warning, "DirectWrite: SEH caught crash");
            }
        }
        __finally
        {
            mtx.unlock();
        }
    }

    /**
     * @brief Implementation body for cleanup_vis_bytes().
     * @details A structured binding on map<VisKey, ...> creates the same hidden temporary that
     *          apply_direct_vis_write_impl works around, so the same _impl split keeps the SEH wrapper free of unwind
     *          state.
     */
    static void cleanup_vis_bytes_impl() noexcept
    {
        auto &origVis = original_vis_map();
        int restoredCount = 0;
        for (const auto &[key, original] : origVis)
        {
            (void)DMK::memory::write_in_place<std::uint8_t>(DMK::Address{key.addr}, original);
            ++restoredCount;
        }
        origVis.clear();

        (void)DMK::log().try_log(DMK::LogLevel::Debug, "Cleanup: {} vis bytes restored", restoredCount);
    }

    void cleanup_vis_bytes() noexcept
    {
        auto &mtx = vis_write_mutex();
        mtx.lock();

        // __try/__finally so the unlock runs even when cleanup_vis_bytes_impl faults or mtx.unlock() itself raises
        // an SEH. Without the guard either path leaks the lock and stalls every subsequent try_lock from the
        // mid-hook.
        __try
        {
            __try
            {
                cleanup_vis_bytes_impl();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                static std::atomic<bool> s_crashLogged{false};
                if (!s_crashLogged.exchange(true, std::memory_order_relaxed))
                    (void)DMK::log().try_log(DMK::LogLevel::Warning, "Cleanup: SEH caught crash");
            }
        }
        __finally
        {
            mtx.unlock();
        }
    }

} // namespace EquipHide
