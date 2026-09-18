/**
 * @file protocol.h
 * @brief Fixed-width contract between the resident dev loader and one logic generation.
 *
 * The loader outlives every generation, so everything it hands across the DLL boundary must have a layout both sides
 * agree on with no shared C++ ABI. A plain C struct with an explicit size and version does that: the logic DLL
 * validates both before touching a field, so a stale generation left in the deploy directory fails loudly instead of
 * reading through a shifted layout.
 *
 * Modeled on DetourModKit's checked-in staged_reload example, which its hot-reload guide treats as the reference pair.
 */
#ifndef CDCORE_DEV_PROTOCOL_H
#define CDCORE_DEV_PROTOCOL_H

#include <DetourModKit/abi/wheel_host.h>

#include <stdint.h>

/// Bump whenever the request layout or the export signatures change.
#define CDCORE_RELOAD_ABI_VERSION 1u

/// Success value returned by the logic DLL's Init and Shutdown exports. Zero is a refusal on both.
#define CDCORE_RELOAD_OK 1u

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @struct CdReloadInitRequest
     * @brief What the loader tells a generation at startup.
     */
    typedef struct CdReloadInitRequest
    {
        /// sizeof(CdReloadInitRequest) as the LOADER knows it.
        uint32_t struct_size;
        /// CDCORE_RELOAD_ABI_VERSION as the loader knows it.
        uint32_t abi_version;
        /// Loader-assigned, strictly increasing, never zero.
        uint64_t generation_id;
        /// The identity the logic DLL must find in wheel_host, so a foreign table is rejected.
        uint64_t expected_host_identity;
        /// Process-lifetime wheel host owned by the loader, or NULL. Valid for the whole process when present.
        const WheelHostTable *wheel_host;
    } CdReloadInitRequest;

    // Exports the loader resolves by name on every generation.
#define CDCORE_RELOAD_INIT_SYMBOL "Init"
#define CDCORE_RELOAD_SHUTDOWN_SYMBOL "Shutdown"
#define CDCORE_RELOAD_REVISION_SYMBOL "Revision"

#ifdef __cplusplus
}
#endif

#endif // CDCORE_DEV_PROTOCOL_H
