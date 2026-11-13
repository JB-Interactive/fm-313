#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# FM-313 host tests: the engine and the app built with the host compiler, run against scripted input.
set -e
cd "$(dirname "$0")/../.."
B=build/host-fm313; mkdir -p "$B"
E=firmware/engine
CC="${CC:-cc}"
W="-std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -Wdouble-promotion -Werror"
SRC="$E/fm313_dsp.c $E/fm313_seq.c $E/fm313_fx.c"
# 1. the firmware unit itself, syntax and warnings (no doubles anywhere: the FPU is single precision)
mkdir -p "$B/gen" && echo 'static const uint8_t TRANSIT_LDR[4] = {0,0,0,0};' > "$B/gen/transit_loader.h"
$CC -fsyntax-only -std=gnu99 -ffreestanding -fno-builtin -Wall -Wno-unused-function -Wno-misleading-indentation \
    -Wdouble-promotion -Werror -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Ifirmware/hal -Ifirmware/src -I$E -Ifirmware/gen -I$B/gen \
    -DFELUCCA_ID='"FM-1_313"' -DAPP_VER='"test"' firmware/src/fm313.c
echo "ok  firmware unit (host syntax)"
# 2. the app against the UI script (buttons, encoders, keys, step write, save + power cycle, undo, tap tempo)
$CC $W -Wno-sign-compare -DFM313_SIM -o "$B/sim" host/sim.c $E/fm313_app.c $SRC -lm
"$B/sim" tests/fm313/ui.txt "$B/ui"
echo "ok  app simulator"
# 3. the demo renders: levels in range, no NaN
$CC $W -Wno-double-promotion -o "$B/render" host/render.c $SRC -lm
for s in 0 1 2 3 4 5; do "$B/render" "$B/scene$s.wav" $s 2>&1 | tee "$B/scene$s.txt"; done
python3 - "$B" <<'PY'
import sys, re, pathlib
for f in sorted(pathlib.Path(sys.argv[1]).glob("scene*.txt")):
    m = re.search(r"peak ([\d.na]+) rms ([\d.na]+)", f.read_text())
    pk, rms = float(m.group(1)), float(m.group(2))
    assert 0.2 < pk <= 0.96 and 0.03 < rms < 0.4, (f.name, pk, rms)
print("ok  render levels")
PY
# 3b. the engine under random use: finite, within full scale, no DC, silent after the last note
$CC -std=c99 -O2 -w -o "$B/stress" tests/fm313/stress.c $E/fm313_dsp.c $E/fm313_fx.c -lm
"$B/stress" 120
echo "ok  engine stress"
# 4. every screen drawn once (tools/screen.py renders the dumps)
"$B/sim" tests/fm313/screens.txt "$B/scr"
echo "ok  screens"
