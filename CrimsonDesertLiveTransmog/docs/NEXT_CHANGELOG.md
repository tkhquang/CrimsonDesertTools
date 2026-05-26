## Helm voice unmuffle and reliability fixes

- Optional setting to stop plate and heavy helmets from muffling your protagonist's voice -- enable `UnmuffleHelmVoice` under `[Experimental]` (off by default; NPC voices still muffle as in vanilla).
- Upgraded DetourModKit dependency to v3.3.0.
- More resilient internal checks against the game's loaded modules.
- Reduced duplicated crash-protection code across modules.
- Fixed first-apply errors after a cold load by waiting until the character is fully ready.
