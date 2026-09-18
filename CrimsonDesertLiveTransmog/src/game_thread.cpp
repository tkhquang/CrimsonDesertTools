#include "game_thread.hpp"

#include "aob_resolver.hpp"

#include <DetourModKit/address.hpp>
#include <DetourModKit/hook.hpp>
#include <DetourModKit/logger.hpp>

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>

// WaitOnAddress / WakeByAddressAll live in the synchronization API set, which the default link line does not pull in.
#pragma comment(lib, "Synchronization.lib")

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

        std::atomic<bool> s_install_attempted{false};
        std::atomic<bool> s_installed{false};
        std::atomic<bool> s_accepting{true};
        std::atomic<std::uint32_t> s_game_thread_id{0};

        // The mailbox word. Every wait in this module parks on its address and every state change wakes that address,
        // so a waiter needs no lock, no event handle and no condition variable, and the hook callback signals with one
        // kernel call. Relaxed loads elsewhere would do for the thread id; the slot itself carries the job payload
        // below with release/acquire.
        std::atomic<Slot> s_slot{Slot::Empty};

        // Job payload. The poster writes both before its release store of Posted and the hook reads them after its
        // acquire exchange to Running, so the slot atomic carries them and they need none of their own.
        JobFn s_job_fn = nullptr;
        void *s_job_context = nullptr;

        // Serializes posters against each other. It is the only lock in this module. Neither the hook callback nor
        // shutdown() takes it, so a poster blocked inside run_blocking cannot hold shutdown up.
        std::mutex s_post_mtx;

        std::atomic<std::uint64_t> s_jobs_run{0};
        std::atomic<bool> s_first_job_logged{false};

        /**
         * @brief Bound on the wait for a claimed job at shutdown.
         * @details The game thread is executing the job, so a longer wait only delays a teardown that a frozen game
         *          would not let finish anyway.
         */
        constexpr std::uint32_t SHUTDOWN_DRAIN_WAIT_MS = 5000;

        /// Wakes every waiter parked on the mailbox word. Lock-free: one kernel call, safe on the callback path.
        void wake_waiters() noexcept
        {
            WakeByAddressAll(&s_slot);
        }

        /**
         * @brief Frame hook body. Runs once per frame at the entry of the update step, on the game thread.
         * @details Idle cost is one atomic load per frame. With a job posted it claims the slot, runs the job inline
         *          (the whole apply executes here, inside the frame, before the engine's own scene-graph work for that
         *          frame), publishes Done and wakes the poster. Logging is left to the poster so the callback never
         *          formats or blocks.
         */
        void on_frame_update(DMK::hook::MidContext & /*ctx*/) noexcept
        {
            if (s_game_thread_id.load(std::memory_order_relaxed) == 0)
                s_game_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);

            if (s_slot.load(std::memory_order_acquire) != Slot::Posted)
                return;

            // The poster may have withdrawn the job between the load above and this exchange. A failed exchange means
            // exactly that, and the frame moves on.
            Slot expected = Slot::Posted;
            if (!s_slot.compare_exchange_strong(expected, Slot::Running, std::memory_order_acq_rel))
                return;

            const JobFn job = s_job_fn;
            void *const context = s_job_context;
            if (job != nullptr)
                job(context);
            s_jobs_run.fetch_add(1, std::memory_order_relaxed);

            s_slot.store(Slot::Done, std::memory_order_release);
            wake_waiters();
        }

        /**
         * @brief Withdraws an unclaimed job.
         * @return true when the slot moved Posted -> Empty, false when the hook already claimed it.
         */
        [[nodiscard]] bool withdraw_unclaimed() noexcept
        {
            Slot expected = Slot::Posted;
            return s_slot.compare_exchange_strong(expected, Slot::Empty, std::memory_order_acq_rel);
        }

        /**
         * @brief Blocks until the mailbox leaves @p from, or until @p timeout_ms passes.
         * @param from The state the caller has observed and waits to see change.
         * @param timeout_ms Bound in milliseconds, or INFINITE.
         * @param stop_on_shutdown When true, a shutdown that stopped accepting jobs ends the wait as well.
         * @return false only on timeout with the mailbox still in @p from.
         * @details The state is re-read before every wait and the wait itself compares the word again, so a wake
         *          that lands between the read and the wait is never lost, and a spurious wake costs one loop pass.
         */
        bool wait_for_slot_change(Slot from, std::uint32_t timeout_ms, bool stop_on_shutdown) noexcept
        {
            const bool bounded = timeout_ms != INFINITE;
            const std::uint64_t deadline = bounded ? GetTickCount64() + timeout_ms : 0;
            for (;;)
            {
                if (s_slot.load(std::memory_order_acquire) != from)
                    return true;
                if (stop_on_shutdown && !s_accepting.load(std::memory_order_acquire))
                    return true;
                DWORD wait = INFINITE;
                if (bounded)
                {
                    const std::uint64_t now = GetTickCount64();
                    if (now >= deadline)
                        return false;
                    wait = static_cast<DWORD>(deadline - now);
                }
                Slot compare = from;
                WaitOnAddress(&s_slot, &compare, sizeof(compare), wait);
            }
        }
    } // namespace

    bool install(DMK::hook::HookStack &hooks) noexcept
    {
        if (s_install_attempted.exchange(true, std::memory_order_acq_rel))
            return s_installed.load(std::memory_order_acquire);

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
            s_installed.store(true, std::memory_order_release);
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
        return s_installed.load(std::memory_order_acquire) && s_accepting.load(std::memory_order_acquire);
    }

    bool is_game_thread() noexcept
    {
        const std::uint32_t id = s_game_thread_id.load(std::memory_order_acquire);
        return id != 0 && id == GetCurrentThreadId();
    }

    RunResult run_blocking(JobFn job, void *context, std::uint32_t timeout_ms) noexcept
    {
        if (job == nullptr)
            return RunResult::Ran;
        if (!s_installed.load(std::memory_order_acquire))
            return RunResult::Unavailable;
        if (is_game_thread())
        {
            job(context);
            return RunResult::Ran;
        }

        // Every step below is an atomic operation, a kernel wait, or the lock guard, which the noexcept rule exempts.
        std::lock_guard<std::mutex> post_lk(s_post_mtx);
        if (!s_accepting.load(std::memory_order_acquire))
            return RunResult::Shutdown;

        s_job_fn = job;
        s_job_context = context;
        s_slot.store(Slot::Posted, std::memory_order_release);

        // Phase 1: wait for a frame to claim the job. Bounded, because no frame may be running. A shutdown ends it too.
        const bool claimed = wait_for_slot_change(Slot::Posted, timeout_ms, /*stop_on_shutdown=*/true);
        if (!claimed || s_slot.load(std::memory_order_acquire) == Slot::Posted)
        {
            if (withdraw_unclaimed())
                return s_accepting.load(std::memory_order_acquire) ? RunResult::Timeout : RunResult::Shutdown;
            // The hook claimed it between the check and the withdrawal: wait for completion like any claimed job.
        }

        // Phase 2: claimed. The game thread is executing the job. Unbounded on purpose: returning early would let the
        // caller proceed while its job still runs.
        wait_for_slot_change(Slot::Running, INFINITE, /*stop_on_shutdown=*/false);
        s_slot.store(Slot::Empty, std::memory_order_release);

        if (!s_first_job_logged.exchange(true, std::memory_order_acq_rel))
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "[game-thread] first job ran on thread {}",
                s_game_thread_id.load(std::memory_order_acquire)
            );
        }
        return RunResult::Ran;
    }

    void shutdown() noexcept
    {
        s_accepting.store(false, std::memory_order_release);
        if (!s_installed.load(std::memory_order_acquire))
            return;

        // Wake a poster blocked on an unclaimed job. It withdraws the job and reports Shutdown.
        wake_waiters();

        // A job the hook already claimed is executing on the game thread. Wait for it, bounded.
        const bool drained =
            wait_for_slot_change(Slot::Running, SHUTDOWN_DRAIN_WAIT_MS, /*stop_on_shutdown=*/false);
        auto &log = DMK::log();
        if (!drained)
        {
            (void)log.try_log(
                DMK::LogLevel::Warning,
                "[game-thread] a job was still running on the game thread after {} ms; continuing shutdown",
                SHUTDOWN_DRAIN_WAIT_MS
            );
        }
        (void)log.try_log(
            DMK::LogLevel::Info,
            "[game-thread] shutdown: {} job(s) ran on the game thread",
            s_jobs_run.load(std::memory_order_relaxed)
        );
    }
} // namespace Transmog::game_thread
