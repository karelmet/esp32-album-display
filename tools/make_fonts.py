"""Convertit Source Sans 3 (ex Source Sans Pro) en polices lissées (format VLW de TFT_eSPI) dans src/fonts.cpp.

À lancer avec le Python de PlatformIO, qui a Pillow :
    ~/.platformio/penv/bin/python tools/make_fonts.py
"""
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
TTF = ROOT / "fonts" / "SourceSans3.ttf"

# nom C, taille en pixels, graisse, jeu de caractères (None = jeu complet)
FONTS = [
    ("FONT_TITLE", 20, 600, None),
    ("FONT_TEXT", 16, 400, None),
    ("FONT_SMALL", 14, 400, None),
    ("FONT_HUGE", 50, 300, "0123456789-°"),  # grande température de l'écran météo
]

# ASCII, Latin-1, Latin étendu A, et la ponctuation typographique courante.
CODEPOINTS = (
    list(range(0x21, 0x7F))
    + list(range(0xA1, 0x180))
    + [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x20AC]
)


def build(size: int, weight: int, charset: str | None) -> bytes:
    font = ImageFont.truetype(str(TTF), size)
    font.set_variation_by_axes([weight])  # seul axe variable : la graisse

    ascent = -font.getbbox("d", anchor="ls")[1]  # haut du "d" au-dessus de la ligne de base
    descent = font.getbbox("p", anchor="ls")[3]  # bas du "p" sous la ligne de base

    glyphs = []
    for cp in (sorted(map(ord, charset)) if charset else CODEPOINTS):
        ch = chr(cp)
        x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
        w, h = x1 - x0, y1 - y0
        if w <= 0 or h <= 0:
            continue
        assert w < 256 and h < 256 and -128 <= x0 < 128
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
        glyphs.append((cp, h, w, round(font.getlength(ch)), -y0, x0, img.tobytes()))

    out = struct.pack(">6i", len(glyphs), 11, size, 0, ascent, descent)
    for cp, h, w, adv, dy, dx, _ in glyphs:
        out += struct.pack(">7i", cp, h, w, adv, dy, dx, 0)
    for g in glyphs:
        out += g[6]
    return out


def main() -> None:
    cpp = ['// Généré par tools/make_fonts.py — ne pas modifier à la main.', '#include "fonts.h"', ""]
    for name, size, weight, charset in FONTS:
        data = build(size, weight, charset)
        cpp.append(f"const uint8_t {name}[] PROGMEM = {{")
        for i in range(0, len(data), 24):
            cpp.append("  " + ",".join(str(b) for b in data[i : i + 24]) + ",")
        cpp.append("};\n")
        print(f"{name}: {size} px, graisse {weight}, {len(data)} octets")
    (ROOT / "src" / "fonts.cpp").write_text("\n".join(cpp), encoding="utf-8")
    (ROOT / "src" / "fonts.h").write_text(
        "// Polices lissées Source Sans 3 (licence OFL, voir fonts/OFL.txt).\n"
        "// Généré par tools/make_fonts.py.\n"
        "#pragma once\n#include <Arduino.h>\n\n"
        + "".join(f"extern const uint8_t {name}[]; // {size} px\n" for name, size, _, _ in FONTS),
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
