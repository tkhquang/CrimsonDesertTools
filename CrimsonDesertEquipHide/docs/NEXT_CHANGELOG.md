## Compatibility

- Added automatic coexistence with the game's built-in headgear visibility setting (v1.02.00+). When the in-game setting is anything other than "Show Always", the mod yields Helm, Mask, and Glasses categories to the official system and only manages the remaining categories. When "Show Always" is selected, the mod manages all categories as before. Controlled by `DeferHeadgear` in `[General]` (default: true). Set to false to override the game's headgear setting
- BaldFix now correctly defers to the game's own hair handling when headgear visibility is officially managed

## Internal Improvements

- Improved performance when toggling equipment visibility
- Reorganized internal code for easier maintenance and future updates
- Adopted newer DetourModKit APIs for faster and more reliable pattern scanning
