/**
 * @file logic_exports.cpp
 * @brief Logic DLL entry point for hot-reload dev builds.
 *
 * Only compiled when TRANSMOG_DEV_BUILD is set by the dev preset.
 * Includes use explicit `../` relative paths so IntelliSense resolves
 * them even when parsing the file standalone (i.e. when the current
 * CMake-Tools preset is prod and this file has no compile_commands entry).
 */

#include "../constants.hpp"
#include "../overlay.hpp"
#include "../transmog.hpp"
#include "../version.hpp"

#include <cdcore/dev_helpers.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

static HMODULE g_hModule = nullptr;
static HANDLE g_instanceMutex = nullptr;

extern "C" __declspec(dllexport) bool Init()
{
    // Process gate -- UAL loads ASIs into ALL processes in the game
    // directory, including crashpad_handler.exe. Bail immediately if
    // we're not in the real game.
    if (!CDCore::Dev::is_target_process(Transmog::GAME_PROCESS_NAME))
        return false;

    DMK::Logger::configure("Transmog", Transmog::LOG_FILE, "%Y-%m-%d %H:%M:%S");
    auto &logger = DMK::Logger::get_instance();

    DMK::AsyncLoggerConfig asyncCfg;
    asyncCfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;
    logger.enable_async_mode(asyncCfg);

    logger.info("[DEV] Logic DLL Init() called");
    Transmog::Version::logVersionInfo();

    // Per-PID mutex -- prevents duplicate ASI loading within the same
    // process (e.g. old production ASI alongside the dev build).
    if (!CDCore::Dev::acquire_instance_mutex(
            Transmog::INSTANCE_MUTEX_PREFIX, g_instanceMutex))
    {
        logger.error("Another instance of CrimsonDesertLiveTransmog is "
                     "already loaded. Check for duplicate .asi files.");
        return false;
    }

    if (!Transmog::init())
    {
        logger.error("Transmog initialization FAILED.");
        return false;
    }

    // Overlay is best-effort; mod still works via hotkeys if it fails.
    (void)Transmog::init_overlay(g_hModule);

    logger.info("Transmog initialization complete.");
    return true;
}

extern "C" __declspec(dllexport) void Shutdown()
{
    auto &logger = DMK::Logger::get_instance();
    logger.info("[DEV] Logic DLL Shutdown() called");

    Transmog::shutdown_overlay();
    Transmog::shutdown();

    CDCore::Dev::release_instance_mutex(g_instanceMutex);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_hModule = hModule;
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH)
    {
        // Safety net: if the loader failed to call our exported
        // `Shutdown()` before FreeLibrary (or Shutdown() hung and the
        // loader's post-shutdown sleep elapsed), the hooks we installed
        // would remain patched-in-place while our trampoline memory
        // unmaps. A fresh reload of this logic DLL would then scan the
        // still-patched prologues, install new hooks on top, and every
        // future call through BatchEquip/VEC would chain into freed
        // memory and crash. DMK_Shutdown is idempotent and detects the
        // Windows loader-lock path internally (detaches worker threads
        // instead of joining), so calling it here is safe even when the
        // exported Shutdown() already ran.
        DMK_Shutdown();
    }

    return TRUE;
}
