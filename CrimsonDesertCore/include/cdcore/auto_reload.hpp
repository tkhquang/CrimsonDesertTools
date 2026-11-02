#ifndef CDCORE_AUTO_RELOAD_HPP
#define CDCORE_AUTO_RELOAD_HPP

#include <chrono>
#include <functional>

namespace CDCore
{
    /**
     * @brief Starts the INI auto-reload watcher and retries a failed start in the background.
     * @details The first attempt runs on the calling thread. A failed attempt starts one worker that retries until the
     *          watcher starts or stop_auto_reload() runs, because the watcher thread can miss its startup handshake
     *          while the game is still loading. The delay grows to a fixed ceiling, which bounds the retry rate.
     *          Call after the INI load, outside the loader lock.
     * @param debounce Passed to DMK::config::enable_auto_reload on every attempt.
     * @param on_reload Passed to DMK::config::enable_auto_reload on every attempt.
     */
    void start_auto_reload(std::chrono::milliseconds debounce, std::function<void(bool)> on_reload);

    /**
     * @brief Stops the retry worker, then the watcher.
     * @details Replaces DMK::config::disable_auto_reload at shutdown. A retry that is mid-attempt must finish first, or
     *          it can start a watcher after the disable. The join can therefore wait out one handshake timeout.
     */
    void stop_auto_reload() noexcept;

} // namespace CDCore

#endif // CDCORE_AUTO_RELOAD_HPP
