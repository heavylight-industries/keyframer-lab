#!/bin/sh
# The browser app's numbers against the C's (after tools/build_kfweb.sh, with the same KF):
#   1. the native build of kf/host/kfweb.c against kfrender, the engine alone (no transients, looped): equal
#   2. build/kfweb.wasm in node against the native build, every control, the transients, slices, reverse,
#      play-once ends, retriggers: equal, byte for byte, and the same printed state
#   3. the native build with the detector's constants as variables (at the unit's values) against it with them
#      fixed (-DKW_FIXED): equal
#
#   sh tools/check_kfweb.sh WAV...      (16-bit PCM WAVs; mono and stereo)
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
O=$R/build/kfweb/check
mkdir -p "$O"
fail=0
for W in "$@"; do
    b=$(basename "$W" .wav)
    # 1. against kfrender: STR 1, TUNE 0, SIZE 60 (K 64), looped, full scale, no transients
    "$R/build/kfweb/kfrender" "$W" "$O/$b.kfrender.wav" --k 64 2>/dev/null
    n=$(python3 -c "import sys,struct; d=open(sys.argv[1],'rb').read(); print(struct.unpack('<I',d[40:44])[0]//4//struct.unpack('<H',d[22:24])[0])" "$O/$b.kfrender.wav")
    printf 'set 7 0.000030517578125\nset 3 2\nset 2 60\nset 6 0\ntrig 0\nrender %s\n' "$n" > "$O/raw.txt"
    "$R/build/kfweb/kfweb_host" "$W" "$O/raw.txt" "$O/$b.raw.f32" > /dev/null
    if python3 - "$O/$b.kfrender.wav" "$O/$b.raw.f32" <<'PY'
import struct, sys
w = open(sys.argv[1], "rb").read()
ch = struct.unpack("<H", w[22:24])[0]
ref = w[44:]
got = open(sys.argv[2], "rb").read()
n = len(ref) // (4 * ch)
if ch == 1:   # (kfweb's output is two channels, the mono copied)
    got = b"".join(got[8 * i:8 * i + 4] for i in range(n))
sys.exit(0 if got[:len(ref)] == ref and len(got) == len(ref) else 1)
PY
    then echo "$b: kfweb (native) == kfrender, $n frames"; else echo "$b: kfweb (native) != kfrender: FAIL"; fail=1; fi
    # 2. the WebAssembly build against the native one
    cat > "$O/full.txt" <<'S'
analyze
thin 1 8
set 0 1.5
set 1 -5
set 2 40
set 3 3
trig 0
render 48000
thin 2 12
set 0 -0.7
render 24000
set 3 1
trig 0
render 30000
set 6 0
render 9600
set 4 20000
set 5 90000
set 3 3
trig 1
render 40000
thin 3 5
set 6 1
set 3 2
set 0 0
set 1 7
set 2 90
trig 0
render 20000
set 0 3.3
set 1 -12
set 2 5
trig 0
render 30000
stop
render 512
S
    "$R/build/kfweb/kfweb_host" "$W" "$O/full.txt" "$O/$b.native.f32" > "$O/$b.native.txt"
    node "$R/kf/host/kfweb_check.mjs" "$R/build/kfweb.wasm" "$W" "$O/full.txt" "$O/$b.wasm.f32" > "$O/$b.wasm.txt"
    "$R/build/kfweb/kfweb_host_fixed" "$W" "$O/full.txt" "$O/$b.fixed.f32" > "$O/$b.fixed.txt"
    if cmp -s "$O/$b.native.f32" "$O/$b.fixed.f32" && cmp -s "$O/$b.native.txt" "$O/$b.fixed.txt"; then
        echo "$b: the detector's constants as variables == fixed"
    else
        echo "$b: the detector's constants as variables != fixed: FAIL"; fail=1
    fi
    if cmp -s "$O/$b.native.f32" "$O/$b.wasm.f32" && cmp -s "$O/$b.native.txt" "$O/$b.wasm.txt"; then
        echo "$b: kfweb.wasm == native, $(($(stat -c %s "$O/$b.native.f32") / 8)) frames; $(grep -c render "$O/$b.native.txt") states"
    else
        echo "$b: kfweb.wasm != native: FAIL"; diff "$O/$b.native.txt" "$O/$b.wasm.txt" | head -5; fail=1
    fi
done
exit $fail
