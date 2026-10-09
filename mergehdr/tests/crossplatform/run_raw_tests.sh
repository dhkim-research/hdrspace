#!/usr/bin/env bash
# RAW-merge checks: a 4-frame Canon EOS R5 Mark II bracket (ND3 filter) through `mergehdr run`.
#   run_raw_tests.sh MERGEHDR RAW_DIR OUT_DIR
# The RAW files and the macOS reference results are too large for the repository; CI downloads
# them from the release "crossplatform-raw-data". The work folder name contains a space, so
# the quoting of the mergehdrcore command line is exercised as well.
set -euo pipefail
MERGEHDR=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
RAW=$(cd "$2" && pwd)
mkdir -p "$3"
OUT=$(cd "$3" && pwd)

BASE=$(mktemp -d)
trap 'rm -rf "$BASE"' EXIT
WORK="$BASE/raw bracket"
mkdir -p "$WORK"
cp "$RAW"/*.CR3 "$WORK/"
cd "$WORK"

CROP="-crop 2400 1600 1024 1024"   # sensor crop: merge, demosaic and colour, no fisheye step
run() {  # run NAME args... : the merged HDR is NAME.hdr, the console text is not compared
    local name=$1; shift
    "$MERGEHDR" -profile R5m2ND3 run "$@" > "$OUT/$name.log" 2>&1 || { cat "$OUT/$name.log"; return 1; }
}
run raw_ahd           $CROP --no-fisheye -o raw_ahd.hdr *.CR3
# absolute paths with a space in them
run raw_dht_linearhdr $CROP --no-fisheye --bloom-prevent -o "$WORK/raw_dht_linearhdr.hdr" "$WORK"/*.CR3
run raw_demosaicfirst $CROP --no-fisheye --merge-weight linearhdr --demosaic dht --demosaic-first -o raw_demosaicfirst.hdr *.CR3
run raw_fisheye_full  -o raw_fisheye_full.hdr *.CR3
# HDR written to stdout (binary mode on Windows)
"$MERGEHDR" -profile R5m2ND3 run $CROP --no-fisheye *.CR3 > raw_ahd_stdout.hdr 2> "$OUT/raw_ahd_stdout.log"
mv ./*.hdr "$OUT/"
rm -f "$OUT"/*.log
echo "done: $(ls "$OUT" | wc -l | tr -d ' ') files in $OUT"
