#ifndef TRANSMOG_VERSION_HPP
#define TRANSMOG_VERSION_HPP

#include <string>

// Bump these three values to release a new version. Nothing else in this file needs an edit.
#define TRANSMOG_VERSION_MAJOR 0
#define TRANSMOG_VERSION_MINOR 15
#define TRANSMOG_VERSION_PATCH 1

// Stringification helpers. Nothing below this line carries a version value.
#define TRANSMOG_VERSION_STRINGIFY_IMPL(x) #x
#define TRANSMOG_VERSION_STRINGIFY(x) TRANSMOG_VERSION_STRINGIFY_IMPL(x)

#define TRANSMOG_VERSION_STRING                                                                                        \
    TRANSMOG_VERSION_STRINGIFY(TRANSMOG_VERSION_MAJOR)                                                                 \
    "." TRANSMOG_VERSION_STRINGIFY(TRANSMOG_VERSION_MINOR) "." TRANSMOG_VERSION_STRINGIFY(TRANSMOG_VERSION_PATCH)

#define TRANSMOG_VERSION_TAG "v" TRANSMOG_VERSION_STRING

namespace Transmog::version
{
    /// Major version component.
    inline constexpr int MAJOR = TRANSMOG_VERSION_MAJOR;
    /// Minor version component.
    inline constexpr int MINOR = TRANSMOG_VERSION_MINOR;
    /// Patch version component.
    inline constexpr int PATCH = TRANSMOG_VERSION_PATCH;

    /** @brief Full version string, e.g. "0.5.0". */
    inline constexpr const char *VERSION_STR = TRANSMOG_VERSION_STRING;

    /** @brief version tag for filenames / GitHub releases, e.g. "v0.5.0". */
    inline constexpr const char *TAG = TRANSMOG_VERSION_TAG;

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
        "https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/" TRANSMOG_VERSION_TAG;

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

} // namespace Transmog::version

#endif // TRANSMOG_VERSION_HPP
