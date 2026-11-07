# DEVELOPMENT.md — FNIRSI HS-02B port of IronOS

This repo ([MYMDO/IronOS-FNIRSI-HS-02B](https://github.com/MYMDO/IronOS-FNIRSI-HS-02B))
is a fork of [IronOS](https://github.com/Ralim/IronOS) targeting the
**FNIRSI HS-02B** soldering iron (C210 cartridges). It builds on PanKleszcz's
`fnirsi_hs02` port + ok2cm's work, merged into `dev`. The tree still builds
`HS02` / `HS02A` (C245) for compatibility, but all docs here are written for
**HS-02B**. This file documents the non-obvious, HS-02-specific facts needed
to work on it. General IronOS docs live in `Documentation/`.

## Build & flash (HS-02B)

- One-shot manual build: `cd source && make model=HS02B -j$(nproc)` (EN default;
  `make model=HS02B firmware-UK` for Ukrainian). Output to flash is
  `source/Hexfile/HS02B_EN_firmware.bin` (carries the 16-byte `bin` header the MSD bootloader
  needs — plain `HS02B_EN.bin` will NOT flash).
- `HS02A` (`model=HS02A`, C245 tips) still builds from the same tree for reference;
  HS-02B differs only in `configuration.h`: `TIP_RESISTANCE 26` (C210 2.6 Ω nominal),
  `TIP_PID_KP 40`, `TIP_PID_KD 4500` (vs 25 / 55 / 6000 on C245).
- Toolchain (Fedora): `arm-none-eabi-gcc-cs arm-none-eabi-gcc-cs-c++ arm-none-eabi-newlib
  arm-none-eabi-binutils-cs` + `pip install bdflib` (translation/font generation).
- **Flashing quirk:** the FNIRSI MSD bootloader mishandles USB write caching. On Linux you
  MUST copy the firmware to the mounted drive **twice**, each followed by `sync`, or the iron
  hangs in the bootloader with a half-written image. `build-hs02.sh --flash` does this.

## Hardware (source: schematic RE + stock-firmware RE)

- **MCU:** Nation **N32L403** (Cortex-M4, 64 MHz). Uses an **STM32F1-style peripheral map**
  (RCC `0x40021000`, GPIOA `0x40010800`, etc.). App is linked at `0x08005800`; the first
  0x5800 is the FNIRSI bootloader (not in the `APP_` images).
- **Display:** 160×80 colour IPS, **ST7735** over SPI @ 16 MHz. Driver: `Core/Drivers/LCD.cpp`
  (`LCD_160x80`), low-level SPI in `Core/BSP/Fnirsi/SPI_Wrapper.cpp`.
  - Panel quirks (learned the hard way doing the boot logo): **column-major** pixel scan
    (the mono UI's `refresh()` sends per-8-row strip, column-major), **INVON** (display
    inversion on — `0x0000` shows white), and the framebuffer/logo colours must be
    **inverted** and sent **RGB** (NOT byte/channel-swapped) to come out true. The source
    logo block also had a 64-pixel header + a trailing junk row.
- **Accelerometer:** Kionix **KXTJ3-1057** on **software (bit-banged) I2C**, PA9=SCL/PA10=SDA,
  addr `0x0E`, INT not wired (polled). Driver `Core/Drivers/KXTJ3.*`.
- **Heater:** AO4407A P-MOSFET driven by PWM on PA8 (`PWR_OUT`); current sense via 10 mΩ
  shunt → PB1 (`I_MEAS`). Tip thermocouple → AD8552 → PA0 (`T_MEAS`), read via ADC+DMA
  (no function pokes the ADC register directly). Amp gain is ambiguous: the KiCad-redrawn
  schematic shows both 48K/1K (=49) and 56K/1K (=57) networks around the two amp channels;
  the port's `OP_AMP_GAIN_STAGE 57` suggests T_SENSE uses 56K/1K. Doesn't matter for
  accuracy — the gain constant cancels in both temperature-model paths (see below).
- **USB-PD:** CH224K sink controller negotiates voltage; MCU sets the voltage tier via
  PB6/PB7 (CFG). This is what caps available wattage.
- **Buttons:** 3 physical — UP=PB4, DOWN=PB5, OK=PB3 (all input-pullup, active low).

## Gotchas / things that bite

- **`LCD` name clash:** `n32l40x.h` defines `LCD` as a peripheral pointer macro. Any file
  that uses the `LCD` **class** after including the Nation headers must `#undef LCD` first
  (see `Core/BSP/Fnirsi/BSP.cpp`).
- **HW I2C is unusable here.** The N32L40x I2C peripheral inherits the classic STM32F1 I2C
  errata (event-timing races needing top-priority IRQ/DMA, corrupt 1–2 byte reads, DMA
  conflicts, glitch sensitivity). Stock firmware and this port both use bit-banged I2C.
- **PWM pipeline width:** `powerPWM` is 395 on this board (> 255). The shared
  `X10WattsToPWM()` / `setTipPWM()` path must be `uint16_t` end to end, otherwise high duty
  wraps mod 256 and output collapses (the "100 W only reaches ~40 W" bug — fixed).

## Temperature control & calibration (branch `fix/hs02-pid-tuning`)

- **Controller:** PID (`TIP_CONTROL_PID`), NOT the ARDC it shipped with. Field-tuned on an
  HS-02A soldering XT60 connectors: KP=40, KI=300, KD=8000, `TIP_PID_INTEGRAL_LIMIT_SCALE 30`.
  Requires the **conditional anti-windup** added to the shared PID in `PIDThread.cpp`
  (upstream-worthy): without it the integral rails to ± the clamp during heatup/overshoot
  and the system limit-cycles around the set point for minutes.
- **Temp model** (`Core/BSP/Fnirsi/ThermoModel.cpp`): the original "21uV per 1C" guess
  over-read ~23% (displayed 450 ≈ real 370). Two paths now:
  1. **Stock calibration auto-adoption:** stock firmware keeps 3 user-calibration words in
     its settings page at `0x0801F800` (word offsets 0x16/0x17/0x18; count = word − bias
     0x7F21/0x7E77/0x7DC4; curve anchors 140/240/340 °C; stock cal MENU shows 150/250/350).
     `0x8000` = neutral "never calibrated" placeholder (decodes to nominal 223/393/572) and
     is REJECTED — the nominal curve over-reads ~25-30% (stock firmware itself shows 230 °C
     at a real 183 °C, hardware-verified). IronOS settings live below `0x0801F000`, so the
     stock page survives reflashing.
  2. **Fallback:** measured 26 uV/°C constant, calibrated with solder melting points
     (Sn45/Pb55: solidus 183 °C "blade can cut", liquidus ~227 °C "melts") — ~3× more
     accurate than stock on an uncalibrated unit (+17 vs +47 °C at 183 °C real).
- **Debug menu "Tip Cal" page** (17th page; home screen → long-press UP): shows the three
  decoded cal counts + "In Use" / "Unused 26uV/C". NOTE: raw ASCII literals render garbled —
  the small font only carries glyphs used by translations; UI text must go through
  `make_translation.py` (`get_constants()` SmallSymbol* entries or `get_debug_menu()`).
- Stock PID for reference (from RE): incremental/velocity form, Kp=2.38 Ki=0.81 Kd=0.28
  (doubles in the .data tail), gain-scheduled ×4.5/×3.0/×2.0 at 9/12/15 V PD tiers.

## UI architecture (IronOS)

Two layers, split by concern:
- **Logic** (`Core/Threads/UI/logic/`): screen state machines / navigation / input handling.
  **Shared across all devices**, resolution-independent (e.g. `HomeScreen.cpp`, `Soldering.cpp`,
  `SettingsMenu.cpp`, `TemperatureAdjust.cpp`). Edit here to change behaviour everywhere.
- **Drawing** (`Core/Threads/UI/drawing/<variant>/`): the actual pixel painting, one variant
  per resolution — `mono_96x16`, `mono_128x32`, and ok2cm's `color_160x80` (17 `draw_*.cpp`
  each). HS-02 uses `color_160x80`. Adding a screen = implement its `draw_*` per variant.

So ok2cm did NOT rewrite the UI — he added a `color_160x80` **drawing** variant and reused the
shared logic. Control changes (e.g. the dedicated OK button) live in the logic layer and apply
to all variants automatically.

## HS-02-specific additions in this fork (branches off `dev`)

- `feat/hs02-ok-button` — OK (PB3) wired as a real third button (`BUTTON_OK_SHORT/LONG`,
  one-shot long-press). Short = enter/confirm/select, long = back. Core input in
  `Core/Drivers/Buttons.cpp` (weak `getButtonOK()`, overridden in the Fnirsi BSP); per-screen
  actions in the logic layer.
- `fix/pwm-uint16-upstream` — the uint16 PWM widening (generic, upstream-worthy).
- `fix/hs02-power-100w` — the above + the colour boot logo (`showBootLogo()`,
  `LCD::drawNativeImage()`, `FRToSSPI::fastSend()`, `FnirsiBootLogo.h`).
- `fix/hs02-pid-tuning` — on top of the above: shared-PID conditional anti-windup +
  configurable integral clamp, ARDC→PID switch with field-tuned gains, corrected temperature
  model with stock-calibration auto-adoption, and the debug-menu "Tip Cal" page (see the
  "Temperature control & calibration" section).

## Reverse-engineering notes

Extensive stock-firmware RE (V1.8 vs V2.1) lives under
`Development Resources/firmwares/` (git-untracked): analysis write-up, schematic reading,
button/interaction map, and the extracted boot logo. Key finding: the temperature control
path (calibration, PID, factory defaults) is byte-identical V1.8↔V2.1; the only temp-relevant
change in V2.1 is the accelerometer tilt-angle logic (a new 90° breakpoint affecting
motion-sleep), which is why V2.1 "holds temperature more aggressively".

## HS-02 Modern UI layer (this repository)

- **Colour UI**: everything colour lives in `Core/Threads/UI/drawing/color_160x80/draw_home_gauge.cpp`
  (gauge screens, boot splash, menu, alerts, calibration). Text comes from `Hs02UiStrings.hpp`
  (`HS02_TEXT(name, en, ru)` → `Hs02UiText::name()`), glyphs from the generated `Hs02ModernFont.hpp`
  (`tools/hs02-ui-fonts/gen_modern_font.py`; re-run after editing strings). Language at run time:
  `SettingsOptions::HS02UiLanguage` (0 auto via `TranslationLanguageCode`, 1 RU, 2 EN).
- **Palettes**: the 2bpp frame has 4 colours per *row band* — call `Display::setColorBands()` every
  frame (bands are re-hashed; a palette-only change forces a refresh). Standard bands: status 0-17,
  value 18-49, bar 50-57, footer 58-79; the menu uses one band per 20 px row.
- **Modes added**: `OperatingMode::ModernMenu` (`logic/ModernMenu.cpp`), `TipCalibration`
  (`logic/TipCalibration.cpp`). Both are colour screens (`isColorScreenMode`). Legacy menu is still
  reachable as `SettingsMenu` from *Advanced*.
- **Controls** (`logic/utils/Hs02Controls.*`): `hs02StepTarget`, `hs02NextPreset`, `hs02RepeatMultiplier`
  (rate-limited ~8/s, ×2 after 1 s, ×5 after 2.5 s). Target changes are saved 2 s after the last press
  and on every mode change (`hs02ControlsFlush` in `guiRenderLoop`). Beeps: `utils/Beeper.*`.
- **New settings** (appended, so old saves keep their values): `PresetTemp1..3`, `BuzzerVolume`,
  `PDVoltage`, `HS02UiLanguage`. Upgrading from < `PresetTemp1` also resets SleepTime/step defaults.
- **Stock page 0x0801F800** (`hs02StockPageBegin/Commit` in the Fnirsi BSP): 100-word stock settings
  block. Word 0 = 20 (version), words 0x16-0x18 = calibration (`count + bias`), word 18 = bootloader
  update flag `0x87456321` (RE of APP_HS_02A_V2.1.1, `NVIC_SystemReset` caller at 0x08009504).
  Words 100-102 = our one-time backup of the factory calibration words.
- **Calibration math**: with stock calibration the model returns `curve(count) - 25 °C + handle`, so
  the calibration stores `truth + 25 - handle` per point before deriving the 140/240/340 anchors.
- **Bootloader header**: HS-02A wants `bin`, HS-02B `BIN` (`FNIRSI_HEADER_TAG` in `source/Makefile`).
  Official HS-02A files: `bin\0` + LE length. Short 8.3 names are safest.
- **CH224K**: CFG2/CFG3 = 00 9 V, 01 12 V, 11 15 V, 10 20 V (`applyPDVoltageSetting`).
- **Screenshots**: `tools/hs02-wasm-preview/render_docs.sh` compiles the real drawing code for the host.
- **Standby GIF**: `custom/standby.gif` → `tools/hs02-standby-gif/gif2hs02.py` → generated
  `color_160x80/Hs02StandbyGif.hpp` (committed; CI regenerates it). Frames are unpacked straight into the
  2bpp framebuffer (`Display::colorFrameBuffer()`) with one `ColorBand` per 4 rows, so playback costs
  ~200 B of RAM. Idle detection / dismissal lives in `logic/HomeScreen.cpp` (`standbyGifService`);
  setting `SettingsOptions::StandbyGif` (0 off, 1..6 = 5 s … 5 min; table duplicated in `ModernMenu.cpp`).
