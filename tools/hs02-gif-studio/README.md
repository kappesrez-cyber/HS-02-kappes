# HS-02 GIF Studio

A web constructor that puts your own GIFs into a finished HS-02 Modern UI firmware file, without rebuilding:

- **standby screensaver** (*Settings → System → GIF screensaver*),
- **boot animation** (*Settings → System → Boot GIF*).

Load a GIF, drag the frame to choose the area (*Fit*: whole frame with borders, *Fill*: crop the edges, *Free*:
stretch any area), set speed, frame range and border colour, check the preview (decoded from the exact bytes the
iron will get) and download the `.bin`. Everything runs in the browser.

## Running it

- **GitHub Pages**: CI deploys it with the firmware of the current `main` (enable *Settings → Pages → Source:
  GitHub Actions* once). The page then offers the four builds (HS-02A/B × RU/EN) directly.
- **Locally**: serve this folder (`python3 -m http.server`) and open `index.html`; load a firmware `.bin` from a
  release with *Own .bin file*. (Put builds into `firmware/<MODEL>_<LANG>.bin` to have them offered.)
- **Command line** (CI uses it for `custom/*.gif`):
  `node cli.mjs --firmware HS02A_RU.bin --standby a.gif [--boot b.gif] [--crop contain|cover] [--speed 1] [--bg #000000] [-o out.bin]`

## Files

| File | What |
| :-- | :-- |
| `hs02gif.mjs` | everything that matters: GIF decoder, crop/scale, 2bpp band encoder, container, firmware patcher, preview decoder |
| `index.html` | the constructor UI |
| `cli.mjs` | command line front end |
| `make_sample.py` | draws the bundled example `custom/standby.gif` |

## How the GIF gets into the firmware

The 160×80 frame is 2 bits per pixel; the LCD driver gives every band of 4 rows its own 4 RGB565 colours. Per
frame the encoder picks the 4 most representative colours of every band (frequent colours plus distinct details),
maps the pixels and packs the frame with a PackBits-style RLE, whole or as an XOR patch over frame 0. Identical
frames and band palettes are stored once. Both animations go into one CRC-checked container appended right after
the firmware image; the firmware finds the spot through its `HS02SLOT` record (the linker symbol
`_hs02_assets_start`), so the tool only has to cut the file there, append the container and fix the 16-byte
bootloader header. The format is documented in `source/Core/Threads/UI/drawing/color_160x80/draw_gif.cpp`.
Space: about 28 KB for both animations (up to the IronOS settings page); when they don't fit, every other frame
of the larger one is dropped until they do.
