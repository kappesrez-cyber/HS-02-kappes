// Renders every HS-02 colour screen with the production drawing code (draw_home_gauge.cpp)
// into raw PPM frames for the README. Used by tools/hs02-wasm-preview/render_docs.sh.
#include "../src/firmware_ui_host.cpp"
#include <cstdio>
#include <cstdlib>

static const char *outDir = ".";
static void dump(const char *name) {
  const uint16_t *px = hs02_ui_rgb565();
  char            fn[512];
  snprintf(fn, sizeof(fn), "%s/%s.ppm", outDir, name);
  FILE *f = fopen(fn, "wb");
  fprintf(f, "P6 160 80 255\n");
  for (int i = 0; i < 160 * 80; i++) {
    const uint16_t v    = px[i];
    unsigned char  p[3] = {(unsigned char)(((v >> 11) & 31) * 255 / 31), (unsigned char)(((v >> 5) & 63) * 255 / 63), (unsigned char)((v & 31) * 255 / 31)};
    fwrite(p, 1, 3, f);
  }
  fclose(f);
}

int main(int argc, char **argv) {
  using namespace Hs02UiText;
  if (argc > 1) {
    outDir = argv[1];
  }
  if (argc > 2) {
    hostUiLanguage = (uint16_t)atoi(argv[2]); // 1 = Russian, 2 = English
  }
  // tip, target, boost, W x10, V x10, source, fault(0 ok / 1 no tip / 2 short)
  hs02_ui_set_state(27, 330, 420, 0, 200, 0, 0);
  hs02_ui_render(0);
  dump("standby");
  hs02_ui_set_state(214, 330, 420, 0, 200, 0, 0);
  hs02_ui_render(0);
  dump("standby_hot");
  hs02_ui_set_state(186, 330, 420, 780, 200, 0, 0);
  hs02_ui_render(1);
  dump("heating");
  hs02_ui_set_state(331, 330, 420, 380, 200, 0, 0);
  hs02_ui_render(1);
  dump("ready");
  hs02_ui_set_state(352, 300, 420, 0, 200, 0, 0);
  hs02_ui_render(1);
  dump("cooling");
  hs02_ui_set_state(162, 330, 420, 0, 200, 0, 0);
  hs02_ui_render(3);
  dump("sleep");
  hs02_ui_set_state(0, 330, 420, 0, 200, 0, 1);
  hs02_ui_render(0);
  dump("no_tip");

  Hs02MenuRow root[3] = {{kMenuSleep(), "", HS02_ROW_SUBMENU}, {kMenuTip(), "", HS02_ROW_SUBMENU}, {kMenuSystem(), "", HS02_ROW_SUBMENU}};
  Display::clearColor();
  ui_draw_menu(kMenuTitle(), 1, 5, root, 3, 1, false);
  dump("menu_root");
  char p1[32], p2[32], p3[32];
  snprintf(p1, sizeof p1, "%s 1", kItemPreset());
  snprintf(p2, sizeof p2, "%s 2", kItemPreset());
  snprintf(p3, sizeof p3, "%s 3", kItemPreset());
  Hs02MenuRow tip[3] = {{p1, "300°", 0}, {p2, "330°", 0}, {p3, "360°", 0}};
  Display::clearColor();
  ui_draw_menu(kMenuTip(), 1, 8, tip, 3, 1, true);
  dump("menu_edit");
  Hs02MenuRow sys[3] = {{kItemSound(), "5", 0}, {kItemBright(), "9", 0}, {kItemHand(), kValAuto(), 0}};
  Display::clearColor();
  ui_draw_menu(kMenuSystem(), 0, 8, sys, 3, 0, false);
  dump("menu_system");
  Display::clearColor();
  ui_draw_menu_confirm(kUpdateTitle(), kUpdateLine1(), kUpdateLine2(), kConfirmHint(), false);
  dump("menu_update");

  Display::clearColor();
  ui_draw_calibration(0, 96, 150, 150, false);
  dump("calib_heating");
  Display::clearColor();
  ui_draw_calibration(1, 249, 250, 254, true);
  dump("calib_ready");
  Display::clearColor();
  ui_draw_warning(HS02_WARN_SETTINGS_RESET);
  dump("warn_reset");
  Display::clearColor();
  ui_draw_warning(HS02_WARN_RUNAWAY);
  dump("warn_runaway");

  // Boot animation frames (same easing as BSP showBootLogo()).
  for (int f = 0; f <= 24; f++) {
    const uint32_t t = f * 1000 / 24, e = t * t * (3000 - 2 * t) / 1000000;
    hs02_ui_set_state((uint16_t)e, 330, 420, 0, 200, 0, 0);
    hs02_ui_render(4);
    char name[32];
    snprintf(name, sizeof name, "boot_%02d", f);
    dump(name);
  }

  // Standby GIF screensaver: one frame every 20 ms over one loop (make_images.py assembles them).
  {
    const uint32_t loopMs = Hs02StandbyGif::kTotalMs, step = 20;
    uint32_t       n      = 0;
    for (uint32_t ms = 0; ms < loopMs && n < 500; ms += step, n++) {
      hs02_ui_set_state((uint16_t)(ms / 10), 330, 420, 0, 200, 0, 0);
      hs02_ui_render(5);
      char name[32];
      snprintf(name, sizeof name, "gif_%03u", (unsigned)n);
      dump(name);
    }
  }
  return 0;
}
