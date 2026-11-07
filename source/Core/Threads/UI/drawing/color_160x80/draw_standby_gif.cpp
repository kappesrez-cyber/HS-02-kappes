// Standby GIF screensaver for the HS-02 colour screen.
//
// The animation comes from custom/standby.gif, converted at build time by
// tools/hs02-standby-gif/gif2hs02.py into Hs02StandbyGif.hpp (see that tool for the format):
// every frame is one complete 2bpp frame packed with a PackBits-style RLE, either whole or as an
// XOR patch over frame 0, plus a 4-colour palette for every band of rows. Unpacking goes
// straight into the framebuffer, so playback needs no RAM beyond one row of band palettes.
#include "Hs02StandbyGif.hpp"
#include "ui_drawing.hpp"

#ifdef LCD_160x80

namespace {
constexpr uint16_t kFrameBytes = (160 * 80) / 4;

// c < 128: c + 1 literal bytes follow; c >= 128: the next byte repeats c - 126 times.
// With `xorOver` the bytes are XORed into `out` instead of replacing it.
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

void unpackFrame(uint16_t index, uint8_t *out) {
  using namespace Hs02StandbyGif;
  const uint8_t *end = kData + kDataSize;
  if (kFrames[index].xorKey) {
    unpack(kData + kFrames[0].dataOffset, end, out, false);
    unpack(kData + kFrames[index].dataOffset, end, out, true);
  } else {
    unpack(kData + kFrames[index].dataOffset, end, out, false);
  }
}

ColorBand gifBands[Hs02StandbyGif::kBandCount];
} // namespace

void ui_draw_standby_gif(uint32_t elapsedMs) {
  using namespace Hs02StandbyGif;
  // Find the frame that is due at this point of the loop.
  uint32_t t     = kTotalMs ? elapsedMs % kTotalMs : 0;
  uint16_t frame = 0;
  while (frame + 1 < kFrameCount && t >= kFrames[frame].durationMs) {
    t -= kFrames[frame].durationMs;
    frame++;
  }

  const PaletteIndex *row = kFramePalettes[kFrames[frame].paletteRow];
  for (uint8_t b = 0; b < kBandCount; b++) {
    gifBands[b].yStart = kBandStart[b];
    for (uint8_t i = 0; i < 4; i++) {
      gifBands[b].colors[i] = kColors[row[b]][i];
    }
  }
  Display::setColorBands(gifBands, kBandCount);
  unpackFrame(frame, Display::colorFrameBuffer());
}

#endif
