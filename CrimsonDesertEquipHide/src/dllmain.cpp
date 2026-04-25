#ifndef EQUIPHIDE_DEV_BUILD

#include "constants.hpp"
#include "equip_hide.hpp"
#include "version.hpp"

#include <DetourModKit.hpp>
#include <DetourModKit/bootstrap.hpp>
#include <DetourModKit/filesystem.hpp>

#include <Windows.h>

namespace
{
    bool init_mod()
    {
        auto &logger = DetourModKit::Logger::get_instance();
        EquipHide::Version::logVersionInfo();

        const auto runtimeDir =
            DetourModKit::Filesystem::get_runtime_directory_utf8();
        logger.info("DLL loaded, runtime dir: {}", runtimeDir);

        if (!EquipHide::init())
        {
            logger.error("Equip hide initialization FAILED. Mod will not function.");
            return false;
        }

        logger.info("Equip hide initialization complete.");
        return true;
    }

    void shutdown_mod()
    {
        EquipHide::shutdown();
    }
} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DetourModKit::AsyncLoggerConfig asyncCfg;
        asyncCfg.overflow_policy = DetourModKit::OverflowPolicy::SyncFallback;

        const DetourModKit::Bootstrap::ModInfo info{
            "EquipHide",
            EquipHide::LOG_FILE,
            EquipHide::GAME_PROCESS_NAME,
            "CrimsonDesertEquipHide_",
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

#endif // !EQUIPHIDE_DEV_BUILD
