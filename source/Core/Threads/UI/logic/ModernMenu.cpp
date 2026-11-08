// HS-02 colour settings menu, laid out like the stock FNIRSI firmware:
//   Settings ─┬─ Sleep    : sleep after, sleep temperature, power off after, motion sensor
//             ├─ Tip      : presets 1-3, +/- step, max power, USB-PD voltage
//             ├─ System   : sound, brightness, orientation, +/- buttons, language,
//             │             GIF screensaver, boot GIF, colour inversion, firmware update,
//             │             factory reset, version
//             ├─ Advanced : the full IronOS settings menu, diagnostics
//             └─ Exit
// Controls: +/- move (held: repeat), OK = open / edit / confirm, hold OK = back.
// After 60 s without input the menu saves and returns to standby (as the stock UI does).
#include "Beeper.h"
#include "Hs02Controls.h"
#include "BSP.h"
#include "Buttons.hpp"
#include "Display.hpp"
#include "OperatingModes.h"
#include "Settings.h"
#include "configuration.h"
#include "ui_drawing.hpp"
#include "../drawing/color_160x80/Hs02UiStrings.hpp"
#include "../../../../version.h"

#ifdef LCD_160x80

void hs02ResetTipCal(); // ThermoModel.cpp

namespace {

enum MenuId : uint8_t { M_ROOT, M_SLEEP, M_TIP, M_SYSTEM, M_ADVANCED };
enum class Kind : uint8_t { Submenu, Setting, Action, Info };
enum Fmt : uint8_t { F_LANG, F_TEMP, F_SLEEP, F_MINUTES, F_OFFNUM, F_WATTS, F_PD, F_BRIGHT, F_ORIENT, F_REVERSE, F_STEP, F_GIF, F_ONOFF, F_NONE };
enum ActionId : uint8_t { A_EXIT, A_IRONOS, A_DIAG, A_UPDATE, A_RESET, A_CALIB, A_CALRESET };

using TextFn = const char *(*)();

struct Item {
  TextFn      label;
  Kind        kind;
  uint8_t     arg; // submenu id, SettingsOptions or action id
  Fmt         fmt;
  int16_t     min, max, step;
  uint8_t     labelNumber; // appended to the label ("Preset 2"); 0 = none
};

using namespace Hs02UiText;
#define SETTING(label, opt, fmt, mn, mx, st) {label, Kind::Setting, (uint8_t)SettingsOptions::opt, fmt, mn, mx, st, 0}

const Item kRootItems[] = {
    {kMenuSleep, Kind::Submenu, M_SLEEP, F_NONE, 0, 0, 0, 0},
    {kMenuTip, Kind::Submenu, M_TIP, F_NONE, 0, 0, 0, 0},
    {kMenuSystem, Kind::Submenu, M_SYSTEM, F_NONE, 0, 0, 0, 0},
    {kMenuAdvanced, Kind::Submenu, M_ADVANCED, F_NONE, 0, 0, 0, 0},
    {kMenuExit, Kind::Action, A_EXIT, F_NONE, 0, 0, 0, 0},
};
const Item kSleepItems[] = {
    SETTING(kItemSleepTime, SleepTime, F_SLEEP, 0, 15, 1),
    SETTING(kItemSleepTemp, SleepTemp, F_TEMP, 100, 250, 10),
    SETTING(kItemShutdown, ShutdownTime, F_MINUTES, 0, 60, 1),
    SETTING(kItemMotion, Sensitivity, F_OFFNUM, 0, 9, 1),
};
const Item kTipItems[] = {
    {kItemPreset, Kind::Setting, (uint8_t)SettingsOptions::PresetTemp1, F_TEMP, 100, MAX_TEMP_C, 5, 1},
    {kItemPreset, Kind::Setting, (uint8_t)SettingsOptions::PresetTemp2, F_TEMP, 100, MAX_TEMP_C, 5, 2},
    {kItemPreset, Kind::Setting, (uint8_t)SettingsOptions::PresetTemp3, F_TEMP, 100, MAX_TEMP_C, 5, 3},
    SETTING(kItemStep, TempChangeShortStep, F_STEP, 1, TEMP_CHANGE_SHORT_STEP_MAX, 1),
    SETTING(kItemPower, PowerLimit, F_WATTS, 0, MAX_POWER_LIMIT, POWER_LIMIT_STEPS),
    SETTING(kItemVoltage, PDVoltage, F_PD, 0, 3, 1),
    {kItemCalib, Kind::Submenu, 0xFF, F_NONE, 0, 0, 0, 0}, // 0xFF: opens the calibration screen
    {kItemCalReset, Kind::Action, A_CALRESET, F_NONE, 0, 0, 0, 0},
};
const Item kSystemItems[] = {
    SETTING(kItemSound, BuzzerVolume, F_OFFNUM, 0, 10, 1),
    SETTING(kItemBright, DisplayBrightness, F_BRIGHT, MIN_BRIGHTNESS, MAX_BRIGHTNESS, BRIGHTNESS_STEP),
    SETTING(kItemHand, OrientationMode, F_ORIENT, 0, MAX_ORIENTATION_MODE, 1),
    SETTING(kItemButtons, ReverseButtonTempChangeEnabled, F_REVERSE, 0, 1, 1),
    SETTING(kItemLanguage, HS02UiLanguage, F_LANG, 0, 2, 1),
    SETTING(kItemGif, StandbyGif, F_GIF, 0, 6, 1),
    SETTING(kItemBootGif, BootGif, F_ONOFF, 0, 1, 1),
    // Hidden before: a value left on by an older firmware inverted the whole screen with no way back.
    SETTING(kItemInvert, DisplayInversion, F_ONOFF, 0, 1, 1),
    {kItemUpdate, Kind::Action, A_UPDATE, F_NONE, 0, 0, 0, 0},
    {kItemReset, Kind::Action, A_RESET, F_NONE, 0, 0, 0, 0},
    {kItemVersion, Kind::Info, 0, F_NONE, 0, 0, 0, 0},
};
const Item kAdvancedItems[] = {
    {kItemIronOS, Kind::Action, A_IRONOS, F_NONE, 0, 0, 0, 0},
    {kItemDiag, Kind::Action, A_DIAG, F_NONE, 0, 0, 0, 0},
};

struct Menu {
  TextFn      title;
  const Item *items;
  uint8_t     count;
};
const Menu kMenus[] = {
    {kMenuTitle, kRootItems, sizeof(kRootItems) / sizeof(Item)},
    {kMenuSleep, kSleepItems, sizeof(kSleepItems) / sizeof(Item)},
    {kMenuTip, kTipItems, sizeof(kTipItems) / sizeof(Item)},
    {kMenuSystem, kSystemItems, sizeof(kSystemItems) / sizeof(Item)},
    {kMenuAdvanced, kAdvancedItems, sizeof(kAdvancedItems) / sizeof(Item)},
};

// ---------------------------------------------------------------- formatting
char *put(char *out, const char *s) {
  while (*s) {
    *out++ = *s++;
  }
  *out = '\0';
  return out;
}
char *putNumber(char *out, uint32_t v) {
  char    tmp[10];
  uint8_t n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n) {
    *out++ = tmp[--n];
  }
  *out = '\0';
  return out;
}

