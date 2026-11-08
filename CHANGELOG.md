# Changelog

All notable changes of **HS-02 Modern UI** (on top of [MYMDO/IronOS-FNIRSI-HS-02B](https://github.com/MYMDO/IronOS-FNIRSI-HS-02B)).

## [Unreleased]

### Added
- **Your own GIFs**, without rebuilding the firmware:
  - **standby screensaver**: after an idle time (*System → GIF screensaver*: Off / 5 s / 15 s / 30 s / 1 / 2 / 5 min,
    default 30 s) the standby GIF plays full screen. Any button or motion returns to standby (the dismissing press is
    swallowed); never shown while the tip is above 50 °C, on a tip fault or below 9 V;
  - **boot animation** instead of the boot splash (one loop, 1.5–8 s, a button skips it; *System → Boot GIF*).
- **GIF web constructor** (`tools/hs02-gif-studio`, deployed to GitHub Pages by CI): load a GIF or a short video (MP4 / WebM / MOV, decoded by the browser: start, length and fps), crop it to 160×80
  (fit / fill / free), set speed, frame range and border colour, see exactly what the iron will show and download a
  patched `.bin`. The GIFs are appended after the firmware image as a CRC-checked container (4 colours per 4-row band,
  RLE, XOR against frame 0); the firmware finds it through the `HS02SLOT` record and the `_hs02_assets_start` linker
  symbol. `cli.mjs` does the same from the command line; CI adds `custom/standby.gif` / `custom/boot.gif` to releases.
- *System → Invert colours*. The colour inversion setting was hidden from the colour menu, so a value left on by an
  older firmware turned the whole screen (and the GIFs) into a negative with no way to switch it off.

## [1.0.1] — 2026-11-06

### Fixed
- **False "Shorted" / "No tip"** on irons whose cold tip reads ~0 µV (amplifier offset differs
  between units): a near-zero reading no longer means short or missing tip, so heating is no longer
  blocked. "No tip" is now reported for a saturated (open) thermocouple, or when the heater has been
  driven ≥ 10 W for 2 s without the reading leaving zero.
- Standby shows a clear "Low input voltage" alert below 9 V (5 V USB / non-PD supply) instead of
  misleading tip states.

## [1.0.0] — 2026-11-06

### Added
- **New colour UI** (`draw_home_gauge.cpp`): status pill, Inter typeface with 2-level anti-aliasing,
  segmented bar; states Standby, Heating, Ready, Cooling, Hot!, Sleep, No tip / Shorted.
- **Per-row palettes** for the 2bpp framebuffer (`ColorBand.hpp`, `LCD::setColorBands`): 4 colours per
  screen band instead of 4 per screen, no extra RAM.
- **Animated boot splash** drawn in code (replaces the 25 KB bitmap logo).
- **Settings menu** in the stock FNIRSI structure: Sleep / Tip / System / Advanced, English and Russian
  (switchable at run time, *Auto* follows the build language).
- **FNIRSI-style controls**: OK = heat, +/− = target with accelerating hold, OK = presets 1-3,
  hold OK = stop / settings; deferred, wear-friendly saving of the target.
- **3-point tip calibration** (150/250/350 °C) in the stock calibration format, with a factory backup
  and *Reset calibration*.
- **Buzzer volume** setting (0-10) and chimes (ready, presets, start/stop).
- **USB-PD voltage** setting (9/12/15/20 V via CH224K CFG2/CFG3).
- **Firmware update from the menu** using the stock bootloader flag (`0x87456321` @ `0x0801F800`, word 18).
- **Colour alerts** for settings reset, missing accelerometer / PD, thermal runaway and low voltage.
- Heater-load bar at temperature (like the stock power bar).
- Tools: Inter font generator, native screenshot renderer, CI building HS02A/HS02B × EN/RU with releases.

### Changed
- Default sleep timeout 50 s → 5 min, +/− step 1 → 5 °C (also migrated on upgrade).
- `TranslationLanguageCode` symbol emitted by `make_translation.py`.

### Fixed
- **HS-02A bootloader header**: lowercase `bin` tag (the HS-02B fork wrote `BIN`, which the HS-02A
  bootloader ignores). Tag chosen per model via `FNIRSI_HEADER_TAG`.
- Buzzer was compiled with volume 0 (silent).
