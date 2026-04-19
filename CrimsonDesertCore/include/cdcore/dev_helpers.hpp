#ifndef CDCORE_DEV_HELPERS_HPP
#define CDCORE_DEV_HELPERS_HPP

// ---------------------------------------------------------------------------
// Shared DLL-lifecycle helpers used by per-mod logic_exports.cpp files in
// dev builds (and by the shared loader in dev/loader_main.cpp).
// ---------------------------------------------------------------------------

#include <Windows.h>

namespace CDCore::Dev
{
    /**
     * @brief Returns true if the current process basename matches @p expected
     *        (case-insensitive).
     *
     * Use this as the first step of any DLL Init() to reject injection into
     * auxiliary processes that live in the game directory — e.g. UAL /
     * ASILoader fan out the ASI into `crashpad_handler.exe` and launcher
     * stubs, which must no-op.
     *
     * Expected name includes the extension, e.g. "CrimsonDesert.exe".
     */
    [[nodiscard]] bool is_target_process(const char *expected) noexcept;

    /**
     * @brief Creates a per-PID named mutex for single-instance gating.
     *
     * Builds the mutex name as `<prefixW><current-PID>` and calls
     * CreateMutexW. On success, @p outHandle holds the mutex handle and
     * the function returns true. If the mutex already exists (a previous
     * ASI already loaded in this process), the handle is closed and the
     * function returns false — the caller should bail out of Init().
     *
     * The caller owns the returned handle and must close it in their
     * Shutdown() path.
     */
    [[nodiscard]] bool acquire_instance_mutex(
        const wchar_t *prefixW, HANDLE &outHandle) noexcept;

    /**
     * @brief Releases a per-instance mutex acquired via
     *        acquire_instance_mutex(). Safe to call on a null handle.
     */
    void release_instance_mutex(HANDLE &mutexHandle) noexcept;

} // namespace CDCore::Dev

#endif // CDCORE_DEV_HELPERS_HPP
