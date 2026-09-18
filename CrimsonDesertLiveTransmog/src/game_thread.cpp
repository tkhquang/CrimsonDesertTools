#include "game_thread.hpp"

#include "aob_resolver.hpp"

#include <DetourModKit/address.hpp>
#include <DetourModKit/hook.hpp>
#include <DetourModKit/logger.hpp>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>

namespace Transmog::game_thread
{
    namespace
    {
        /**
         * @brief Mailbox state. The poster moves Empty -> Posted, the frame hook moves Posted -> Running -> Done, and
         *        the poster moves Done -> Empty once it has observed completion. A withdrawal moves Posted -> Empty
         *        and is legal only while the hook has not claimed the job, which the compare-exchange guarantees.
         */
        enum class Slot : std::uint8_t
        {
            Empty,
            Posted,
            Running,
            Done,
        };

        std::atomic<bool> g_installAttempted{false};
        std::atomic<bool> g_installed{false};
        std::atomic<bool> g_accepting{true};
        std::atomic<std::uint32_t> g_gameThreadId{0};
        std::atomic<Slot> g_slot{Slot::Empty};

        // Job payload. The poster writes both before its release store of Posted and the hook reads them after its
        // acquire exchange to Running, so the slot atomic carries them and they need none of their own.
        JobFn g_jobFn = nullptr;
        void *g_jobContext = nullptr;

        // Serializes posters against each other. Neither the hook callback nor shutdown() takes it, so a poster blocked
        // inside run_blocking cannot hold shutdown up.
        std::mutex g_postMtx;

        // Completion handshake between the hook and the blocked poster. The hook takes g_waitMtx only for the
        // lock-unlock that pairs its Done store with the poster's predicate check, never while the job runs.
        std::mutex g_waitMtx;
        std::condition_variable g_waitCv;

        std::atomic<std::uint64_t> g_jobsRun{0};
        std::atomic<bool> g_firstJobLogged{false};

        /// Bound on the wait for a claimed job at shutdown. The game thread is executing it, so a longer wait only
        /// delays a teardown that a frozen game would not let finish anyway.
        constexpr std::chrono::milliseconds SHUTDOWN_DRAIN_WAIT{5000};

        /**
         * @brief Frame hook body. Runs once per frame at the entry of the update step, on the game thread.
         * @details Idle cost is one atomic load per frame. With a job posted it claims the slot, runs the job inline
         *          (the whole apply executes here, inside the frame, before the engine's own scene-graph work for that
         *          frame), publishes Done and wakes the poster. Logging is left to the poster so the callback never
         *          formats or blocks.
         */
        void on_frame_update(DMK::hook::MidContext & /*ctx*/) noexcept
        {
            if (g_gameThreadId.load(std::memory_order_relaxed) == 0)
                g_gameThreadId.store(GetCurrentThreadId(), std::memory_order_release);

            if (g_slot.load(std::memory_order_acquire) != Slot::Posted)
                return;

            // The poster may have withdrawn the job between the load above and this exchange. A failed exchange means
            // exactly that, and the frame moves on.
            Slot expected = Slot::Posted;
            if (!g_slot.compare_exchange_strong(expected, Slot::Running, std::memory_order_acq_rel))
                return;

            const JobFn job = g_jobFn;
            void *const context = g_jobContext;
            if (job != nullptr)
                job(context);
            g_jobsRun.fetch_add(1, std::memory_order_relaxed);

            g_slot.store(Slot::Done, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(g_waitMtx);
            }
            g_waitCv.notify_all();
        }

        /// Withdraws an unclaimed job. True when the slot moved Posted -> Empty, false when the hook already claimed
        /// it.
        [[nodiscard]] bool withdraw_unclaimed() noexcept
        {
            Slot expected = Slot::Posted;
            return g_slot.compare_exchange_strong(expected, Slot::Empty, std::memory_order_acq_rel);
        }
    } // namespace

    bool install(DMK::hook::HookStack &hooks) noexcept
    {
        if (g_installAttempted.exchange(true, std::memory_order_acq_rel))
            return g_installed.load(std::memory_order_acquire);

        auto &log = DMK::log();
        try
        {
            const std::uintptr_t target = anchor_address(AnchorId::FrameUpdate);
            if (target == 0)
            {
                log.warning(
                    "[game-thread] FrameUpdate anchor unresolved; frame hook NOT armed - applies run on the worker "
                    "thread and the claim-erase race window stays open"
                );
                return false;
            }

            auto hook = DMK::hook::mid_at(
                DMK::hook::MidRequest{
                    .name = "FrameUpdate",
                    .target = DMK::Address{target},
                },
                &on_frame_update
            );
            if (!hook)
            {
                log.warning(
                    "[game-thread] mid-hook creation at {:#x} failed ({}); applies run on the worker thread",
                    target,
                    hook.error().message()
                );
                return false;
            }
            if (auto armed = hook->enable(); !armed)
            {
                log.warning(
                    "[game-thread] mid-hook at {:#x} could not be armed ({}); applies run on the worker thread",
                    target,
                    armed.error().message()
                );
                return false;
            }
            hooks.push(std::move(*hook));
            g_installed.store(true, std::memory_order_release);
            log.info("[game-thread] frame hook armed at {:#x}; applies run on the game thread", target);
            return true;
        }
        catch (...)
        {
            (void)log.try_log(
                DMK::LogLevel::Warning,
                "[game-thread] install stopped on an exception; applies run on the worker thread"
            );
            return false;
        }
    }

