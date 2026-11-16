#!/bin/bash
# usage: bench.sh <exe> <outdir> [frames]: Tekken 4 from power-on into a fight with scripted presses, software renderer
exe=$1; out=$2; frames=${3:-4200}
G=/c/ps2-native-recomp-main/games/Tekken-4
rm -rf "$out"/*; mkdir -p "$out"
cp $G/states/run_2026-10-29_22-54-13_f000000.inputs.ps2 "$out/card.ps2"
presses=(--press 300:start); for f in 500 650 800 950; do presses+=(--press $f:cross); done; for f in 1050 1250 1450 1650; do presses+=(--press $f:start); done; for ((f=2000; f<frames; f+=40)); do presses+=(--press $f:square); done
s=$(date +%s%N)
PS2_GS=${GS:-off} "$exe" $G/disc/SLUS_203.28 --disc $G/disc.iso --memcard "$out/card.ps2" --rtc 1793310853 --frames $frames "${presses[@]}" --frames-dir "$out" --frame-every 100 > "$out/log.txt" 2>&1
code=$?
e=$(date +%s%N)
echo "exit $code, $(( (e-s)/1000000 )) ms, $(ls $out/*.ppm | wc -l) frames"
