#!/usr/bin/env python3
"""contact_sheet.py out.jpg shot1.png shot2.png ... : grid of labelled thumbnails (4 columns)."""
import sys
from PIL import Image, ImageDraw
files = sys.argv[2:]; W, H, C = 480, 270, 4
rows = (len(files) + C - 1) // C
sheet = Image.new('RGB', (W * C, H * rows), 'black')
for i, f in enumerate(files):
    im = Image.open(f).convert('RGB').resize((W, H))
    d = ImageDraw.Draw(im); d.rectangle((0, 0, 130, 16), fill='black'); d.text((4, 2), f.split('/')[-1], fill='white')
    sheet.paste(im, ((i % C) * W, (i // C) * H))
sheet.save(sys.argv[1], quality=80)
