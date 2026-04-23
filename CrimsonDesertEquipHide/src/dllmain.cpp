#ifndef EQUIPHIDE_DEV_BUILD

#include "constants.hpp"
#include "equip_hide.hpp"
#include "version.hpp"

#include <cdcore/dev_helpers.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

static HANDLE g_shutdownEvent = nullptr;
static HANDLE g_instanceMutex = nullptr;
static HANDLE g_lifecycleThread = nullptr;

static DWORD WINAPI lifecycle_thread(LPVOID /*param*/)
{
    // Process gate -- UAL loads ASIs into every process in the game
    // directory including crashpad_handler.exe. Bail immediately.
    if (!CDCore::Dev::is_target_process(EquipHide::GAME_PROCESS_NAME))
        return 0;

    DMK::Logger::configure("EquipHide", EquipHide::LOG_FILE, "%Y-%m-%d %H:%M:%S");
    auto &logger = DMK::Logger::get_instance();

    DMK::AsyncLoggerConfig asyncCfg;
    asyncCfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;
    logger.enable_async_mode(asyncCfg);

    EquipHide::Version::logVersionInfo();

    std::wstring runtimeDirW = DMK::Filesystem::get_runtime_directory();
    std::string runtimeDir(runtimeDirW.begin(), runtimeDirW.end());
    logger.info("DLL loaded, runtime dir: {}", runtimeDir);

    if (!CDCore::Dev::acquire_instance_mutex(
            EquipHide::INSTANCE_MUTEX_PREFIX, g_instanceMutex))
    {
        logger.error("Another instance of CrimsonDesertEquipHide is already loaded. "
                     "Check for duplicate .asi files in the game directory.");
        return 1;
    }

    if (!EquipHide::init())
    {
        logger.error("Equip hide initialization FAILED. Mod will not function.");
        return 1;
    }

    logger.info("Equip hide initialization complete.");

    WaitForSingleObject(g_shutdownEvent, INFINITE);
    EquipHide::shutdown();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);

        g_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_shutdownEvent)
            return FALSE;

        g_lifecycleThread = CreateThread(nullptr, 0, lifecycle_thread, nullptr, 0, nullptr);
        if (!g_lifecycleThread)
        {
            CloseHandle(g_shutdownEvent);
            g_shutdownEvent = nullptr;
            return FALSE;
        }
        break;

    case DLL_PROCESS_DETACH:
        if (g_shutdownEvent)
        {
            SetEvent(g_shutdownEvent);
            if (lpReserved == nullptr && g_lifecycleThread)
                WaitForSingleObject(g_lifecycleThread, 5000);

            CloseHandle(g_shutdownEvent);
            g_shutdownEvent = nullptr;
        }
        if (g_lifecycleThread)
        {
            CloseHandle(g_lifecycleThread);
            g_lifecycleThread = nullptr;
        }
        CDCore::Dev::release_instance_mutex(g_instanceMutex);
        break;

    default:
        break;
    }

    return TRUE;
}

#endif // !EQUIPHIDE_DEV_BUILD
