## Build & Tooling
- PlatformIO: `.venv/bin/pio`
- Build command: `.venv/bin/pio run -e murphy_m4`
- Upload: `.venv/bin/pio run -e murphy_m4 --target upload`

## i18n
- Add new string keys to `lib/I18n/translations/english.yaml`
- Regenerate C++ files: `python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/`

## Key Conventions
- Settings: add fields to `src/CrossPointSettings.h`, entries to `src/SettingsList.h`, and picker/display logic to `src/activities/settings/SettingsActivity.cpp`
- Board-specific code guards with `gpio.deviceIsMurphyM4()` and `#ifdef CROSSPOINT_BOARD_MURPHY_M4`
