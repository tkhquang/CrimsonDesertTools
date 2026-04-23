## [Title for next release]

- Compatibility update for Crimson Desert v1.04.00
- Fix crash on world load: PostfixEval AOB resolved to a mid-instruction address on v1.04.00, causing the trampoline to run a truncated register save and corrupt rbp. Added explicit prologue candidates for both v1.03.01 and v1.04.00 (full-function anchors) and recalibrated the body-only fallback offset
- Fix per-character Parts overrides on v1.04.00: the actor slot-index byte moved from +0x60 to +0x50 (+0x60 is no longer a discriminator). Companion diff table ({Damiane=1, Oongka=2}) unchanged
- Add resolver poll thread: drives `resolve_player_vis_ctrls` on a 1s cadence so cold load and in-session character swaps are detected without relying on the EquipVisCheck hook firing
- Add cold-load safety net in the EquipVisCheck hook: if the body list is still empty when the hook fires, run the resolve pass inline once so DefaultHidden categories apply on first visibility decision
- Add INFO logs on UserActor swap and in-session controlled-actor swap (makes save-load and swap transitions visible in the log)
- Known issue: leg/pants flashes during movement while chest is hidden on v1.04.00. `CascadeFix` relies on EquipVisCheck firing at v1.03.01 rates; on v1.04.00 the hook's event set is narrower. Deferred to a future update
