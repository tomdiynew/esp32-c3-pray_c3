"""Draw the prayer art as SVG, render it with headless Edge/Chrome and pack it for the firmware.

The prayer scene background has three layers: the church (windows, light, candle bokeh), the praying portrait
cropped from art/logo_src.png pasted into the arched frame, and the front (frame, votive-candle rack with its
unlit red glass cups, counter panel). Sprites are drawn in screen coordinates; cups / flames / orbs around a design
center (FX, FY).

Outputs:
    main/art/logo.rgb565   start screen: art/logo_src.png scaled to 240x280 (big-endian RGB565)
    main/art/church.rgb565 the prayer scene background
    main/art/sprites.bin   sprites: w*h RGB565 (big-endian) followed by w*h alpha bytes, each
    main/sprites.h         sprite table, candle slots, layout constants

    python tools/fix_logo.py        # once: art/logo_orig.png -> art/logo_src.png (background crucifix removed)
    python tools/make_art.py [--preview art/preview.png]
"""
import argparse
import os
import pathlib
import random
import subprocess
import tempfile

from PIL import Image, ImageDraw

W, H = 240, 280
ROOT = pathlib.Path(__file__).resolve().parent.parent

BROWSERS = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
]

# ------------------------------------------------------------------------------------------------
# Layout shared with the C code
# ------------------------------------------------------------------------------------------------
PILL_X, PILL_Y, PILL_W, PILL_H = 16, 8, 208, 46
ARCH_X, ARCH_Y, ARCH_W, ARCH_H = 65, 60, 110, 130          # portrait inside the frame
PORTRAIT_CROP = (170, 40, 1010, 1033)                      # in art/logo_src.png
FX, FY = 120, 140
ROWS_Y = (216, 240, 264)                                   # cup centers of the three tiers
COLS_X = [36 + 24 * i for i in range(8)]
AMEN_CX, AMEN_CY = 198, 84
CARD_CX, CARD_CY, CARD_W, CARD_H = 120, 212, 212, 132   # over the candle rack: never over the face
BUTTON_CX, BUTTON_CY, BUTTON_W = 184, 262, 90

GOLD, CREAM, RED = "#e2b45a", "#fff3dc", "#b8322a"
KAI, HEI = "STKaiti", "Microsoft YaHei"


def slots():
    """Fill order: front tier from the middle outwards, then the middle tier, then the back tier"""
    order = []
    for row in (2, 1, 0):
        cols = sorted(range(8), key=lambda c: (abs(c - 3.5), c))
        order += [(COLS_X[c], ROWS_Y[row]) for c in cols]
    return order


def star4(cx, cy, r, fill="#fff3c4", opacity=1.0):
    k = r * 0.3
    return (f'<path d="M{cx} {cy - r} Q{cx + k} {cy - k} {cx + r} {cy} Q{cx + k} {cy + k} {cx} {cy + r} '
            f'Q{cx - k} {cy + k} {cx - r} {cy} Q{cx - k} {cy - k} {cx} {cy - r} Z" fill="{fill}" opacity="{opacity}"/>')


