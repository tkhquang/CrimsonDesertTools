## Sturdier save-load detection

- Per-character hide rules stay correct immediately after a save-load instead of needing a short settle window.
- The active protagonist is detected purely from in-game data, so loading EquipHide alongside other tools is one step lighter.
- All three protagonists are tracked from frame zero without needing the user to cycle to each one first.
- Fixed cases where part hide rules silently failed to take effect when the mod started while still on the main menu.
- A single typo in the Parts list no longer blocks every other entry from taking effect.
