#!/bin/bash
# render a grid of candidate cameras
cams=(
"3 1.7 8.0 8 2.5 12.5"
"6 1.7 7.5 10 2.5 12.5"
"9 1.7 7.5 12 2.5 12.5"
"12 1.7 7.5 14 2.5 12.5"
"15 1.7 7.5 16 2.5 12.5"
"18 1.7 8.5 15 2.5 13.5"
"2 1.7 12 6 2.5 15"
"19 1.7 12 16 2.5 15"
"8 1.7 6.0 12 2.5 11.5"
"11 1.7 5.5 11 2.5 11.5"
"14 1.7 6.0 14 2.5 11.5"
"5 1.7 9.0 8 2.5 11.5"
"17 1.7 9.0 15 2.5 11.5"
)
i=0
for c in "${cams[@]}"; do
  i=$((i+1))
  VF_NO_OVERLAY=1 timeout 300 ./build/voxelforge --shot /tmp/opencode/cs_$i.ppm --cam $c >/dev/null 2>&1
  echo "rendered cs_$i: $c"
done
