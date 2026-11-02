#include <cdcore/auto_reload.hpp>

#include <DetourModKit/config.hpp>
#include <DetourModKit/detail/worker.hpp>
#include <DetourModKit/logger.hpp>

#include <algorithm>
#include <memory>
#include <stop_token>
#include <thread>
#include <utility>

namespace CDCore
{
    namespace
    {
        namespace DMK = DetourModKit;

        constexpr std::chrono::milliseconds FIRST_RETRY_DELAY{5000};
        constexpr std::chrono::milliseconds MAX_RETRY_DELAY{60000};
        constexpr std::chrono::milliseconds STOP_POLL_SLICE{100};

        std::unique_ptr<DMK::StoppableWorker> s_retry_worker;

        [[nodiscard]] bool attempt_succeeded(DMK::config::AutoReloadStatus status) noexcept
        {
            return status == DMK::config::AutoReloadStatus::Started ||
                   status == DMK::config::AutoReloadStatus::AlreadyRunning;
        }

        /// Returns false when a stop arrives before @p delay elapses.
        [[nodiscard]] bool wait_for_retry(const std::stop_token &stop, std::chrono::milliseconds delay)
        {
            for (auto remaining = delay; remaining.count() > 0; remaining -= STOP_POLL_SLICE)
            {
                if (stop.stop_requested())
                    return false;
                std::this_thread::sleep_for(std::min(remaining, STOP_POLL_SLICE));
            }
            return !stop.stop_requested();
        }

        void retry_body(
            const std::stop_token &stop,
            std::chrono::milliseconds debounce,
            const std::function<void(bool)> &on_reload
        )
        {
            auto delay = FIRST_RETRY_DELAY;
            for (int attempt = 1; wait_for_retry(stop, delay); ++attempt)
            {
                const auto status = DMK::config::enable_auto_reload(debounce, on_reload);
                if (attempt_succeeded(status))
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Info, "INI auto-reload started on retry {}", attempt);
                    return;
                }
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "INI auto-reload retry {} failed (status enum {})",
                    attempt,
                    static_cast<int>(status)
                );
                delay = std::min(delay * 2, MAX_RETRY_DELAY);
            }
        }
    } // namespace

    void start_auto_reload(std::chrono::milliseconds debounce, std::function<void(bool)> on_reload)
    {
        const auto status = DMK::config::enable_auto_reload(debounce, on_reload);
        if (attempt_succeeded(status))
            return;

        DMK::log().warning("INI auto-reload could not start (status enum {})", static_cast<int>(status));

        // NoPriorLoad means the caller skipped the INI load, and another attempt cannot change that.
        if (status != DMK::config::AutoReloadStatus::StartFailed)
            return;

        s_retry_worker = std::make_unique<DMK::StoppableWorker>(
            "CD_AutoReloadRetry",
            [debounce, on_reload = std::move(on_reload)](std::stop_token stop)
            { retry_body(stop, debounce, on_reload); }
        );
    }

    void stop_auto_reload() noexcept
    {
        // ~StoppableWorker requests stop and joins, so the reset is the join.
        s_retry_worker.reset();
        DMK::config::disable_auto_reload();
    }

} // namespace CDCore
