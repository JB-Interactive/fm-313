# FM-313

A minimal, 303-style bass line firmware for the M-VAVE FM-1: one monophonic acid voice, a step sequencer with 64 patterns, and the effects acid needs. Sound quality comes before feature count.

**Status: 0.7.2, the first published version.** The engine, the sequencer and the controls pass their tests in the host simulator, the platform passes its safety tests, and the installer page passes its browser tests against a simulated FM-1. On hardware, FM-313 was developed on one FM-1 with Windows 11 and Edge.

FM-313 is an independent project. It is not made, endorsed or supported by Roland or M-VAVE. Installing it is at your own risk.

## Install

- From the [FM-1 Firmware Hub](https://thefirmwarehub.com), or
- with this project's installer page (`tools/make_web.py`, below), or
- `python3 tools/fm1_install.py build/fm313.fwsc`, or any installer that takes packages of the Felucca family.

If this is your first custom firmware, keep [FM-1 Transporter](https://github.com/kurogedelic/FM-1-transporter) or [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker) at hand.

FM-313 answers installers with its own identity, `FM-1_313`. Its update loader answers as `ota-FM-1_900`, the update mode that the Firmware Hub and the installers of the Felucca family carry on with. The flash chip of newer FM-1s (UC25HQ80, JEDEC `0xB36014`) is accepted like the earlier one (`0x856014`): in the firmware, in its USB rescue and in the update loader.

**Updating while FM-313 runs.** FM-313 serves no update session itself. On an installer's install command, or SETUP > UPDATE (SAVE twice), the FM-1 restarts into its update loader, which writes the firmware. Nothing is written for that restart: switching the FM-1 off and on plays again. The installers of this family go on with such a loader by themselves.

**Known issue.** On some computers (seen with Windows 11) an installer may report that it found no FM-1 at the first try. FM-313 then restarts into its update loader by itself: press Install again. On such a computer MIDI OUT over USB may not be reliable either.

**If the FM-1 does not start:** hold OCT− while switching on. That opens the USB rescue; then install again.

## Origin and legal

The sound engine is an independent clean-room development. It rests only on publicly described behaviour of the TB-303 circuit: a diode ladder low-pass with a high-pass in the resonance path, the filter and amplifier envelopes, the accent capacitor (the "wow"), the RC slide, and the square wave shaped from the sawtooth. 

The platform (boot stub, update loader, USB, keys, display) comes from [TransitFM1](https://github.com/Quixotic7/TransitFM1), which builds on Felucca, Dinghy, SLOOP and ChoralRoot. The key scan with the analog rows comes from felucca-bsp (MPL-2.0). Everything is under GPL-3.0-only; see `LICENSING.md` for the details.

## Sound engine (`firmware/engine/fm313_dsp.c`)

| Part | How |
| --- | --- |
| Oscillator | A capacitor ramp (a slightly bent sawtooth) with PolyBLEP. The square is shaped from the ramp, so its tops slope. Also "4-OSC", four detuned sawtooths. A slow pitch drift of ±3 cents |
| Filter | A 4-pole diode ladder as one coupled nonlinear system, solved implicitly (trapezoidal) per sample; tanh conductances for the diode pairs, a coupling capacitor in the resonance path (the bass stays at high resonance, the resonance is weaker at low cutoff), feedback just below self-oscillation |
| Envelopes | Filter envelope with a decay of 0.2 to 2 s, fixed at the minimum on an accent. The amplifier envelope has a fast attack, a long decay and a 3.5 ms release |
| Accent | An envelope through an RC stage whose time constant grows with the resonance, so accents in a row build up; they also raise the amplifier |
| Slide | An RC glide with a fixed time; the envelopes are not retriggered |
| Distortion | Overdrive, distortion, fuzz, crunch (a wavefolder), inside the oversampling so nothing aliases |
| Rate | 88.2 kHz inside, a 47-tap half-band FIR down to 44.1 kHz |
| Effects | Ping-pong delay (tempo-synced), FDN reverb, soft limiter |

Measured: aliasing at C6 with the filter open is below −73 dB. On a host computer the voice takes about 52 million instructions a second, about 71 million with delay and reverb. SETUP > UPDATE shows the load on the FM-1 itself. If it crackles, `-DFM313_OS=1` halves the load, without oversampling.

## Screen

The top line shows the pattern as bank and number (A01 to D16) with small marks for a chain (x3), triplets (3), lock (L) and pattern sound (S), then the tempo, and on the right the state (RUN, STOP, REC; in red the running SCATTER, or SETUP). Below it is an oscilloscope after Felucca's: one min/max value a column, a line with a narrow glow, slowly auto-scaled; only the pixels that move are drawn. When a value is turned, the same area shows it large with a bar. With a menu open, the area shows the keyboard itself for SEQ, EDIT, ENV and LFO: 16 white keys with the black ones above, and on every key what it does in this menu. SCATTER needs no drawing: its kinds are on black keys 1–8, its depth on white keys 1–16. Below that are the 16 steps (red: accent, ring: slide, white: now) and five soft keys that always show what the four knobs and ALGORITHM do right now. The key LEDs show the running step on the 16 white keys, or a layer's choice. Font: Share Tech Mono (SIL OFL 1.1).

## Controls

Three rules. The FM-1's labels mean what they say. A held button is a layer whose card the screen shows; tapped, it opens its menu. The four knobs on the right are always the sound.

| Control | Tap / turn (FX to LFO: menu open/closed; held: only while held) | With SEL |
| --- | --- | --- |
| KNOB 1–4 | Cutoff, resonance, env mod, decay | KNOB 4: reverb size in the FX menu |
| ALGORITHM | Accent | – |
| SELECT | Tempo | Shuffle |
| PRESETS | Sound preset (22 own sounds, matched in loudness by `tools/preset_loudness.py`; CLASSIC is 0.5 dB above the rest) | – |
| Keys | Stopped: play (overlapping: slide). Running: transpose (C4 = 0). Realtime recording: record | Accent |
| FX | Menu: KNOB 1–4 = drive, tone, delay, reverb; SELECT = distortion type; ALGORITHM = delay time; PRESETS = feedback; SEL+KNOB 4 = reverb size | – |
| SEQ | Menu: white keys = patterns 1–16 of the bank, black keys 1–4 = banks A–D, hold one white key and press a second = chain; KNOB 1 pattern tune, KNOB 2 pitch shift, KNOB 3 length, KNOB 4 chain; ALGORITHM shuffle; only while held: PLAY triplets, SAVE lock, ENV pattern sound, OCT± bank | Next pattern |
| EDIT | Menu: white keys 1–10 = copy, paste, clear, random, mutate, shift −1, shift +1, lock, sound to pattern, undo | Undo |
| ARP | SCATTER menu: black keys 1–8 start a kind (the same key again: stop), white keys 1–16 = depth 1–16, ALGORITHM = depth, SELECT = kind; nothing runs before a kind is chosen; it keeps running after the menu closes, HOME stops it | Last kind on/off |
| ENV | ENV strip menu: white keys = env mod by position, black keys = decay | Next wave (saw, square, 4-OSC) |
| LFO | XY play menu: long glides between the keys | Tap tempo |
| PLAY | Start/stop | – |
| REC | Step write | Realtime recording |
| SAVE | Save (held 1 s: undo) | – |
| HOME | Back to the scope: menu closed, SCATTER off, transpose 0 | – |
| GLO | SETUP: SELECT picks the row, ALGORITHM (or a knob) changes it | – |
| OCT− / OCT+ | Keyboard octave | – |

**Keys.** The keys are plain contacts, without velocity or pressure. SEL with a key gives an accent.

**Step write (REC).** The screen shows the pattern as a small piano roll: one column a step, the height is the pitch, red is an accent, a line a slide, a bar a tie; the column under the cursor is highlighted. A key sets this step's note and moves on; SEL with the key gives the accent, a key still held when the next one is struck gives the slide, as you would play it. The knobs are the step under the cursor: KNOB 1 (and SELECT) which step, KNOB 2 the note in semitones, KNOB 3 accent, KNOB 4 slide, ALGORITHM the octave; the soft keys show the values. FX sets a rest, LFO a tie, EDIT ends the pattern at the cursor. Random is optional: ARP rolls the step a note from the notes the pattern already has (else C minor pentatonic), SEL+ARP rolls all notes and keeps the rhythm, accents and slides. Stopped, every step you set sounds briefly. In realtime recording a key held into the next step gives a tie, overlapping keys a slide, SEL with the key an accent; EDIT held erases what passes.

**Usable without a manual.** One rule for all six layer buttons: a tap opens its menu, another layer button switches straight to its menu (from SETUP too; not in step write, where the buttons write), HOME or the same button closes it, held it shows only while held. Only one menu is open at a time, and the button's LED shows which. A help line under the scope shows a hint that fits the situation every 4.5 seconds. Messages name below what else a button does. In SETUP the footer explains the selected row.

**SETUP.** LOCAL, ACCENT AT, AFTERTOUCH, SCOPE, TIPS, LEDS and PHONES are kept when you leave SETUP; MIDI CH, CLOCK, MIDI OUT, TUNE and LEVEL belong to the sound and are kept with SAVE. MIDI CH, CLOCK (internal/MIDI), MIDI OUT, LOCAL (off: the keys only send MIDI), TUNE (430–450 Hz), LEVEL, ACCENT AT (the MIDI velocity from which a note is an accent), AFTERTOUCH (what MIDI aftertouch moves: filter, vibrato, nothing), SCOPE (curve, level, off), TIPS (the help line under the scope), LEDS (NORMAL; NIGHT: the active LEDs stay at full brightness and all others glow faintly, an outline of the panel in the dark; NIGHT+: the same, brighter), PHONES (NORMAL or LEFT FIX, below), UPDATE (SAVE twice: restart into the update loader).

**Headphones silent on the left.** A report: the dry sound only on the right, with delay or reverb on both sides. The firmware sends the converter the same value left and right for the dry sound. A left side that falls silent exactly when both channels are equal fits a connection where the headphones' common return sits on the right output (the left ear hears L − R, the right −R): a plug not fully in, a four-pole headset plug, an adapter. SETUP > PHONES LEFT FIX compensates exactly that (it sends L − R and −R, at half the level, since both ears now play), and only while the jack reports a plug; the speaker keeps the normal signal. The footer shows JACK: IN or OUT.

**Feature set.** Modelled on the publicly listed features of Roland's TB-3, in an implementation of its own: step and realtime recording, recording external MIDI, a triplet grid, pattern length while playing, shuffle, tap tempo, random patterns and random mutation, copy/paste, chains of neighbouring patterns, transposing by key, pattern tune (±700 cents) and master tune (430–450 Hz), pitch shift, pattern sound, pattern lock, sound presets, SCATTER with eight kinds in 16 depths (by MIDI CC too), local off, MIDI clock in and out, notes out. The keys take the touch pad's jobs: the ENV strip, the XY play and the pattern choice. The 22 presets are sounds of this project, not the TB-3's.

**MIDI (USB).** Notes play the voice or transpose the running pattern (C4 = 0); a velocity from ACCENT AT up gives an accent; in realtime recording they are recorded. Aftertouch moves what SETUP > AFTERTOUCH says, CC 1 gives vibrato, CC 102/103/104 switch SCATTER (on/off, kind, depth). CC 74 cutoff, 71 resonance, 75 env mod, 72 decay, 73 accent, 12 drive, 91 reverb, 94 delay. With MIDI clock chosen in SETUP, clock, start, continue and stop drive the sequencer. With SETUP > MIDI OUT, FM-313 sends the notes it plays (accent = velocity 127; a slide sends the new note before the old one's note off) and, as the master, clock, start and stop. See the known issue under "Install".

**Storage.** Patterns, the sound and SETUP's choices, in two banks of 8 KiB from flash 0xD0000, written in turn and guarded by a CRC. A save that is cut leaves the earlier state. Without a valid state the defaults load, with the demo patterns. The sound pauses briefly during a save, because the flash driver runs with interrupts off.

## Build

```sh
tools/get_toolchain.sh          # the JieLi Linux toolchain into ~/.jieli (pkgman.jieliapp.com)
./build.sh                      # -> build/fm313.fwsc
sh tests/fm313/run.sh           # host tests: firmware syntax, app simulator, render levels, every screen
sh tests/run_tests.sh           # the platform's safety tests (loader, boot stub, update protocol)
```

Without a local toolchain: `.github/workflows/build.yml` builds the firmware on GitHub and offers the package as a workflow artifact. A tag `vX.Y.Z` runs `.github/workflows/release.yml`, the Firmware Hub's shared build, which signs the package and attaches `fm313.fwsc` to the release.

Listening without hardware: `cc -O2 host/render.c firmware/engine/fm313_{dsp,seq,fx}.c -lm -o render && ./render out.wav 0` (scenes 0–5).

## Installer page

`python3 tools/make_web.py VERSION ASSETS build/fm313-installer.html` builds a single HTML file with the presentation, sound clips and a Web MIDI installer; the firmware is inside the page. Open it locally in Chrome or Edge. `python3 tests/fm313/browser_test.py build/fm313-installer.html` runs it in a real browser against a simulated FM-1.
