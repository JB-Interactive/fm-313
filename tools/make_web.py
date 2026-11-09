#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the single-file FM-313 web page (installer + presentation): web/index.html with fm1pkg.js, fm1ota.js and
fm313.js inlined as one module, the package build/fm313.fwsc embedded with its SHA-256, the demo clips and the screen.

  tools/make_web.py VERSION ASSETS_DIR OUT.html
ASSETS_DIR holds c_saw.mp3 c_acc.mp3 c_dist.mp3 c_sq.mp3 screen.png wave.json (host/render.c, host/sim.c)."""
import base64, hashlib, json, re, sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
version, assets, out = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
web = root / "web"

def strip_module(src):
    src = re.sub(r'^import .*?;\s*$', '', src, flags=re.M)
    return re.sub(r'^export (?=(const|function|async function|class) )', '', src, flags=re.M)

bundle = "\n".join(strip_module((web / f).read_text()) for f in ("fm1pkg.js", "fm1ota.js", "fm313.js"))
pkg = (root / "build" / "fm313.fwsc").read_bytes()
rel = {"version": version, "sha256": hashlib.sha256(pkg).hexdigest(), "b64": base64.b64encode(pkg).decode()}
b64 = lambda p: base64.b64encode((assets / p).read_bytes()).decode()
page = (web / "index.html").read_text()
for key, val in {"@@CLIP_SAW@@": b64("c_saw.mp3"), "@@CLIP_ACC@@": b64("c_acc.mp3"), "@@CLIP_DIST@@": b64("c_dist.mp3"),
                 "@@CLIP_SQ@@": b64("c_sq.mp3"), "@@SCREEN@@": b64("screen.png"),
                 "@@WAVE@@": (assets / "wave.json").read_text().strip(), "@@RELEASE@@": json.dumps(rel)}.items():
    assert key in page, key
    page = page.replace(key, val)
tag = '<script type="module" src="fm313.js"></script>'
assert tag in page
page = page.replace(tag, '<script type="module">\n' + bundle.replace("</script", "<\\/script") + "\n</script>")
out.write_text(page)
print(f"{out}: {len(page)} bytes, package sha256 {rel['sha256']}")
