#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The presets' loudness, for their gain column (firmware/engine/fm313_app.c PRESETS): each preset plays the demo
patterns A01-A03 in the host simulator, and the app's output is measured as ITU-R BS.1770 integrated loudness.
Prints each preset's loudness and the factor for its gain that would put it at the mean of presets 2-14 (the
reference set; CLASSIC is 0.5 dB above them on purpose).

  sh tests/fm313/run.sh && python3 tools/preset_loudness.py N_PRESETS      (needs numpy, soundfile, pyloudnorm)"""
import subprocess, sys, tempfile
from pathlib import Path
import soundfile as sf, pyloudnorm as pyln

root = Path(__file__).resolve().parents[1]
sim = root / "build/host-fm313/sim"
n = int(sys.argv[1])
out = []
with tempfile.TemporaryDirectory() as d:
    for i in range(n):
        s = Path(d) / "p.txt"
        s.write_text((f"enc PRESETS {i}\n" if i else "") + "press PLAY\nwait 7700\n" +
                     "".join(f"btn SEQ 1\ntap {k}\nbtn SEQ 0\nwait 7700\n" for k in (1, 2)))
        subprocess.run([str(sim), str(s), str(Path(d) / "o")], check=True, capture_output=True)
        x, fs = sf.read(str(Path(d) / "o.wav"))
        out.append(pyln.Meter(fs).integrated_loudness(x))
ref = sum(out[1:14]) / 13
for i, l in enumerate(out):
    t = ref + 0.5 if i == 0 else ref
    print(f"{i + 1:2d}  {l:6.2f} LUFS  {l - t:+5.2f} dB   gain x {10 ** ((t - l) / 20):.3f}")
