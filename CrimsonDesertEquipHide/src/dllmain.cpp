#ifndef EQUIPHIDE_DEV_BUILD

#include "constants.hpp"
#include "equip_hide.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <cstring>

static HANDLE g_shutdownEvent = nullptr;
static HANDLE g_instanceMutex = nullptr;

/// Mod lifecycle thread — runs init, waits for shutdown signal, then tears down
/// outside the loader lock (joining threads from DllMain would deadlock).
static DWORD WINAPI lifecycle_thread(LPVOID /*param*/)
{
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        const char *exeName = std::strrchr(exePath, '\\');
        exeName = exeName ? exeName + 1 : exePath;
        if (_stricmp(exeName, EquipHide::GAME_PROCESS_NAME) != 0)
            return 0;
    }

    DMK::Logger::configure("EquipHide", EquipHide::LOG_FILE, "%Y-%m-%d %H:%M:%S");
    auto &logger = DMK::Logger::get_instance();

    DMK::AsyncLoggerConfig asyncCfg;
    asyncCfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;
    logger.enable_async_mode(asyncCfg);

    logger.info("DLL loaded, runtime dir: {}", DMK::Filesystem::get_runtime_directory());

    wchar_t mutexName[64];
    wsprintfW(mutexName, L"%s%lu", EquipHide::INSTANCE_MUTEX_PREFIX, GetCurrentProcessId());
    g_instanceMutex = CreateMutexW(nullptr, FALSE, mutexName);
    if (g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        logger.error("Another instance of CrimsonDesertEquipHide is already loaded. "
                     "Check for duplicate .asi files in the game directory.");
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
        return 1;
    }

    if (!EquipHide::init())
    {
        logger.error("Equip hide initialization FAILED. Mod will not function.");
        return 1;
    }

    logger.info("Equip hide initialization complete.");

    // Block until DLL_PROCESS_DETACH signals us
    WaitForSingleObject(g_shutdownEvent, INFINITE);

    // Shutdown runs here, outside the loader lock
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

        // CreateThread is safe from DllMain (unlike std::thread on some CRTs)
        if (!CreateThread(nullptr, 0, lifecycle_thread, nullptr, 0, nullptr))
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

            // On FreeLibrary (lpReserved == nullptr): give the lifecycle thread
            // time to shut down.  On process exit (lpReserved != nullptr) the OS
            // has already terminated all threads, so the event is never consumed
            // and we skip the wait.
            if (lpReserved == nullptr)
                Sleep(200);

            CloseHandle(g_shutdownEvent);
            g_shutdownEvent = nullptr;
        }
        if (g_instanceMutex)
        {
            CloseHandle(g_instanceMutex);
            g_instanceMutex = nullptr;
        }
        break;

    default:
        break;
    }

    return TRUE;
}

#endif // !EQUIPHIDE_DEV_BUILD
