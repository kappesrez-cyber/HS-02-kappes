
#include "Buttons.hpp"
#include "OperatingModes.h"
#include "ui_drawing.hpp"
#include "utils/Beeper.h"
#include "utils/Hs02Controls.h"
#include "utils/OperatingModeUtilities.h"

bool showExitMenuTransition = false;

#if defined(LCD_160x80)
// ---------------------------------------------------------------- standby GIF screensaver
// After the configured idle time (Settings > System > GIF screensaver) the standby screen plays
// custom/standby.gif full screen. Any button or moving the iron brings the standby screen back;
// the press that does so is swallowed (until released) so it can't start heating by accident.
// It never covers a hot tip (> 50 C), a tip fault or a low supply voltage.
namespace {
constexpr uint16_t kGifDelaySeconds[] = {0, 5, 15, 30, 60, 120, 300}; // index = StandbyGif setting
constexpr uint32_t kGifMaxTipTempC    = 50;
bool               gifActive          = false;
bool               gifSwallowKeys     = false;
TickType_t         gifStart           = 0;
TickType_t         gifViewEnterTime   = 0;
} // namespace

// Returns true when the screensaver drew this frame; may clear `buttons` (swallowed press).
static bool standbyGifService(ButtonState &buttons, const guiContext *cxt, uint32_t tipTempC, bool undervoltage) {
  if (gifViewEnterTime != cxt->viewEnterTime) { // (re)entered standby: start fresh
    gifViewEnterTime = cxt->viewEnterTime;
    gifActive        = false;
    gifSwallowKeys   = false;
  }
  if (gifSwallowKeys) {
    if (buttons == BUTTON_NONE && !getButtonA() && !getButtonB() && !getButtonOK()) {
      gifSwallowKeys = false;
    }
    buttons = BUTTON_NONE;
  }

  const uint16_t   setting = getSettingValue(SettingsOptions::StandbyGif);
  const TickType_t now     = xTaskGetTickCount();
  const bool       allowed = setting > 0 && setting < sizeof(kGifDelaySeconds) / sizeof(kGifDelaySeconds[0]) && !undervoltage
                       && getInputVoltageX10(getSettingValue(SettingsOptions::VoltageDiv), 0) >= 90 && tipTempC < kGifMaxTipTempC
                       && !isTipDisconnected() && !isTipShorted();

  if (gifActive) {
    const bool pressed = lastButtonTime > gifStart;
    if (!allowed || pressed || lastMovementTime > gifStart) {
      gifActive = false;
      if (pressed) {
        gifSwallowKeys = true;
        buttons        = BUTTON_NONE;
      }
      return false;
    }
    ui_draw_standby_gif(now - gifStart);
    return true;
  }
  if (!allowed) {
    return false;
  }
  TickType_t lastActivity = cxt->viewEnterTime;
  if (lastButtonTime > lastActivity) {
    lastActivity = lastButtonTime;
  }
  if (lastMovementTime > lastActivity) {
    lastActivity = lastMovementTime;
  }
  if (now - lastActivity < (TickType_t)kGifDelaySeconds[setting] * TICKS_SECOND) {
    return false;
  }
  gifActive = true;
  gifStart  = now;
  ui_draw_standby_gif(0);
  return true;
}

// HS-02: FNIRSI-style standby. OK = start heating, hold OK (or both +/-) = settings,
// +/- = change the target (held: repeat). The heater is always off here.
static OperatingMode handleHomeButtonsHS02(const ButtonState buttons, guiContext *cxt) {
  cxt->transitionMode = TransitionAnimation::None;
  switch (buttons) {
  case BUTTON_OK_SHORT:
    if (!isTipDisconnected() && !isTipShorted()) {
      beep(40);
      return OperatingMode::Soldering;
    }
    break;
  case BUTTON_OK_LONG:
  case BUTTON_BOTH_LONG:
    return OperatingMode::ModernMenu;
  default:
    hs02StepTarget(buttons);
    break;
  }
  return OperatingMode::HomeScreen;
}
#endif

