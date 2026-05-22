## Multi-character world-entry auto-apply

- Every visible protagonist now has their saved transmog preset applied automatically on world entry, without needing to cycle to each character first.
- Followers freshly summoned during play also get their preset applied as soon as their body finishes loading.
- Sturdier save-load detection: the active character's preset is re-applied immediately on load instead of needing a short settle window.
- Switching the editing character in the dropdown no longer leaves stale fake items installed on the body.
- Added an optional Dump Item-to-Prefab TSV diagnostic for one-shot capture of every item's icon mesh name.
- Added an optional Dump Item Catalog TSV diagnostic, gated by its own INI key instead of trace-level logging.
- Hardened the two underlying engine-registry address lookups so the diagnostics keep working after game patches.
- Fixed brief part flashes during animation transitions that could persist for the whole session when the mod was loaded before reaching the main menu.
- New "Apply To Selected" checkbox next to the character dropdown: edits while pinned to a non-controlled character now render on that character's body, not on the one you control. Untick to keep the old cross-body behavior.
- Preset swaps on a non-controlled character now correctly remove the previous preset's items before installing the new ones, instead of stacking outfits on top of each other.
- Compatibility fix for Crimson Desert game version 1.08.00
