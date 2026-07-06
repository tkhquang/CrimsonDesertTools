// body_variant_hook.cpp
#include "body_variant_hook.hpp"

#include "aob_resolver.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>
#include <safetyhook.hpp>

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace DMK = DetourModKit;

namespace Transmog::BodyVariantHook
{
    namespace
    {
        // sub_141F90630(arg1, arg2, arg3, arg4) -> void. arg1 (rcx) holds the per-body variant-entry list at +0x3E0
        // and its count at +0x3E8 (stride 0x58). The sole caller discards rax, so the wrapper forwards no return. The
        // args are taken at register width (uint64_t) to avoid any truncation across the trampoline; the engine reads
        // only the low parts (edx, r8w).
        using ResolverFn = void (*)(void *, std::uint64_t, std::uint64_t, void *);
        ResolverFn g_origResolver = nullptr;

        // The char-class bypass jz byte inside the resolver's token-match loop (resolved via
        // k_charClassBypassCandidates into resolved_addrs().charClassBypass). 0xEB force-accepts entry[0]; 0x74
        // restores the natural jz.
        std::uintptr_t g_bypassAddr = 0;

        // Set true once install() has wired both hooks. Read by the transmog apply path via is_active() to decide
        // whether to engage the legacy bypass-force fallback. Written once during init before any apply runs; the
        // apply reads run on the game thread afterwards, so a relaxed atomic is sufficient.
        std::atomic<bool> g_active{false};

        // Observation state for the current resolver call. The resolver and its render call run on the same thread;
        // thread_local keeps the flags correct even if the engine resolves an appearance off the game thread.
        thread_local bool tl_inResolver = false;
        thread_local bool tl_rendered = false;

        void write_bypass(std::uint8_t value) noexcept
        {
            if (!g_bypassAddr)
            {
                return;
            }
            const auto byteValue = static_cast<std::byte>(value);
            (void)DMK::Memory::write_bytes(reinterpret_cast<std::byte *>(g_bypassAddr), &byteValue, 1);
        }

        // Pure observer on the render primitive: notes that the resolver rendered a matching variant. Guarded by
        // tl_inResolver so it only counts renders from the natural (observed) resolver pass -- never the forced retry,
        // nor renders from unrelated callers of this primitive.
        void on_render(SafetyHookContext & /*ctx*/) noexcept
        {
            if (tl_inResolver)
            {
                tl_rendered = true;
            }
        }

        void on_resolver(void *arg1, std::uint64_t arg2, std::uint64_t arg3, void *arg4) noexcept
        {
            // Only intervene during an LT transmog apply; real equips (and any other resolve) run untouched so the
            // engine still hides items that legitimately have no mesh for the wearer.
            if (!in_transmog().load(std::memory_order_relaxed))
            {
                g_origResolver(arg1, arg2, arg3, arg4);
                return;
            }

            // Run the natural, un-bypassed match and observe whether it rendered. Save/restore the flags so a nested
            // resolver call (should the engine ever make one) keeps this frame's observation isolated. The engine
            // resolver faults on early load (game data not ready); __finally restores the flags even on an SEH unwind
            // so a fault cannot strand tl_inResolver true and mis-attribute a later frame's render.
            const bool prevInResolver = tl_inResolver;
            const bool prevRendered = tl_rendered;
            bool rendered = false;
            tl_rendered = false;
            tl_inResolver = true;
            __try
            {
                g_origResolver(arg1, arg2, arg3, arg4);
                rendered = tl_rendered;
            }
            __finally
            {
                tl_inResolver = prevInResolver;
                tl_rendered = prevRendered;
            }

            if (rendered)
            {
                // A variant entry matched the wearer's body -> the correct per-body mesh already rendered.
                return;
            }

            // Nothing matched: an NPC/cross-class item that would otherwise be invisible on the player. Re-run the
            // resolver with the char-class bypass so its match loop force-accepts entry[0] -- the same fallback the old
            // transmog-side bypass gave, but now ONLY when the natural match failed. tl_inResolver is false here, so
            // this forced pass is not observed. The bypass byte is a live GLOBAL code byte: __finally guarantees the
            // 0x74 restore even if the forced pass SEH-unwinds, so a fault cannot leave the bypass forced-on for every
            // later resolve (which would reintroduce the male-variant mis-render on all characters). A plain RAII guard
            // is insufficient here -- under /EHsc, C++ destructors do not run during an async SEH unwind.
            __try
            {
                write_bypass(0xEB);
                g_origResolver(arg1, arg2, arg3, arg4);
            }
            __finally
            {
                write_bypass(0x74);
            }
        }
    } // namespace

    bool install()
    {
        auto &logger = DMK::Logger::get_instance();
        auto &hookMgr = DMK::HookManager::get_instance();

        const auto resolverAddr = resolve_address(k_bodyVariantResolverCandidates, "BodyVariantResolver");
        const auto renderAddr = resolve_address(k_bodyVariantRenderCandidates, "BodyVariantRender");

        if (!resolverAddr || !renderAddr)
        {
            logger.warning("[body-variant] AOB resolve failed (resolver={:#x} render={:#x}) -- per-body mesh hook "
                           "disabled",
                           resolverAddr, renderAddr);
            return false;
        }

        g_bypassAddr = resolved_addrs().charClassBypass;
        if (!g_bypassAddr)
        {
            logger.warning("[body-variant] charClassBypass unresolved -- NPC-item fallback will be a no-op");
        }

        // Install the observer first: the resolver hook must never fire before the primitive it observes is watched.
        const auto renderRes = hookMgr.create_mid_hook("BodyVariantRenderObserve", renderAddr, &on_render);
        if (!renderRes.has_value())
        {
            logger.warning("[body-variant] render observer midhook failed: {}",
                           DMK::Hook::error_to_string(renderRes.error()));
            return false;
        }

        const auto resolverRes =
            hookMgr.create_inline_hook("BodyVariantResolver", resolverAddr, reinterpret_cast<void *>(on_resolver),
                                       reinterpret_cast<void **>(&g_origResolver));
        if (!resolverRes.has_value())
        {
            logger.warning("[body-variant] resolver inline hook failed: {}",
                           DMK::Hook::error_to_string(resolverRes.error()));
            // Roll back the render observer installed above. Without this a partial install leaves a dangling midhook
            // firing on the render primitive for the rest of the process: on_render would keep taking the SafetyHook
            // context save/restore round-trip on every render call while only ever no-op'ing (tl_inResolver never set,
            // since the resolver hook that sets it did not install).
            (void)hookMgr.remove_hook(*renderRes);
            return false;
        }

        logger.info("[body-variant] hooks installed: resolver={:#x} render={:#x} bypass={:#x}", resolverAddr,
                    renderAddr, g_bypassAddr);
        g_active.store(true, std::memory_order_relaxed);
        return true;
    }

    bool is_active() noexcept
    {
        return g_active.load(std::memory_order_relaxed);
    }
} // namespace Transmog::BodyVariantHook
