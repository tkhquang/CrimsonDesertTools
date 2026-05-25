## Save-load and Color Override fixes

- Fixed a burst of transient errors that could appear in the log during the first few seconds after loading a save.
- Transmog now waits for the game to finish setting up your character before applying, instead of retrying through errors.
- Internal refactor: identify character components by class name so the mod keeps working if the game shuffles their order.
- Fixed an FPS drop when applying transmog while Color Override is enabled.
