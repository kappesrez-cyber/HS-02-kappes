// User GIF animations for the HS-02 colour screen: the standby screensaver and the boot animation.
//
// The animations are not compiled in. The web constructor (tools/hs02-gif-studio) converts the
// user's GIFs and appends them as one "asset container" right after the firmware image
// (hs02AssetArea(), see Core/BSP/Fnirsi/assets_slot.c), so a finished .bin can be customised
// without rebuilding. Layout (little endian, everything 4-byte aligned):
//
//   container   "HS02GIF1", u32 size, u32 crc32 of bytes [16, size), u32 standby offset,
//               u32 boot offset (0 = none), 2 x u32 reserved                       (32 bytes)
//   animation   u16 frames, u8 bands, u8 index size (1/2), u32 loop ms, u16 colours, u16 rows,
//               u32 colours/rows/frames/data offsets, u32 data size              (32 bytes)
//               u8 band start rows[bands] (padded to 4)
//   colours     u16 RGB565[colours][4]          one 4-colour palette per band
//   rows        index[rows][bands]              which palette every band uses, per frame
//   frames      {u32 data offset, u16 ms, u16 row, u8 xor, 3 pad}[frames]
//   data        per frame: one complete 3200-byte 2bpp frame (byte y*40 + x/4, pixel x%4 in bits
//               2*(x%4)) packed PackBits-style: c < 128 -> c+1 literal bytes; c >= 128 -> next byte
//               c-126 times. With xor = 1 the frame is XORed over frame 0 instead of replacing it.
//
// Frames are unpacked straight into the framebuffer, so playback needs no frame buffer of its own.
#include "ui_drawing.hpp"

extern "C" const uint8_t *hs02AssetArea(uint32_t *maxBytes); // BSP.h (Fnirsi BSP: assets_slot.c)

#ifdef LCD_160x80

