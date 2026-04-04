## Runtime Hash Resolution & Bug Fixes

### Bug Fixes
- Fixed face/eye mesh disappearing when hiding necklace, rings, or other accessories
- Fixed helmet and mask not hiding/showing correctly when toggled via hotkey
- Fixed equipment on wrong character parts being hidden after game updates

### Improvements
- Equipment hashes are now fully resolved at runtime - no more hardcoded values that break across game patches
- Faster and more reliable hash scanning with smart retry logic
- Cleaner log output - removed excessive warnings during normal operation
- Init message now accurately reflects mod readiness state

### Technical
- Removed all compile-time fallback hashes; part map is purely runtime-resolved
- Widened IndexedStringA scan range for better coverage across game versions
- Deferred scan thread uses adaptive intervals and never exhausts
- Vis byte cleanup after hash rebuild prevents orphaned hidden parts
- Armor injection flags reset on toggle for correct re-injection
