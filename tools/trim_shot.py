#!/usr/bin/env python3
"""Trim a test-run screenshot to the game's picture. A windowless run's shot is the whole presented texture (2560x1440 here);
the game draws its frame into the top-left part of it, at its current render size (70 to 80 % of it in the shots so far),
and only the window's present step would scale that part to the screen. This keeps the part that is not black.
    trim_shot.py <png> [<png> ...]      (each file is replaced by its trimmed copy)"""
import sys
from PIL import Image

for path in sys.argv[1:]:
    im = Image.open(path).convert('RGB')
    box = im.point(lambda v: 255 if v > 8 else 0).getbbox()   # the non-black part (a dark game frame still has pixels above 8)
    if box and (box[2] - box[0], box[3] - box[1]) != im.size:
        im.crop((0, 0, box[2], box[3])).save(path)              # from the corner: the frame starts at 0,0 even where its own edge is dark
        print(f'{path}: {im.size[0]}x{im.size[1]} -> {box[2]}x{box[3]}')
