#pragma once

/**
 * @file helm_audio_filter.hpp
 * @brief PlateHelm voice-muffle suppression at the passive-skill registration boundary.
 *
 * The engine's helm-muffle path runs through this chain:
 *
 *   iteminfo[idx].equip_passive_skill_list      // {skill_id, level} per item
 *       |
 *       v
 *   per-skill iteminfo audio-classifier vector  // 8-byte stride
 *                                               // {u16 tag, u16 0, u16 lvl, u16 0}
 *       |
 *       v
 *   per-tag passive-skill registrar             // THIS HOOK target. Per iteration it writes a 224-byte
 *                                               // descriptor and dispatches it through the registry's
 *                                               // vtable[46] into the actor's skill manager. The audio
 *                                               // dispatcher later reads the registered entries to decide
 *                                               // voice muffling.
 *
 * Structural identification of the audio-classifier call path:
 *   - `a7` (7th positional arg, single byte) is 0 for every audio-classifier registration. It is non-zero for every
 *     other caller of the registrar. Verify that against the registrar's cross-references on patch day.
 *   - `a3` (the tag pointer) points into an 8-byte vector entry `{ u16 tag, u16 0, u16 lvl, u16 0 }` ONLY for
 *     audio-classifier calls. Other passive paths use different `a3` layouts. The `(u16)*(a3+4) == (u16)a4` equality
 *     is a deterministic format-check that admits only the audio-classifier code path.
 *
 * Muffle-class identification (chain walk via engine RTTI):
 *
 *   record       = skill_tag_resolver(a3)             // engine u16-tag -> skill record resolver
 *   level_table  = *(record + 0x18)                   // per-level table
 *   inner_arr    = *(level_table + 0x00)              // first inner ptr
 *   first_entry  = *(inner_arr + 0x00)                // level 1 entry #1
 *   vtable       = *(first_entry + 0x00)              // class vtable
 *
 * If `vtable` equals the resolved `pa::GameAudioEffectBuffData` vtable, the tag is a voice-muffle audio buff and
 * qualifies for suppression. Other classes (`pa::VoidPassiveBuffData`, `pa::ImmuneBuffData`, etc.) pass through.
 *
 * Evidence basis:
 *   - Tag 0x64B (skill 91000, internal name `"PlateHelm_Audio"`) resolves to `pa::GameAudioEffectBuffData` with Korean
 *     description `"투구 착용 시 먹먹한 소리"` (Muffled sound when wearing helmet).
 *   - Tag 0x64C (skill 91001, `"PlateHelm_Audio_OpenableHelm"`, visor-closed variant) resolves to the same class with
 *     the same description.
 *   - Other tags in the same iteminfo vector (0x647 / 0x650) resolve to non-audio classes (item stat / sound-attack
 *     immunity) and must remain pass-through, so helm-derived stats and sound-attack resistance survive the filter.
 *   - The muffle helms are the helms whose iteminfo equips skill 91000 or 91001. To re-derive that set, cross-check an
 *     iteminfo dump for those two skill ids. The chain walk identifies the set from the engine class alone, so a
 *     content patch that adds new tags backed by the same class needs no code change.
 *   - Footstep / armor-clank audio uses unrelated character-config fields (`_footStepSoundEvent` etc.) and never
 *     reaches this registrar. Those systems carry no collateral risk.
 *
 * Per-actor scope (any-protagonist gate by appearance):
 *
 * The hook suppresses muffle for any actor whose appearance path classifies as one of the 3 protagonists (Kliff,
 * Damiane, Oongka), regardless of which protagonist the player currently drives.
 *
 *   appearance file (e.g. `cd_phm_macduff_00000.app_xml` for Kliff in armor, or `cd_phm_oongka_00000.app_xml` for
 *   Oongka)
 *      -> CDCore::classify_appearance_by_path()
 *      -> {Kliff, Damiane, Oongka, Unknown}
 *   isProtagonist = actor in {Kliff, Damiane, Oongka}
 *
 * Why protagonist-set, not controlled-only: all 3 protagonist voices must play unmuffled even when not driven. A
 * Damiane body that spawns as a roster NPC alongside a Kliff-driven player still needs her muffle stripped, so the
 * muffled-helmet effect does not leak onto her dialogue lines while the player walks past.
 *
 * Collateral safety: the chain-walk vtable check above restricts the suppress universe to `pa::GameAudioEffectBuffData`
 * only. A protagonist-classified body loses only its voice muffle. Item-stat, sound-attack-immunity and generic combat
 * passives all pass through unchanged.
 *
 * Pre-world note: when the actor's appearance config is not yet wired up, the path resolver returns an empty buffer and
 * `classify_appearance_by_path` yields Unknown. The gate rejects, the trampoline runs, and the engine muffles as usual.
 * The next equip cycle after the appearance is wired runs through the hook, and the classifier then resolves cleanly.
 *
 * Bypass-safety analysis.
 *
 * SUPPRESS returns before it invokes the trampoline, so the whole registrar body is skipped. The only pre-branch side
 * effect inside that body is one entry-block virtual call on a `pa::ServerTransformSyncActorComponent`. That target is
 * rate-limited (>=100-tick gate when arg2 == 0), role-gated (network branch only for roles {3,4,5,6}, so a role=1
 * protagonist is a complete no-op), and refcount-balanced inside its own body. A skipped invocation has zero observable
 * correctness impact and zero refcount delta. The INI toggle that gates the feature keeps a runtime safety lever, in
 * case a future engine patch reshapes that call's contract.
 *
 * Alternative hook points considered and rejected:
 *   - Consumer layer (the audio-state publisher / Wwise SetState): attempts here mute the world bus, because muffle
 *     state is tightly coupled with RTPC voice attenuation plus Switch routing. The audio reader code also lives in
 *     obfuscated `.tls` space, so static RE is impractical.
 *   - iteminfo desc+0x100 source mutation: a persistent data change that affects NPC inventories, save serialization,
 *     and network sync. Out of scope for a per-actor mod.
 */

namespace Transmog::HelmAudioFilter
{
    /**
     * @brief Resolve dependencies and install the passive-skill registrar inline hook.
     *
     * Idempotent. Resolves three signatures here and defers a fourth:
     *   1. the per-tag passive-skill registrar entry (see `k_helmAudioRegistrarCandidates`)
     *   2. `pa::GameAudioEffectBuffData` vtable, the class marker for muffle-class identification (see
     *      `k_gameAudioEffectVtableCandidates`)
     *   3. engine player static, the root of the Kliff init-race fallback chain (see `k_playerStaticCandidates`). On
     *      AOB failure the hook still installs. Only the first-frame Kliff fallback goes dark.
     *   4. the engine tag resolver for the chain walk (see `k_skillTagResolverBodyAob`, picked by RTTI class name).
     *      This one binds lazily on the first registrar call, because its scan needs a live pa::SkillInfoManager.
     *
     * On any required AOB or hook install failure, logs a warning and leaves the feature disabled. The rest of LT
     * continues to load.
     *
     * @return true if the hook is now installed (including a prior successful install), false otherwise.
     */
    bool init();

} // namespace Transmog::HelmAudioFilter
