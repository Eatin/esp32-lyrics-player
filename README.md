# ESP32-S3 音乐歌词机 · esp32-lyrics-player

把微雪 **ESP32-S3-Touch-LCD-3.49 (V2)** 的长条屏横过来当「桌面歌词屏」：
电脑上照常用**酷狗**放歌，板子只负责显示 —— **左边专辑封面，右边两行歌词**。

> 完整文档（引脚表、HTTP API、排障手册）见 **[MusicPlayer/README.md](MusicPlayer/README.md)**。

```
横置 640 × 172
┌───────────┬────────────────────────────────┐
│           │  歌名                           │
│  专辑封面  │  歌手 · 专辑                    │
│ 156×156   │                                │
│           │  当前歌词（大、亮）              │
│           │  下一句（小、暗）                │
│           │  ▓▓▓▓▓▓▓░░░░░░░  进度            │
└───────────┴────────────────────────────────┘
```

## 为什么做成「PC 桥接」而不是板子自己联网播歌

- ESP32-S3 **只有 BLE，没有蓝牙 A2DP** —— 板子放不了手机里的流媒体音乐。
- 板子**没有 JPEG 硬件解码器**，所以封面在 **PC 端**解码并缩成 156×156 RGB565 再推过去，
  板子只做一次 memcpy，不做任何解码。
- 结果是：**电脑照常用酷狗**（歌单、音质、会员全不变），板子当纯显示器，各司其职。

## 特性

- **横屏 640×172**：在刷屏回调里做软件转置，运行时可切换上下朝向（`POST /api/rot?dir=`）
- **歌词页**：左封面 + 右两行歌词；没有返回按钮，**长按屏幕 0.8 秒**回播放器页
- **播放器页**：曲目列表、播放控制、音量、状态
- **中文字库**：GB2312 全集（6763 汉字 + 682 符号）。因 LVGL `bitmap_index` 只有 20 bit
  （单套位图必须 < 1 MB），拆成主/次两套 4bpp 用 `fallback` 串联，视觉无缝
- **锂电池电源闩锁**：启动早期拉高 TCA9554 `EXIO6`（`SYS_EN`）接过电源自锁；
  长按背面 PWR 键 2 秒关机
- **网页控制台**：网络设置 / 屏幕方向 / 电源与电池电压 / 远程关机 / 清空曲目

## 目录

| 路径 | 说明 |
|---|---|
| `MusicPlayer/` | 固件（Arduino sketch）+ 完整中文文档 |
| `tools/kugou_bridge.py` | PC 端桥接：读酷狗播放状态 → 抓 LRC 歌词与封面 → 推给板子 |
| `tools/gen_font.py` | 中文字库生成器（字符集 / 字号 / bpp 可配） |
| `tools/font_audit.py` | 字库覆盖对账，排查「某个字显示成方框」 |
| `tools/font_preview.py` | 按 fallback 链离线渲染预览 PNG |
| `tools/boot_cap.py` | 复位并抓取串口启动日志 |
| `ref/` | 官方示例源码（排障时的对照参考） |

## 构建

需要自行准备 **arduino-cli** 与 **Arduino-ESP32 3.3.0** 核心（体积原因未入库，
见 `.gitignore` 与 `MusicPlayer/README.md` 的环境准备章节）。然后：

```bash
arduino-cli --config-file tools/arduino-cli.yaml compile \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,CPUFreq=240,DebugLevel=none,FlashMode=qio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,UploadSpeed=921600,USBMode=hwcdc" \
  --build-path tools/build_w1 MusicPlayer

arduino-cli --config-file tools/arduino-cli.yaml upload -p <串联口> \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,CPUFreq=240,DebugLevel=none,FlashMode=qio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,UploadSpeed=921600,USBMode=hwcdc" \
  --input-dir tools/build_w1
```

分区表用工程自带的 `MusicPlayer/partitions.csv`（app 7 MB × 2，保留双 OTA；
回收了原厂方案里从未使用的 9.9 MB FAT 分区），所以 `PartitionScheme=custom`。

## 许可

本仓库暂未附带 License 文件。若希望他人能自由使用 / 修改 / 分发，建议补一个（如 MIT）。
