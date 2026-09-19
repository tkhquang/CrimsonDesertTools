#ifndef TRANSMOG_CONSTANTS_HPP
#define TRANSMOG_CONSTANTS_HPP

#include "version.hpp"

namespace Transmog
{
    inline constexpr const char *MOD_VERSION = TRANSMOG_VERSION_STRING;
    inline constexpr const char *MOD_NAME = "CrimsonDesertLiveTransmog";
    inline constexpr const char *MOD_AUTHOR = "tkhquang";
    inline constexpr const char *MOD_SOURCE = "https://github.com/tkhquang/CrimsonDesertTools";
    inline constexpr const char *LOG_FILE = "CrimsonDesertLiveTransmog.log";
    inline constexpr const char *INI_FILE = "CrimsonDesertLiveTransmog.ini";
    inline constexpr const char *PRESETS_FILE = "CrimsonDesertLiveTransmog_presets.json";
    inline constexpr const char *DISPLAY_NAMES_FILE = "CrimsonDesertLiveTransmog_display_names.tsv";
    inline constexpr const char *ITEM_LOCALIZATION_FILE = "CrimsonDesertLiveTransmog_item_localization.pack";
    inline constexpr const char *INTERFACE_TRANSLATIONS_FILE = "CrimsonDesertLiveTransmog_interface_translations.json";
    inline constexpr const char *INSTANCE_MUTEX_PREFIX = "CrimsonDesertLiveTransmog_";

    /// The loader refuses to attach unless the host process carries this image name.
    inline constexpr const char *GAME_PROCESS_NAME = "CrimsonDesert.exe";

} // namespace Transmog

#endif // TRANSMOG_CONSTANTS_HPP
