// body_variant_hook.hpp
#ifndef TRANSMOG_BODY_VARIANT_HOOK_HPP
#define TRANSMOG_BODY_VARIANT_HOOK_HPP

namespace Transmog::BodyVariantHook
{
    /**
     * @brief Install the per-body mesh resolver hook and its render observer.
     *
     * The engine's variant resolver (sub_141F90630) renders the item variant entry whose token list matches the
     * wearer's body token, or nothing if none match. This hook wraps it so that, during an LT transmog apply, the
     * natural per-body match runs (giving each character its correct cd_ph[mw]_* mesh); only when nothing matched -- an
     * NPC/cross-class item that would otherwise be invisible -- does it re-run the resolver with the char-class bypass
     * to force the first (entry[0]) variant. Real equips are left untouched. This replaces LT's former transmog-side
     * bypass toggling, which always forced entry[0] and so mis-rendered every wearer's mesh.
     *
     * @return true if both the resolver inline hook and the render observer midhook installed.
     */
    [[nodiscard]] bool install();

    /**
     * @brief Whether both hooks are live, i.e. install() completed successfully.
     *
     * False when either the resolver or render AOB failed to resolve, or either hook failed to install. The transmog
     * apply path reads this to decide its fallback: with the hook down it forces the char-class bypass for the whole
     * apply window (the pre-hook behavior), so an unresolved hook degrades to visible-but-default-mesh rendering
     * instead of blocking the transmog outright.
     *
     * @return true once install() has succeeded; false otherwise.
     */
    [[nodiscard]] bool is_active() noexcept;
} // namespace Transmog::BodyVariantHook

#endif // TRANSMOG_BODY_VARIANT_HOOK_HPP
