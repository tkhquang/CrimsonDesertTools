## Companion transmog fix and stability update

- Fixed companions (such as Oongka and Damiane) showing their default gear instead of your chosen outfit after they leave and return to the scene; their transmog now reapplies on its own
- Refreshed the color and dye data for game version 1.12.02
- Updated the underlying framework (DetourModKit) to 3.9.0 for better stability and future compatibility
- More resilient to future game updates: some internal addresses now self-correct after a patch
- Added an advanced INI setting (SelfHealWindow) to help the mod recover after a game update
- Standardised the source-code formatting and added automatic style enforcement for future development
- Tidied developer comments and documentation for consistency, with no change to in-game behaviour
