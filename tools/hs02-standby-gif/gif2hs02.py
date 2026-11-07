#!/usr/bin/env python3
"""Convert your own GIF into the HS-02 standby screensaver.

    python3 tools/hs02-standby-gif/gif2hs02.py                    # custom/standby.gif
    python3 tools/hs02-standby-gif/gif2hs02.py my.gif --preview preview.gif

Writes source/Core/Threads/UI/drawing/color_160x80/Hs02StandbyGif.hpp, which the firmware
plays full screen in standby (Settings > System > GIF screensaver). Rebuild the firmware
afterwards (or just push: CI converts custom/standby.gif on every build).

How a 2-bit screen shows a colour GIF
-------------------------------------
The firmware keeps one 160x80 frame at 2 bits per pixel (4 colours), but the LCD driver
gives every horizontal *band* of rows its own 4-colour palette (ColorBand.hpp). This tool
cuts every frame into bands of --band-height rows (default 4, i.e. 20 bands) and picks the
best 4 colours for each band of each frame, so a frame can show up to 80 colours in total.
Simple artwork with flat colours (pixel art, logos, cartoons) looks best; photos and heavily
dithered GIFs lose detail and take more flash.

Storage: frame 0 is the raw 2bpp frame (3200 bytes) packed with a PackBits-style RLE; every
other frame is packed either whole or as the XOR against frame 0, whichever is smaller (a
still background then costs almost nothing). Identical frames and band palettes are stored
once. When the result doesn't fit
--max-bytes, every other frame is dropped (its time goes to the frame before it) until it
fits. The HS-02 has ~35 KB of free flash; the default budget leaves room for future changes.

Requires Pillow (pip install pillow).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageColor, ImageSequence

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_INPUT = ROOT / "custom" / "standby.gif"
DEFAULT_OUTPUT = ROOT / "source/Core/Threads/UI/drawing/color_160x80/Hs02StandbyGif.hpp"

W, H = 160, 80
ROW_BYTES = W // 4
FRAME_BYTES = ROW_BYTES * H  # 3200
FRAME_ENTRY_SIZE = 8  # sizeof(Hs02StandbyGif::Frame)


# ---------------------------------------------------------------- loading & scaling
def load_frames(path: Path, fit: str, bg: tuple, resample: str, speed: float):
    """Returns [(RGB 160x80 image, duration ms)], with GIF disposal/transparency resolved."""
    src = Image.open(path)
    frames = []
    for frame in ImageSequence.Iterator(src):
        duration = frame.info.get("duration", src.info.get("duration", 100)) or 0
        if duration < 20:  # browsers treat 0/10 ms as 100 ms; so do we
            duration = 100
        duration = max(20, min(65535, round(duration / speed)))
        rgba = frame.convert("RGBA")
        frames.append((rgba, duration))
    if not frames:
        raise SystemExit(f"{path}: no frames")

    sw, sh = frames[0][0].size
    if fit == "stretch":
        tw, th = W, H
    else:
        scale = (min if fit == "contain" else max)(W / sw, H / sh)
        tw, th = max(1, round(sw * scale)), max(1, round(sh * scale))
    if resample == "auto":
        ratio = tw / sw
        integral = ratio >= 2 and abs(ratio - round(ratio)) < 1e-6 and th == round(sh * round(ratio))
        method = Image.NEAREST if integral or (tw, th) == (sw, sh) else Image.LANCZOS
    else:
        method = Image.NEAREST if resample == "nearest" else Image.LANCZOS

    out = []
    for rgba, duration in frames:
        scaled = rgba.resize((tw, th), method) if (tw, th) != rgba.size else rgba
        canvas = Image.new("RGBA", (W, H), bg + (255,))
        if fit == "cover":  # centre crop of the scaled frame
            x0, y0 = (tw - W) // 2, (th - H) // 2
            canvas.alpha_composite(scaled.crop((x0, y0, x0 + W, y0 + H)))
        else:
            canvas.alpha_composite(scaled, ((W - tw) // 2, (H - th) // 2))
        out.append((canvas.convert("RGB"), duration))
    return out


# ---------------------------------------------------------------- colour
def rgb565(c) -> int:
    r, g, b = c
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def rgb565_to_rgb(v: int):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))  # same rounding as to_565_image


def to_565_image(img: Image.Image) -> Image.Image:
    """Round every pixel to what the panel can show, so equal panel colours merge."""
    five = [(v & 0xF8) | (v >> 5) for v in range(256)]
    six = [(v & 0xFC) | (v >> 6) for v in range(256)]
    return img.point(five + six + five)


def nearest(colour, palette):
    return min(range(len(palette)), key=lambda i: distance(colour, palette[i]))


def distance(a, b) -> int:
    # Weighted RGB distance (cheap approximation of perceived difference).
    return 2 * (a[0] - b[0]) ** 2 + 4 * (a[1] - b[1]) ** 2 + 3 * (a[2] - b[2]) ** 2


def cluster(counts, k: int):
    """k representative colours of [(count, rgb)].

    Seeds with the most common colour, then keeps adding the colour that is both frequent and
    far from the chosen ones (so a small but distinct detail such as a glowing tip keeps its own
    colour instead of being averaged away), then refines with a few weighted k-means passes.
    """
    counts = sorted(counts, reverse=True)
    centres = [counts[0][1]]
    while len(centres) < k:
        best = max(counts, key=lambda nc: (nc[0] ** 0.3) * min(distance(nc[1], c) for c in centres))
        centres.append(best[1])
    for _ in range(4):
        sums = [[0, 0, 0, 0] for _ in centres]
        for n, col in counts:
            i = min(range(len(centres)), key=lambda j: distance(col, centres[j]))
            acc = sums[i]
            acc[0] += n
            acc[1] += n * col[0]
            acc[2] += n * col[1]
            acc[3] += n * col[2]
        centres = [(round(a[1] / a[0]), round(a[2] / a[0]), round(a[3] / a[0])) if a[0] else c for a, c in zip(sums, centres)]
    return centres


def pick_palette(strips: list[Image.Image], dither: bool):
    """Best 4 colours for the given band strip(s); returns sorted [(r,g,b)] of length 4."""
    if len(strips) == 1:
        joined = strips[0]
    else:
        joined = Image.new("RGB", (W, sum(s.height for s in strips)))
        y = 0
        for s in strips:
            joined.paste(s, (0, y))
            y += s.height
    counts = joined.getcolors(W * H * len(strips))
    if len(counts) <= 4:  # few enough distinct colours: exact
        pal = [c for _, c in counts]
    else:
        pal = cluster(counts, 4)
    pal = sorted({rgb565(c) for c in pal})
    while len(pal) < 4:
        pal.append(pal[-1] if pal else 0)
    return [rgb565_to_rgb(v) for v in pal]


def map_strip(strip: Image.Image, palette, dither: bool):
    """Returns the strip's pixels as palette indices (row-major)."""
    if dither:
        pal_img = Image.new("P", (1, 1))
        flat = [v for c in palette for v in c]
        pal_img.putpalette(flat + flat[:3] * (256 - 4))
        q = strip.quantize(palette=pal_img, dither=Image.Dither.FLOYDSTEINBERG)
        return [min(i, 3) for i in q.tobytes()]
    cache = {}
    out = []
    raw = strip.tobytes()
    for i in range(0, len(raw), 3):
        c = (raw[i], raw[i + 1], raw[i + 2])
        idx = cache.get(c)
        if idx is None:
            idx = cache[c] = nearest(c, palette)
        out.append(idx)
    return out


