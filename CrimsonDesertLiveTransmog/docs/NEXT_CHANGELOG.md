## Crimson Desert 1.04.00 Support

- Restored full functionality on game version 1.04.00. Transmog apply, real-armor teardown, the picker filter, and the PlayerSafe check all work again after the patch
- Cloak items show up in the Cloak category again (the game renumbered cloaks on 1.04.00 and the picker was dropping them into Other)
- Display name coverage is now complete: the picker shows a friendly name for every 1.04.00 item (was missing ~300 names after the patch)
- Hot-reloading the mod mid-game no longer strips your character or crashes the next apply. Auto-apply waits for the item catalog to finish loading before re-applying your preset
- Append now creates a blank preset with every slot set to hide (ticked + none) and applies it immediately, instead of copying your current outfit
- New Copy button duplicates the currently-visible picker rows into a new preset so you can fork without overwriting
- Unsaved picker edits now show an orange `[UNSAVED -- click Save]` banner at the top of the overlay and tint the Save button orange with a `*` marker
- Developer hot-reload is more resilient: reloading a rebuilt logic DLL no longer leaves stale hooks behind
