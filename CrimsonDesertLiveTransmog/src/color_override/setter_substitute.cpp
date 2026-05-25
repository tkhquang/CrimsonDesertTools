#include "setter_substitute.hpp"
#include "color_carrier_set.hpp"
#include "color_matinst_owner.hpp"
#include "color_pending_overrides.hpp"
#include "color_picker_state.hpp"
#include "color_state.hpp"
#include "color_swatch_table.hpp"
#include "color_token_discovery.hpp"
#include "color_token_interner_hook.hpp"
#include "color_token_table.hpp"
#include "matinst_probe.hpp"
#include "one_shot_log_set.hpp"
#include "host_scope.hpp"
#include "../aob_resolver.hpp"
#include "../dye_record_inject.hpp"
#include "../shared_state.hpp"

#include <DetourModKit.hpp>
#include <safetyhook.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace Transmog::ColorOverride::SetterSubstitute
{
    namespace
    {
        // The setter target is the BYTE-variant of the engine's 4-byte
        // property write path. Resolved live via the AOB cascade in
        // `aob_resolver.hpp` (`k_setterByteCandidates`); see that file
        // for the byte-vs-dword discriminator that keeps the candidate
        // unique against its sibling at sub_14091CC90.

        // Property descriptor field offsets (read from rcx).
        constexpr std::ptrdiff_t kDesc_Size = 0x70;     // u32 byte-size
        constexpr std::ptrdiff_t kDesc_Callback = 0x78; // qword fn ptr

        // matInst struct offsets + carrier vtables live in
        // color_override/matinst_probe.hpp (shared with the publisher
        // hook). Reference them via `MatInstProbe::k_offMi_*` /
        // `MatInstProbe::k_carrierVtable_*`.
        namespace Probe = Transmog::ColorOverride::MatInstProbe;

        std::atomic<bool> g_applyWindowActive{false};

        // Primary slot context. transmog_apply.cpp wraps each per-
        // slot apply call with set_active_slot(slotIdx) / -1. This is
        // also mirrored into ColorOverride::State by set_active_slot
        // so the publisher hook (installed by ColorOverride::init)
        // sees the same slot during its async per-frame fires.
        std::atomic<int> g_activeSlot{-1};

        // Tail of the apply window: the engine fires per-property
        // setter writes asynchronously for some frames after slotPop
        // returns. Keep substituting for kApplyTailMs after the
        // window closes so the natural post-apply render writes are
        // also captured.
        constexpr std::int64_t kApplyTailMs = 300;
        std::atomic<std::int64_t> g_windowEndMs{0};

        std::int64_t now_ms() noexcept
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(
                       steady_clock::now().time_since_epoch())
                .count();
        }

        // Counters
        std::atomic<std::uint64_t> g_hookFires{0};
        std::atomic<std::uint64_t> g_substitutions{0};
        std::atomic<std::uint64_t> g_defaultsCaptured{0};
        std::atomic<std::uint64_t> g_hostRejects{0};
        std::atomic<std::uint64_t> g_swatchMisses{0};
        std::atomic<std::uint64_t> g_slotUnknown{0};

        // ---- Diagnostic dedup logs ----
        //
        // Bounded first-sighting dedup for token / vtable / (slot,
        // hash) one-shot logs. See `one_shot_log_set.hpp` for the
        // race-vs-duplicate-log trade-off.

        OneShotLogSet<std::uint32_t, 64> g_seenTokens;
        OneShotLogSet<std::uintptr_t, 16> g_seenVtables;
        // Diagnostic: dump the first 8 distinct property-descriptor
        // shapes the setter sees. Lets us confirm where the real
        // 16-bit interner token id lives in the descriptor (we
        // currently read `*(rdx-8+0x28)` as u32, which gives -1 layer
        // for tokens that ARE in the discovered slots -- suggesting
        // wrong field).
        OneShotLogSet<std::uintptr_t, 8> g_seenDescShapes;
        // First-fire-per-(slot, content_hash) dedup. Logged once per
        // distinct (slot, hash) so a new region shows up exactly one
        // time per session without drowning the hot path.
        std::array<OneShotLogSet<std::uint32_t, 32>, ::Transmog::k_slotCount>
            g_seenSlotHashes;

        // SEH-guarded read of one u32 with fault-tolerance. Returns
        // 0 on fault. Used by the descriptor dump below so a bad
        // address in one slot doesn't abort the whole row.
        std::uint32_t seh_read_u32(std::uintptr_t addr) noexcept
        {
            __try
            {
                return *reinterpret_cast<const std::uint32_t *>(addr);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        // Diagnostic: print the descriptor at `dst_prop` as a row of
        // u32 at +0x00..+0x3C. Used on the first 8 distinct descriptor
        // addresses to locate per-class fields.
        //
        // Field-offset map (identified during the 2026-05-25
        // publish-chain RE; offsets are STRUCT-RELATIVE so they're
        // patch-stable, unlike the RVAs of the functions that touch
        // them):
        //   +0x00 = vtbl (qword). The "Color permutations" descriptor
        //           class is identified via its constructor's call
        //           to a string-interner helper with the literal
        //           "Color" as one of the arguments.
        //   +0x20 = u32 hash-key. Compared against a hash computed
        //           from arg `a4` inside the 4-arg dispatch wrapper's
        //           internal filter (`if (!a4 || hash_match)`).
        //           Mismatch -> dispatch silently skipped.
        //   +0x28 = u32 token id (= our `tokId`).
        //   +0x44 = u32 flag field. Bit 0x200 (= 1<<9) is the
        //           "publish-skip" gate. Read by the descriptor's
        //           vtbl[20] (a tiny 11-byte function whose body is
        //           `mov eax,[rcx+0x44]; shr eax,9; not al; and al,1;
        //           ret`). When the bit is set the descriptor is
        //           silently filtered out of the publish iterator's
        //           inner loop.
        //   +0x70 = u32 byte-size offset (kDesc_Size). Controls the
        //           target-relative write offset and the inline
        //           vs callback path in the setter.
        //   +0x78 = qword callback fn ptr (kDesc_Callback). Tail-
        //           called by the per-channel setter when +0x70 == 0.
        //
        // Publish chain shape that reaches THIS setter (functions
        // named by ROLE since RVAs shift on patch):
        //   vector-sync driver
        //     -> v37.vtbl[44] (a per-class wrapper that tail-calls
        //        the publish iterator)
        //     -> publish iterator (iterates v10+0x38 / count v10+0x40
        //        queue, gates each item by desc.vtbl[20] = the
        //        publish-skip filter above)
        //     -> 4-arg dispatch wrapper (gates by a4-hash vs +0x20)
        //     -> desc.vtbl[89] = per-channel property setter (= us).
        void log_descriptor_shape(std::uintptr_t dst_prop) noexcept
        {
            std::uint32_t w[16]{};
            for (int i = 0; i < 16; ++i)
                w[i] = seh_read_u32(dst_prop + std::uintptr_t(i) * 4);
            DetourModKit::Logger::get_instance().debug(
                "[dye-setter-sub] desc@{:#x}  "
                "+00={:08X} +04={:08X} +08={:08X} +0C={:08X} "
                "+10={:08X} +14={:08X} +18={:08X} +1C={:08X} "
                "+20=hashKey{:08X} +24={:08X} +28=tok16{:08X} +2C={:08X} "
                "+30={:08X} +34={:08X} +38={:08X} +3C={:08X}",
                dst_prop,
                w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
        }

        // SEH-guarded property token-id read.
        bool read_token_id(std::uintptr_t a2, std::uint32_t &out) noexcept
        {
            if (a2 == 0)
                return false;
            __try
            {
                const auto dst_prop = a2 - 8;
                out = *reinterpret_cast<const std::uint32_t *>(dst_prop + 0x28);
                // One-shot diagnostic: dump 16 u32s of the first 8
                // distinct descriptor shapes. SEH-guarded helpers, so
                // failure on any slot just yields 0 in that column.
                if (g_seenDescShapes.insert_unique(dst_prop))
                    log_descriptor_shape(dst_prop);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Per-thread BGRA override buffer. Engine reads 4 bytes from
        // ctx.r8 -- we point it here. The buffer's lifetime spans only
        // a single midhook callback: the engine's setter consumes
        // ctx.r8 synchronously inside the trampoline that runs after
        // we return, and no reentrant path enters the setter, so
        // thread-local storage is sufficient.
        thread_local std::uint8_t s_overrideBuf[4]{};

        // SEH-guarded property-size read (from the descriptor at rcx).
        bool read_desc_size(std::uintptr_t rcx,
                            std::uint32_t &out) noexcept
        {
            __try
            {
                out = *reinterpret_cast<const std::uint32_t *>(
                    rcx + kDesc_Size);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // SEH-guarded read of the 4 BGRA bytes the engine is about to
        // copy from ctx.r8. Returns false on fault.
        bool read_source_bgra(std::uintptr_t r8,
                              std::uint8_t out[4]) noexcept
        {
            if (r8 == 0)
                return false;
            __try
            {
                out[0] = *reinterpret_cast<const std::uint8_t *>(r8 + 0);
                out[1] = *reinterpret_cast<const std::uint8_t *>(r8 + 1);
                out[2] = *reinterpret_cast<const std::uint8_t *>(r8 + 2);
                out[3] = *reinterpret_cast<const std::uint8_t *>(r8 + 3);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // matInst probe + submesh-name read live in
        // color_override/matinst_probe.{hpp,cpp} (shared with the
        // publisher hook). Use `Probe::probe_from_wrapper` /
        // `Probe::read_submesh_name`.

        // Resolve which LT slot owns this matInst. Tries the
        // MatInstOwner map first (populated by the publisher hook on
        // dst matInsts), falls back to the carrier set's content-hash
        // ownership, and finally to the active apply window. Returns
        // -1 if ownership can't be established.
        int resolve_slot(std::uintptr_t mi, std::uint32_t content_hash) noexcept
        {
            // Primary: the slot the apply path explicitly told us is
            // active. Always set while transmog_apply.cpp is inside
            // its per-slot work; -1 elsewhere.
            int slot = g_activeSlot.load(std::memory_order_acquire);
            if (slot >= 0)
                return slot;
            // Secondary: ColorOverride owner map (populated by the
            // publisher hook on dst matInsts during each apply).
            slot = ColorOverride::MatInstOwner::lookup_verified(
                mi, content_hash);
            if (slot >= 0)
                return slot;
            slot = ColorOverride::CarrierSet::find_slot_by_hash(content_hash);
            if (slot >= 0)
                return slot;
            return ColorOverride::State::active_apply_slot().load(
                std::memory_order_acquire);
        }

        void on_setter_mid(safetyhook::Context &ctx) noexcept
        {
            g_hookFires.fetch_add(1, std::memory_order_relaxed);

            // Periodic counter dump every ~12k fires.
            const auto fires = g_hookFires.load(std::memory_order_relaxed);
            if (fires % 12288 == 0)
            {
                std::uint8_t r = 0, g = 0, b = 0;
                bool active = DyeRecordInject::
                    get_published_first_active_rgb(&r, &g, &b);
                const auto hs = ColorOverride::HostScope::snapshot_stats();
                // windowOpen indicates the engine-vs-our-gate timing:
                // setter calls outside the LT-apply window + tail are
                // silently dropped before any of the counters above
                // increment (so a sudden fires-count gap correlates
                // with windowOpen=0 at the time). tailMs is the
                // post-close grace period (see kApplyTailMs).
                const bool winOpen =
                    g_applyWindowActive.load(std::memory_order_relaxed);
                const auto winAge =
                    now_ms() - g_windowEndMs.load(std::memory_order_relaxed);
                DetourModKit::Logger::get_instance().trace(
                    "[dye-setter-sub] fires={} subs={} defs={} miss={} "
                    "noSlot={} hostRej={} windowOpen={} winAge_ms={} "
                    "tailMs={} "
                    "active={} rgb=({:02X},{:02X},{:02X}) "
                    "hs[player={} npc={} freed={}]",
                    fires,
                    g_substitutions.load(std::memory_order_relaxed),
                    g_defaultsCaptured.load(std::memory_order_relaxed),
                    g_swatchMisses.load(std::memory_order_relaxed),
                    g_slotUnknown.load(std::memory_order_relaxed),
                    g_hostRejects.load(std::memory_order_relaxed),
                    winOpen ? 1 : 0, winAge, kApplyTailMs,
                    active, r, g, b,
                    hs.player, hs.npc, hs.freed);
            }

            // Gate 1: only inside the LT-apply window or short tail.
            const bool inWindow =
                g_applyWindowActive.load(std::memory_order_relaxed) || (now_ms() - g_windowEndMs.load(std::memory_order_relaxed) < kApplyTailMs);
            if (!inWindow)
                return;

            // Gate 2: host-scope. The setter detour fires from MANY
            // actors -- player, NPCs in render range, mounts, scenery.
            // Each NPC wearing the same shader template contributes
            // its own matInst (unique stable_id) to the capture set,
            // padding the row cap with non-player rows. Filter to
            // player-owned hosts only.
            if (!ColorOverride::HostScope::is_current_host_player_owned(ctx.rsp))
            {
                g_hostRejects.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            // No descriptor-class gate by design: comparing `ctx.rcx`
            // against a hardcoded engine-global is patch-fragile and
            // only filters by class identity, not by property name.
            // The downstream gates -- apply window, host scope,
            // matInst probe success, slot resolution, name capture,
            // override_active row -- already keep unrelated property
            // writes from substituting, so the extra heuristic earns
            // nothing.

            // Token id (for grouping + UI display). NOT a gate.
            std::uint32_t tokId = 0;
            if (read_token_id(ctx.rdx, tokId))
            {
                // Lazy warm-up: cold-start can leave the discovery
                // table underpopulated (Windows lazy-commit means
                // some registrar code pages weren't faulted in at
                // module-init scan time) and the interner table can
                // grow after our initial walk. Retry both when we
                // see a token we can't classify; throttled internally
                // so this is a cheap no-op once warmed.
                if (ColorOverride::TokenTable::token_layer(tokId) < 0)
                {
                    ColorOverride::TokenSlotDiscovery::
                        retry_if_underpopulated(33);
                    ColorOverride::InternerHook::refresh();
                }
                if (g_seenTokens.insert_unique(tokId))
                {
                    DetourModKit::Logger::get_instance().debug(
                        "[dye-setter-sub] token seen: 0x{:08X} layer={} "
                        "channel={}",
                        tokId,
                        ColorOverride::TokenTable::token_layer(tokId),
                        ColorOverride::TokenTable::channel_kind(tokId));
                }
            }
            if (tokId == 0)
                return;
            // No token-allowlist gate. Every dye-class property write
            // that reaches here is a candidate swatch -- tokens we
            // can't classify get bucketed under "misc" in the UI but
            // still substitute via the user's Recolor picks.
            // Substitution itself only fires when the user has an
            // `override_active` row, so unrelated writes that slip
            // through resolve_slot still don't get substituted.
            const std::uint16_t tok16 = static_cast<std::uint16_t>(tokId);

            // matInst identity probe.
            Probe::MatInstFields mf{};
            if (!Probe::probe_from_wrapper(ctx.rdi, mf))
                return;
            if (mf.content_hash == 0)
                return;

            // Slot-agnostic pending match.
            //
            // Before resolving slot ownership, consult PendingOverrides
            // by (submesh_name, token_id) alone. If the active preset
            // has a saved override whose submesh+token matches what
            // the engine is writing, substitute the user's RGB and
            // skip the slot-routing pipeline. Name-based match is
            // sufficient because submesh names are globally unique
            // within an outfit's dyeable surface set. The pending
            // entry stays in the map after a hit so repeated engine
            // writes to the same property keep substituting.
            if (ColorOverride::PendingOverrides::has_any())
            {
                char pend_sm[40] = {0};
                const bool sm_ok =
                    Probe::read_submesh_name(mf.mi, pend_sm, sizeof(pend_sm));
                if (sm_ok && pend_sm[0] != '\0')
                {
                    std::uint8_t pr = 0, pg = 0, pb = 0;
                    const bool pend_hit =
                        ColorOverride::PendingOverrides::lookup_any_slot(
                            pend_sm, tok16, pr, pg, pb);
                    if (pend_hit)
                    {
                        // Snapshot the engine's source bytes BEFORE
                        // redirecting ctx.r8 -- they are the row's
                        // natural default and feed the picker's
                        // "engine natural" reference dot via the
                        // placeholder-promote call below.
                        std::uint8_t src_bgra[4] = {0, 0, 0, 0xFF};
                        if (!read_source_bgra(ctx.r8, src_bgra))
                        {
                            src_bgra[0] = src_bgra[1] = src_bgra[2] = 0;
                            src_bgra[3] = 0xFF;
                        }
                        s_overrideBuf[0] = pb;
                        s_overrideBuf[1] = pg;
                        s_overrideBuf[2] = pr;
                        s_overrideBuf[3] = 0xFF;
                        ctx.r8 = reinterpret_cast<std::uintptr_t>(
                            &s_overrideBuf);
                        g_substitutions.fetch_add(
                            1, std::memory_order_relaxed);
                        ColorOverride::SwatchTable::
                            promote_placeholder_identity(
                                pend_sm, tok16,
                                mf.content_hash, mf.stable_id,
                                mf.template_id,
                                /*def_r=*/src_bgra[2],
                                /*def_g=*/src_bgra[1],
                                /*def_b=*/src_bgra[0],
                                /*def_a=*/src_bgra[3]);
                        return;
                    }
                }
            }

            // Slot ownership.
            int slot = resolve_slot(mf.mi, mf.content_hash);
            if (slot < 0)
            {
                g_slotUnknown.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            // Diagnostic: dedup-log the first ~16 distinct vtables
            // we observe AFTER slot resolution succeeds. Helps
            // verify which C++ classes the active carrier emits
            // and whether they ever shift after a carrier swap.
            if (g_seenVtables.insert_unique(mf.vtable))
            {
                DetourModKit::Logger::get_instance().debug(
                    "[dye-setter-sub] vtable observed: 0x{:X} "
                    "(slot={} hash=0x{:08X} template=0x{:04X})",
                    mf.vtable, slot, mf.content_hash, mf.template_id);
            }

            // Capture submesh name early -- needed by the
            // placeholder-first slot router below AND by
            // lookup_or_insert plus the one-shot log later.
            // SEH-protected; empty on bail.
            char sm_name[40] = {0};
            const bool nameOk =
                Probe::read_submesh_name(mf.mi, sm_name, sizeof(sm_name));

            // Placeholder-first slot router.
            //
            // The engine's body-mesh rebuild is batched across slots:
            // helm/boots/glove matInsts can be re-bound during the
            // Chest apply window, so resolve_slot() can mis-attribute
            // helm hashes to Chest. The user's saved placeholder is
            // the authoritative "this submesh belongs to slot X"
            // signal; when one exists for (submesh, token), trust it
            // over resolve_slot's answer.
            if (nameOk)
            {
                const int placeholderSlot =
                    ColorOverride::SwatchTable::find_placeholder_slot(
                        sm_name, tok16);
                if (placeholderSlot >= 0)
                    slot = placeholderSlot;
            }
            // First-fire log per (slot, content_hash). A per-slot
            // SET (not last-hash dedup) is required so hair/eye
            // re-fires can't mask a chest matInst between its own
            // fires.
            if (g_seenSlotHashes[slot].insert_unique(mf.content_hash))
            {
                DetourModKit::Logger::get_instance().debug(
                    "[dye-setter-sub] slot={} hash={:#x} "
                    "submesh='{}'{}",
                    slot, mf.content_hash,
                    sm_name[0] ? sm_name : "",
                    nameOk ? "" : " (no-name)");
            }

            // Skip name-less regions (hair, eyes, internal effect
            // materials). Their parent has no `_subMeshName`
            // (Material[+0x10] = nullptr, or wrapper +0x28 = module-
            // resident empty-string sentinel). They re-render every
            // frame inside the apply window and bleed into every
            // slot's capture set without user value. To debug a
            // coverage gap, comment this out and search the log for
            // "(no-name)" lines.
            if (!nameOk)
            {
                g_swatchMisses.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            const int row = ColorOverride::SwatchTable::lookup_or_insert(
                slot, mf.content_hash, mf.stable_id, mf.template_id,
                tok16, /*expect_open=*/true, sm_name);
            // Pending-override consume: if the user previously
            // persisted an RGB for this (slot, submesh, token), apply
            // it now. Pending entries stay in the map so subsequent
            // re-captures (post-teardown / reapply) keep auto-
            // applying without user action.
            if (row >= 0 && ColorOverride::PendingOverrides::slot_has_pending(slot))
            {
                std::uint8_t pr = 0, pg = 0, pb = 0;
                if (ColorOverride::PendingOverrides::lookup(
                        slot, sm_name, tok16, pr, pg, pb))
                {
                    const auto rowIdx = static_cast<std::size_t>(row);
                    ColorOverride::SwatchTable::set_override_rgb(
                        slot, rowIdx, pr, pg, pb);
                    ColorOverride::SwatchTable::set_override_active(
                        slot, rowIdx, true);
                }
            }
            if (row < 0)
            {
                g_swatchMisses.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const auto rowIdx = static_cast<std::size_t>(row);

            // Read the engine's natural BGRA value -- the bytes the
            // setter is about to copy from `ctx.r8`. The master dye
            // toggle lives in SwatchTable's per-slot storage rather
            // than PickerState because the UI's
            // `ImGui::Checkbox("Dye##dye_on")` writes through that
            // storage; gating on the same value avoids a stale-read
            // race.
            const bool slotEn =
                ColorOverride::SwatchTable::slot_enabled_get(slot);
            auto *ovr =
                ColorOverride::SwatchTable::override_row(slot, rowIdx);
            const bool rowOverride =
                ovr && ovr->override_active;

            std::uint8_t bgra[4]{};
            const bool readOk = read_source_bgra(ctx.r8, bgra);

            if (readOk)
            {
                // Always capture the engine value here -- `ctx.r8`
                // still holds the natural source bytes (we haven't
                // redirected it yet). Running capture regardless of
                // override state means `ovr.def_r/g/b` stays
                // populated for already-overridden rows, so the
                // picker's "engine default" reference dot keeps
                // working after a preset switch + re-apply.
                // `capture_default_if_unset` is idempotent via its
                // own `def_seen_mask & 1` gate; only the first fire
                // per row actually writes.
                //
                // Source byte order: BGRA at `ctx.r8`.
                ColorOverride::SwatchTable::capture_default_if_unset(
                    slot, rowIdx,
                    /*r=*/bgra[2], /*g=*/bgra[1],
                    /*b=*/bgra[0], /*a=*/bgra[3]);
                g_defaultsCaptured.fetch_add(1, std::memory_order_relaxed);
            }

            // Only substitute when BOTH the slot is enabled AND this
            // specific row's override flag is on. Otherwise the
            // engine value flows through unchanged.
            if (!slotEn || !rowOverride || !ovr)
                return;

            // size > 0  -> direct 4-byte DWORD memcpy path
            // size == 0 -> callback dispatch path (non-DWORD types)
            std::uint32_t fieldOffset = 0;
            if (!read_desc_size(ctx.rcx, fieldOffset))
                return;
            if (fieldOffset == 0)
                return;

            const auto ur = ovr->r;
            const auto ug = ovr->g;
            const auto ub = ovr->b;

            // BGRA pack.
            s_overrideBuf[0] = ub;
            s_overrideBuf[1] = ug;
            s_overrideBuf[2] = ur;
            s_overrideBuf[3] = 0xFF;
            ctx.r8 = reinterpret_cast<std::uintptr_t>(&s_overrideBuf);
            g_substitutions.fetch_add(1, std::memory_order_relaxed);
        }
    } // namespace

    bool init() noexcept
    {
        auto &log = DetourModKit::Logger::get_instance();
        const auto target = ::Transmog::resolve_address(
            ::Transmog::k_setterByteCandidates,
            "ColorOverride::SetterSubstitute");
        if (target == 0)
            return false;

        namespace DMK = DetourModKit;
        auto &hookMgr = DMK::HookManager::get_instance();
        auto res = hookMgr.create_mid_hook(
            "ColorOverride::SetterSubstitute", target, &on_setter_mid);
        if (!res.has_value())
        {
            log.warning(
                "[dye-setter-sub] mid-hook FAILED at {:#x}: {}",
                target,
                DetourModKit::Hook::error_to_string(res.error()));
            return false;
        }
        // The hooked function is the per-channel property setter:
        // called as descriptor vtbl[89] from the 4-arg dispatch
        // wrapper -- itself reached as vtbl[66] from the publish
        // iterator. Writes 4 bytes at `target + desc[+0x70]` then
        // fires the publish-notify helper. See `log_descriptor_shape`
        // above for the full publish-chain map.
        log.info(
            "[dye-setter-sub] mid-hook installed at {:#x} "
            "(per-row swatch table active)",
            target);
        return true;
    }

    void set_active_slot(int slot) noexcept
    {
        if (!::Transmog::flag_color_override().load(
                std::memory_order_acquire))
            return;
        g_activeSlot.store(slot, std::memory_order_release);
        // Mirror into ColorOverride::State so the publisher hook
        // resolves the right slot when its TLS-stack fallback is
        // empty (orch hooks aren't installed on v1.06). Also sets
        // the apply-window deadline so the publisher's window
        // check doesn't bail.
        if (slot >= 0)
            ColorOverride::mark_apply_begin(slot);
    }

    void set_apply_window(bool active) noexcept
    {
        if (!::Transmog::flag_color_override().load(
                std::memory_order_acquire))
            return;
        g_applyWindowActive.store(active, std::memory_order_release);
        if (active)
        {
            // Wipe first-sighting log so the next session re-logs each
            // distinct token. Concurrent setter fires during the clear
            // see size=0 (count is zeroed before entries) -- worst
            // case the post-clear fire logs a duplicate, benign.
            g_seenTokens.clear();
        }
        else
        {
            g_windowEndMs.store(now_ms(), std::memory_order_release);
        }
    }

} // namespace Transmog::ColorOverride::SetterSubstitute
