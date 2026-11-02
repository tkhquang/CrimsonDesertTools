#include "param_slot_guard.hpp"

#include "aob_resolver.hpp"

#include <DetourModKit.hpp>

#include <atomic>
#include <cstdint>
#include <new>
#include <utility>

namespace Transmog::param_slot_guard
{
    namespace
    {
        /**
         * @brief ComponentParamSlot: `void *f(void **table, uint32_t slot)`, resolved through
         *        AnchorId::ComponentParamSlot.
         * @details The body reads only rcx and edx. It writes r8 and r9 before any read of them and reads no stack
         *          argument, so this two-parameter form forwards every input the engine consumes.
         */
        using SlotFn = void *(*)(void **, std::uint32_t);

        /// The engine body behind the hook. Published before Hook::enable(), so the detour never reads null.
        std::atomic<SlotFn> s_original{nullptr};

        /// Latches the first guarded call, which is the only one that logs.
        std::atomic<bool> s_reported{false};

        /**
         * @brief Counted detour for ComponentParamSlot.
         * @return null for a null @p table, the slot miss that every caller handles. Any other call returns the
         *         engine result.
         */
        void *on_slot(void **table, std::uint32_t slot) noexcept
        {
            if (table == nullptr)
            {
                // The first hit names the slot kind of the skipped effect. Later hits repeat on every interaction and
                // add nothing, so they stay silent.
                if (!s_reported.exchange(true, std::memory_order_relaxed))
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Info,
                        "[param-slot-guard] null slot table for slot {:#x}, effect skipped",
                        slot
                    );
                }
                return nullptr;
            }
            return s_original.load(std::memory_order_acquire)(table, slot);
        }
    } // namespace

    bool install(DMK::hook::HookStack &hooks) noexcept
    {
        auto &log = DMK::log();

        const auto addr = anchor_address(AnchorId::ComponentParamSlot);
        if (addr == 0)
        {
            (void)log.try_log(
                DMK::LogLevel::Warning,
                "[param-slot-guard] ComponentParamSlot AOB failed - an interaction in transmogged Gadget_Gloves can "
                "crash the game"
            );
            return false;
        }

        try
        {
            // The counted form owns the teardown drain. The engine calls this function from several threads, so a raw
            // detour gives no proof that every call left the Logic DLL before it unmaps.
            auto installed = DMK::hook::inline_at<&on_slot>(DMK::hook::InlineRequest{
                .name = "ComponentParamSlot",
                .target = DMK::Address{addr},
            });
            if (!installed)
            {
                (void)log.try_log(
                    DMK::LogLevel::Warning,
                    "[param-slot-guard] hook creation failed at {:#x}: {}",
                    addr,
                    installed.error().message()
                );
                return false;
            }

            DMK::hook::Hook &hook = hooks.push(std::move(*installed));
            s_original.store(hook.original<SlotFn>(), std::memory_order_release);
            if (auto armed = hook.enable(); !armed)
            {
                (void)log.try_log(
                    DMK::LogLevel::Warning,
                    "[param-slot-guard] hook could not be armed at {:#x}: {}",
                    addr,
                    armed.error().message()
                );
                return false;
            }
        }
        catch (const std::bad_alloc &)
        {
            // The request name and the stack slot are the only allocations. A failed push destroys the disabled hook.
            (void)log.try_log(DMK::LogLevel::Warning, "[param-slot-guard] install at {:#x} ran out of memory", addr);
            return false;
        }

        (void)log.try_log(DMK::LogLevel::Info, "[param-slot-guard] hooked ComponentParamSlot at {:#x}", addr);
        return true;
    }
} // namespace Transmog::param_slot_guard
