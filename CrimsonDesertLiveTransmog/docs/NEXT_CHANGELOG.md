## Multi-Character Support (Kliff, Damiane, Oongka)

- Preset lists are now stored per character. The active list swaps automatically when you change who you are controlling in-game, and each character remembers its own "last applied" preset ([#34](https://github.com/tkhquang/CrimsonDesertTools/pull/34))
- New body-kind (male / female) filter in the picker so mismatched-body armor is hidden by default. Per-character override available in the overlay for body-swap mod users ([#34](https://github.com/tkhquang/CrimsonDesertTools/pull/34))
- Display name table is now loaded from `CrimsonDesertLiveTransmog_display_names.tsv`. The `.tsv` must be installed alongside the `.asi`; without it the picker falls back to raw item ids ([#34](https://github.com/tkhquang/CrimsonDesertTools/pull/34))
- Added `LICENSE` (MIT) to the mod folder ([#34](https://github.com/tkhquang/CrimsonDesertTools/pull/34))
- Shared AOB utilities moved into the new `CrimsonDesertCore` static library. No behavioural change ([#33](https://github.com/tkhquang/CrimsonDesertTools/pull/33))
- README, Nexus bbcode, template readme, and Acknowledgements refreshed for the multi-character release
