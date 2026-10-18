#!/usr/bin/env python3
"""Convert a JPEG/PNG cover to bounded, monochrome BMP for the e-ink music player.

Usage: python3 scripts/prepare_music_cover.py photo.jpg '/Volumes/TF/Music/song.bmp'
Requires Pillow. Does not edit the input or the audio file.
"""
import argparse
from pathlib import Path
from PIL import Image, ImageOps


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.suffix.lower() != '.bmp':
        parser.error('output must end in .bmp')
    if args.image.resolve() == args.output.resolve():
        parser.error('input and output must differ')
    with Image.open(args.image) as source:
        cover = ImageOps.exif_transpose(source)
        # Flatten alpha onto white before scaling, preserving transparent artwork.
        rgba = cover.convert('RGBA')
        white = Image.new('RGBA', rgba.size, 'white')
        white.alpha_composite(rgba)
        cover = ImageOps.fit(white.convert('L'), (384, 384), method=Image.Resampling.LANCZOS)
        cover.convert('1', dither=Image.Dither.FLOYDSTEINBERG).save(args.output, format='BMP')
    print(f'Saved {args.output} (384x384, monochrome BMP)')


if __name__ == '__main__':
    main()
