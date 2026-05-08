## Expanded Slot Coverage and Body-Mesh Prefab Picker

- Slot picker now covers 10 equipment slots (helm, chest, cloak, gloves, boots, necklace, lantern, glasses, mask, backpack) instead of the 5 armor slots only. Weapon, ring, earring, and bracelet slots are not yet supported.
- New body-mesh prefab picker lets you pick a prefab variant to swap onto a slot, in addition to the regular item picker
- Body-mesh prefab selections persist with your preset (saved as a new `prefabName` field; older preset files load unchanged)
- Preset JSON format updated: slots are now stored as a keyed object (by slot name) instead of a positional array, so future slot additions will not shift older saves. Legacy array-format presets load automatically and are rewritten in the new format on next save.
- Optional cross-slot prefab browser mode: list every body-mesh prefab in one place and apply it to its native slot from any picker
- Removed the hard world-load timeout in the standalone overlay path. The mod now waits indefinitely for the game world to become ready (with a heartbeat log every minute) instead of giving up and warning, which prevented the overlay from initializing on slow boots.
