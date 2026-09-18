#ifndef TRANSMOG_SOCKET_MESH_OVERRIDE_HPP
#define TRANSMOG_SOCKET_MESH_OVERRIDE_HPP

#include <DetourModKit/hook.hpp>

/**
 * @file socket_mesh_override.hpp
 * @brief Replaces the mesh a socket is about to wear, rather than removes it afterwards.
 *
 * A real equip change is VISIBLE before LT reacts: both equip hooks call the engine first and only then schedule a
 * debounced apply, so the real mesh is built, drawn, and removed a debounce later.
 *
 * A suppression at the reconciler changes nothing: `SlotPopulator` writes the new item into the
 * slot array before any of that runs, and the realize rebuilds from the slot state. The mesh choice happens
 * further in, in `PartDescriptorBuild`: it expands the part to mesh ids, takes each one's canonical wrapper into
 * the FIRST field of a 112-byte descriptor, and appends that descriptor to the rebuild request.
 *
 * That field is the socket's mesh. This hook rewrites it to LT's target for the slot, so the transmog mesh is what
 * gets attached in the first place. Nothing flashes because the real mesh is never built.
 *
 * Why here and not at the append itself: LT already hooks the append (the StructCopy hook), but at that depth the
 * slot is unknown, so substitution there can only be keyed by SOURCE mesh - which is why a carrier is matched and
 * an arbitrary real item is not. `PartDescriptorBuild` takes the slot tag as an argument, so the override can be
 * keyed by SOCKET and works for any incoming item without registering its meshes first.
 */

namespace Transmog::socket_mesh_override
{
    /**
     * @brief Resolve and hook PartDescriptorBuild.
     * @return true when the hook is installed.
     * @note Safe to call more than once; later calls are no-ops.
     */
    [[nodiscard]] bool install(DetourModKit::hook::HookStack &hooks) noexcept;

    /// Descriptors rewritten so far. Diagnostic.
    [[nodiscard]] unsigned overridden_count() noexcept;
} // namespace Transmog::socket_mesh_override

#endif // TRANSMOG_SOCKET_MESH_OVERRIDE_HPP
