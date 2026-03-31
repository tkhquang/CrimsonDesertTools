/// Logic DLL entry point for hot-reload dev builds.
/// Exports Init() and Shutdown() for the loader stub to call.

#include "equip_hide.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

// =========================================================================
// Exported lifecycle functions
// =========================================================================
extern "C" __declspec(dllexport) bool Init()
{
    DMK::Logger::configure("EquipHide", EquipHide::LOG_FILE, "%Y-%m-%d %H:%M:%S");
    auto &logger = DMK::Logger::get_instance();

    DMK::AsyncLoggerConfig asyncCfg;
    asyncCfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;
    logger.enable_async_mode(asyncCfg);

    logger.info("[DEV] Logic DLL Init() called");

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
}

// =========================================================================
// DllMain — minimal, no thread spawning
// =========================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hModule);

    return TRUE;
}