void formatValue(const Item &item, char *out) {
  const uint16_t v = getSettingValue((SettingsOptions)item.arg);
  char          *p = out;
  switch (item.fmt) {
  case F_TEMP:
  case F_STEP:
    put(putNumber(p, v), "°");
    break;
  case F_SLEEP: // IronOS: 0 off, 1..5 = x10 s, 6..15 = (n-5) min
    if (!v) {
      put(p, kValOff());
    } else if (v < 6) {
      p = putNumber(p, v * 10);
      *p++ = ' ';
      put(p, kSeconds());
    } else {
      p = putNumber(p, v - 5);
      *p++ = ' ';
      put(p, kValMin());
    }
    break;
  case F_MINUTES:
    if (!v) {
      put(p, kValOff());
    } else {
      p = putNumber(p, v);
      *p++ = ' ';
      put(p, kValMin());
    }
    break;
  case F_OFFNUM:
    if (!v) {
      put(p, kValOff());
    } else {
      putNumber(p, v);
    }
    break;
  case F_WATTS:
    if (!v) {
      put(p, kValMax());
    } else {
      p = putNumber(p, v);
      *p++ = ' ';
      put(p, kWatt());
    }
    break;
  case F_PD: {
    static const uint8_t volts[4] = {9, 12, 15, 20};
    p                             = putNumber(p, volts[v > 3 ? 3 : v]);
    *p++                          = ' ';
    put(p, kVolt());
  } break;
  case F_BRIGHT: // 1..105 in steps of 13 -> level 1..9
    putNumber(p, (v - MIN_BRIGHTNESS) / BRIGHTNESS_STEP + 1);
    break;
  case F_ORIENT:
    put(p, v == 0 ? kValRight() : (v == 1 ? kValLeft() : kValAuto()));
    break;
  case F_REVERSE:
    put(p, v ? kValSwapped() : kValNormal());
    break;
  case F_LANG:
    put(p, v == 1 ? kValLangRu() : (v == 2 ? kValLangEn() : kValLangAuto()));
    break;
  case F_ONOFF:
    put(p, v ? kValOn() : kValOff());
    break;
  case F_GIF: { // idle time before the standby GIF starts (same table as HomeScreen.cpp)
    static const uint16_t seconds[7] = {0, 5, 15, 30, 60, 120, 300};
    const uint16_t        s          = seconds[v > 6 ? 6 : v];
    if (!s) {
      put(p, kValOff());
    } else if (s < 60) {
      p    = putNumber(p, s);
      *p++ = ' ';
      put(p, kSeconds());
    } else {
      p    = putNumber(p, s / 60);
      *p++ = ' ';
      put(p, kValMin());
    }
  } break;
  default:
    *p = '\0';
    break;
  }
}

