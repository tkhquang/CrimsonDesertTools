## Multi-Character Support

- Shared AOB scanning utilities moved into the new `CrimsonDesertCore` static library. No behavioural change ([#33](https://github.com/tkhquang/CrimsonDesertTools/pull/33))
- Minor post-extraction cleanups in `aob_resolver`, `armor_injection`, `bald_fix`, `equip_hide`, `player_detection`, and `visibility_write`. No user-visible change ([#34](https://github.com/tkhquang/CrimsonDesertTools/pull/34))
- Per-character Parts overrides. Every category section now accepts `[Section:Kliff]`, `[Section:Damiane]`, and `[Section:Oongka]` subsections; the mod picks the active character's list automatically on in-game swap. Missing subsection inherits from the base `[Section]` list. Use `Parts = NONE` to disable a category for a specific character without affecting the others
- Bald Fix now works for Kliff, Damiane, and Oongka
- Fixed: swapping between characters no longer leaves the previous character's hide state stuck on the new character's body (e.g. hiding Oongka's chest parts and then swapping to Kliff no longer leaves Kliff bare-chested)
- Per-character detection is now robust across saves regardless of party composition
