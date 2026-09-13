# BeatStep Sync

**BeatStepSync** is a VCV Rack 2 module providing two-way live synchronization between VCV Rack and a real Arturia BeatStep hardware sequencer over MIDI SysEx. 

*Note: This module requires the original Arturia BeatStep hardware (it is not compatible with the BeatStep Pro model). It communicates using Arturia's reverse-engineered SysEx protocol.*

> **Non-affiliation notice:** This project is not affiliated with, endorsed by, or officially connected to Arturia. It interoperates with the BeatStep using its reverse-engineered SysEx protocol purely for interoperability. No Arturia software, firmware, or copyrighted documentation text is redistributed, and no Arturia logo or trademark artwork is used anywhere in this plugin's own graphics (the panel is drawn from scratch in code, no SVG assets).

## Features

- **Bidirectional Hardware Sync:** Fully synchronizes step notes, gates, and all 9 global sequencer parameters both ways between VCV Rack and your physical BeatStep.
- **On-Device Preset Management:** Browse, save, and recall all 16 hardware preset memory slots directly from the panel or via CV/Trigger inputs, with live highlighting of the slot currently addressed by CV.
- **CV Control of Global Parameters:** All 9 global parameters can be driven directly by CV (0-10V), in addition to their knobs.
- **Configurable Randomization:** The note-octave range and gate density used by the Rnd Notes / Rnd Gates buttons are adjustable from the module's right-click menu.
- **Robust SysEx Engine:** Built with a bounded wait-for-reply polling loop and settlement delays to ensure rock-solid hardware communication without dropped packets or race conditions.
- **Custom Panel Layout:** Clean, custom-drawn 32HP single-column interface designed specifically for live performance and deep hardware integration.

---

## Panel Layout & Controls

The module panel is arranged in a 32HP single column, from top to bottom:

1. **Title Bar:** Centered module title ("BEATSTEP SYNC") with a "Connected" / "Disconnected" status dot and label at the top right, reflecting whether a MIDI output device is currently selected.
2. **MIDI I/O Displays:** Two single-line clickable displays side-by-side for MIDI IN and MIDI OUT. Each shows the selected device name. Long OS device names are truncated from the *front* with a leading `...` so the trailing, most-distinguishing port number remains visible. Click either display to open a combined driver and device selection menu.
3. **Step Grid (Steps 1–16):** A 2-row × 8-column grid of square step pads.
   - **Display:** Each pad shows its step number (top-left) and the actual sounding note name (e.g., `C4`), which incorporates the current Transpose knob offset applied to the raw stored note. A small dot at the bottom-right indicates gate status (filled green = gate on).
   - **Interactions:** Left-click toggles the gate; using the scroll wheel over a pad nudges its raw note by ±1 semitone; right-click opens a context menu to toggle the gate or jump the raw note to any octave, labeled `Oct -4`…`Oct +4` relative to the Transpose center (`Oct 0`), each showing what it currently sounds like given the live Transpose value.
4. **Utility Buttons:** Positioned directly beneath the step grid:
   - **Rnd Notes:** Randomizes notes for all active steps within the C2–C5 range.
   - **Rnd Gates:** Randomizes gate states at approximately 60% density.
   - **Refresh:** Forces an immediate extra polling pass to re-sync state.
5. **Global Parameters:** A single row of 9 small knobs representing the global sequencer settings. Each knob has its name centered directly above it and its live value centered directly below it for clear association:
   - `Channel`, `Transpose`, `Scale`, `Mode`, `Step sz`, `Length`, `Swing`, `Gate len`, `Legato`.
   - Scrolling is always one integer step per wheel tick, regardless of range. Dragging steps by a fixed pixel distance too, but that distance scales so a full sweep of any parameter's range takes a similar total drag distance (clamped 8-40px/step) -- few-state params like `Legato` (3 states) or `Mode`/`Step sz` (4 states) get a larger, gentler step instead of their whole range collapsing into a couple centimeters of drag.
