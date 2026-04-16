#ifndef TRANSMOG_DEV_BUILD

#include "constants.hpp"
#include "overlay.hpp"
#include "transmog.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <cstring>

static HANDLE g_shutdownEvent = nullptr;
static HANDLE g_instanceMutex = nullptr;
static HANDLE g_lifecycleThread = nullptr;
static HMODULE g_hModule = nullptr;

static DWORD WINAPI lifecycle_thread(LPVOID /*param*/)
{
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        const char *exeName = std::strrchr(exePath, '\\');
        exeName = exeName ? exeName + 1 : exePath;
        if (_stricmp(exeName, Transmog::GAME_PROCESS_NAME) != 0)
            return 0;
    }

    DMK::Logger::configure("Transmog", Transmog::LOG_FILE, "%Y-%m-%d %H:%M:%S");
    auto &logger = DMK::Logger::get_instance();

    DMK::AsyncLoggerConfig asyncCfg;
    asyncCfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;
    logger.enable_async_mode(asyncCfg);

    std::wstring runtimeDirW = DMK::Filesystem::get_runtime_directory();
    std::string runtimeDir;
    if (!runtimeDirW.empty())
    {
        int needed = WideCharToMultiByte(
            CP_UTF8, 0,
            runtimeDirW.data(), static_cast<int>(runtimeDirW.size()),
            nullptr, 0, nullptr, nullptr);
        if (needed > 0)
        {
            runtimeDir.resize(static_cast<std::size_t>(needed));
            WideCharToMultiByte(
                CP_UTF8, 0,
                runtimeDirW.data(), static_cast<int>(runtimeDirW.size()),
                runtimeDir.data(), needed, nullptr, nullptr);
        }
    }
    logger.info("DLL loaded, runtime dir: {}", runtimeDir);

    wchar_t mutexName[64];
    wsprintfW(mutexName, L"%s%lu", Transmog::INSTANCE_MUTEX_PREFIX, GetCurrentProcessId());
    g_instanceMutex = CreateMutexW(nullptr, FALSE, mutexName);
    if (g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        logger.error("Another instance of CrimsonDesertLiveTransmog is already loaded. "
                     "Check for duplicate .asi files in the game directory.");
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
        return 1;
    }

    if (!Transmog::init())
    {
        logger.error("Transmog initialization FAILED. Mod will not function.");
        return 1;
    }

    // Overlay is best-effort; mod still works via hotkeys if it fails.
    (void)Transmog::init_overlay(g_hModule);

    logger.info("Transmog initialization complete.");

    WaitForSingleObject(g_shutdownEvent, INFINITE);
    Transmog::shutdown_overlay();
    Transmog::shutdown();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        g_hModule = hModule;

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

#endif // !TRANSMOG_DEV_BUILD
