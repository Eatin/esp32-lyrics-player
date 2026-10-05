#!/usr/bin/env bash
# ============================================================================
#  ESP32-S3-Touch-LCD-3.49 MusicPlayer —— 一键编译 + 烧录脚本  (已验证可用)
#
#  用法:
#     ./build_flash.sh                 # 编译
#     ./build_flash.sh COM4            # 编译 + 烧录到指定串口
#     ./build_flash.sh COM4 upload     # 同上
#     ./build_flash.sh COM4 monitor    # 编译 + 烧录 + 打开串口监视器(115200)
#
#  依赖:
#     tools/arduino-cli.exe
#     tools/arduino-cli.yaml
#     tools/arduino-user/libraries/lvgl         (LVGL 8.4.0)
#     tools/arduino-user/libraries/lv_conf.h    ★必须与 lvgl 同级★
#
#  重要环境前提（缺一不可）:
#     - esp32 core 3.3.0
#     - 预编译库 esp32-arduino-libs/idf-release_v5.5-b66b5448-v1  (IDF 5.5)
#       ← 若装成 v5.1 会报 `driver/i2c_master.h: No such file`
#     - 工具链 esp-x32 (GCC 14.2)、esptool 5.x
# ============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
CLI="$HERE/arduino-cli.exe"
CFG="$HERE/arduino-cli.yaml"
SKETCH="$ROOT/MusicPlayer"
BUILD="$HERE/build_v5"

PORT="${1:-}"
ACTION="${2:-upload}"

# --- 板级参数（对应官方文档 Tools 设置）------------------------------------
# 注意：必须用 --fqbn 后缀 或 --board-options 传递，不能用 --build-property。
#
# ★ PartitionScheme 必须用 custom★：字库扩容后 app 约 3.9 MB，原来的
#   app3M_fat9M_16MB（3 MB APP/9.9 MB FATFS）装不下，而且那个 9.9 MB FAT
#   分区本工程从未使用（只用 SD 卡）。
#   真正的分区布局由**工程目录下的 MusicPlayer/partitions.csv** 决定
#   （platform.txt: recipe.hooks.prebuild.1 会让 sketch 目录的 partitions.csv
#   优先于菜单选项），内容是 app 7 MB × 2（保留 OTA）。
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,CPUFreq=240,DebugLevel=none,FlashMode=qio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,UploadSpeed=921600,USBMode=hwcdc"

echo "==> 工具链"
"$CLI" --config-file "$CFG" version
echo "==> 已安装核心"
"$CLI" --config-file "$CFG" core list

echo "==> 编译 $SKETCH"
"$CLI" --config-file "$CFG" compile \
  --fqbn "$FQBN" \
  --build-path "$BUILD" \
  "$SKETCH"

if [ -z "$PORT" ]; then
  echo "==> 编译完成（未指定串口，跳过烧录）"
  exit 0
fi

echo "==> 烧录到 $PORT"
"$CLI" --config-file "$CFG" upload \
  -p "$PORT" \
  --fqbn "$FQBN" \
  --input-dir "$BUILD"
echo "==> 烧录成功"

if [ "$ACTION" = "monitor" ]; then
  echo "==> 打开串口监视器 (115200)，Ctrl+C 退出"
  "$CLI" --config-file "$CFG" monitor -p "$PORT" --config baudrate=115200
fi