6. **Global Parameter CV Row:** Directly beneath each of the 9 knob columns sits a small CV input jack, sharing the knob's own name label above it. When patched, the CV (0–10V) overrides the knob, quantized in equal steps across that parameter's own range.
7. **On-Device Preset Memory Section:** Labeled *"BEATSTEP PRESETS (device memory)"*, located at the bottom of the module:
   - **Preset Grid:** An 8×2 grid of 16 numbered slot buttons (intentionally styled smaller than the step pads to indicate secondary importance). The slot currently selected for Save/Recall is highlighted amber; the slot currently addressed by `Slot CV` (if patched) is highlighted cyan, independently of the amber selection.
   - **Buttons & Readout:** `Save -> Slot`, `Recall <- Slot`, and a `Selected: NN` indicator. *(Note: Save is manual-only to prevent accidental destructive overwrites of hardware memory via errant CV triggers).*
   - **CV/Gate Jacks:** `Slot CV` (1V per slot, 0–15V covering slots 1–16) and `Recall Trig` (rising edge detection to trigger recalls).

---

## Global Parameters

All 9 global parameters are fully read from the hardware, written to it, and displayed live:

- **Channel:** MIDI channel (1–16).
- **Transpose:** Base-note value displayed as a note name, restricted to the hardware's confirmed usable range of C2–C6 (reference point for "no transpose" is note 60 / `0x3C`, i.e. C4).
- **Scale:** Chromatic / Major / Minor / Dorian / Mixolydian / Harmonic Minor / Blues / User.
- **Mode:** Forward / Reverse / Alternating / Random.
- **Step sz:** 1/4, 1/8, 1/16, 1/32.
- **Length:** 1–16 steps.
- **Swing:** 50–75%.
- **Gate len:** 50–99%.
- **Legato:** Off / On / Reset.

### Legato Modes
Behavioral semantics verified against Arturia's official BeatStep User's Manual (Section 6.5.5):
- **Off (Default):** Notes are entirely separate. The Note Off of one step always occurs before the Note On of the next. Per-step Gate Length functions normally.
- **On:** Consecutive notes overlap. The Note On of the next step happens before the Note Off of the previous step, eliminating audible retriggers/attacks between tied notes (true legato). Gate Length has no effect in this mode.
- **Reset:** Identical overlapping/legato behavior to **On**, *except* the hardware forces a Note Off for the final step and a Note On for the first step at the start of every loop boundary. This provides continuous legato phrasing within the pattern while guaranteeing a clean retrigger at the start of each loop (ideal for retriggering envelopes or LFOs reliably once per cycle).

---

## CV Control of Global Parameters

All 9 global parameters can be driven by CV in addition to their knob, each with its own dedicated input jack directly below the corresponding knob column: `Channel`, `Transpose`, `Scale`, `Mode`, `Step sz`, `Length`, `Swing`, `Gate len`, `Legato`.

- Each CV input accepts **0–10V**, quantized in equal-width steps across that parameter's own discrete range (e.g. Transpose CV 0–10V maps to C2–C6 in semitone steps; Scale CV 0–10V selects among the 8 scales).
- Writes are rate-limited to roughly one SysEx update per 20ms per input, so a fast LFO or noise source patched into a CV won't flood the hardware's MIDI link.
- When a CV input is patched, it overrides the corresponding knob; unpatching returns control to the knob.

## Randomization Settings

The `Rnd Notes` and `Rnd Gates` buttons use ranges configurable from the module's right-click context menu:

