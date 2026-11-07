// Where user assets (the standby / boot GIFs) live: appended right after the firmware image.
//
// The web constructor (tools/hs02-gif-studio) takes a finished firmware .bin, finds this
// record by its tag, cuts the file at `assetsStart` (dropping any assets appended before) and
// appends a new asset container there, so no rebuild is needed. `assetsLimit` is the first
// address the container must not reach (the IronOS settings page).
#include "BSP.h"
#include "configuration.h"
#include <stdint.h>

extern const uint8_t _hs02_assets_start[]; // linker script: end of the image in flash

typedef struct {
  char           tag[8];      // "HS02SLOT"
  uint32_t       version;     // 1
  const uint8_t *assetsStart; // absolute flash address
  uint32_t       assetsLimit; // absolute flash address (exclusive)
  uint32_t       imageBase;   // flash address of the first byte of the .bin (without its 16-byte header)
} Hs02AssetSlot;

__attribute__((used)) const Hs02AssetSlot hs02AssetSlot = {
    {'H', 'S', '0', '2', 'S', 'L', 'O', 'T'}, 1, _hs02_assets_start, SETTINGS_START_PAGE, 0x08005800,
};

const uint8_t *hs02AssetArea(uint32_t *maxBytes) {
  // Read through a volatile pointer: the record must stay in the image (the tools look for it).
  const volatile Hs02AssetSlot *slot  = &hs02AssetSlot;
  const uint8_t                *start = slot->assetsStart;
  const uint32_t                limit = slot->assetsLimit;
  *maxBytes                           = (uint32_t)start < limit ? limit - (uint32_t)start : 0;
  return start;
}