// Apply settings whose effect should be visible / audible immediately.
void applyLive(const Item &item) {
  switch ((SettingsOptions)item.arg) {
  case SettingsOptions::DisplayBrightness:
    Display::setBrightness(getSettingValue(SettingsOptions::DisplayBrightness));
    break;
  case SettingsOptions::DisplayInversion:
    Display::setInverseDisplay(getSettingValue(SettingsOptions::DisplayInversion));
    break;
  case SettingsOptions::BuzzerVolume:
    beep(80); // preview the new loudness
    break;
  case SettingsOptions::PDVoltage:
    applyPDVoltageSetting();
    break;
  case SettingsOptions::OrientationMode:
    if (getSettingValue(SettingsOptions::OrientationMode) != orientationMode_t::AUTO) {
      Display::setRotation(getSettingValue(SettingsOptions::OrientationMode) == orientationMode_t::LEFT);
    }
    break;
  default:
    break;
  }
}

void stepSetting(const Item &item, int8_t direction, uint8_t multiplier) {
  // Accelerate only where it helps (temperatures and the power limit), not for short lists.
  const bool    fast = item.fmt == F_TEMP || item.fmt == F_WATTS || item.fmt == F_MINUTES;
  int32_t       v    = getSettingValue((SettingsOptions)item.arg) + direction * item.step * (fast ? multiplier : 1);
  if (v < item.min) {
    v = item.min;
  } else if (v > item.max) {
    v = item.max;
  }
  setSettingValue((SettingsOptions)item.arg, (uint16_t)v);
  applyLive(item);
}

// ---------------------------------------------------------------- state
// scratch_state.state1 = menu id, state2 = cursor, state5 = editing, state6 = confirm action (0 none)
// state3 = last input tick, state4 = last repeat tick
constexpr TickType_t kTimeout = 60 * TICKS_SECOND;

OperatingMode leave(OperatingMode to) {
  saveSettings();
  return to;
}

} // namespace

