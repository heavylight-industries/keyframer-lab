#!/bin/sh
# The keyframer in the browser: build/kfweb.wasm (kf/host/kfweb.c with kf/kf.c and kf/tkeo.c), the page
# (kf/host/kfweb.html with the module inlined) and the native driver the check compares it with.
#
#   sh tools/build_kfweb.sh                          (the engine in this checkout's kf/)
#   KF=../dt2-keyframer-elemod/kf sh tools/build_kfweb.sh   (another checkout's engine, as it is there)
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
KF=$(cd "${KF:-$R/kf}" && pwd)
O=$R/build/kfweb
mkdir -p "$O"
W="clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin -ffp-contract=off -Wall -Wno-unused-function"
$W "-DKW_KF_C=\"$KF/kf.c\"" "-DKW_TKEO_C=\"$KF/tkeo.c\"" -c "$R/kf/host/kfweb.c" -o "$O/kfweb.o"
$W "-DKW_TKEO_H=\"$KF/tkeo.h\"" -c "$R/kf/host/kfweb_def.c" -o "$O/kfweb_def.o"
wasm-ld --no-entry --export-all --initial-memory=$((40 * 1024 * 1024)) --max-memory=$((64 * 1024 * 1024)) \
    "$O/kfweb.o" "$O/kfweb_def.o" -o "$R/build/kfweb.wasm"
echo "build/kfweb.wasm: $(stat -c %s "$R/build/kfweb.wasm") bytes (engine: $KF)"
H="cc -std=c99 -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -ffp-contract=off"
$H "-DKW_KF_C=\"$KF/kf.c\"" "-DKW_TKEO_C=\"$KF/tkeo.c\"" "-DKW_TKEO_H=\"$KF/tkeo.h\"" \
    "$R/kf/host/kfweb_host.c" "$R/kf/host/kfweb_def.c" -lm -o "$O/kfweb_host"
# (the constants fixed: the check's reference for the variables at the unit's values)
$H -DKW_FIXED "-DKW_KF_C=\"$KF/kf.c\"" "-DKW_TKEO_C=\"$KF/tkeo.c\"" "$R/kf/host/kfweb_host.c" -lm -o "$O/kfweb_host_fixed"
cc -std=c99 -O2 -Wall -ffp-contract=off "$KF/kf.c" "$KF/tkeo.c" "$KF/host/kfrender.c" -lm -o "$O/kfrender"
# the page: kf/host/kfweb.html with the module inlined (a body for claude.ai's artifact host, and a whole
# document for any static host)
python3 - "$R" <<'PY'
import base64, os, sys
r = sys.argv[1]
page = open(r + "/kf/host/kfweb.html").read()
b64 = base64.b64encode(open(r + "/build/kfweb.wasm", "rb").read()).decode()
assert page.count("__KFWEB_WASM__") == 1
page = page.replace("__KFWEB_WASM__", b64)
open(r + "/build/kfweb.html", "w").write(page)
os.makedirs(r + "/build/kfweb-site", exist_ok=True)
open(r + "/build/kfweb-site/index.html", "w").write(
    '<!doctype html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n'
    '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n</head>\n<body>\n'
    + page + '</body>\n</html>\n')
print("build/kfweb.html, build/kfweb-site/index.html")
PY
