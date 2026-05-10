## Capture Real Dye, Disabled-Mode Cleanup, Standalone Overlay Polish

- Capture Outfit now also snapshots the live dye and material state on every captured slot, so loading a captured preset paints each item in the colours it had at capture time instead of the factory palette.
- Toggling Live Transmog off (or hitting Clear) now correctly tears down NPC carrier visuals on slots that had no real backing item, so disabled mode no longer leaves a ghost helm or cloak painted on the actor.
- Restoring a real item after unticking a slot, or after toggling Live Transmog off, now keeps the item's current inventory dye instead of resetting it to the factory palette.
- New per-slot Sparse toggle in the dye picker. Sparse (default) emits only the channels you set and lets the engine paint the rest with its own defaults; turn it off when doing cross-class fake transmog so every channel uses your colour and the carrier's default palette is fully suppressed.
- Standalone overlay font is now built at the target pixel size (sharper text on 1440p and 4K) instead of being stretched, and the window, item picker, slot label column, and picker button widths all auto-fit the live font so 4K screens no longer show wide empty bands.
- Standalone overlay mouse clicks now use rising-edge detection over the overlay window: clicks that begin inside the game and drag onto the overlay no longer fire phantom widget events, and clicks that begin on the overlay still complete cleanly even if the mouse leaves the overlay before release.