# ---------------------------------------------------------------- packing
def pack_2bpp(indices: list[int]) -> bytes:
    """160x80 indices -> the firmware frame layout: byte y*40 + x/4, pixel x%4 at bits 2*(x%4)."""
    out = bytearray(FRAME_BYTES)
    for i, v in enumerate(indices):
        y, x = divmod(i, W)
        out[y * ROW_BYTES + x // 4] |= (v & 3) << ((x % 4) * 2)
    return bytes(out)


def rle_encode(data: bytes) -> bytes:
    """PackBits-style: c < 128 -> c+1 literal bytes follow; c >= 128 -> next byte repeated c-126 times."""
    out = bytearray()
    literal = bytearray()
    i, n = 0, len(data)

    def flush():
        while literal:
            chunk = literal[:128]
            out.append(len(chunk) - 1)
            out.extend(chunk)
            del literal[:128]

    while i < n:
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 129:
            run += 1
        if run >= 3:
            flush()
            out.append(126 + run)
            out.append(data[i])
            i += run
        else:
            literal.extend(data[i:i + run])
            i += run
    flush()
    return bytes(out)


def rle_decode(data: bytes, size: int = FRAME_BYTES) -> bytes:
    """Reference decoder (mirrors draw_standby_gif.cpp); used to self-check the output."""
    out = bytearray()
    i = 0
    while len(out) < size and i < len(data):
        c = data[i]
        i += 1
        if c < 128:
            out.extend(data[i:i + c + 1])
            i += c + 1
        else:
            out.extend(bytes([data[i]]) * (c - 126))
            i += 1
    return bytes(out[:size])


# ---------------------------------------------------------------- encoding
def quantize_frames(frames, band_h: int, palette_mode: str, dither: bool):
    """Returns bands [(y, h)] and per frame (packed 2bpp bytes, (band palettes as 565 tuples), ms)."""
    bands = [(y, min(band_h, H - y)) for y in range(0, H, band_h)]
    global_pals = None
    if palette_mode == "global":
        global_pals = [pick_palette([img.crop((0, y, W, y + h)) for img, _ in frames], dither) for y, h in bands]
    out = []
    for img, duration in frames:
        indices, frame_pal = [], []
        for b, (y, h) in enumerate(bands):
            strip = img.crop((0, y, W, y + h))
            pal = global_pals[b] if global_pals else pick_palette([strip], dither)
            frame_pal.append(tuple(rgb565(c) for c in pal))
            indices.extend(map_strip(strip, pal, dither))
        packed = pack_2bpp(indices)
        if out and out[-1][0] == packed and out[-1][1] == tuple(frame_pal) and out[-1][2] + duration <= 65535:
            out[-1] = (packed, out[-1][1], out[-1][2] + duration)  # identical consecutive frames: merge
        else:
            out.append((packed, tuple(frame_pal), duration))
    return bands, out


def encode(bands, qframes):
    """Builds the tables of the header.

    Frame 0 is stored whole (the key frame). Every other frame is stored either whole or as
    the XOR against the key frame, whichever packs smaller: with a still background the
    XOR is mostly zeros. The firmware then needs no extra RAM, it just unpacks the key frame
    and XORs the patch over it.
    """
    colors, color_ids = [], {}  # unique 4-colour band palettes
    rows, row_ids = [], {}  # per-frame band -> palette index rows
    blobs, blob_ids = [], {}
    entries = []  # [blob, ms, row, xor]
    key = qframes[0][0]
    for i, (packed, frame_pal, duration) in enumerate(qframes):
        row = []
        for pal in frame_pal:
            if pal not in color_ids:
                color_ids[pal] = len(colors)
                colors.append(pal)
            row.append(color_ids[pal])
        row = tuple(row)
        if row not in row_ids:
            row_ids[row] = len(rows)
            rows.append(row)
        whole = rle_encode(packed)
        patch = rle_encode(bytes(a ^ b for a, b in zip(packed, key))) if i else None
        blob_key = (True, patch) if patch is not None and len(patch) < len(whole) else (False, whole)
        if blob_key not in blob_ids:
            blob_ids[blob_key] = len(blobs)
            blobs.append(blob_key)
        entries.append([blob_ids[blob_key], duration, row_ids[row], blob_key[0]])
    return {"bands": bands, "colors": colors, "rows": rows, "blobs": blobs, "entries": entries, "key": key}


def index_type(n: int) -> tuple[str, int]:
    return ("uint8_t", 1) if n <= 256 else ("uint16_t", 2)


def flash_size(enc) -> int:
    _, idx = index_type(len(enc["colors"]))
    return (len(enc["colors"]) * 8 + len(enc["rows"]) * len(enc["bands"]) * idx + len(enc["bands"]) +
            sum(len(b) for _, b in enc["blobs"]) + len(enc["entries"]) * FRAME_ENTRY_SIZE)


def drop_every_other(frames):
    out = []
    for i in range(0, len(frames), 2):
        img, d = frames[i]
        if i + 1 < len(frames):
            d = min(65535, d + frames[i + 1][1])
        out.append((img, d))
    return out


# ---------------------------------------------------------------- output
def write_header(path: Path, source_name: str, enc):
    offsets, total = [], 0
    for _, b in enc["blobs"]:
        offsets.append(total)
        total += len(b)
    data = b"".join(b for _, b in enc["blobs"])
    itype, _ = index_type(len(enc["colors"]))
    lines = [
        "#pragma once",
        f"// Generated by tools/hs02-standby-gif/gif2hs02.py from {source_name} -- do not edit by hand.",
        "// Replace custom/standby.gif and re-run the tool (CI does this on every build).",
        "#include <stdint.h>",
        "",
        "namespace Hs02StandbyGif {",
        "struct Frame {",
        "  uint16_t dataOffset; // into kData: PackBits-style RLE of one 3200-byte 2bpp frame",
        "  uint16_t durationMs;",
        "  uint16_t paletteRow; // index into kFramePalettes",
        "  uint16_t xorKey;     // 1: the data is XORed over frame 0 instead of replacing it",
        "};",
        f"typedef {itype} PaletteIndex;",
        f"constexpr uint8_t  kBandCount  = {len(enc['bands'])};",
        f"constexpr uint16_t kFrameCount = {len(enc['entries'])};",
        f"constexpr uint32_t kTotalMs    = {sum(e[1] for e in enc['entries'])};",
        f"constexpr uint16_t kDataSize   = {len(data)};",
        "",
        "// First row of every band.",
        "const uint8_t kBandStart[kBandCount] = {" + ", ".join(str(y) for y, _ in enc["bands"]) + "};",
        "",
        "// Unique 4-colour band palettes (RGB565).",
        f"const uint16_t kColors[{len(enc['colors'])}][4] = {{",
    ]
    for pal in enc["colors"]:
        lines.append("    {" + ", ".join(f"0x{v:04X}" for v in pal) + "},")
    lines.append("};")
    lines.append("")
    lines.append("// Palette of every band, per frame (indices into kColors).")
    lines.append(f"const PaletteIndex kFramePalettes[{len(enc['rows'])}][kBandCount] = {{")
    for row in enc["rows"]:
        lines.append("    {" + ", ".join(str(v) for v in row) + "},")
    lines.append("};")
    lines.append("")
    lines.append("const Frame kFrames[kFrameCount] = {")
    for blob, duration, row, xor in enc["entries"]:
        lines.append(f"    {{{offsets[blob]}, {duration}, {row}, {int(xor)}}},")
    lines.append("};")
    lines.append("")
    lines.append("const uint8_t kData[kDataSize] = {")
    for i in range(0, len(data), 20):
        lines.append("    " + ", ".join(f"0x{v:02X}" for v in data[i:i + 20]) + ",")
    lines.append("};")
    lines.append("} // namespace Hs02StandbyGif")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def decode_frame(enc, entry) -> bytes:
    """Unpacks one frame exactly like draw_standby_gif.cpp."""
    blob, _, _, xor = entry
    raw = rle_decode(enc["blobs"][blob][1])
    assert len(raw) == FRAME_BYTES, "RLE stream too short"
    if xor:
        key = rle_decode(enc["blobs"][enc["entries"][0][0]][1])
        raw = bytes(a ^ b for a, b in zip(raw, key))
    return raw


def render_device_frames(enc):
    """Decodes the generated tables like the firmware; returns [(RGB image, ms)]."""
    out = []
    for entry in enc["entries"]:
        raw = decode_frame(enc, entry)
        img = Image.new("RGB", (W, H))
        px = img.load()
        row = enc["rows"][entry[2]]
        for b, (y0, h) in enumerate(enc["bands"]):
            cols = [rgb565_to_rgb(v) for v in enc["colors"][row[b]]]
            for y in range(y0, y0 + h):
                for x in range(W):
                    px[x, y] = cols[(raw[y * ROW_BYTES + x // 4] >> ((x % 4) * 2)) & 3]
        out.append((img, entry[1]))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gif", nargs="?", type=Path, default=DEFAULT_INPUT, help="input GIF (default: custom/standby.gif)")
    ap.add_argument("-o", "--output", type=Path, default=DEFAULT_OUTPUT, help="generated header")
    ap.add_argument("--fit", choices=["contain", "cover", "stretch"], default="contain",
                    help="contain: whole GIF, letterboxed (default); cover: fill the screen, crop; stretch: distort to 160x80")
    ap.add_argument("--bg", default="#000000", help="letterbox / transparency colour (default black)")
    ap.add_argument("--resample", choices=["auto", "nearest", "smooth"], default="auto",
                    help="auto: nearest for exact integer upscales (pixel art), smooth otherwise")
    ap.add_argument("--band-height", type=int, default=4, help="rows per 4-colour band (1-80, default 4)")
    ap.add_argument("--palette", choices=["frame", "global"], default="frame",
                    help="frame: best colours per frame (default); global: one set for the whole GIF (smaller, no colour flicker)")
    ap.add_argument("--dither", action="store_true", help="Floyd-Steinberg dithering (looks smoother, costs much more flash)")
    ap.add_argument("--speed", type=float, default=1.0, help="playback speed factor (2 = twice as fast)")
    ap.add_argument("--max-frames", type=int, default=200, help="frame limit before encoding (default 200)")
    ap.add_argument("--max-bytes", type=int, default=24 * 1024, help="flash budget in bytes (default 24576)")
    ap.add_argument("--preview", type=Path, help="also write a GIF of exactly what the iron will show (3x)")
    args = ap.parse_args(argv)

    if not 1 <= args.band_height <= H:
        ap.error("--band-height must be 1..80")
    if args.speed <= 0:
        ap.error("--speed must be > 0")
    if not args.gif.is_file():
        ap.error(f"{args.gif} not found")
    bg = ImageColor.getrgb(args.bg)[:3]

    frames = load_frames(args.gif, args.fit, bg, args.resample, args.speed)
    frames = [(to_565_image(img), d) for img, d in frames]
    source_frames = len(frames)
    while len(frames) > args.max_frames:
        frames = drop_every_other(frames)

    while True:
        enc = encode(*quantize_frames(frames, args.band_height, args.palette, args.dither))
        size = flash_size(enc)
        if size <= args.max_bytes and sum(len(b) for _, b in enc["blobs"]) < 65536:
            break
        if len(frames) == 1:
            raise SystemExit(f"error: a single frame needs {size} bytes, more than --max-bytes {args.max_bytes}. "
                             "Try --palette global, a larger --band-height, or simpler artwork.")
        print(f"  {len(frames)} frames need {size} bytes > {args.max_bytes}: dropping every other frame", file=sys.stderr)
        frames = drop_every_other(frames)

    # Self-check: every stored frame must decode back to exactly what was encoded.
    _, qframes = quantize_frames(frames, args.band_height, args.palette, args.dither)
    for entry, (packed, _, _) in zip(enc["entries"], qframes):
        assert decode_frame(enc, entry) == packed, "encoder self-check failed"

    try:
        name = args.gif.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        name = args.gif.name
    write_header(args.output, name, enc)
    entries = enc["entries"]
    total_ms = sum(e[1] for e in entries)
    print(f"wrote {args.output.relative_to(ROOT) if args.output.resolve().is_relative_to(ROOT) else args.output}: "
          f"{len(entries)} frames (from {source_frames}), {total_ms / 1000:.2f} s loop, "
          f"{len(enc['colors'])} band palettes, {sum(e[3] for e in entries)} XOR frames, {size} bytes of flash")

    if args.preview:
        shown = render_device_frames(enc)
        imgs = [img.resize((W * 3, H * 3), Image.NEAREST) for img, _ in shown]
        imgs[0].save(args.preview, save_all=True, append_images=imgs[1:], duration=[d for _, d in shown], loop=0, disposal=1)
        print(f"preview: {args.preview}")


if __name__ == "__main__":
    main()
