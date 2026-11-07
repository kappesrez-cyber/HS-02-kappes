#pragma once
#include "ColorBand.hpp"
#include "Font.h"
#include "OperatingModes.h"
#include <stdint.h>
#define FONT_SMALL_HEIGHT 16
#define FONT_SMALL_WIDTH 12
#define FONT_LARGE_HEIGHT 32
#define FONT_LARGE_WIDTH 24
class Display {
public:
  static void clearColor();
  static void setColorMode(bool) {}
  static void clearScreenColor() { clearColor(); }
  static uint8_t *colorFrameBuffer();
  static void setColorPalette(const uint16_t *palette);
  static void setColorBands(const ColorBand *bands, uint8_t count);
  static void fillRectColor(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t color);
  static void drawRingColor(uint8_t cx, uint8_t cy, uint8_t r, uint8_t thickness, uint8_t color, float start, float end);
  static void drawTickColor(uint8_t cx, uint8_t cy, float angle, uint8_t inner, uint8_t outer, uint8_t color);
  static void printColor(const char *text, uint8_t x, uint8_t y, FontStyle style, uint8_t color, uint8_t maxChars = 255);
  static void printNumberColor(uint16_t number, uint8_t places, uint8_t x, uint8_t y, FontStyle style, uint8_t color);
  static void drawBitmapColor(const uint8_t *bitmap, uint8_t width, uint8_t height, uint8_t x, uint8_t y, uint8_t color);
};

struct Hs02MenuRow {
  const char *label;
  const char *value;
  uint8_t     flags;
};
enum Hs02MenuRowFlags : uint8_t { HS02_ROW_SUBMENU = 1 };
void ui_draw_menu(const char *title, uint8_t position, uint8_t count, const Hs02MenuRow *rows, uint8_t rowCount, uint8_t selected, bool editing);
void ui_draw_menu_confirm(const char *title, const char *line1, const char *line2, const char *hint, bool danger);
void ui_draw_alert(const char *title, const char *line1, const char *line2, const char *hint, uint32_t accentRgb);
enum Hs02Warning : uint8_t { HS02_WARN_SETTINGS_RESET, HS02_WARN_NO_ACCEL, HS02_WARN_NO_PD, HS02_WARN_RUNAWAY };
void ui_draw_warning(uint8_t which);
void ui_draw_warning_undervoltage_idle(void);
enum class Hs02Gif : uint8_t { Standby = 0, Boot = 1 };
bool     ui_gif_available(Hs02Gif kind);
uint32_t ui_gif_duration_ms(Hs02Gif kind);
bool     ui_draw_gif(Hs02Gif kind, uint32_t elapsedMs);
void ui_draw_calibration(uint8_t step, TemperatureType_t current, TemperatureType_t setpoint, TemperatureType_t reading, bool ready);
