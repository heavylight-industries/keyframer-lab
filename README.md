# keyframer-lab

The keyframe time stretcher from the Digitakt II keyframer mod, with its TKEO transient detector, running live in
the browser: **https://heavylight-industries.github.io/keyframer-lab/**

The engine is the unit's own C, compiled to WebAssembly with no changes to its arithmetic, so what you hear and see
is what the Digitakt plays. It starts on a synthesized drum loop. Open a folder or drop files to use your own
samples. Everything runs in the page; nothing is uploaded.

## What you see

- **Splices · sample**: the whole sample. The reference head walks through it at STR's rate, the play head reads it
  at TUNE's rate. Each splice (the play head crossfading back to the reference) draws an arc in the lane above.
  Slices alternate two colours; a dot marks each slice THIN keeps.
- **Zoomed in**: the heads up close, with the keyframes (the sample's peaks and troughs) between them. When more
  than K of them lie between the heads, the play head splices. Drag it to look around while nothing plays.
- **The detector**: VOL (the loudest hits), GRID (even steps, each moved to the nearest transient) or RAND, THIN
  (how many transients the engine keeps as borders), Smoothing (the TKEO follower's cutoff) and Cut snap (how far a
  cut may move to a zero crossing).
- **Keyframes**: the analyzer's threshold. A peak or trough becomes a keyframe only when it differs from the last
  one by more than this; the unit runs 0.01 of full scale, so quiet passages and the noise floor get few keyframes
  and the grains adapt to the material.
- **The knobs**: STR (the reference's speed), TUNE (the play head's pitch, ±60 semitones), SIZE (K, the leash).
  Lock STR + TUNE to keep their ratio while slowing everything down to watch.

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
this repo: the owner's notes and Claude's replies, without the tool output.

## License

GNU AGPL-3.0-or-later (LICENSE).
