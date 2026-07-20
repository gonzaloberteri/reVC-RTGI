#!/usr/bin/env python3
"""Assemble a folder of frame_*.bmp dumps into an optimized animated GIF.

Used by capture.ps1 to build the ray-traced (motion) side of each comparison.
A single shared palette is derived from a middle frame so colors stay stable
across the loop, and dithering is disabled so that regions the static camera
never changes stay byte-identical frame to frame -- which lets Pillow's
frame-diff optimizer shrink the file dramatically.

Usage: py assemble_gif.py <frames_dir> <out.gif> [width=720] [fps=12] [colors=256]
"""
import sys, glob, os
from PIL import Image


def main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        sys.exit(2)
    frames_dir = sys.argv[1]
    out = sys.argv[2]
    width = int(sys.argv[3]) if len(sys.argv) > 3 else 720
    fps = float(sys.argv[4]) if len(sys.argv) > 4 else 12.0
    colors = int(sys.argv[5]) if len(sys.argv) > 5 else 256

    paths = sorted(glob.glob(os.path.join(frames_dir, "frame_*.bmp")))
    if not paths:
        sys.stderr.write("assemble_gif: no frame_*.bmp in %s\n" % frames_dir)
        sys.exit(2)

    rgb = []
    for p in paths:
        im = Image.open(p).convert("RGB")
        if width and im.width != width:
            h = round(im.height * width / im.width)
            im = im.resize((width, h), Image.LANCZOS)
        rgb.append(im)

    # Shared palette from a middle frame keeps colors stable and unchanged
    # pixels identical, so optimize=True can diff frames effectively.
    pal_src = rgb[len(rgb) // 2].quantize(colors=colors, method=Image.FASTOCTREE)
    frames = [im.quantize(palette=pal_src, dither=Image.NONE) for im in rgb]

    frames[0].save(
        out,
        save_all=True,
        append_images=frames[1:],
        duration=int(round(1000.0 / fps)),
        loop=0,
        optimize=True,
    )
    size = os.path.getsize(out)
    print("assemble_gif: %s  %d frames  %dpx  %.0ffps  %d colors  %.2f MB"
          % (os.path.basename(out), len(frames), width, fps, colors, size / 1048576.0))


if __name__ == "__main__":
    main()
