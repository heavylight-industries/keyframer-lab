# keyframer-lab

The keyframe time stretcher from the Digitakt II keyframer mod, with its TKEO transient detector, running live in
the browser: **https://heavylight-industries.github.io/keyframer-lab/**

The engine is the unit's own C, compiled to WebAssembly with no changes to its arithmetic, so what you hear and see
is what the Digitakt plays. It starts on a synthesized drum loop. Open a folder or drop files to use your own
samples. Everything runs in the page; nothing is uploaded.

## What you see

- **Splices · sample**: the whole sample. The reference head walks through it at STR's rate, the play head reads it
  at TUNE's rate. Each splice (the play head crossfading back to the reference) draws an arc in the lane above.
  Slices alternate two colours, each as bright as its loudest hit (the detector's peak there), so the strong and
  the soft hits read at a glance; a dot marks each slice THIN keeps. A stereo sample's channels are also analysed
  apart, as on the unit (elemod's stereo detection): each lane's slices are THIN's picks from its own channel's
  transients, in the same mode and number (as the unit's WERP+ picks THIN per channel), and its voice plays those
  borders; in RAND, or wherever the channels' hits differ, the two lanes' sections don't line up. The numbers on top
  are the shared slices a click or a pad plays. Each channel's voice is drawn apart too: its heads in its lane, its
  splices as arcs (the left's rising from the splice lane's floor, the right's hanging from its top), and a row of its
  own in the zoomed view.
- **Zoomed in**: the heads up close, each a small flag on its line (the legend names them: the reference, the play
  head, the incoming head of a crossfade), with the keyframes (the sample's peaks and troughs) between them, ticked on
  the floor and shaded. When more than K of them lie between the heads, the play head splices: the dotted line with
  its own flag marks where. Drag it to look around while nothing plays.
- **Light or dark**: the switch in the header (the screens stay dark).
- **Load**: the header's readout, the engine's render time against the audio it made (the audio thread), and the
  page's drawing against each frame.
- **The detector**: VOL (the loudest hits), GRID (even steps, each moved to the nearest transient) or RAND, THIN
  (how many transients the engine keeps as borders), LEN (how many slices a clicked slice or a pad plays, as the
  Digitakt's SLICE machine) and Smoothing (the TKEO follower's cutoff; 46 Hz to start, the unit's 25.6 Hz a snap point).
- **PLAY**: forwards or backwards (the play head's direction: the grains), once or looped. The song's direction is
  STR's sign, as on the unit.
- **Keyframes**: the analyzer keeps a peak or trough only when it differs from the last keyframe by more than 0.01
  of full scale (the unit's), so quiet passages and the noise floor get few keyframes and the grains adapt.
- **The knobs**: STR (the reference's speed), TUNE (the play head's pitch, ±60 semitones), SIZE (K, the leash).
  The lock is a three-way switch: Tune follows (turning STR takes TUNE along at their ratio), Off, or Str follows
  (turning TUNE takes STR along); slow everything down to watch, the splices and borders the same.

## MIDI and your DAW

In Chrome or Edge, **Connect MIDI** and the page listens to your controllers and your DAW:
- **Pads play slices**: from C1 (note 36) up, each note plays the next slice, its velocity the level. **Learn pads**
  maps your own pads in order (a Korg padKONTROL's scene, say). In the loop modes, releasing a pad stops its slice.
- **Learn knobs**: touch a control on the page, then move a knob or slider on the controller.
- **Clock**: the DAW's tempo is shown; its start and stop start and stop playback, and **Fit the region to N bars**
  sets STR so the region spans that many bars at the DAW's tempo.

To record it in a DAW, pick an output on the page (no change to your system's): **BlackHole 2ch** on macOS,
**VB-CABLE** on Windows, and arm a track that records from it. On Linux with PipeWire, wire the browser's output to
the DAW in qpwgraph or Helvum. The page shows these steps for your system, with the virtual MIDI port for each
(the IAC bus, loopMIDI, PipeWire's own).

## Building

Needs clang with the wasm32 target, wasm-ld, python3, and node for the check.

    sh tools/build_kfweb.sh                  # build/kfweb.wasm, build/kfweb.html, build/kfweb-site/index.html
    cp build/kfweb-site/index.html index.html  # the page GitHub Pages serves

`KF=path/to/kf sh tools/build_kfweb.sh` builds against another checkout's engine sources.

## Checking

    sh tools/check_kfweb.sh some.wav other.wav   # 16-bit PCM WAVs, mono or stereo

For each file it checks that:
1. the native build of the app's engine glue equals `kfrender` (the engine on the host) sample for sample;
2. the WebAssembly build, run in node, equals the native build byte for byte, through every control: transients,
   slices, reverse, play-once endings, retriggers;
3. the detector with its constants as variables (for Smoothing and Cut snap), left at the unit's values, equals the
   fixed build.

## Layout

| path | what |
|---|---|
| `kf/kf.c`, `kf/kf.h` | the keyframe engine (the DAFx26 paper's method): reference and play heads, extrema keyframes, splices past K, transient borders |
| `kf/tkeo.c`, `kf/tkeo.h` | the TKEO transient detector and its slice pickers |
| `kf/host/kfweb.c` | the glue: the unit's ONESHOT path (STR, TUNE, SIZE, PLAY, edge fades), the analysis, the picture's data |
| `kf/host/kfweb.html` | the page (the module inlined at build) |
| `kf/host/kfweb_host.c`, `kfweb_check.mjs`, `kfrender.c` | the native and node drivers and the host renderer the check compares |
| `tools/` | the build and the check |

The engine files come from the keyframer mod's tree; changes to the engine are made there first and copied here.

## How it was made

[docs/conversation.md](docs/conversation.md) is the conversation in which it was built, from the first brief to
this repo: heavylight's notes and Claude's replies, without the tool output.

## License

GNU AGPL-3.0-or-later (LICENSE).
