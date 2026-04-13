## NPC and Damaged Armor Support

- **NPC and damaged armor variants now work** - items previously greyed out or invisible (like Antra, Aant, and other NPC-specific armors) can now be equipped as transmog. The mod automatically handles the conversion behind the scenes.
- **New item picker badges** - NPC/damaged items show a cyan `(carrier)` tag in the picker so you know they use the new system. Truly incompatible items (horse tack, pet armor) remain red.
- **New filter: "Hide variants"** - toggle visibility of NPC/damaged items in the picker. Off by default so you can see everything.
- **"Safe only" filter renamed** - the old "Hide variants" filter is now "Safe only" and only hides items that would crash the game (non-humanoid gear).
- **Fixed: real armor not hiding on game load** - when slots were set to active with no item selected, real armor would briefly show on load. Fixed.
- **Fixed: unticking a slot now restores real armor** - previously unticking a slot checkbox could leave the slot empty. Now the real equipped item reappears.
- **Fixed: switching presets with NPC items** - switching from an NPC preset to a normal preset no longer leaves ghost meshes from the previous preset.
