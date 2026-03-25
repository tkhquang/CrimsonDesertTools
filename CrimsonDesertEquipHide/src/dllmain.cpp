#include "equip_hide.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

static HMODULE g_hModule = nullptr;
static HANDLE g_shutdownEvent = nullptr;

/// Mod lifecycle thread — runs init, waits for shutdown signal, then tears down
/// outside the loader lock (joining threads from DllMain would deadlock).
static DWORD WINAPI lifecycle_thread(LPVOID /*param*/)
{
    DMK::Logger::configure("EquipHide", "CrimsonDesertEquipHide.log", "%Y-%m-%d %H:%M:%S");
    auto& logger = DMK::Logger::get_instance();
    logger.enable_async_mode();

    logger.info("DLL loaded, runtime dir: {}", DMK::Filesystem::get_runtime_directory());

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
        g_hModule = hModule;
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
        break;

    default:
        break;
    }

    return TRUE;
}