namespace {
constexpr uint16_t kFrameBytes = (160 * 80) / 4;
constexpr uint8_t  kMaxBands   = 40;

struct Container {
  char     magic[8];
  uint32_t size;
  uint32_t crc;
  uint32_t offsets[2]; // Hs02Gif::Standby, Hs02Gif::Boot
  uint32_t reserved[2];
};
struct Animation {
  uint16_t frameCount;
  uint8_t  bandCount;
  uint8_t  indexSize;
  uint32_t totalMs;
  uint16_t colorCount;
  uint16_t rowCount;
  uint32_t colorsOffset;
  uint32_t rowsOffset;
  uint32_t framesOffset;
  uint32_t dataOffset;
  uint32_t dataSize;
  // followed by uint8_t bandStart[bandCount]
};
struct Frame {
  uint32_t dataOffset;
  uint16_t durationMs;
  uint16_t row;
  uint8_t  xorKey;
  uint8_t  pad[3];
};
static_assert(sizeof(Container) == 32 && sizeof(Animation) == 32 && sizeof(Frame) == 12, "asset layout");

uint32_t crc32(const uint8_t *p, uint32_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  while (len--) {
    crc ^= *p++;
    for (uint8_t i = 0; i < 8; i++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

bool fits(uint32_t offset, uint32_t bytes, uint32_t size) { return offset <= size && bytes <= size - offset; }

// Structural check of one animation inside a container of `size` bytes.
const Animation *checkAnimation(const uint8_t *base, uint32_t offset, uint32_t size) {
  if (!offset || (offset & 3) || !fits(offset, sizeof(Animation), size)) {
    return nullptr;
  }
  const Animation *a    = reinterpret_cast<const Animation *>(base + offset);
  const uint32_t   rest = size - offset;
  if (!a->frameCount || !a->bandCount || a->bandCount > kMaxBands || (a->indexSize != 1 && a->indexSize != 2) || !a->colorCount || !a->rowCount
      || !fits(sizeof(Animation), a->bandCount, rest) || !fits(a->colorsOffset, (uint32_t)a->colorCount * 8, rest)
      || !fits(a->rowsOffset, (uint32_t)a->rowCount * a->bandCount * a->indexSize, rest) || !fits(a->framesOffset, (uint32_t)a->frameCount * sizeof(Frame), rest)
      || !fits(a->dataOffset, a->dataSize, rest) || ((a->colorsOffset | a->rowsOffset | a->framesOffset) & 3)) {
    return nullptr;
  }
  const uint8_t *bandStart = reinterpret_cast<const uint8_t *>(a + 1);
  if (bandStart[0] != 0) {
    return nullptr;
  }
  for (uint8_t b = 1; b < a->bandCount; b++) {
    if (bandStart[b] <= bandStart[b - 1] || bandStart[b] >= 80) {
      return nullptr;
    }
  }
  return a;
}

// Validated once (CRC over up to ~30 KB takes a few ms), then cached.
const Animation *animations[2];
bool             assetsChecked = false;

void checkAssets() {
  assetsChecked     = true;
  uint32_t       max  = 0;
  const uint8_t *base = hs02AssetArea(&max);
  if (!base || max < sizeof(Container)) {
    return;
  }
  const Container *c = reinterpret_cast<const Container *>(base);
  static const char kMagic[8] = {'H', 'S', '0', '2', 'G', 'I', 'F', '1'};
  for (uint8_t i = 0; i < 8; i++) {
    if (c->magic[i] != kMagic[i]) {
      return;
    }
  }
  if (c->size < sizeof(Container) || c->size > max || (c->size & 3) || crc32(base + 16, c->size - 16) != c->crc) {
    return;
  }
  for (uint8_t i = 0; i < 2; i++) {
    animations[i] = checkAnimation(base, c->offsets[i], c->size);
  }
}

const Animation *animation(Hs02Gif kind) {
  if (!assetsChecked) {
    checkAssets();
  }
  return (uint8_t)kind < 2 ? animations[(uint8_t)kind] : nullptr;
}

// Unpacks one RLE stream into `out` (or XORs it over `out`).
void unpack(const uint8_t *src, const uint8_t *srcEnd, uint8_t *out, bool xorOver) {
  uint16_t pos = 0;
  while (pos < kFrameBytes && src < srcEnd) {
    const uint8_t c     = *src++;
    uint16_t      count = c < 128 ? c + 1 : c - 126;
    if (count > kFrameBytes - pos) {
      count = kFrameBytes - pos;
    }
    if (c < 128) {
      for (uint16_t i = 0; i < count && src < srcEnd; i++, pos++) {
        const uint8_t value = *src++;
        out[pos]            = xorOver ? (uint8_t)(out[pos] ^ value) : value;
      }
    } else if (src < srcEnd) {
      const uint8_t value = *src++;
      for (uint16_t i = 0; i < count; i++, pos++) {
        out[pos] = xorOver ? (uint8_t)(out[pos] ^ value) : value;
      }
    }
  }
}

ColorBand gifBands[kMaxBands];
} // namespace

bool ui_gif_available(Hs02Gif kind) { return animation(kind) != nullptr; }

uint32_t ui_gif_duration_ms(Hs02Gif kind) {
  const Animation *a = animation(kind);
  return a ? a->totalMs : 0;
}

bool ui_draw_gif(Hs02Gif kind, uint32_t elapsedMs) {
  const Animation *a = animation(kind);
  if (!a) {
    return false;
  }
  const uint8_t *base   = reinterpret_cast<const uint8_t *>(a);
  const Frame   *frames = reinterpret_cast<const Frame *>(base + a->framesOffset);

  // The frame that is due at this point of the loop.
  uint32_t t     = a->totalMs ? elapsedMs % a->totalMs : 0;
  uint16_t frame = 0;
  while (frame + 1 < a->frameCount && t >= frames[frame].durationMs) {
    t -= frames[frame].durationMs;
    frame++;
  }
  const Frame &f = frames[frame];

  // Band palettes of this frame.
  const uint8_t  *bandStart = base + sizeof(Animation);
  const uint16_t *colors    = reinterpret_cast<const uint16_t *>(base + a->colorsOffset);
  const uint8_t  *row       = base + a->rowsOffset + (uint32_t)(f.row < a->rowCount ? f.row : 0) * a->bandCount * a->indexSize;
  for (uint8_t b = 0; b < a->bandCount; b++) {
    uint16_t index = a->indexSize == 2 ? (uint16_t)(row[b * 2] | (row[b * 2 + 1] << 8)) : row[b];
    if (index >= a->colorCount) {
      index = 0;
    }
    gifBands[b].yStart = bandStart[b];
    for (uint8_t i = 0; i < 4; i++) {
      gifBands[b].colors[i] = colors[index * 4 + i];
    }
  }
  Display::setColorBands(gifBands, a->bandCount);

  // Pixels.
  const uint8_t *data = base + a->dataOffset, *dataEnd = data + a->dataSize;
  uint8_t       *out  = Display::colorFrameBuffer();
  if (f.xorKey && f.dataOffset < a->dataSize && frames[0].dataOffset < a->dataSize) {
    unpack(data + frames[0].dataOffset, dataEnd, out, false);
    unpack(data + f.dataOffset, dataEnd, out, true);
  } else if (f.dataOffset < a->dataSize) {
    unpack(data + f.dataOffset, dataEnd, out, false);
  }
  return true;
}

#endif
