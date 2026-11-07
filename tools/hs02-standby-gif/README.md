# Standby GIF converter

`gif2hs02.py` turns any GIF into the standby screensaver of the HS-02 colour UI
(*Settings → System → GIF screensaver*). It writes
`source/Core/Threads/UI/drawing/color_160x80/Hs02StandbyGif.hpp`, which
`draw_standby_gif.cpp` plays full screen.

```sh
pip install pillow
python3 tools/hs02-standby-gif/gif2hs02.py                          # custom/standby.gif
python3 tools/hs02-standby-gif/gif2hs02.py my.gif --preview prev.gif
```

CI runs it on `custom/standby.gif` before every build, so replacing that file and pushing is
enough to get firmware with your animation.

## Options

| Option | Default | What it does |
| :-- | :-- | :-- |
| `--fit contain\|cover\|stretch` | `contain` | whole GIF with borders · fill the screen and crop · distort to 160×80 |
| `--bg COLOUR` | `#000000` | colour of the borders and of transparent pixels |
| `--resample auto\|nearest\|smooth` | `auto` | `auto` keeps pixel art sharp on exact integer upscales |
| `--band-height N` | `4` | rows per 4-colour band (smaller = more colours, more flash) |
| `--palette frame\|global` | `frame` | best colours per frame, or one set for the whole GIF (smaller, no colour flicker) |
| `--dither` | off | Floyd–Steinberg dithering: smoother gradients, much more flash |
| `--speed X` | `1.0` | playback speed factor |
| `--max-frames N` | `200` | frame limit (every other frame is dropped above it) |
| `--max-bytes N` | `24576` | flash budget; the HS-02 has roughly 33 KB free, the linker stops a build that doesn't fit |
| `--preview FILE` | | GIF (3×) of exactly what the iron will show, decoded from the generated data |

## Format

The 160×80 frame is 2 bits per pixel; the LCD driver gives every band of rows its own 4 RGB565
colours (`ColorBand.hpp`). Per frame the tool picks the 4 most representative colours of every
band (frequent colours plus distinct details), maps the pixels to them and packs the frame with a
PackBits-style RLE (`c < 128`: `c+1` literal bytes; `c >= 128`: next byte `c-126` times). Frame 0
is stored whole; every other frame whole or as an XOR patch over frame 0, whichever is smaller.
Identical frames and band palettes are stored once. The firmware unpacks straight into the
framebuffer, so playback needs no extra frame buffer.

`make_sample.py` draws the bundled example (`custom/standby.gif`).