OperatingMode gui_modernMenu(const ButtonState buttons, guiContext *cxt) {
  auto          &st   = cxt->scratch_state;
  const TickType_t now = xTaskGetTickCount();
  if (st.state3 == 0) {
    st.state3 = now; // first frame in the menu
    if (cxt->previousMode == OperatingMode::TipCalibration) {
      st.state1 = M_TIP; // come back to where calibration was started
      for (uint8_t i = 0; i < sizeof(kTipItems) / sizeof(Item); i++) {
        if (kTipItems[i].kind == Kind::Submenu && kTipItems[i].arg == 0xFF) {
          st.state2 = i;
        }
      }
    }
  }
  if (buttons != BUTTON_NONE) {
    st.state3 = now;
  } else if (now - st.state3 > kTimeout) {
    return leave(OperatingMode::HomeScreen);
  }
  cxt->transitionMode   = TransitionAnimation::None;
  currentTempTargetDegC = 0; // heater always off while in the menu

  uint8_t     menuId = st.state1 < sizeof(kMenus) / sizeof(Menu) ? st.state1 : 0;
  const Menu &menu   = kMenus[menuId];
  uint8_t     cursor = st.state2 < menu.count ? st.state2 : 0;
  const Item &item   = menu.items[cursor];

  // Translate +/- into a direction (held = repeat ~8/s); honour the "swap buttons" setting.
  int8_t  direction  = 0;
  uint8_t multiplier = 1;
  if (buttons == BUTTON_F_SHORT || buttons == BUTTON_B_SHORT || buttons == BUTTON_F_LONG || buttons == BUTTON_B_LONG) {
    multiplier = hs02RepeatMultiplier(buttons == BUTTON_F_LONG || buttons == BUTTON_B_LONG);
    if (multiplier) {
      direction = (buttons == BUTTON_F_SHORT || buttons == BUTTON_F_LONG) ? 1 : -1;
      if (getSettingValue(SettingsOptions::ReverseButtonTempChangeEnabled)) {
        direction = -direction;
      }
    }
  }

  // ---- confirmation dialog
  if (st.state6) {
    const uint8_t action = (uint8_t)st.state6;
    if (buttons == BUTTON_OK_SHORT) {
      if (action == A_UPDATE) {
        saveSettings();
        enterFirmwareUpdateMode(); // does not return
      }
      if (action == A_CALRESET) {
        hs02ResetTipCal();
        beep(80);
        st.state6 = 0;
        return OperatingMode::ModernMenu;
      }
      resetSettings();
      Display::setBrightness(getSettingValue(SettingsOptions::DisplayBrightness));
      applyPDVoltageSetting();
      beep(80);
      return OperatingMode::HomeScreen;
    }
    if (buttons == BUTTON_OK_LONG) {
      st.state6 = 0;
    } else {
      if (action == A_UPDATE) {
        ui_draw_menu_confirm(kUpdateTitle(), kUpdateLine1(), kUpdateLine2(), kConfirmHint(), false);
      } else if (action == A_CALRESET) {
        ui_draw_menu_confirm(kCalTitle(), kCalReset1(), kCalReset2(), kConfirmHint(), true);
      } else {
        ui_draw_menu_confirm(kResetTitle(), kResetLine1(), kResetLine2(), kConfirmHint(), true);
      }
      return OperatingMode::ModernMenu;
    }
  }

  // ---- input
  if (st.state5) { // editing a value
    if (direction) {
      stepSetting(item, direction, multiplier);
    } else if (buttons == BUTTON_OK_SHORT || buttons == BUTTON_OK_LONG) {
      st.state5 = 0;
      saveSettings();
    }
  } else if (direction) {
    cursor    = (uint8_t)((cursor + menu.count + direction) % menu.count);
    st.state2 = cursor;
  } else if (buttons == BUTTON_OK_SHORT) {
    switch (item.kind) {
    case Kind::Submenu:
      if (item.arg == 0xFF) {
        return leave(OperatingMode::TipCalibration);
      }
      st.state1 = item.arg;
      st.state2 = 0;
      break;
    case Kind::Setting:
      st.state5 = 1;
      break;
    case Kind::Action:
      switch (item.arg) {
      case A_EXIT:
        return leave(OperatingMode::HomeScreen);
      case A_IRONOS:
        return leave(OperatingMode::SettingsMenu);
      case A_DIAG:
        return leave(OperatingMode::DebugMenuReadout);
      default:
        st.state6 = item.arg; // update / reset ask first
        break;
      }
      break;
    case Kind::Info:
    default:
      break;
    }
  } else if (buttons == BUTTON_OK_LONG) {
    if (menuId == M_ROOT) {
      return leave(OperatingMode::HomeScreen);
    }
    // Back to the parent (root), with the cursor on the submenu we came from.
    for (uint8_t i = 0; i < sizeof(kRootItems) / sizeof(Item); i++) {
      if (kRootItems[i].kind == Kind::Submenu && kRootItems[i].arg == menuId) {
        st.state2 = i;
      }
    }
    st.state1 = M_ROOT;
  }

  if (st.state6) {
    return OperatingMode::ModernMenu; // dialog is drawn next frame
  }

  // ---- draw (menu may have changed above)
  menuId              = st.state1;
  const Menu &shown   = kMenus[menuId];
  cursor              = st.state2;
  const uint8_t first = (shown.count <= 3 || cursor == 0) ? 0 : (cursor >= shown.count - 1 ? shown.count - 3 : cursor - 1);
  static char   labels[3][28];
  static char   values[3][16];
  Hs02MenuRow   rows[3];
  uint8_t       rowCount = 0;
  for (uint8_t i = first; i < shown.count && rowCount < 3; i++, rowCount++) {
    const Item &it = shown.items[i];
    char       *l  = put(labels[rowCount], it.label());
    if (it.labelNumber) {
      *l++ = ' ';
      putNumber(l, it.labelNumber);
    }
    values[rowCount][0] = '\0';
    if (it.kind == Kind::Setting) {
      formatValue(it, values[rowCount]);
    } else if (it.kind == Kind::Info) {
      put(values[rowCount], BUILD_VERSION);
    }
    rows[rowCount] = {labels[rowCount], values[rowCount], (uint8_t)(it.kind == Kind::Submenu ? HS02_ROW_SUBMENU : 0)};
  }
  ui_draw_menu(shown.title(), cursor, shown.count, rows, rowCount, cursor - first, st.state5 != 0);
  return OperatingMode::ModernMenu;
}

#else
OperatingMode gui_modernMenu(const ButtonState buttons, guiContext *cxt) {
  (void)buttons;
  (void)cxt;
  return OperatingMode::SettingsMenu;
}
#endif
