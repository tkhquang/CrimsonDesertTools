#ifndef TRANSMOG_DEV_BUILD

#include "constants.hpp"
#include "overlay.hpp"
#include "transmog.hpp"
#include "version.hpp"

#include <DetourModKit.hpp>
#include <DetourModKit/bootstrap.hpp>
#include <DetourModKit/filesystem.hpp>

#include <Windows.h>

namespace
{
    // Module handle captured at attach. The lifecycle worker spawned by
    // Bootstrap::on_dll_attach runs init_fn off the loader lock; the
    // overlay needs the host module handle to register a Win32 hook
    // class, so we cache it here.
    HMODULE s_hModule = nullptr;

    // Init body invoked on the worker thread (off loader lock).
    bool init_mod()
    {
        auto &logger = DetourModKit::Logger::get_instance();
        Transmog::Version::logVersionInfo();

        const auto runtimeDir =
            DetourModKit::Filesystem::get_runtime_directory_utf8();
        logger.info("DLL loaded, runtime dir: {}", runtimeDir);

        if (!Transmog::init())
        {
            logger.error("Transmog initialization FAILED. Mod will not function.");
            return false;
        }

        // Overlay is best-effort; mod still works via hotkeys if it fails.
        (void)Transmog::init_overlay(s_hModule);

        logger.info("Transmog initialization complete.");
        return true;
    }

    void shutdown_mod()
    {
        Transmog::shutdown_overlay();
        Transmog::shutdown();
    }
} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        s_hModule = hModule;

        DetourModKit::AsyncLoggerConfig asyncCfg;
        asyncCfg.overflow_policy = DetourModKit::OverflowPolicy::SyncFallback;

        const DetourModKit::Bootstrap::ModInfo info{
            "Transmog",
            Transmog::LOG_FILE,
            Transmog::GAME_PROCESS_NAME,
            "CrimsonDesertLiveTransmog_",
            asyncCfg,
        };

        return DetourModKit::Bootstrap::on_dll_attach(
            hModule, info, &init_mod, &shutdown_mod);
    }

    case DLL_PROCESS_DETACH:
        DetourModKit::Bootstrap::on_dll_detach(lpReserved != nullptr);
        break;

    default:
        break;
    }

    return TRUE;
}

#endif // !TRANSMOG_DEV_BUILD