def window(x, w):
    rnd = random.Random(x)
    panes = "".join(f'<rect x="{x + i * 8 + (4 if j % 2 else 0) - 4}" y="{64 + j * 6}" width="7" height="7" '
                    f'transform="rotate(45 {x + i * 8 + (4 if j % 2 else 0) - 0.5} {67.5 + j * 6})" '
                    f'fill="{rnd.choice(("#6f9ad8", "#b9d0f0", "#e8c870", "#8ab4e8", "#d8e6f8", "#5a86c8"))}"/>'
                    for i in range(int(w // 8) + 2) for j in range(23))
    r = w / 2
    return (f'<g opacity="0.5" filter="url(#soft)"><clipPath id="w{x}"><path d="M{x} 196 V{64 + r} A{r} {r} 0 0 1 {x + w} {64 + r} V196 Z"/></clipPath>'
            f'<g clip-path="url(#w{x})">{panes}</g></g>'
            f'<path d="M{x} 196 V{64 + r} A{r} {r} 0 0 1 {x + w} {64 + r} V196" fill="none" stroke="#c9a060" stroke-width="2.5" opacity="0.7"/>')


def church():
    rnd = random.Random(11)
    bokeh = "".join(f'<circle cx="{rnd.uniform(0, 240):.0f}" cy="{rnd.uniform(60, 200):.0f}" r="{rnd.uniform(4, 11):.1f}" fill="#ffe0a0" '
                    f'opacity="{rnd.uniform(0.2, 0.5):.2f}" filter="url(#blur)"/>' for _ in range(16))
    beams = "".join(f'<path d="M{x} 60 L{x + 30} 60 L{x + 90 * d + 40} 280 L{x + 90 * d - 10} 280 Z" fill="#fff4d0" opacity="0.14"/>'
                    for x, d in ((14, 1), (196, -1)))
    return f"""
<defs>
  <linearGradient id="wall" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f3dcb4"/><stop offset="1" stop-color="#c89868"/></linearGradient>
  <radialGradient id="light" cx="120" cy="120" r="130" gradientUnits="userSpaceOnUse">
    <stop offset="0" stop-color="#fff8e0" stop-opacity="0.9"/><stop offset="1" stop-color="#fff8e0" stop-opacity="0"/></radialGradient>
  <filter id="blur" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="3"/></filter>
  <filter id="soft" x="-10%" y="-10%" width="120%" height="120%"><feGaussianBlur stdDeviation="2"/></filter>
</defs>
<rect width="{W}" height="{H}" fill="url(#wall)"/>
{window(8, 44)}{window(188, 44)}
<circle cx="120" cy="120" r="130" fill="url(#light)"/>
{beams}{bokeh}
"""


def lily(x, y, s, rot):
    petals = "".join(f'<path d="M0 0 C4 -5 5 -12 0 -16 C-5 -12 -4 -5 0 0 Z" transform="rotate({a})" fill="#fffdf6" stroke="#dcd4bc" stroke-width="0.5"/>'
                     for a in range(0, 360, 60))
    return (f'<g transform="translate({x} {y}) rotate({rot}) scale({s})">'
            f'<path d="M-2 4 C-12 10 -18 20 -20 30 M2 4 C10 12 16 18 22 26" stroke="#5a8a3e" stroke-width="2" fill="none"/>'
            f'<ellipse cx="-12" cy="16" rx="8" ry="3" transform="rotate(40 -12 16)" fill="#6a9a4a"/>'
            f'{petals}<circle r="2.2" fill="#d8e4a0"/></g>')


def front():
    rows = ""
    for y in ROWS_Y:  # brass shelf under each tier, unlit cups on it
        rows += (f'<rect x="20" y="{y + 7}" width="200" height="4" rx="1.5" fill="url(#brass)" stroke="#8a6224" stroke-width="0.5"/>'
                 + "".join(f'<path d="M{x - 7} {y - 7} L{x - 6} {y + 7} H{x + 6} L{x + 7} {y - 7} Z" fill="#6a1c18" stroke="#3a0c0a" stroke-width="0.6"/>'
                           f'<ellipse cx="{x}" cy="{y - 7}" rx="7" ry="1.8" fill="#e8dcc8"/>'
                           f'<line x1="{x}" y1="{y - 8}" x2="{x}" y2="{y - 11}" stroke="#3a2a1a" stroke-width="0.8"/>'
                           f'<path d="M{x - 5} {y - 4} L{x - 4.5} {y + 4}" stroke="#fff" stroke-width="0.8" opacity="0.25"/>' for x in COLS_X))
    return f"""
<defs>
  <linearGradient id="gold" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fbe3a0"/><stop offset="1" stop-color="#c8903a"/></linearGradient>
  <linearGradient id="brass" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f0cf80"/><stop offset="1" stop-color="#a8742a"/></linearGradient>
</defs>
<!-- portrait frame -->
<path d="M{ARCH_X - 7} {ARCH_Y + ARCH_H + 4} V{ARCH_Y + ARCH_W / 2} A{ARCH_W / 2 + 7} {ARCH_W / 2 + 7} 0 0 1 {ARCH_X + ARCH_W + 7} {ARCH_Y + ARCH_W / 2} V{ARCH_Y + ARCH_H + 4} Z
         M{ARCH_X} {ARCH_Y + ARCH_H} V{ARCH_Y + ARCH_W / 2} A{ARCH_W / 2} {ARCH_W / 2} 0 0 1 {ARCH_X + ARCH_W} {ARCH_Y + ARCH_W / 2} V{ARCH_Y + ARCH_H} Z"
      fill="url(#gold)" fill-rule="evenodd" stroke="#a8742a" stroke-width="0.8"/>
<path d="M{ARCH_X - 3} {ARCH_Y + ARCH_W / 2} A{ARCH_W / 2 + 3} {ARCH_W / 2 + 3} 0 0 1 {ARCH_X + ARCH_W + 3} {ARCH_Y + ARCH_W / 2}" fill="none" stroke="#fff4c8" stroke-width="0.8"/>
<!-- candle rack -->
<rect x="14" y="200" width="212" height="80" rx="4" fill="#4a2c16" opacity="0.35"/>
<rect x="22" y="274" width="4" height="6" fill="#8a6224"/><rect x="214" y="274" width="4" height="6" fill="#8a6224"/>
{rows}
{lily(10, 222, 0.9, -20)}{lily(228, 230, 0.8, 25)}
<!-- counter panel -->
<rect x="{PILL_X + 2}" y="{PILL_Y + 3}" width="{PILL_W}" height="{PILL_H}" rx="{PILL_H / 2}" fill="#a8743a" opacity="0.3"/>
<rect x="{PILL_X}" y="{PILL_Y}" width="{PILL_W}" height="{PILL_H}" rx="{PILL_H / 2}" fill="#fffaf0" stroke="{GOLD}" stroke-width="1.8"/>
"""


# ------------------------------------------------------------------------------------------------
# Sprites
# ------------------------------------------------------------------------------------------------
def cup_lit():
    x, y = FX, FY
    return f"""
<defs><radialGradient id="cg" cx="{x}" cy="{y + 1}" r="10" gradientUnits="userSpaceOnUse">
  <stop offset="0" stop-color="#ffb070"/><stop offset="0.6" stop-color="#e0402a"/><stop offset="1" stop-color="#9a1c16"/></radialGradient></defs>
<path d="M{x - 7} {y - 7} L{x - 6} {y + 7} H{x + 6} L{x + 7} {y - 7} Z" fill="url(#cg)" stroke="#6a1410" stroke-width="0.6"/>
<ellipse cx="{x}" cy="{y - 7}" rx="7" ry="1.8" fill="#fff0c8"/>
<path d="M{x - 5} {y - 4} L{x - 4.5} {y + 4}" stroke="#fff" stroke-width="0.9" opacity="0.4"/>
"""


def flame():
    x, y = FX, FY
    return f"""
<defs><radialGradient id="fg" cx="{x}" cy="{y}" r="11" gradientUnits="userSpaceOnUse">
  <stop offset="0" stop-color="#ffe6a0" stop-opacity="0.75"/><stop offset="1" stop-color="#ffe6a0" stop-opacity="0"/></radialGradient></defs>
<circle cx="{x}" cy="{y}" r="11" fill="url(#fg)"/>
<path d="M{x} {y - 7} C{x + 3.2} {y - 2} {x + 3.2} {y + 3} {x} {y + 4} C{x - 3.2} {y + 3} {x - 3.2} {y - 2} {x} {y - 7} Z" fill="#ffb84a"/>
<path d="M{x} {y - 2.5} C{x + 1.6} {y} {x + 1.6} {y + 2.5} {x} {y + 3.2} C{x - 1.6} {y + 2.5} {x - 1.6} {y} {x} {y - 2.5} Z" fill="#fff8e0"/>
"""


def orb():
    return f"""
<defs><radialGradient id="og" cx="{FX}" cy="{FY}" r="10" gradientUnits="userSpaceOnUse">
  <stop offset="0" stop-color="#ffffff"/><stop offset="0.35" stop-color="#fff2c0" stop-opacity="0.9"/><stop offset="1" stop-color="#ffd880" stop-opacity="0"/></radialGradient></defs>
<circle cx="{FX}" cy="{FY}" r="10" fill="url(#og)"/>"""


def ring():
    return f"""
<defs><radialGradient id="rg" cx="{FX}" cy="{FY}" r="18" gradientUnits="userSpaceOnUse">
  <stop offset="0.5" stop-color="#fff0b0" stop-opacity="0"/><stop offset="0.75" stop-color="#fff0b0" stop-opacity="0.9"/>
  <stop offset="1" stop-color="#fff0b0" stop-opacity="0"/></radialGradient></defs>
<circle cx="{FX}" cy="{FY}" r="18" fill="url(#rg)"/>"""


def amen():
    return (f'<text x="{AMEN_CX}" y="{AMEN_CY + 8}" text-anchor="middle" font-family="{KAI}" font-weight="bold" font-size="22" '
            f'fill="#ffe6a0" stroke="#8a4a1a" stroke-width="4" stroke-linejoin="round" paint-order="stroke">阿们</text>')


def rays():
    beams = "".join(f'<path d="M{120 + dx * 0.15:.0f} -10 L{120 + dx - 12} 290 L{120 + dx + 12} 290 Z" fill="url(#ray)"/>'
                    for dx in (-150, -95, -45, 0, 45, 95, 150))
    return f"""
<defs><linearGradient id="ray" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fffbe6" stop-opacity="0.75"/>
  <stop offset="1" stop-color="#fffbe6" stop-opacity="0"/></linearGradient></defs>
{beams}"""


def card():
    x, y = CARD_CX - CARD_W / 2, CARD_CY - CARD_H / 2
    return f"""
<defs><linearGradient id="parch" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fffaf0"/><stop offset="1" stop-color="#f3e2c2"/></linearGradient></defs>
<rect x="{x + 2}" y="{y + 4}" width="{CARD_W}" height="{CARD_H}" rx="10" fill="#6a4020" opacity="0.3"/>
<rect x="{x}" y="{y}" width="{CARD_W}" height="{CARD_H}" rx="10" fill="url(#parch)" stroke="{GOLD}" stroke-width="2"/>
<rect x="{x + 5}" y="{y + 5}" width="{CARD_W - 10}" height="{CARD_H - 10}" rx="7" fill="none" stroke="{GOLD}" stroke-width="0.8" opacity="0.7"/>
<path d="M{CARD_CX - 50} {y + 17} h26 M{CARD_CX + 24} {y + 17} h26" stroke="{GOLD}" stroke-width="0.8"/>
"""


def button():
    w, h = BUTTON_W, 28
    x, y = BUTTON_CX - w / 2, BUTTON_CY - h / 2
    return f"""
<defs><linearGradient id="btn" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff4d8"/><stop offset="1" stop-color="#e8c47a"/></linearGradient>
  <filter id="sh" x="-20%" y="-50%" width="140%" height="200%"><feGaussianBlur stdDeviation="1.5"/></filter></defs>
<text x="{BUTTON_CX}" y="{y - 7}" text-anchor="middle" font-family="{KAI}" font-weight="bold" font-size="13" fill="#3a1a08" filter="url(#sh)" opacity="0.8">与耶稣一同祷告</text>
<text x="{BUTTON_CX}" y="{y - 7}" text-anchor="middle" font-family="{KAI}" font-weight="bold" font-size="13" fill="#fff8e6">与耶稣一同祷告</text>
<rect x="{x - 3}" y="{y - 3}" width="{w + 6}" height="{h + 6}" rx="{h / 2 + 3}" fill="#fff4c8" opacity="0.45"/>
<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{h / 2}" fill="url(#btn)" stroke="#a8742a" stroke-width="1.4"/>
<text x="{BUTTON_CX}" y="{BUTTON_CY + 5}" text-anchor="middle" font-family="{HEI}" font-weight="bold" font-size="14" fill="{RED}">开始祷告</text>
"""


def toast():
    return (f'<rect x="{120 - 86}" y="{128 - 20}" width="172" height="40" rx="12" fill="#5a3418" opacity="0.88"/>'
            f'<text x="120" y="134" text-anchor="middle" font-family="{HEI}" font-weight="bold" font-size="16" fill="#fff8e6">点烛记录已清零</text>')


def sprites():
    return [("button", button()), ("toast_reset", toast()), ("amen", amen()), ("card", card()), ("rays", rays()),
            ("cup_lit", cup_lit()), ("flame", flame()), ("orb", orb()), ("ring", ring()), ("mote", star4(FX, FY, 3, "#fffbe8"))]


# ------------------------------------------------------------------------------------------------
# Rendering + packing
# ------------------------------------------------------------------------------------------------
def render(cells, browser):
    cols = 6
    rows = (len(cells) + cols - 1) // cols
    divs = "".join(f'<svg style="position:absolute;left:{(i % cols) * W}px;top:{(i // cols) * H}px" width="{W}" height="{H}" '
                   f'viewBox="0 0 {W} {H}" xmlns="http://www.w3.org/2000/svg">{body}</svg>\n' for i, body in enumerate(cells))
    with tempfile.TemporaryDirectory() as tmp:
        page, png = os.path.join(tmp, "art.html"), os.path.join(tmp, "sheet.png")
        with open(page, "w", encoding="utf-8") as f:
            f.write(f'<!doctype html><html><head><meta charset="utf-8"><style>html,body{{margin:0;background:transparent;'
                    f'overflow:hidden}}</style></head><body>{divs}</body></html>')
        subprocess.run([browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                        f"--window-size={cols * W},{rows * H}", "--default-background-color=00000000",
                        f"--user-data-dir={os.path.join(tmp, 'profile')}", f"--screenshot={png}",
                        "--virtual-time-budget=500", pathlib.Path(page).as_uri()], check=True, capture_output=True, timeout=180)
        sheet = Image.open(png).convert("RGBA")
    return [sheet.crop(((i % cols) * W, (i // cols) * H, (i % cols + 1) * W, (i // cols + 1) * H)) for i in range(len(cells))]


def rgb565_be(r, g, b):
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return bytes((v >> 8, v & 0xFF))


def write_rgb565(img, path):
    raw = img.convert("RGB").tobytes()
    path.write_bytes(b"".join(rgb565_be(*raw[i:i + 3]) for i in range(0, len(raw), 3)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", help="save a composed preview PNG here")
    args = ap.parse_args()
    browser = next((b for b in BROWSERS if os.path.exists(b)), None)
    if not browser:
        raise SystemExit("Edge/Chrome not found")

    src = Image.open(ROOT / "art" / "logo_src.png").convert("RGB")
    logo = src.resize((W, H), Image.LANCZOS)
    spr = sprites()
    imgs = render([church(), front()] + [body for _, body in spr], browser)

    scene = Image.new("RGBA", (W, H), (243, 220, 180, 255))
    scene.alpha_composite(imgs[0])
    portrait = src.crop(PORTRAIT_CROP).resize((ARCH_W, ARCH_H), Image.LANCZOS)
    mask = Image.new("L", (ARCH_W, ARCH_H), 0)
    md = ImageDraw.Draw(mask)
    md.ellipse((0, 0, ARCH_W - 1, ARCH_W - 1), fill=255)
    md.rectangle((0, ARCH_W // 2, ARCH_W - 1, ARCH_H - 1), fill=255)
    scene.paste(portrait, (ARCH_X, ARCH_Y), mask)
    scene.alpha_composite(imgs[1])
    scene = scene.convert("RGB")

    art = ROOT / "main" / "art"
    art.mkdir(exist_ok=True)
    write_rgb565(logo, art / "logo.rgb565")
    write_rgb565(scene, art / "church.rgb565")

    blob, table = bytearray(), []
    for (name, _), im in zip(spr, imgs[2:]):
        box = im.split()[3].getbbox()
        crop = im.crop(box)
        w, h = crop.size
        raw = crop.tobytes()
        table.append((name, len(blob), w, h, box[0], box[1]))
        blob += b"".join(rgb565_be(*raw[i:i + 3]) for i in range(0, len(raw), 4))
        blob += raw[3::4]
    (art / "sprites.bin").write_bytes(blob)

    order = slots()
    enum = "\n".join(f"    SPR_{n.upper()}," for n, *_ in table)
    rows = "\n".join(f"    {{{off:7d}, {w:3d}, {h:3d}, {x:3d}, {y:3d}}},  // {n}" for n, off, w, h, x, y in table)
    hdr = f"""/* Generated by tools/make_art.py - do not edit. */
#pragma once

#include <stdint.h>

typedef struct {{
    uint32_t off;   // into sprites.bin: w*h RGB565 (big-endian), then w*h alpha bytes
    uint16_t w, h;
    int16_t  x, y;  // top-left on screen at the design position
}} sprite_t;

enum {{
{enum}
    SPR_COUNT,
}};

#define FX       {FX}   // design center of cups, flames, orbs, rings and motes
#define FY       {FY}
#define PILL_X   {PILL_X}
#define PILL_Y   {PILL_Y}
#define PILL_W   {PILL_W}
#define PILL_H   {PILL_H}
#define CARD_CX  {CARD_CX}
#define CARD_CY  {CARD_CY}
#define CARD_W   {CARD_W}
#define CARD_H   {CARD_H}

#define CANDLE_COUNT {len(order)}
static const int16_t s_candles[CANDLE_COUNT][2] = {{{", ".join(f"{{{x}, {y}}}" for x, y in order)}}};  // cup centers, fill order

static const sprite_t s_sprites[SPR_COUNT] = {{
{rows}
}};
"""
    (ROOT / "main" / "sprites.h").write_text(hdr, encoding="utf-8")
    print(f"logo/church 2x{W * H * 2} bytes, sprites.bin {len(blob)} bytes, {len(table)} sprites")

    if args.preview:
        by = {n: im for (n, _), im in zip(spr, imgs[2:])}
        start = logo.convert("RGBA")
        start.alpha_composite(by["button"])

        def lit(im, n):
            for x, y in order[:n]:
                im.alpha_composite(by["cup_lit"], (x - FX, y - FY))
                im.alpha_composite(by["flame"], (x - FX, y - 13 - FY))
            return im

        a = lit(scene.convert("RGBA"), 7)
        a.alpha_composite(by["ring"], (order[6][0] - FX, order[6][1] - 13 - FY))
        a.alpha_composite(by["orb"], (150 - FX, 180 - FY))
        a.alpha_composite(by["amen"], (0, -8))
        b = lit(scene.convert("RGBA"), 24)
        b.alpha_composite(by["rays"])
        b.alpha_composite(by["card"])
        sheet = Image.new("RGB", (W * 3 + 20, H), "white")
        for k, im in enumerate((start, a, b)):
            sheet.paste(im, (k * (W + 10), 0))
        sheet.save(args.preview)


if __name__ == "__main__":
    main()
