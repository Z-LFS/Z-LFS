#!/bin/bash
# 用法: sudo ./reset_zns_zones.sh /dev/nvme3n2 10
# 参数1: ZNS 设备名
# 参数2: 需要 reset 的 zone 数量（默认 5）

DEV=$1
COUNT=${2:-5}

if [[ -z "$DEV" ]]; then
    echo "用法: $0 <zns_device> [zone_count]"
    exit 1
fi

echo "[INFO] 获取设备 $DEV 的 zone 信息..."
# 提取起始扇区 (十进制)
STARTS=$(blkzone report $DEV | awk '/start:/ {print strtonum($2)}' | head -n $COUNT)

echo "[INFO] 将 reset 前 $COUNT 个 zones:"
echo "$STARTS"

for start in $STARTS; do
    echo "[RESET] zone 起始扇区: $start"
    blkzone reset --offset $start --count 1 $DEV --force
done

echo "[INFO] reset 完成，重新查看前 $COUNT 个 zone 状态:"
blkzone report $DEV | head -n $COUNT
