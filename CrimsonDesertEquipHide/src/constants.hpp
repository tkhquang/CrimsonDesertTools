#ifndef EQUIPHIDE_CONSTANTS_HPP
#define EQUIPHIDE_CONSTANTS_HPP

#include "version.hpp"

namespace EquipHide
{
    /// Full mod version string, e.g. "0.9.1".
    inline constexpr const char *MOD_VERSION = VERSION_STRING;

    /// Mod name the session banner, the log file header and DMK::ModInfo carry.
    inline constexpr const char *MOD_NAME = Version::MOD_NAME;

    /// Mod author credited in the release metadata.
    inline constexpr const char *MOD_AUTHOR = Version::AUTHOR;

    /// Source repository that hosts the mod.
    inline constexpr const char *MOD_SOURCE = Version::REPOSITORY;

    /// Nexus Mods page for the mod.
    inline constexpr const char *MOD_NEXUS = Version::NEXUS_URL;

    /// Log file name DMK::ModInfo opens next to the DLL.
    inline constexpr const char *LOG_FILE = "CrimsonDesertEquipHide.log";

    /// Settings file the session loads through DMK::ini().
    inline constexpr const char *INI_FILE = "CrimsonDesertEquipHide.ini";

    /// Prefix of the named mutex that keeps one mod instance per process.
    inline constexpr const char *INSTANCE_MUTEX_PREFIX = "CrimsonDesertEquipHide_";

    /// Process name the mod refuses to load outside of.
    inline constexpr const char *GAME_PROCESS_NAME = "CrimsonDesert.exe";

} // namespace EquipHide

#endif // EQUIPHIDE_CONSTANTS_HPP
