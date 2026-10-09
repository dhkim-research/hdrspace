#!/usr/bin/env bash
# Runs the cross-platform mergehdr checks and writes every result into OUT_DIR.
#   run_tests.sh MERGEHDR HDRVDP_DIR DATA_DIR OUT_DIR
# MERGEHDR    mergehdr executable
# HDRVDP_DIR  folder that contains data/ with the HDR-VDP spectra (mergehdr/hdrvdp3)
# DATA_DIR    test inputs (a47.hdr, a64.hdr)
# The same script makes the macOS reference (reference/) and the Windows results;
# compare.py compares the two folders. Commands run inside a work folder with relative
# paths, so nothing machine-specific ends up in the text output.
set -euo pipefail
MERGEHDR=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
HDRVDP_DIR=$(cd "$2" && pwd)
DATA=$(cd "$3" && pwd)
mkdir -p "$4"
OUT=$(cd "$4" && pwd)
PPD=8.52   # the PPD of the full-size picture; at 2.84 (512 px / 180 deg) the reference contrast is zero

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp -R "$HDRVDP_DIR/data" "$WORK/data"   # HDR-VDP looks for data/... from the working folder
cp "$DATA"/a47.hdr "$DATA"/a64.hdr "$WORK/"
cd "$WORK"

run() {  # run NAME args... : stdout -> NAME.txt
    local name=$1; shift
    "$MERGEHDR" "$@" > "$OUT/$name.txt"
}

run evalglare_d        evalglare -d a47.hdr
run evalglare_check    evalglare -c evalglare_check.hdr a64.hdr
for map in l m rod lm adaptation detectable-contrast eqv-luminance; do
    run "pmap_$map" perceptualmap --map "$map" --ppd $PPD -o "pmap_$map.hdr" a47.hdr
done
run viewvis_hdrvdp3    view_visibility -ref a47.hdr -test a64.hdr --ppd $PPD -o viewvis_hdrvdp3.hdr
run viewvis_proxy      view_visibility -ref a47.hdr -test a64.hdr --ppd $PPD --detail contrast-proxy -o viewvis_proxy.hdr
run rotate             rotate -angle 12.5 -output rotate.hdr a47.hdr
run hdrcrop            hdrcrop -crop 64 32 256 300 -output hdrcrop.hdr a64.hdr

for f in *.hdr; do
    case "$f" in a47.hdr|a64.hdr) ;; *) mv "$f" "$OUT/";; esac
done
echo "done: $(ls "$OUT" | wc -l | tr -d ' ') files in $OUT"