- **Rnd Notes: min/max octave** — sets the raw note range (C0–C8) used when randomizing note pitches. Since Rnd Notes writes raw notes (same as the step pad's manual octave picker), the menu also shows what the range currently sounds like given the live Transpose value.
- **Rnd Gates: density** — sets the probability (10%–90%) that a given step's gate is turned on.

---

## Presets & Recall Behavior

- **Saving:** Clicking `Save -> Slot` stores the current panel and sequence state directly into the currently selected hardware memory slot. This action is manual-only for safety.
- **Recalling:** Recalls can be executed manually by clicking `Recall <- Slot` (using the selected slot in the grid) or externally via `Slot CV` paired with a rising edge on `Recall Trig`.
- **Live CV Highlight:** Whenever `Slot CV` is patched, the preset grid cell it currently points to is highlighted cyan in real time, independent of (and possibly different from) the amber-highlighted slot selected for `Save -> Slot` / `Recall <- Slot`.

---

## Notes on Hardware Sync (Troubleshooting & Fixes)

This section documents specific historical bugs and their resolutions for user troubleshooting reference:
- **Flaky Gate/Note Sync:** Early polling implementations used a fixed 15ms sleep window after sending read requests, causing replies arriving outside the window to be dropped (as subsequent F0 bytes reset the SysEx reassembly buffer). This has been replaced with a bounded wait-for-reply loop (checking every 2ms, timing out at 60ms, and returning immediately upon complete frame assembly).
- **Inconsistent Recall:** Recalling a preset causes the hardware to internally switch active patterns before its parameters reflect the change. If poll requests landed too quickly, queries returned a half-old/half-new data mix. This is resolved by inserting a ~150ms settlement delay before the next polling pass specifically following recalls.
- **Octave Display Correction:** The note display formula was previously offset by one octave (`note/12 - 2`, showing raw note 60 as `C3`). This has been updated to standard middle-C convention (`note/12 - 1`, where raw note 60 = `C4`). Context menu octave pickers have been aligned accordingly.
- **Full Parameter Sync:** Prior versions only supported pattern length synchronization. All 9 global parameters (transpose, scale, mode, step size, swing, gate length, legato, etc.) are now fully synchronized bidirectionally.
- **Randomization Reverting to Old Values:** `Rnd Notes` / `Rnd Gates` used to fire up to 16 SysEx writes directly from the button click, back-to-back with no gap, on the GUI thread -- both outrunning the hardware's own receive rate (dropping writes) and racing the background poller's own concurrent reads of the same steps, either of which could make the change look like it "reverted." Both buttons now just flag the request; the actual writes happen on the poll thread itself (which is the only thread that ever touches MIDI I/O), paced ~8ms apart, so there's no burst, no race, and no GUI blocking.
- **Slow Hardware-to-Software Feedback:** Writes (software -> hardware) are immediate, but a physical change on the hardware (turning a knob, hitting a pad) was only picked up on the poller's next full pass, and the gap between passes was up to 2 seconds (~1 second average wait). This gap is now capped much lower, so physical changes reflect back into the panel noticeably faster.
- **Manually Turning a Knob Fought Itself:** After the poller's gap was shortened (previous point), its periodic "sync hardware state into the knobs" step started interrupting an in-progress manual knob drag -- the knob would get yanked back to the last-confirmed (already stale) hardware value faster than a manual turn could move it forward. Each global param now gets a brief grace window after a manual (or CV) change during which the poller's push is skipped for that param, giving the write its own read-back round trip time to catch up first.
- **Misleading Octave Picker Labels:** The step pad's right-click octave picker used to show absolute note names (`C0`…`C8`), which only matched what the pad actually sounds like when Transpose was at its default center -- picking "C4" with Transpose shifted elsewhere silently set the raw note to 60 without making the step sound like C4 at all. Relabeled as `Oct -4`…`Oct +4` relative to the Transpose center, each entry now also shows what it currently sounds like given the live Transpose value.

---

## Threading Model

- **GUI Thread:** Handles panel rendering, user clicks, scroll events, and context menus.
- **Audio Thread (`process()`):** Updates panel indicator lights, pushes synced values into knobs, and monitors the `Recall Trig` input to send recall SysEx commands directly (sending MIDI output from the audio thread is fully supported in VCV Rack).
- **Background Poll Thread:** Continuously re-reads step notes/gates and all 9 global parameters from the hardware to reflect physical adjustments made directly on the BeatStep back into the GUI. Shared state (`BSState`) is protected by a mutex, and MIDI output transmissions are serialized via a dedicated mutex to prevent collisions between the poll, audio, and GUI threads.

---

## Credits

- **SysEx Protocol Reverse-Engineering:** [untergeek.de BeatStep SysEx Guide](https://www.untergeek.de/2014/11/taming-arturias-beatstep-sysex-codes-for-programming-via-ipad/)

---

## Build & Install

- Build using the VCV Rack SDK Makefile system (`RACK_DIR=/path/to/Rack-SDK make`), or use the bundled build script (`./build.sh` supports `build`, `install`, and `clean` subcommands).
- **Linux Installation Path:** `~/.local/share/Rack2/plugins-lin-x64/BeatStepSync`
- *Note:* `jq` is required as a build dependency because the VCV Rack SDK's `plugin.mk` shells out to `jq` to read metadata from `plugin.json`.