OperatingMode handleHomeButtons(const ButtonState buttons, guiContext *cxt) {
  if (buttons != BUTTON_NONE && cxt->scratch_state.state1 == 0) {
    return OperatingMode::HomeScreen; // Ignore button press
  } else {
    cxt->scratch_state.state1 = 1;
  }
#if defined(LCD_160x80)
  return handleHomeButtonsHS02(buttons, cxt);
#endif
  switch (buttons) {
  case BUTTON_NONE:
    // Do nothing
    break;
  case BUTTON_BOTH:
    break;

  case BUTTON_B_LONG:
#if defined(LCD_160x80)
    // The colour gauge screen and the mono menu system share LCD buffer storage (see LCD.hpp).
    // Their slide transition routines interpret frames differently, so every crossing between
    // the two representations uses a hard cut.
    cxt->transitionMode = TransitionAnimation::None;
#else
    cxt->transitionMode = TransitionAnimation::Down;
#endif
    return OperatingMode::DebugMenuReadout;
    break;
  case BUTTON_F_LONG:
#ifdef PROFILE_SUPPORT
    if (!isTipDisconnected() && !isTipShorted()) {
#if defined(LCD_160x80)
      cxt->transitionMode = TransitionAnimation::None;
#else
      cxt->transitionMode = TransitionAnimation::Left;
#endif
      return OperatingMode::SolderingProfile;
    } else {
      return OperatingMode::HomeScreen;
    }
#else
#if defined(LCD_160x80)
    cxt->transitionMode = TransitionAnimation::None;
#else
    cxt->transitionMode = TransitionAnimation::Left;
#endif
    return OperatingMode::TemperatureAdjust;
#endif
    break;
  case BUTTON_OK_SHORT: // Dedicated OK button enters soldering (same as front-press)
  case BUTTON_F_SHORT:
    if (!isTipDisconnected() && !isTipShorted()) {
#if defined(LCD_160x80)
      cxt->transitionMode = TransitionAnimation::None; // Staying within the colour gauge screen family.
#else
      bool detailedView   = getSettingValue(SettingsOptions::DetailedIDLE) && getSettingValue(SettingsOptions::DetailedSoldering);
      cxt->transitionMode = detailedView ? TransitionAnimation::None : TransitionAnimation::Left;
#endif
      return OperatingMode::Soldering;
    }
    break;
  case BUTTON_B_SHORT:
#if defined(LCD_160x80)
    cxt->transitionMode = TransitionAnimation::None;
#else
    cxt->transitionMode = TransitionAnimation::Right;
#endif
    return OperatingMode::SettingsMenu;
    break;
  default:
    break;
  }
  return OperatingMode::HomeScreen;
}

OperatingMode drawHomeScreen(ButtonState buttons, guiContext *cxt) {

  currentTempTargetDegC       = 0; // ensure tip is off
#if defined(LCD_160x80)
  hs02ControlsService();
#endif
  const uint16_t v            = getInputVoltageX10(getSettingValue(SettingsOptions::VoltageDiv), 0);
  const bool     undervoltage = (v < lookupVoltageLevel());
  uint32_t       tipTemp      = TipThermoModel::getTipInC();

#if defined(LCD_160x80)
  if (standbyGifService(buttons, cxt, tipTemp, undervoltage)) {
    return OperatingMode::HomeScreen; // screensaver on screen: buttons only dismiss it
  }
  if (undervoltage) {
    ui_draw_warning_undervoltage_idle();
  } else {
    ui_draw_home_gauge_idle(tipTemp);
  }
#else
  if (undervoltage) {
    ui_draw_warning_undervoltage_idle();
  } else {
    // Setup LCD Cursor location
    if (Display::getRotation()) {
      Display::setCursor(50, 0);
    } else {
      Display::setCursor(-1, 0);
    }
    if (getSettingValue(SettingsOptions::DetailedIDLE)) {
      ui_draw_homescreen_detailed(tipTemp);
    } else {
      ui_draw_homescreen_simplified(tipTemp);
    }
  }
#endif

  // Block entering heating mode if voltage is low
  if (undervoltage) {
    if (buttons == BUTTON_F_SHORT || buttons == BUTTON_OK_SHORT || buttons == BUTTON_F_LONG) {
      return OperatingMode::HomeScreen;
    }
  }

  return handleHomeButtons(buttons, cxt);
}
