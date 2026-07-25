#!/usr/bin/env python3
"""Render the plugin icon from its SVG source.

Icon128.svg is the source of truth; Icon128.png is generated. Edit the SVG and
re-run this - never hand-edit the PNG.

Renders at 4x and box-downsamples with LANCZOS rather than asking the SVG
rasteriser for 128px directly: the arc strokes and the small joints alias badly
at native size, and supersampling is what keeps them clean.

Usage:  python3 Build/Scripts/render-icon.py [--preview]

  --preview   also write a side-by-side sheet at 128/64/32 px to the scratch
              dir, for checking that the mark still reads when Unreal's plugin
              browser or a Fab listing card shows it small.

Requires: cairosvg, pillow  (pip install cairosvg pillow)
"""

import io
import os
import sys

try:
    import cairosvg
    from PIL import Image
except ImportError:
    sys.exit("Missing deps. Run: pip install cairosvg pillow")

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RES = os.path.join(REPO, "ProjectSandbox", "Plugins", "Open3DBroadcast", "Resources")
SRC = os.path.join(RES, "Icon128.svg")
OUT = os.path.join(RES, "Icon128.png")

SIZE = 128
SUPERSAMPLE = 4


def render(px):
    """Rasterise the SVG at px, supersampled, returning an RGBA image."""
    big = cairosvg.svg2png(
        url=SRC, output_width=px * SUPERSAMPLE, output_height=px * SUPERSAMPLE
    )
    img = Image.open(io.BytesIO(big)).convert("RGBA")
    return img.resize((px, px), Image.LANCZOS)


def main():
    if not os.path.exists(SRC):
        sys.exit(f"Source not found: {SRC}")

    icon = render(SIZE)
    icon.save(OUT, "PNG", optimize=True)
    print(f"wrote {os.path.relpath(OUT, REPO)}  ({icon.size[0]}x{icon.size[1]}, "
          f"{os.path.getsize(OUT)} bytes)")

    if "--preview" in sys.argv:
        sizes = [128, 64, 32]
        pad = 16
        sheet_w = sum(sizes) + pad * (len(sizes) + 1)
        sheet_h = max(sizes) + pad * 2
        # Mid grey: shows both the dark tile's edge and any light fringing.
        sheet = Image.new("RGBA", (sheet_w, sheet_h), (128, 128, 128, 255))
        x = pad
        for s in sizes:
            sheet.paste(render(s), (x, pad + (max(sizes) - s) // 2))
            x += s + pad
        scratch = os.environ.get("CLAUDE_SCRATCH", "/tmp")
        path = os.path.join(scratch, "icon_preview.png")
        sheet.save(path, "PNG")
        print(f"wrote preview sheet: {path}")


if __name__ == "__main__":
    main()
