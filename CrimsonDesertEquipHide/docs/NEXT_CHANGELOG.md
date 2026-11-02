## Fixed chest hiding for companions

- Fixed the chest staying visible on other characters when `CascadeFix` is on, because a chest hidden on one character blocked the hide on the rest
- Fixed `CascadeFix` freezing chest changes on NPCs that use the same part as a hidden protagonist chest
- INI auto-reload now retries in the background if it fails to start during game launch
- Fixed tools such as the pickaxe showing on the character's back while stowed and clipping with two-handed weapons. Disabled categories no longer change how the game shows their parts. With `Tools` enabled, remove `CD_Tool_Axe`, `CD_Tool_Hammer`, `CD_Tool_Broom`, `CD_Tool_FarmScythe`, `CD_Tool_Pickaxe` and `CD_Tool_Shovel` from existing INI files
- Pikes and the stowed two-handed scabbard now hide with `TwoHandWeapons`. Existing INI files need `CD_TwoHandWeapon_Pike` and `CD_TwoHandWeapon_Sword_IN` added to that section's `Parts`