    bool available() noexcept
    {
        return g_installed.load(std::memory_order_acquire) && g_accepting.load(std::memory_order_acquire);
    }

    bool is_game_thread() noexcept
    {
        const std::uint32_t id = g_gameThreadId.load(std::memory_order_acquire);
        return id != 0 && id == GetCurrentThreadId();
    }

    RunResult run_blocking(JobFn job, void *context, std::uint32_t timeout_ms) noexcept
    {
        if (job == nullptr)
            return RunResult::Ran;
        if (!g_installed.load(std::memory_order_acquire))
            return RunResult::Unavailable;
        if (is_game_thread())
        {
            job(context);
            return RunResult::Ran;
        }

        // True from the Posted store until completion is observed. It tells the catch block below whether a job is out
        // there, because a report of Ran must never be given for a job that did not execute.
        bool posted = false;
        try
        {
            std::lock_guard<std::mutex> post_lk(g_postMtx);
            if (!g_accepting.load(std::memory_order_acquire))
                return RunResult::Shutdown;

            g_jobFn = job;
            g_jobContext = context;
            g_slot.store(Slot::Posted, std::memory_order_release);
            posted = true;

            std::unique_lock<std::mutex> lk(g_waitMtx);

            // Phase 1: wait for a frame to claim the job. Bounded, because no frame may be running. A shutdown wakes
            // this too.
            (void)g_waitCv.wait_for(
                lk,
                std::chrono::milliseconds(timeout_ms),
                []
                {
                    return g_slot.load(std::memory_order_acquire) != Slot::Posted ||
                           !g_accepting.load(std::memory_order_acquire);
                }
            );
            if (g_slot.load(std::memory_order_acquire) == Slot::Posted && withdraw_unclaimed())
                return g_accepting.load(std::memory_order_acquire) ? RunResult::Timeout : RunResult::Shutdown;

            // Phase 2: claimed. The game thread is executing the job. Unbounded on purpose: returning early would let
            // the caller proceed while its job still runs.
            g_waitCv.wait(lk, [] { return g_slot.load(std::memory_order_acquire) == Slot::Done; });
            g_slot.store(Slot::Empty, std::memory_order_release);
            posted = false;
        }
        catch (...)
        {
            if (!posted)
                return RunResult::Unavailable;
            // The wait machinery failed with a job out there. An unclaimed job is withdrawn and reported as a timeout
            // so the caller re-arms. A claimed job is executing on the game thread, so this waits for it the primitive
            // way rather than let the caller proceed concurrently with its own job.
            if (withdraw_unclaimed())
                return RunResult::Timeout;
            while (g_slot.load(std::memory_order_acquire) == Slot::Running)
                Sleep(1);
            g_slot.store(Slot::Empty, std::memory_order_release);
        }

        if (!g_firstJobLogged.exchange(true, std::memory_order_acq_rel))
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "[game-thread] first job ran on thread {}",
                g_gameThreadId.load(std::memory_order_acquire)
            );
        }
        return RunResult::Ran;
    }

    void shutdown() noexcept
    {
        g_accepting.store(false, std::memory_order_release);
        if (!g_installed.load(std::memory_order_acquire))
            return;

        auto &log = DMK::log();
        try
        {
            // Wake a poster blocked on an unclaimed job. It withdraws the job and reports Shutdown.
            {
                std::lock_guard<std::mutex> lk(g_waitMtx);
            }
            g_waitCv.notify_all();

            // A job the hook already claimed is executing on the game thread. Wait for it, bounded.
            std::unique_lock<std::mutex> lk(g_waitMtx);
            const bool drained = g_waitCv.wait_for(
                lk,
                SHUTDOWN_DRAIN_WAIT,
                [] { return g_slot.load(std::memory_order_acquire) != Slot::Running; }
            );
            if (!drained)
            {
                log.warning(
                    "[game-thread] a job was still running on the game thread after {} ms; continuing shutdown",
                    SHUTDOWN_DRAIN_WAIT.count()
                );
            }
            log.info(
                "[game-thread] shutdown: {} job(s) ran on the game thread",
                g_jobsRun.load(std::memory_order_relaxed)
            );
        }
        catch (...)
        {
            (void)log.try_log(DMK::LogLevel::Warning, "[game-thread] shutdown wait stopped on an exception");
        }
    }
} // namespace Transmog::game_thread
