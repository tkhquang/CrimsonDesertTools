## Crimson Desert 1.04.00 Support

- Updated internal offsets so apply, picker filtering, and real-armor teardown all work on game version 1.04.00. Transmog no longer crashes or silently no-ops after the patch
- Display name table refreshed against the latest save-editor dataset so the picker shows proper names for every 1.04.00 item (full 6334/6334 coverage, previously 6019)
- Cloak items now appear in the Cloak category again. The game renumbered the cloak type code on 1.04.00, which dropped them into "Other" in previous builds
- PlayerSafe filter re-tuned for 1.04.00 so cross-body items stay correctly marked for Kliff / Damiane / Oongka
- Hot-reloading the mod no longer strips your character or crashes on the next apply. Auto-apply now waits until the item catalog finishes loading before re-applying your active preset
- "Append" button and hotkey now create a blank preset with every slot ticked + none (a clean hide-all template) and apply it immediately, instead of copying your current outfit
- New "Copy" button duplicates the currently-visible picker rows into a new preset, so you can fork an existing preset without overwriting it
- Unsaved picker edits now show an orange `[UNSAVED -- click Save]` banner at the top of the overlay and tint the Save button orange with a `*` marker, so pending edits are hard to miss
- Developer-build hot-reload is more resilient: the DLL now runs its hook teardown from both the loader shutdown path and `DLL_PROCESS_DETACH`, so re-injecting a rebuilt logic DLL no longer leaves stale hooks behind
