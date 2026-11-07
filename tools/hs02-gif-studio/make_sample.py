#!/usr/bin/env python3
"""Draws the sample standby animation shipped as custom/standby.gif.

A soldering iron resting on the bottom of the screen, its tip glowing, smoke curling up,
and the model name on the right. Original artwork, drawn in code so it can be regenerated:

    python3 tools/hs02-gif-studio/make_sample.py      # writes custom/standby.gif

Replace custom/standby.gif with your own GIF to change the screensaver.
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "custom" / "standby.gif"
FONT = ROOT / "tools/hs02-ui-fonts/fonts/InterDisplay-Bold.otf"

W, H = 160, 80
FRAMES, FRAME_MS = 24, 90
TIP = (34, 66)  # glowing end of the tip


def draw_iron(d: ImageDraw.ImageDraw, glow: float):
    # Handle with grip rings, metal shaft and the glowing tip. Each 4-row band of the screen has
    # 4 colours, so the iron sticks to black + dark grey + light grey + one tip colour.
    metal, handle = (150, 150, 158), (48, 48, 54)
    d.rounded_rectangle((92, 60, 156, 72), 6, fill=handle)
    for x in (100, 106, 112):
        d.rectangle((x, 61, x + 2, 71), fill=metal)
    d.rectangle((54, 63, 92, 69), fill=metal)
    hot = (255, int(70 + 130 * glow), int(10 + 40 * glow))  # orange -> yellow and back
    d.polygon([(54, 62), (54, 70), (TIP[0], TIP[1] + 1), (TIP[0], TIP[1] - 1)], fill=hot)


def draw_smoke(d: ImageDraw.ImageDraw, t: float):
    # Three puffs per loop, each rising and growing while it fades from light to dark grey.
    for k in range(3):
        p = (t + k / 3) % 1.0
        y = TIP[1] - 12 - p * 50
        x = TIP[0] + 2 + math.sin(p * 2 * math.pi * 1.3 + k) * (3 + 6 * p)
        r = 2 + p * 5
        level = int(170 - 120 * p)
        d.ellipse((x - r, y - r, x + r, y + r), fill=(level, level, level + 6))


def main():
    font = ImageFont.truetype(str(FONT), 22)
    frames = []
    for f in range(FRAMES):
        t = f / FRAMES
        img = Image.new("RGB", (W, H), (0, 0, 0))
        d = ImageDraw.Draw(img)
        d.fontmode = "1"  # crisp text: no anti-alias colours to spend
        draw_smoke(d, t)
        draw_iron(d, 0.5 + 0.5 * math.sin(t * 2 * math.pi))
        d.text((74, 14), "HS-02", font=font, fill=(255, 159, 10))
        dot = (48, 209, 88) if f < FRAMES // 2 else (60, 60, 64)
        d.ellipse((76, 46, 81, 51), fill=dot)
        d.text((86, 43), "standby", font=ImageFont.truetype(str(FONT), 10), fill=(142, 142, 147))
        frames.append(img)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    frames[0].save(OUT, save_all=True, append_images=frames[1:], duration=FRAME_MS, loop=0)
    print(f"wrote {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
