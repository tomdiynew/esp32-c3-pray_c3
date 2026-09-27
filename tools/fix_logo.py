"""Remove the crucifix from the background of art/logo_orig.png -> art/logo_src.png.

In the picture Jesus himself is praying, so a crucifix hanging behind him is out of place. The crucifix stands in
front of the (already blurred) stained-glass window on the right; it is covered with the window's own texture,
taken from the same window left of it, mirrored, blurred to match and blended in with a feathered mask.

    python tools/fix_logo.py
"""
import pathlib

from PIL import Image, ImageDraw, ImageFilter, ImageOps

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC, DST = ROOT / "art" / "logo_orig.png", ROOT / "art" / "logo_src.png"

COVER = (968, 0, 1161, 735)      # crucifix: beam, corpus, crossbar and its base
SAMPLE = (872, 0, 968, 735)      # stained glass of the same window, left of the crucifix
FEATHER = 22


def main():
    im = Image.open(SRC).convert("RGB")
    x0, y0, x1, y1 = COVER
    w, h = x1 - x0, y1 - y0
    strip = im.crop(SAMPLE)
    patch = Image.new("RGB", (w, h))
    x, flip = 0, True
    while x < w:  # alternate mirrored copies so the seams line up
        piece = ImageOps.mirror(strip) if flip else strip
        patch.paste(piece, (x, 0))
        x += strip.width
        flip = not flip
    patch = patch.filter(ImageFilter.GaussianBlur(5))

    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rectangle((FEATHER, 0, w, h - FEATHER), fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(FEATHER / 2))
    im.paste(patch, (x0, y0), mask)
    im.save(DST)
    print(f"{DST.name}: covered {COVER}")


if __name__ == "__main__":
    main()
