/**
 * @file logic_exports.cpp
 * @brief Logic DLL entry point for hot-reload dev builds.
 */

#include "constants.hpp"
#include "overlay.hpp"
#include "transmog.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

static HMODULE g_hModule = nullptr;
static HANDLE g_instanceMutex = nullptr;

extern "C" __declspec(dllexport) bool Init()
{
    // Process gate — UAL loads ASIs into ALL processes in the game
    // directory, including crashpad_handler.exe.  Bail immediately
    // if we're not in the real game.
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        const char *exeName = std::strrchr(exePath, '\\');
        exeName = exeName ? exeName + 1 : exePath;
        if (_stricmp(exeName, Transmog::GAME_PROCESS_NAME) != 0)
            return false;
    }

    DMK::Logger::configure("Transmog", Transmog::LOG_FILE, "%Y-%m-%d %H:%M:%S");
    auto &logger = DMK::Logger::get_instance();

    DMK::AsyncLoggerConfig asyncCfg;
    asyncCfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;
    logger.enable_async_mode(asyncCfg);

    logger.info("[DEV] Logic DLL Init() called");

    // Per-PID mutex — prevents duplicate ASI loading within the same
    // process (e.g. old production ASI alongside the dev build).
    wchar_t mutexName[64];
    wsprintfW(mutexName, L"%s%lu",
              Transmog::INSTANCE_MUTEX_PREFIX, GetCurrentProcessId());
    g_instanceMutex = CreateMutexW(nullptr, FALSE, mutexName);
    if (g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        logger.error("Another instance of CrimsonDesertLiveTransmog is "
                     "already loaded. Check for duplicate .asi files.");
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
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

    if (g_instanceMutex)
    {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_hModule = hModule;
    }

    return TRUE;
}
