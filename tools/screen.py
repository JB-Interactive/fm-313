#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""host/sim.c screen dumps (.scr: 240x240 RGB565, then the lit buttons) -> PNG, 2x, nearest neighbour.
  tools/screen.py out.png a.scr [b.scr ...]   (several: side by side)"""
import sys
import numpy as np
from PIL import Image
ims = []
for fn in sys.argv[2:]:
    d = open(fn, "rb").read()
    px = np.frombuffer(d[:240 * 240 * 2], dtype=np.uint16).reshape(240, 240).astype(np.uint32)
    rgb = np.dstack([(px >> 11) * 255 // 31, ((px >> 5) & 63) * 255 // 63, (px & 31) * 255 // 31]).astype(np.uint8)
    ims.append(Image.fromarray(rgb).resize((480, 480), Image.NEAREST))
W = len(ims) * 480 + (len(ims) - 1) * 24
out = Image.new("RGB", (W, 480), (40, 40, 40))
for i, im in enumerate(ims):
    out.paste(im, (i * 504, 0))
out.save(sys.argv[1])
