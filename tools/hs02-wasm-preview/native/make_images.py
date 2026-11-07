#!/usr/bin/env python3
"""Turns the raw 160x80 PPM frames from render_docs.cpp into README images.

usage: make_images.py <frames_dir> <docs_images_dir> <font_dir>
  frames_dir/{ru,en}/*.ppm  ->  docs_images_dir/{ru,en}/<screen>.png (framed, 3x)
                                docs_images_dir/{ru,en}/boot.gif
                                docs_images_dir/banner-{ru,en}.png  (1280x640 social preview)
                                docs_images_dir/standby_gif.gif     (standby GIF screensaver)
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SCALE = 3
PAD = 14
BEZEL = (22, 22, 24, 255)
BORDER = (58, 58, 62, 255)


def rounded_mask(size, radius):
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size[0] - 1, size[1] - 1), radius, fill=255)
    return mask


def frame(screen: Image.Image, scale=SCALE, transparent=True) -> Image.Image:
    """Scale the LCD frame up (nearest) and put it in a rounded device bezel."""
    lcd = screen.convert("RGB").resize((160 * scale, 80 * scale), Image.NEAREST)
    w, h = lcd.width + 2 * PAD, lcd.height + 2 * PAD
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0) if transparent else BEZEL)
    d = ImageDraw.Draw(out)
    d.rounded_rectangle((0, 0, w - 1, h - 1), 22, fill=BEZEL, outline=BORDER, width=2)
    out.paste(lcd, (PAD, PAD), rounded_mask(lcd.size, 6 * scale // 3))
    return out


def load(path: Path) -> Image.Image:
    return Image.open(path).convert("RGB")


def banner(frames: Path, out: Path, fonts: Path, subtitle: str):
    W, H = 1280, 640
    img = Image.new("RGB", (W, H))
    top, bottom = (12, 12, 16), (28, 28, 36)
    for y in range(H):
        t = y / (H - 1)
        img.paste(tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)), (0, y, W, y + 1))
    d = ImageDraw.Draw(img)
    title_font = ImageFont.truetype(str(fonts / "InterDisplay-Bold.otf"), 72)
    sub_font = ImageFont.truetype(str(fonts / "Inter-Medium.otf"), 30)
    d.text((80, 90), "HS-02 Modern UI", font=title_font, fill=(255, 255, 255))
    d.text((82, 185), subtitle, font=sub_font, fill=(142, 142, 147))
    # accent dots like the status pills
    for i, c in enumerate([(255, 159, 10), (48, 209, 88), (10, 132, 255), (100, 210, 255)]):
        d.ellipse((84 + i * 26, 244, 100 + i * 26, 260), fill=c)
    shots = ["heating", "ready", "menu_root"]
    tiles = [frame(load(frames / f"{s}.ppm"), scale=2) for s in shots]
    gap = 34
    total = sum(t.width for t in tiles) + gap * (len(tiles) - 1)
    x = (W - total) // 2
    for t in tiles:
        img.paste(t, (x, 352), t)
        x += t.width + gap
    img.save(out, optimize=True)


def main():
    frames_root, out_root, fonts = (Path(a) for a in sys.argv[1:4])
    subtitles = {
        "en": "Custom IronOS firmware for FNIRSI HS-02A / HS-02B soldering irons",
        "ru": "Кастомная прошивка IronOS для паяльников FNIRSI HS-02A / HS-02B",
    }
    for lang in ("ru", "en"):
        src = frames_root / lang
        dst = out_root / lang
        dst.mkdir(parents=True, exist_ok=True)
        for ppm in sorted(src.glob("*.ppm")):
            if ppm.stem.startswith(("boot_", "gif_")):
                continue
            frame(load(ppm)).save(dst / f"{ppm.stem}.png", optimize=True)
        boot = [frame(load(p), transparent=False).convert("RGB") for p in sorted(src.glob("boot_*.ppm"))]
        boot += [boot[-1]] * 14  # hold the green "Ready" frame
        boot[0].save(dst / "boot.gif", save_all=True, append_images=boot[1:], duration=50, loop=0, optimize=True)
        banner(src, out_root / f"banner-{lang}.png", fonts, subtitles[lang])
    standby_gif(frames_root / "en", out_root / "standby_gif.gif")


def standby_gif(src: Path, out: Path):
    """The standby GIF screensaver as the firmware draws it (frames rendered every 20 ms)."""
    shots, durations = [], []
    for p in sorted(src.glob("gif_*.ppm")):
        img = load(p)
        if shots and img.tobytes() == shots[-1].tobytes():
            durations[-1] += 20
        else:
            shots.append(img)
            durations.append(20)
    if not shots:
        return
    tiles = [frame(s, transparent=False).convert("RGB") for s in shots]
    tiles[0].save(out, save_all=True, append_images=tiles[1:], duration=durations, loop=0, optimize=True)


if __name__ == "__main__":
    main()
