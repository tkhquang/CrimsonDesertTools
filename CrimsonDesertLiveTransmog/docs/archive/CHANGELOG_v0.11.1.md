## Instant Apply and preset-hide fixes

- Upgraded DetourModKit dependency from v3.2.3 to v3.2.4.
- Demoted some verbose logs.
- Fix: real Mask/Necklace (and other accessory slots) now hide correctly when switching to a preset with all slots set to (none). Previously only the manual transmog→none path worked; preset switches left the real mesh visible on first-claim hide.
- Fix: Instant Apply now works correctly for body-mesh prefab picks.
