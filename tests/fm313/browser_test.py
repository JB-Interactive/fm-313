#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The installer page in a real Chrome against a fake FM-1 (tests/fm313/fake_fm1.js, from web/test_installer.mjs):
refused over a Felucca beta, the override, the page's own question answered with a click, the install to the end.
  python3 tests/fm313/browser_test.py build/fm313-installer.html [chrome]"""
import sys, time
from pathlib import Path
from playwright.sync_api import sync_playwright
page_path = Path(sys.argv[1]).resolve()
chrome = sys.argv[2] if len(sys.argv) > 2 else None
fake = (Path(__file__).parent / "fake_fm1.js").read_text()
with sync_playwright() as p:
    b = p.chromium.launch(executable_path=chrome) if chrome else p.chromium.launch()
    pg = b.new_page()
    errs = []
    pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.add_init_script(fake)
    pg.goto(page_path.as_uri())
    pg.click("#go")
    pg.wait_for_function("document.getElementById('status').textContent.includes('Felucca beta')", timeout=20000)
    assert pg.is_visible("#override"), "the override is offered"
    pg.check("#override")
    pg.click("#go")
    pg.wait_for_selector("#ask:not([hidden])", timeout=20000)
    pg.click("#ask-yes")                      # 0.2 shipped with this button blocked by body.busy
    pg.wait_for_function("document.getElementById('status').textContent.startsWith('Done')", timeout=60000)
    up = pg.evaluate("window.__dev.upgrades")
    print("status:", pg.inner_text("#status"), "| upgrades:", up, "| errors:", errs)
    assert up == 2 and not errs
    # FM-313 0.7 over itself: it restarts into its loader (on UPGRADE; when its answer is not collected; at once),
    # the page goes on with the loader, and never asks the new firmware anything
    for mode in ("step1", "jump", "stuck", "silent"):   # step1: the family's two steps, FM-313's own way (0.7.3)
        pg = b.new_page()
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.add_init_script("window.__FAKE = { identity: 'FM-1_313', name: 'FM-1', fm313: %s };" % ("null" if mode == "step1" else "'%s'" % mode) + fake)
        pg.goto(page_path.as_uri())
        pg.click("#go")
        pg.wait_for_selector("#ask:not([hidden])", timeout=30000)
        pg.click("#ask-yes")
        pg.wait_for_function("document.getElementById('status').textContent.startsWith('Done')", timeout=60000)
        up, post = pg.evaluate("[window.__dev.upgrades, window.__dev.postHs]")
        print("fm313 %-6s:" % mode, pg.inner_text("#status"), "| loader writes:", up, "| asked after:", post, "| errors:", errs)
        assert up == (2 if mode == "step1" else 1) and post == 0 and not errs
    b.close()
print("browser test passed")
