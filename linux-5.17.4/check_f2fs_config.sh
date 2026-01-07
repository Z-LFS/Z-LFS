#!/bin/bash
# 检查内核配置中 F2FS + 压缩支持

CONFIG_FILE="./.config"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "❌ 没有找到 .config 文件，请先执行 make menuconfig 并保存退出"
    exit 1
fi

check_config() {
    local option=$1
    grep -q "^$option=y" "$CONFIG_FILE"
    if [ $? -eq 0 ]; then
        echo "✅ $option 已启用"
    else
        echo "❌ $option 未启用"
    fi
}

echo "=== 检查 F2FS 和压缩算法配置 ==="
check_config CONFIG_F2FS_FS
check_config CONFIG_F2FS_FS_LZ4
check_config CONFIG_F2FS_FS_ZSTD
check_config CONFIG_LZ4_COMPRESS
check_config CONFIG_ZSTD_COMPRESS
check_config CONFIG_ZSTD_DECOMPRESS
