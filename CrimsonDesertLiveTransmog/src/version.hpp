#ifndef TRANSMOG_VERSION_HPP
#define TRANSMOG_VERSION_HPP

#include <string>

// Bump these three values to release a new version. Nothing else in this file needs an edit.
#define VERSION_MAJOR 0
#define VERSION_MINOR 15
#define VERSION_PATCH 1

// Stringification helpers. Nothing below this line carries a version value.
#define VERSION_STRINGIFY_IMPL(x) #x
#define VERSION_STRINGIFY(x) VERSION_STRINGIFY_IMPL(x)

#define VERSION_STRING                                                                                                 \
    VERSION_STRINGIFY(VERSION_MAJOR)                                                                                   \
    "." VERSION_STRINGIFY(VERSION_MINOR) "." VERSION_STRINGIFY(VERSION_PATCH)

#define VERSION_TAG "v" VERSION_STRING

namespace Transmog::Version
{
    /// Major version component.
    inline constexpr int MAJOR = VERSION_MAJOR;
    /// Minor version component.
    inline constexpr int MINOR = VERSION_MINOR;
    /// Patch version component.
    inline constexpr int PATCH = VERSION_PATCH;

    /** @brief Full version string, e.g. "0.5.0". */
    inline constexpr const char *VERSION_STR = VERSION_STRING;

    /** @brief Version tag for filenames / GitHub releases, e.g. "v0.5.0". */
    inline constexpr const char *TAG = VERSION_TAG;

    /** @brief Semantic-versioning string (currently an alias for VERSION_STR). */
    inline constexpr const char *SEMVER = VERSION_STRING;

    /** @brief Compile-time date ("Apr 19 2026") from __DATE__. */
    inline constexpr const char *BUILD_DATE = __DATE__;
    /** @brief Compile-time HH:MM:SS from __TIME__. */
    inline constexpr const char *BUILD_TIME = __TIME__;

    /// Mod identifier used for log lines, file names, and the deployed asset names.
    inline constexpr const char *MOD_NAME = "CrimsonDesertLiveTransmog";
    /// Mod author.
    inline constexpr const char *AUTHOR = "tkhquang";
    /// Source repository URL.
    inline constexpr const char *REPOSITORY = "https://github.com/tkhquang/CrimsonDesertTools";
    /// Nexus Mods page URL.
    inline constexpr const char *NEXUS_URL = "https://www.nexusmods.com/crimsondesert/mods/1056";

    /** @brief GitHub release URL matching this version. */
    inline constexpr const char *RELEASE_URL =
        "https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/" VERSION_TAG;

    /** @brief Expected artifact filename, e.g. "CrimsonDesertLiveTransmog_v0.5.0.zip". */
    inline std::string get_artifact_name()
    {
        return std::string(MOD_NAME) + "_" + TAG + ".zip";
    }

    /**
     * @brief Logs mod identity, version, and build timestamp via DMK::Logger. Requires the logger to already be
     *        configured.
     */
    void log_version_info();

} // namespace Transmog::Version

#endif // TRANSMOG_VERSION_HPP
