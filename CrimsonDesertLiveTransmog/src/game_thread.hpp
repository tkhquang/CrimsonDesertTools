#ifndef TRANSMOG_GAME_THREAD_HPP
#define TRANSMOG_GAME_THREAD_HPP

#include <DetourModKit/hook.hpp>

#include <cstdint>

/**
 * @file game_thread.hpp
 * @brief Runs engine-mutating work on the game's main thread, through a one-slot mailbox drained at the top of each
 *        frame.
 *
 * The engine mutates its scene graph without locks: the appearance-claim vector on an assembly node (data at
 * `node+0x58`, count at `node+0x60`, stride 16, owning pointer at `entry+0x08`), the part lists and the prefab
 * wrappers. It gets away with that because every mutation and every walk over that data runs on the main thread,
 * inside the frame, in an order its own scheduler fixes. An equip or a tear-down issued from any other thread runs
 * the same engine code and performs the same erases, but now they overlap the main thread's walks. The erase nulls
 * an owner slot that is still inside the count, a walker on the main thread loads the slot and dereferences it
 * (`+0x28`, `+0x38`, `+0x88`, no null check), and the process faults. The window is a few microseconds per erase,
 * the walkers are many, and only one walker shape carries a guard (claim_walk_guard.hpp), so the crash is rare,
 * timing-dependent, and reproduces most easily on a preset switch that tears several parts down in one burst.
 *
 * The apply worker exists for debouncing, not for parallelism. This module keeps the worker and moves only the
 * EXECUTION of the apply onto the main thread: the worker posts the apply body as a job and blocks until the frame
 * hook has run it. The hook sits at the entry of the per-frame update step (the FrameUpdate anchor), before that
 * frame's scene-graph work, so the erases the apply performs never overlap a walk. That closes the whole class at
 * once, including walkers a future patch adds, without a per-site guard table.
 *
 * One job at a time, on purpose. The debounce already coalesces requests into one apply, and a queue would only let
 * a stale apply run after a newer one had been requested. The worker is the only poster. Posting is serialized
 * anyway, so a second caller cannot clobber a posted job.
 *
 * Failure modes are explicit. Without the hook (the anchor missed on a new build, or the install failed) the caller
 * learns that from @ref RunResult::Unavailable and decides whether to run inline. A job no frame picks up (no frame
 * runs while the window is minimized or a load hitches) is withdrawn after a timeout and reported as
 * @ref RunResult::Timeout, so the caller re-arms instead of hanging.
 */

namespace Transmog::game_thread
{
    /// Outcome of @ref run_blocking.
    enum class RunResult : std::uint8_t
    {
        /// The job ran to completion on the game thread (or inline, when the caller already was the game thread).
        Ran,
        /// No frame hook is armed. The job did NOT run. The caller decides whether to run it inline.
        Unavailable,
        /// No frame claimed the job within the timeout. The job did NOT run and was withdrawn.
        Timeout,
        /// @ref shutdown ran before a frame claimed the job. The job did NOT run.
        Shutdown,
    };

    /**
     * @brief A job body.
     * @details Runs inside a hook callback on the game thread, so it must not throw, must not block on the thread
     *          that posted it, and must keep engine faults inside its own SEH frames.
     */
    using JobFn = void (*)(void *context) noexcept;

    /**
     * @brief Arms the frame hook at the FrameUpdate anchor.
     * @param hooks The mod's hook stack. It receives the hook, so teardown restores the site with the rest.
     * @return true when the hook is armed. false means the anchor missed or the install failed, and @ref run_blocking
     *         reports @ref RunResult::Unavailable for the whole session.
     * @note Call it after resolve_all_anchors() and before any worker that posts jobs starts. A later call is a no-op.
     * @note Setup/control-plane only: patches code and pushes onto the hook stack. Never call it under the loader
     *       lock.
     */
    [[nodiscard]] bool install(DetourModKit::hook::HookStack &hooks) noexcept;

    /**
     * @brief True while the frame hook is armed and @ref shutdown has not run.
     * @note Callback-safe: two atomic reads.
     */
    [[nodiscard]] bool available() noexcept;

    /**
     * @brief True when the calling thread is the one the frame hook fires on.
     * @details Learned from the hook's first fire, so it reads false until the first frame after @ref install.
     * @note Callback-safe: an atomic read and a thread-id compare.
     */
    [[nodiscard]] bool is_game_thread() noexcept;

    /**
     * @brief Runs @p job on the game thread and blocks until it has finished.
     * @param job The body to run. A null job is a no-op that reports @ref RunResult::Ran.
     * @param context Passed to @p job unchanged.
     * @param timeout_ms Longest wait for a frame to CLAIM the job. Once claimed, the call waits for completion
     *                   without a bound, because the game thread is executing the job and returning early would let
     *                   the caller proceed concurrently with its own work.
     * @return See @ref RunResult. Only @ref RunResult::Ran means the job executed.
     * @details Called from the game thread itself, the job runs inline and the call reports @ref RunResult::Ran, so
     *          any thread can use this without deadlocking the frame.
     * @note Blocks the calling thread. Never call it from a hook callback or while holding a lock the job needs.
     */
    [[nodiscard]] RunResult run_blocking(JobFn job, void *context, std::uint32_t timeout_ms) noexcept;

    /**
     * @brief Stops accepting jobs, withdraws an unclaimed one, and waits (bounded) for a running one to finish.
     * @details Call it after shutdown_requested() is raised and BEFORE the posting workers are joined: a worker
     *          blocked in @ref run_blocking wakes up here and returns, which is what lets its join complete. The hook
     *          itself stays armed until the hook stack is cleared, and nothing is posted after this call.
     * @note Setup/control-plane only.
     */
    void shutdown() noexcept;
} // namespace Transmog::game_thread

#endif // TRANSMOG_GAME_THREAD_HPP
