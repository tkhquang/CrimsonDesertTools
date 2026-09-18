/**
 * @file dllmain.cpp
 * @brief DLL entry point wiring the mod lifecycle to the DetourModKit Session.
 *
 * bootstrap_attach() performs the process and single-instance gates without allocating, then runs on_ready(session)
 * on its own worker thread off the loader lock; ~Session (also on that worker) clears the binding scope and tears the
 * DMK subsystems down in order.
 *
 * DetourModKit does not own the mod's own state. Hooks are caller-owned handles, so EquipHide::shutdown() is the only
 * path that restores the patched prologues, and this file calls it.
 */

#ifndef EQUIPHIDE_DEV_BUILD

#include "constants.hpp"
#include "equip_hide.hpp"
#include "version.hpp"

#include <DetourModKit/async_logger_config.hpp>
#include <DetourModKit/error.hpp>
#include <DetourModKit/filesystem.hpp>
#include <DetourModKit/logger.hpp>
#include <DetourModKit/session.hpp>

#include <Windows.h>

namespace
{
    DMK::Result<void> on_ready(DMK::Session &session)
    {
        auto &logger = session.log();
        EquipHide::version::log_version_info();
        logger.info("DLL loaded, runtime dir: {}", DMK::filesystem::get_runtime_directory_utf8());

        auto ready = EquipHide::init(session);
        if (!ready)
        {
            logger.error("Equip hide initialization FAILED ({}).", ready.error().message());
            return ready;
        }

        logger.info("Equip hide initialization complete.");
        return {};
    }
} // namespace

// The module handle stays unnamed: bootstrap_attach captures the calling module itself, because DetourModKit links
// statically into this DLL and its code address resolves to this HMODULE.
BOOL APIENTRY DllMain(HMODULE, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DMK::AsyncLoggerConfig async_config;
        // Fall back to synchronous logging if the async queue overflows, so no diagnostic line is lost during a burst
        // (startup or teardown).
        async_config.overflow_policy = DMK::OverflowPolicy::SyncFallback;

        const DMK::ModInfo info{
            .name = EquipHide::MOD_NAME,
            .log_file = EquipHide::LOG_FILE,
            .game_process_name = EquipHide::GAME_PROCESS_NAME,
            .instance_mutex_prefix = EquipHide::INSTANCE_MUTEX_PREFIX,
            .log = async_config,
            // Single DLL, one Session per run, so the default Truncate gives one log per game launch. The dev loader
            // needs Append instead, because it keeps every generation's teardown records in one file.
            .log_open_mode = DMK::LogOpenMode::Truncate,
            // No [file:line] stamp in a shipped build: this log is read by players and by whoever handles a bug
            // report, and a source path is noise to both. The dev loader keeps the Trace stamp for debugging.
            .log_source_stamp_mode = DMK::LogSourceStampMode::never(),
        };

        // A gate refusal (wrong process, a duplicate load already holding the mutex) is a reason for this DLL to go
        // away, not for the host to fail, so report it as a failed attach and let the loader unmap us.
        return DMK::bootstrap_attach(info, &on_ready).has_value() ? TRUE : FALSE;
    }

    case DLL_PROCESS_DETACH:
        // lpReserved == NULL is an explicit FreeLibrary. Run the mod teardown so the patched prologues are restored.
        // The attempt is best-effort, because a ~Hook under the loader lock pins the backend rather than leaving a
        // half-restored target. lpReserved != NULL is process exit: the OS has already killed every other thread, so a
        // touch of patched pages there is a UAF and the abandon path inside bootstrap_detach is the correct no-op.
        if (lpReserved == nullptr)
        {
            // The verdict is discarded on purpose. DllMain cannot refuse a FreeLibrary already in progress, and this
            // ASI is loaded once for the process, so no later load exists for a pinned backend to hand a stale image
            // to. shutdown() logs the failure itself.
            (void)EquipHide::shutdown();
        }
        DMK::bootstrap_detach(lpReserved);
        break;

    default:
        break;
    }

    return TRUE;
}

#endif // !EQUIPHIDE_DEV_BUILD
