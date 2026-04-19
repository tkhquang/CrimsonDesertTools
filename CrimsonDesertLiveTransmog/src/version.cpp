#include "version.hpp"

#include <DetourModKit.hpp>

namespace Transmog::Version
{
    void logVersionInfo()
    {
        auto &logger = DMK::Logger::get_instance();
        logger.info("----------------------------------------------------------");
        logger.info("{} {} ({} {})", MOD_NAME, TAG, BUILD_DATE, BUILD_TIME);
        logger.info("Author: {}", AUTHOR);
        logger.info("Source: {}", REPOSITORY);
        logger.info("Nexus:  {}", NEXUS_URL);
        logger.info("----------------------------------------------------------");
    }

} // namespace Transmog::Version
