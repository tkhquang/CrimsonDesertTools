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

extern "C" __declspec(dllexport) bool Init()
{
    DMK::Logger::configure("Transmog", Transmog::LOG_FILE, "%Y-%m-%d %H:%M:%S");
    auto &logger = DMK::Logger::get_instance();

    DMK::AsyncLoggerConfig asyncCfg;
    asyncCfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;
    logger.enable_async_mode(asyncCfg);

    logger.info("[DEV] Logic DLL Init() called");

    if (!Transmog::init())
    {
        logger.error("Transmog initialization FAILED.");
        return false;
    }

    if (Transmog::init_overlay(g_hModule))
        logger.info("ReShade overlay registered — open ReShade (Home key) for Transmog UI");
    else
        logger.info("ReShade not detected — overlay unavailable (mod still works via hotkeys)");

    logger.info("Transmog initialization complete.");
    return true;
}

extern "C" __declspec(dllexport) void Shutdown()
{
    auto &logger = DMK::Logger::get_instance();
    logger.info("[DEV] Logic DLL Shutdown() called");

    Transmog::shutdown_overlay();
    Transmog::shutdown();
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
