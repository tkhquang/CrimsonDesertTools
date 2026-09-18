#ifndef EQUIPHIDE_VERSION_HPP
#define EQUIPHIDE_VERSION_HPP

#include <string>

// Bump these three values to release a new version. Nothing else in this file needs an edit.
#define VERSION_MAJOR 0
#define VERSION_MINOR 9
#define VERSION_PATCH 1

// Stringification helpers. Nothing below this line carries a version value.
#define VERSION_STRINGIFY_IMPL(x) #x
#define VERSION_STRINGIFY(x) VERSION_STRINGIFY_IMPL(x)

#define VERSION_STRING                                                                                                 \
    VERSION_STRINGIFY(VERSION_MAJOR)                                                                                   \
    "." VERSION_STRINGIFY(VERSION_MINOR) "." VERSION_STRINGIFY(VERSION_PATCH)

#define VERSION_TAG "v" VERSION_STRING

namespace EquipHide::Version
{
    /** @brief Numeric version components, taken from the three macros above. */
    inline constexpr int MAJOR = VERSION_MAJOR;
    inline constexpr int MINOR = VERSION_MINOR;
    inline constexpr int PATCH = VERSION_PATCH;

    /** @brief Full version string, e.g. "0.5.2". */
    inline constexpr const char *VERSION_STR = VERSION_STRING;

    /** @brief Version tag for filenames / GitHub releases, e.g. "v0.5.2". */
    inline constexpr const char *TAG = VERSION_TAG;

    /** @brief Semantic-versioning string (currently an alias for VERSION_STR). */
    inline constexpr const char *SEMVER = VERSION_STRING;

    /// The __DATE__ stamp of this build.
    inline constexpr const char *BUILD_DATE = __DATE__;
    /// The __TIME__ stamp of this build.
    inline constexpr const char *BUILD_TIME = __TIME__;

    /** @brief Project identity strings, fixed for the lifetime of the mod. */
    inline constexpr const char *MOD_NAME = "CrimsonDesertEquipHide";
    inline constexpr const char *AUTHOR = "tkhquang";
    inline constexpr const char *REPOSITORY = "https://github.com/tkhquang/CrimsonDesertTools";
    inline constexpr const char *NEXUS_URL = "https://www.nexusmods.com/crimsondesert/mods/554";

    /** @brief GitHub release URL matching this version. */
    inline constexpr const char *RELEASE_URL =
        "https://github.com/tkhquang/CrimsonDesertTools/releases/tag/equip-hide/" VERSION_TAG;

    /** @brief Expected artifact filename, e.g. "CrimsonDesertEquipHide_v0.5.2.zip". */
    [[nodiscard]] inline std::string get_artifact_name()
    {
        return std::string(MOD_NAME) + "_" + TAG + ".zip";
    }

    /**
     * @brief Logs mod identity, version, and build timestamp through DMK::log(). Requires the logger to already be
     *        configured.
     */
    void log_version_info();

} // namespace EquipHide::Version

#endif // EQUIPHIDE_VERSION_HPP
