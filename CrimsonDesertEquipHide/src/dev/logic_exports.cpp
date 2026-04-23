/**
 * @file logic_exports.cpp
 * @brief Logic DLL entry point for hot-reload dev builds.
 *
 * Only compiled when EQUIPHIDE_DEV_BUILD is set by the dev preset.
 * Includes use explicit `../` relative paths so IntelliSense resolves
 * them even when parsing the file standalone (i.e. when the current
 * CMake-Tools preset is prod and this file has no compile_commands entry).
 */

#include "../equip_hide.hpp"
#include "../constants.hpp"
#include "../version.hpp"

#include <cdcore/dev_helpers.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

static HANDLE g_instanceMutex = nullptr;

extern "C" __declspec(dllexport) bool Init()
{
    // Process gate -- UAL loads ASIs into ALL processes in the game
    // directory, including crashpad_handler.exe. Bail immediately if
    // we're not in the real game.
    if (!CDCore::Dev::is_target_process(EquipHide::GAME_PROCESS_NAME))
        return false;

    DMK::Logger::configure("EquipHide", EquipHide::LOG_FILE, "%Y-%m-%d %H:%M:%S");
    auto &logger = DMK::Logger::get_instance();

    DMK::AsyncLoggerConfig asyncCfg;
    asyncCfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;
    logger.enable_async_mode(asyncCfg);

    logger.info("[DEV] Logic DLL Init() called");
    EquipHide::Version::logVersionInfo();

    // Per-PID mutex -- prevents duplicate ASI loading within the same
    // process (e.g. old production ASI alongside the dev build).
    if (!CDCore::Dev::acquire_instance_mutex(
            EquipHide::INSTANCE_MUTEX_PREFIX, g_instanceMutex))
    {
        logger.error("Another instance of CrimsonDesertEquipHide is "
                     "already loaded. Check for duplicate .asi files.");
        return false;
    }

    if (!EquipHide::init())
    {
        logger.error("Equip hide initialization FAILED.");
        return false;
    }

    logger.info("Equip hide initialization complete.");
    return true;
}

extern "C" __declspec(dllexport) void Shutdown()
{
    auto &logger = DMK::Logger::get_instance();
    logger.info("[DEV] Logic DLL Shutdown() called");

    EquipHide::shutdown();

    CDCore::Dev::release_instance_mutex(g_instanceMutex);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hModule);

    return TRUE;
}
