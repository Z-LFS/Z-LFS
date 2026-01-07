#!/bin/bash

OUTDIR=~/Z-LFS/blkzoneRpt
mkdir -p "$OUTDIR"

for dev in /dev/nvme3n{2..9}; do
    logfile="$OUTDIR/$(basename $dev).log"

    echo ">>> 正在生成 $dev 的 blkzone report ..."
    sudo blkzone report "$dev" | nl -v 0 -w 10 | sed 's/^[[:space:]]*//' > "$logfile"

    fu=$(grep -o "(fu)" "$logfile" | wc -l)
    em=$(grep -o "(em)" "$logfile" | wc -l)
    oi=$(grep -o "(oi)" "$logfile" | wc -l)

    echo "统计结果 for $dev:"
    echo "  fu (full)   : $fu"
    echo "  em (empty)  : $em"
    echo "  oi (open)   : $oi"
    echo ""
done
