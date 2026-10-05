# ESP32-S3-Touch-LCD-3.49 触摸音乐播放器

基于微雪 **ESP32-S3-Touch-LCD-3.49（V2）** 开发板的**桌面歌词机**：
电脑上用酷狗放歌，板子通过 Wi-Fi 接收歌名/歌手/进度、歌词**和专辑封面**，
横放显示 —— **左边封面，右边两行歌词**（当前句 + 下一句）。

- 3.49" 172×640 电容触摸屏，**横屏 640×172**，LVGL v8.4 界面
- **主推用法 —— 电脑酷狗 + 板子显词**：`tools/kugou_bridge.py` 读 Windows 上酷狗的播放状态
  （UI Automation 取位置、SMTC 取元数据），自动抓 LRC **和 156×156 专辑封面**后推给板子。
  **声音从电脑出**，板子完全不碰音频，音质不受板子影响；误差可用「歌词对轴」微调
- **封面为什么不在板子上解码**：ESP32-S3 没有 JPEG 硬解，软解要占几十 KB flash 和几百 ms CPU。
  PC 端本来就要用 Pillow 缩放，顺手转成 **156×156 RGB565（大端，48KB）** 一次 POST 传完 ——
  板子端**零解码、零 flash 开销**。抓不到封面时板子画一张按歌名取色的「唱片」占位图
- **内置中文字体**：自研 LVGL 字库（`lv_font_cjk_16` / `lv_font_cjk_20`，**GB2312 全集**：
  6763 汉字 + 682 符号 + 拉丁补充 + ♪★♥ 等，共 7840 字形），无需联网转换。
  20px 在 4bpp 下位图超 1 MB（LVGL `bitmap_index` 只有 20 bit），故拆成主/次两套用
  `fallback` 串起来，见下方「字体」一节
- **内置网页控制台**（`http://<板子IP>/`）：连接状态、歌词对轴微调、**屏幕方向切换**、手动上传 `.lrc`、网络设置
- **备用玩法 —— 板载 SD 卡本地播放 WAV**：TF 卡 → ESP32-S3 → ES8311 DAC → NS4150 功放 → MX1.25 扬声器；
  支持 WAV(PCM) 8/16/24/32 bit、单/双声道、采样率自动识别并重配 ES8311，播完自动下一首
- **两种联网方式**：优先连**家里局域网**（与电脑同网即可），连不上自动退回**自建热点** `ESP32-Player`；
  两种模式都能在网页里改配置（`/wifi`）。另有 **UDP 自动发现**，电脑端不用手填板子 IP

> **为什么不是蓝牙音箱？** ESP32-S3 只有 **BLE 5.0，没有蓝牙经典（BR/EDR）**，做不了 A2DP；
> LE Audio 需要 BLE 5.2+，S3 也不支持。所以音频不走板子 —— **电脑放，板子只显词**。

---

## 1. 硬件准备

| 项目 | 说明 |
|---|---|
| 开发板 | ESP32-S3-Touch-LCD-3.49 / 3.49B（**V2**，PCB 丝印 `Rev1.1`，外壳贴 `V2` 标签） |
| 扬声器 | 8Ω 2W 左右喇叭，接到背面 **MX1.25 2PIN** 接口（A 款也可用板载喇叭） |
| 存储卡 | Micro SD / TF 卡，**FAT32** 格式，放 WAV 音乐 |
| 数据线 | Type-C 数据线（用于烧录） |
| 锂电池 | 3.7V MX1.25 锂电池（**A 款自带**，B 款需另购）。见 §1.1 锂电池供电 |

> ⚠️ **注意 V1 与 V2 引脚不同**：V1 已于 2026-06-08 起停产。若你手上是 V1（LCD_BL=GPIO8、LCD_RST=GPIO21、无 EXIO 背光使能），请见文末「V1 版适配」。

### 1.1 锂电池供电（必须知道的一个坑）

这块板**没有真正的电源开关**。锂电池供电时，它是靠**按住 PWR 键**硬撑着供电的，
闩锁挂在 TCA9554 的 **EXIO6（`SYS_EN`）** 上：

```
按下 PWR ──► 硬件给整机供电 ──► ESP32 启动
                                    │
                   固件必须把 EXIO6 拉高 ◄── 交给固件「自锁」
                                    │
松开 PWR ──► 闩锁已由 EXIO6 维持 ──► 板子继续运行 ✅
             （若固件没拉高 → 松手即断电 ❌）
```

所以固件里有这条硬性要求：

- 开机**第 0 步**（`MusicPlayer.ino` 的 `setup()` 最前面）就调 `board_power_hold()`，
  把 `EXIO6` 设成输出并拉高。**比任何打印、任何初始化都早**，否则来不及。
- `lvgl_port.c::example_lcd_exio_init()` 与 `board_io.c::board_exio_init()` 里还会各补一次，
  防止后续初始化把它改回输入/低电平。

行为说明：

| 操作 | 结果 |
|---|---|
| 插着 USB | 一直运行（USB 直供，与闩锁无关） |
| 有电池 + 按住 PWR 约 2 秒直到屏幕亮 | 松手后继续运行；此时可拔掉 USB |
| 开机日志 `[PWR] 电池自锁 SYS_EN(EXIO6)=1 -> ESP_OK` | 自锁成功 |
| 开机日志出现 `⚠ 电源闩锁没接上` | TCA9554 通信异常，检查 I2C / 是否 V1 板 |

网页控制台（`http://<板子IP>/`）新增「电源 / 锂电池」卡片，显示当前供电来源与电池电压；
拔掉 USB 后这里还刷得出数字，就说明自锁生效了。

**怎么关机**（和官方 `07_BATT_PWR_Test` 同款做法）：

| 操作 | 结果 |
|---|---|
| **长按板背 PWR 键 2 秒** | 固件把 `EXIO6` 拉低 → 电池供电时整机断电（再按 PWR 才能开机） |
| 网页控制台「关机」按钮 | 同上（`POST /api/pwr?off=1`） |
| 插着 USB 时长按 | 板子不会真断电（USB 还在供），固件会在 300ms 后**把闩锁自动闩回去**并打日志，所以插线调试可以放心长按 |

> ⚠️ **GPIO16 一个脚两个用途**：它既是对外的 `SYS_OUT`（官方用来判断供电来源），
> **也是背面那颗 PWR 电源键**（官方 `button_bsp.c` 里 `USER_KEY_2 = 16`，低电平有效）。
> 所以按键按下的瞬间这个脚也会被拉到 0，代码里读「供电源」时要避开按下的时刻——
> `board_io.c` 的按键任务就是「先看见松开，才开始计时」，不会被自己按出来的低电平骗到。

> 电池电压读 **GPIO4**（ADC1_CH3，板载 1:3 分压，同官方 `01_ADC_Test`）。
> 供电源标志读 **GPIO16（`SYS_OUT`）**，高 = 电池供电 —— 与官方 `07_BATT_PWR_Test` 的
> `power_Test()` 判法一致。

---

## 2. 开发环境准备

### 2.1 Arduino IDE + ESP32 支持

1. 安装 Arduino IDE（1.8.x / 2.x 均可）。
2. 按微雪教程配置 ESP32 支持：<https://docs.waveshare.net/ESP32-Arduino-Tutorials/Arduino-IDE-Setup>
   **开发板包版本必须为 `esp32 by Espressif Systems 3.3.0`**（在同一版本的开发板包上验证过）。

### 2.2 安装 LVGL 8.4.0（离线库）

从微雪提供的示例程序包中取 LVGL：

- 下载 V2 示例程序包：<https://gitee.com/waveshare/esp32-s3-touch-lcd-3.49-v2>
- 也可用 V1 打包好的 zip：<https://files.waveshare.net/wiki/ESP32-S3-Touch-LCD-3.49/ESP32-S3-Touch-LCD-3.49-Demo.zip>

安装步骤（以示例包 `Arduino/libraries/lvgl8/` 为例）：

1. 把 `lvgl8/lvgl` 整个文件夹复制到 Arduino 的 `libraries` 目录，**重命名为 `lvgl`**
   （Windows 默认路径：`C:\Users\<你>\Documents\Arduino\libraries\lvgl`）
2. 把 `lvgl8/lv_conf.h` 复制到刚放好的 `libraries/lvgl/` 目录下（与 `lvgl.h` 同级）
3. 若 `libraries` 里已有旧版 `lvgl`，请先删除，避免版本冲突

> LVGL v8 与 v9 驱动不兼容，本工程**只支持 LVGL 8.4.0**。
> 微雪示例包中 `lvgl8` 与 `lvgl9` 目录都已提供，本项目使用 `lvgl8`。

### 2.3 Arduino IDE 工程参数（Tools 菜单）

| 选项 | 取值 |
|---|---|
| Board | **ESP32S3 Dev Module** |
| USB CDC On Boot | **Enabled** |
| CPU Frequency | 240MHz (WiFi) |
| Flash Mode | QIO 80MHz |
| Flash Size | **16MB (128Mb)** |
| PSRAM | **OPI PSRAM** |
| Partition Scheme | **Custom**（分区布局由工程目录下的 `partitions.csv` 决定：app 7MB × 2） |
| Upload Speed | 921600 |
| Port | 板子对应的 COM 口 |

---

## 3. 编译与烧录

1. 把整个 `MusicPlayer` 文件夹放到 Arduino 的 sketchbook 目录（`Documents/Arduino/MusicPlayer`）。
2. 用 Arduino IDE 打开 `MusicPlayer.ino`。
3. 选择好开发板与串口，点击 **上传**。
4. 首次编译需要 3~10 分钟（LVGL 库较大），属正常现象。
5. 打开串口监视器（**115200**）可看到日志：

```
==============================================
 ESP32-S3-Touch-LCD-3.49  Music Player (V2)
==============================================
Tracks found: 3, SD ready: 1
Ready. Tap a song to play.
```

**下载失败时**：按住 `BOOT` → 单击 `RESET` → 松开 `RESET` → 再松开 `BOOT`，进入下载模式后重新上传。

---

## 4. 音乐文件要求（WAV）

把音乐放到 TF 卡根目录或一级子目录（`/sdcard/Music/`），文件名后缀 `.wav`（大小写均可）。

推荐格式：**16 bit / 44.1 kHz / 立体声**（兼容性最好）。

ES8311 支持的采样率：8000 / 11025 / 12000 / 16000 / 22050 / 24000 / 32000 / 44100 / 48000 Hz。
其它位深（8/24/32 bit）与单声道会自动转换为 16 bit 立体声播放。

### MP3 / FLAC 转 WAV

本工程不内置 MP3 解码器，请先转成 WAV：

```bash
# 单个文件
ffmpeg -i song.mp3 -acodec pcm_s16le -ar 44100 -ac 2 song.wav

# 整个文件夹批量转换
for f in *.mp3; do ffmpeg -i "$f" -acodec pcm_s16le -ar 44100 -ac 2 "${f%.mp3}.wav"; done
```

> WAV 体积约为 MP3 的 10 倍（44.1kHz/16bit 立体声约 10MB/分钟），请注意 TF 卡容量。

---

## 5. 界面操作

### 5.1 歌词页（横屏 640×172，主界面）

```
┌──────────┬─────────────────────────────────────────────┐
│          │ 歌名                                         │
│  专辑封面 │ 歌手 · 专辑                                  │
│ 156×156  │ 当前歌词（大、亮青色）                        │
│          │ 下一句（小、灰蓝）                            │
│          │ ▓▓▓▓▓▓░░░░░░░░░░░░░  进度条                  │
└──────────┴─────────────────────────────────────────────┘
                                    时间          来源/地址
```

这一页是长期摆在桌面上的主界面，所以**屏幕上故意不放任何按钮和状态字**：

- **没有「返回」按钮**。想回播放器页：**在屏上长按约 0.8 秒**（隐藏手势，整块屏都有效；
  响应时间由 `lvgl_port.c` 的 `indev_drv.long_press_time = 800` 控制），
  或者等停播超过 1 分钟自动回去。
- **右下角「来源 / 地址」那一行只在出问题时才出现**：
  歌在放 + 有歌词 = 电脑推送一切正常，整行隐藏（不显示 IP，也不显示「有歌词」这种废话）；
  只有**没推送 / 没歌词**时才亮出来 —— 那时它才有用（告诉你去哪个 IP 开控制台）。
- **自动切屏**：电脑一开始推歌就自动进歌词页；停止播放超过 1 分钟自动回播放器页
- 没有封面时，左侧显示一张按歌名取色的「唱片」占位图
- 没有歌词时，右侧两行位置显示提示文案

### 5.2 播放器页（本机 TF 卡播放，备用）

| 区域 | 操作 |
|---|---|
| 顶栏 | 显示「模式 + 板子 IP」或「电脑推送 歌名-歌手」；右侧 **重扫** / **歌词** 按钮 |
| 左侧列表 | 上下滑动浏览，**点击某一首** 立即播放（当前曲目高亮） |
| 进度条 / 时间 | 显示当前播放位置与总时长 |
| `◀◀` / `▶·❚❚` / `▶▶` | 上一首 / 播放暂停 / 下一首 |
| 音量滑条 | 0~100 级音量（ES8311 硬件音量） |

串口日志会打印每首曲目的采样率、声道与位深，便于排查。

### 5.3 屏幕朝向

面板原生是 172×640 竖屏，固件按**横屏**渲染（刷屏时软件转置）。
横放时如果画面**上下颠倒**，说明你把它转的是另一个方向，两种办法都行：

- **不用重烧**：手机/电脑打开 `http://<板子IP>/` → 「屏幕方向」→ 点「方向 B」（立刻生效）
- 或直接 POST：`curl -X POST http://<板子IP>/api/rot?dir=1`

> 朝向只存在内存里，**重启后回到「方向 A」**。想固定成 B，把 `lvgl_port.c` 里
> `s_rot_dir` 的初值改成 1 再烧一次即可。

---

## 6. 电脑酷狗放歌 + 板子当歌词机（当前主推）

**架构**：声音在电脑上放，板子只当一块歌词屏。板子完全不碰音频。

```
   电脑（Windows）                                      ESP32-S3 歌词机
 ┌───────────────────────────┐                   ┌────────────────────────┐
 │  酷狗音乐（正在播放）       │                   │  横屏歌词页：           │
 │        │                  │                   │   左：专辑封面 156×156  │
 │        ├─ UI Automation ──┼──► 播放位置/时长   │   右：歌名/歌手         │
 │        │  （界面文本控件）  │                   │        当前句（大）      │
 │        └─ SMTC ───────────┼──► 歌名/歌手/状态  │        下一句（小）      │
 │                           │                   │        进度条 + 时间     │
 │  kugou_bridge.py          │  HTTP /api/ext/*  │  自动切屏：一放歌就显示   │
 │   ├─ 酷狗接口抓 LRC        ├──────────────────►│                        │
 │   └─ 酷狗接口抓专辑封面     │   同一局域网 2.4G   └────────────────────────┘
 │      → Pillow 缩放 156×156 │                    封面直接收 RGB565 原始像素，
 │      → RGB565(48KB) 直推   │                    板子零解码、零额外 flash
 └───────────────────────────┘
```

**为什么绕这么大一圈**：ESP32-S3 **只有 BLE，没有蓝牙经典（A2DP）**，做不了蓝牙音箱；
而 iOS 不允许第三方 App 读别的 App 的播放进度。所以「手机放歌 + 板子看词」在 S3 上走不通。
换到 Windows 后这两个限制都没了 —— 这也正是本方案的立足点。

**为什么位置要从界面读**：酷狗**确实会注册 SMTC 会话**（歌名/歌手/播放状态都能读到），
但实测它的 `Position` / `Duration` **恒为 0**，不提供时间轴。
所以桥接脚本改用 **UI Automation 读酷狗界面上那个 `00:39/04:49` 文本控件**（每秒刷新），
再用挂钟时间把它补成连续值推给板子。两条通道分工：**UIA 管位置，SMTC 管元数据兜底**。

### 6.1 一次性准备：让板子连上你家路由器

板子必须和电脑在**同一个局域网**，所以先给它配好 WiFi（只需做一次，凭据存在 NVS，断电不丢）：

1. 手机连上板子的热点 **`ESP32-Player`**（密码 `12345678`）
2. 浏览器打开 **`http://192.168.4.1/wifi`**
3. 填**家里路由器的 2.4G WiFi 名和密码** → 保存并重启
4. 重启后板子屏幕底部会显示 `局域网 192.168.1.23` 之类的地址 —— 这就是它在新网络里的 IP
5. 以后电脑和板子都在这个路由器下，**不需要再配**

> 板子**只支持 2.4GHz**。若路由器把 2.4G/5G 合并成一个名字连不上，就给 2.4G 单独设个名字。
> 密码填错或路由不可达时，板子会**自动退回热点**，不会失联 —— 再进 `/wifi` 改就行。

### 6.2 电脑端：运行歌词桥

```bash
cd D:\wb\wave\tools
espvenv\Scripts\python.exe -m pip install uiautomation            # 必需：读酷狗播放位置
espvenv\Scripts\python.exe -m pip install winrt-runtime winrt-Windows.Media.Control \
    winrt-Windows.Foundation winrt-Windows.Foundation.Collections  # 可选：元数据兜底
```

```bash
espvenv\Scripts\python.exe kugou_bridge.py          # 自动发现板子并开始桥接
```

常用参数：

| 参数 | 作用 |
|---|---|
| `--probe` | **先跑这个**。分别打印 UIA 与 SMTC 读到的内容，确认能读到酷狗 |
| `--host 192.168.1.23` | 手动指定板子 IP（自动发现失败时用） |
| `--offset -300` | 整体时间偏移（毫秒）。歌词普遍偏早填负数，偏晚填正数 |
| `--dry-run` | 不连板子，只打印将要推送的内容 |
| `--interval 0.3` | 位置推送间隔，默认 0.5 秒 |

启动时脚本会：

1. 先试 `http://musicplayer.local/`，再向 `255.255.255.255:48899` 广播 `ESP32LYRICS?`
   —— 板子会单播应答自己的 IP，所以**不用手填地址**（结果存进 `bridge_config.json`）
2. 每 0.5 秒读一次酷狗：歌名 / 歌手 / 位置 / 播放状态
3. **换歌时**才去抓歌词（1~3 秒），抓到后连歌词一起 `POST /api/ext/track`
4. 之后持续 `POST /api/ext/pos` 推送位置
5. 退出（Ctrl+C）时通知板子清空

### 6.3 歌词从哪来

抓歌词的顺序（结果缓存在 `tools/lyrics_cache/`，同一首歌第二次是秒开）：

1. **酷狗**：`mobiles.kugou.com` 搜歌拿到**文件 hash** → 拿 hash 去 `krcs.kugou.com` 精确匹配
   （优先「官方推荐歌词」）→ `lyrics.kugou.com/download` 取 LRC（内容是 base64，需解码）
2. **网易云**兜底：搜歌 → `/api/song/lyric`
3. 都失败 → 推送空歌词，板子显示「这首歌没有歌词」，此时可以**手动上传**（见 6.4）

拿到 LRC 后会做规范化：

- 去掉 UTF-8 BOM 和 `[id:$00000000]` 这类 KRC 头
- **`[ti:]` / `[ar:]` 存在但内容为空时，用真实歌名/歌手补上**（酷狗很常见，必须处理）

### 6.3.1 封面从哪来

1. 按歌名/歌手搜歌（复用上面那次搜索），从结果里取 **`album_id`**
2. `mobilecdn.kugou.com/api/v3/album/info?albumid=…` → `data.imgurl`
   （注意返回串自带 `{size}` 占位符，要替换成 `480` 再拼 host）
3. 下载 480×480 JPEG → **Pillow 缩放到 156×156** → 转 **RGB565 大端**（顺带做 4×4 有序抖动，
   否则 5bit 的 R/B 在渐变封面上会有明显色带）
4. `POST /api/ext/cover`（body 就是 48672 字节原始像素）
5. 拿不到封面就发 `POST /api/ext/cover?clear=1`，板子改画占位图（不会留着上一首的图）

结果缓存在 `tools/covers/*.rgb565`，同一首歌第二次是秒开。不想抓封面就加 `--no-cover`。

> **为什么推原始像素而不是 JPEG**：S3 没有 JPEG 硬解；LVGL 自带的 SJPG 解码器要额外占
> 几十 KB flash（本工程 flash 已用到 84%），而且每张图要几百 ms CPU。
> PC 端反正要用 Pillow 缩放，顺手转 RGB565 最省事。48KB 在局域网上一次 POST 就传完了。

### 6.4 板子网页：接收控制台

浏览器打开 `http://<板子IP>/`（或 `http://musicplayer.local/`），这是一个**控制台**，不是播放器：

| 区块 | 作用 |
|---|---|
| 状态卡 | 电脑是否在线、当前歌名/歌手、位置/时长、距上次推送多久 |
| 歌词对轴 | `−1s / −0.5s / −0.1s / +0.1s / +0.5s / +1s` 与重置。板子上的歌词比歌声快就点减号 |
| 手动上传歌词 | 选 `.lrc`/`.txt` 文件（自动识别 UTF-8/GBK）或直接粘贴文本，覆盖板子当前歌词 |
| 屏幕方向 | 横放时画面上下颠倒就点「方向 B」，立刻生效、不用重烧 |
| 电源 / 锂电池 | 显示当前供电来源与电池电压。拔掉 USB 后还刷得出数字 = 电池自锁生效。下方「关机」按钮 = 松开 `SYS_EN` 闩锁（等价于长按板背 PWR 键 2 秒） |
| 其他 | 进 `/wifi` 网络设置；清空板子当前曲目 |

### 6.5 板端接口一览

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 接收控制台页面 |
| POST | `/api/ext/track?dur=&title=&artist=&album=` | **body 即 LRC 原文**（UTF-8）。新曲目：重置位置、置为播放、解析歌词 |
| POST | `/api/ext/pos?p=<ms>&s=<0\|1\|2>[&d=<总时长ms>]` | 上报播放位置与状态（0=停 1=播 2=暂停）。`d` 用来在换歌瞬间自愈时长 |
| POST | `/api/ext/cover` | **body 即 156×156 RGB565 大端原始像素**（48672 字节）；`?clear=1` 清掉封面 |
| GET | `/api/ext/state` | 状态 JSON：`{ever,on,live,s,p,lp,d,off,ago,rx,jump,title,ar,al,lrc,lines,cover,cseq}` |
| POST | `/api/ext/offset?d=<毫秒>` 或 `?v=<毫秒>` | 歌词整体偏移（`d` 为增量，`v` 为绝对值） |
| POST | `/api/ext/clear` | 清空外部曲目（连封面一起清） |
| POST/GET | `/api/rot?dir=<0\|1>` | 屏幕横屏朝向切换（0=方向A 默认，1=方向B） |
| GET | `/api/pwr` | 电源状态：`{batt,mv,pct,sys_out,pwr,rot}`，`batt=1` 表示电池供电，`pwr=1` 表示 PWR 键正被按住 |
| POST | `/api/pwr?off=1` | 关机：松开 `SYS_EN` 闩锁（电池供电即断电；USB 供电则自动闩回去） |
| GET | `/wifi` / `GET /api/net` / `POST /api/wifi` / `POST /api/wifi/forget` | 网络配置 |
| GET | `/api/list` `/api/sync` `/media/*` `/lrc/*` `POST /api/lyrics` | 旧的「板子/网页放歌」通道，保留作备用 |

**UDP 自动发现**：板子监听 `48899`，收到 `ESP32LYRICS?` 就回复
`ESP32LYRICS!<ip>|<局域网/热点>|tracks=N|ssid=<当前SSID>|port=80|ver=1`。

### 6.6 板端设计要点

- **横屏是「软件转置」**（`user_config.h` 的 `Rotated = USER_DISP_ROT_90`，`lvgl_port.c`）：
  面板原生 172×640，LVGL 逻辑画布 640×172，刷屏时把逻辑帧转置成面板坐标。
  - 官方示例是先把整屏转置进一块 **220KB** 的 PSRAM 中转 buffer 再分条推；
    本工程改成**按面板 64 行为一条、直接组装进 DMA 缓冲**，省掉那块 220KB，
    而且读源是 128 字节连续（4 个 cache line）、写目标工作集只有 22KB（能待在 cache 里）。
  - 朝向有两个（设备逆时针/顺时针各转 90°），运行时用 `/api/rot?dir=` 切，不用重烧。
- **锚点 + 漂移平滑**：板子只把 PC 上报的位置当**锚点**，之后用自己 40MHz 微秒时钟插值
  （`pos = anchor + (now - anchor_us)/1000`），所以 PC 上报间隔抖到 2 秒也不会卡词。
  - 上报值与预期偏差 **> 2 秒** → 判定为拖进度，直接重锚
  - 小幅漂移 → **只吸收 1/4**，避免歌词来回跳
- **统一播放源**：`web_player` 暴露 `np_source()/np_title()/np_artist()/np_album()/np_position_ms()/
  np_lyric_pos_ms()/np_cover_*()` 一组接口，UI 不关心数据来自电脑还是本机：
  **电脑推送优先，其次板子本地/网页**。
- **封面双缓冲**：板端两块 48KB PSRAM 交替写，**整帧收完才切换指针**，
  UI 不会看到半张图（歌词页自己再 memcpy 一份，跟 `ext_link` 完全解耦）。
- **歌词偏移与位置分离**：进度条用真实位置 `np_position_ms()`，歌词检索用
  `np_lyric_pos_ms()`（= 真实位置 + 偏移），所以对轴不会把进度条带偏。
- **自动切屏**：`np_state() != 0` 时自动进歌词页；停止超过 60 秒自动退回播放器页。
- **内存**：曲目表、歌词缓冲、封面缓冲都在 **PSRAM**；UDP 发现任务固定 4KB 栈。
- **网络接入（`wifi_link.cpp`）**：开机优先 STA 连家庭 WiFi（15s 超时），失败退回热点；
  `loop()` 里 `wifi_link_tick()` 每 5 秒检查，掉线自动重连、DHCP 换 IP 也同步刷新屏幕与页面。
- **电源自锁先于一切**：`setup()` 的**第一条**语句就是 `board_power_hold()`
  （把 TCA9554 `EXIO6/SYS_EN` 拉高）。锂电池供电时这是「能不能活下来」的前置条件，
  所以它排在串口打印、复位原因诊断、屏幕初始化之前。详见 §1.1。

### 6.7 中文字库（"有的字显示成方框"的根因与改法）

**现象**：歌名/歌手/歌词里个别汉字变成一个**空心方框**，后面的字还往前挤一点。

**根因（两层，缺一不可）**：

1. 字库字符集不够。早期只收了 **GB2312 一级汉字（3755 个）**，二级汉字
   （奕 嵩 泷 靓 痣 …）一个都没有。歌名歌手名里这类字很常见
   （陈**奕**迅 / 许**嵩** / 汪苏**泷** / 张**靓**颖 / 朱砂**痣**）。
2. LVGL 对"找不到字形"的处理不是留白，而是**画占位框**：
   - `lv_conf.h` 里 `LV_USE_FONT_PLACEHOLDER = 1`
   - `lv_font.c:109-124` 返回 `box_w = line_height/2`、`box_h = line_height`、
     `adv_w = box_w + 2`（正常汉字步进应为 20px，这里只有 12px）
   - `lv_draw_sw_letter.c:107-121` 拿到这个占位字形就 `draw_rect` 画一个空心矩形

   所以缺字 = **10×21 的方框 + 后面文字左移 8px**，看起来就是"方框 + 乱码"。

**现在的做法**：字符集扩到 **GB2312 全集**（682 符号 + 6763 汉字）
+ CJK 标点 U+3000-303F + 全角 U+FF00-FFEF + 拉丁补充 U+00A0-00FF + ♪★♥ 等符号，
共 **7840 字形**。

**为什么 20px 拆成两套**：LVGL 的 `lv_font_fmt_txt_glyph_dsc_t.bitmap_index`
只有 **20 bit（位图必须 < 1 MB）**，而 GB2312 全集在 20px/4bpp 下要 **1.33 MB**：

| 字体 | 字号 | bpp | 字符集 | 位图 |
|---|---|---|---|---|
| `lv_font_cjk_16` | 16 | 4 | 全集 | 872 KB ✅ |
| `lv_font_cjk_20` | 20 | 4 | 符号 + 一级汉字 | 781 KB ✅ |
| `lv_font_cjk_20b` | 20 | 4 | 二级汉字（3008） | 549 KB ✅ |

`lv_font_cjk_20b` 通过 `lv_font_t.fallback` 挂到 `lv_font_cjk_20` 上，
两者同字号同 bpp、`line_height`/`base_line` 一致，**视觉无缝**。

> ⚠️ 两个坑：
> 1. LVGL 8.4 **没有** `lv_font_set_fallback()` 这个 API，只能直接给结构体字段赋值；
>    而 `const` 字体位于 flash，写了会崩 —— 所以 `lv_font_cjk_20` **故意声明为非 const**
>    （定义在 `lv_font_cjk_20.c`，头文件里也是 `extern lv_font_t`）。
> 2. `lv_font_cjk_setup()` 必须在**创建任何文字控件之前**调用
>    （见 `MusicPlayer.ino` 的 `setup()` 第 4 步），否则二级字仍会退化成方框。

**分区表也跟着改了**：字库位图从 1.31 MB 涨到 2.15 MB，app 从 2.68 MB 涨到约 3.9 MB，
原来的 `app3M_fat9M_16MB`（3 MB APP）装不下。而那个 **9.9 MB 的 FAT 分区本工程从未使用**
（只用 microSD），于是改用**工程目录下的 `partitions.csv`**（FQBN 里 `PartitionScheme=custom`），
布局为 **app 7 MB × 2（保留双 OTA 槽）**。

**自己扩字库**（比如遇到繁体、生僻字、emoji）：

```bash
cd tools
python gen_font.py --probe                      # 先看覆盖率和体积，别白跑
# 编辑 gen_font.py 的 build_charset()，把要补的码点加进去
python gen_font.py                              # 重新生成三个 .c
python font_audit.py "有问题的歌名 歌手"          # 核对是否已覆盖
python font_preview.py "有问题的歌名" --size 20   # 按 fallback 链渲染预览图
```
再把生成的 `.c` 一起编译烧录。若某套位图超过 1 MB，生成器会直接报错并提示怎么办。

### 6.8 备用玩法

- **手机浏览器放歌**：板子还保留着旧的网页播放通道（`/`），手机连上板子后可以在
  `/api/list` 里看到 TF 卡曲目（MP3/WAV/FLAC…），音频下载到手机内存后由手机播放，
  歌词走 `/api/lyrics` + `/api/sync`。**注意这条路只有连板子热点时才能用外网**。
- **板子自己放歌**：TF 卡里放 **WAV(PCM)**，用屏幕上的界面直接播放（音质受板载 ES8311 + 小喇叭限制）。
  同名 `.lrc` 会被歌词页使用。
- **想让任何播放器都能跟唱**：理论上可以给板子加「麦克风听歌对轴」（ES7210 已在板上），
  但当前版本未实现 —— 现在依赖电脑端推送位置。
---

## 7. 代码结构

```
MusicPlayer/
├── MusicPlayer.ino              # 主程序：初始化顺序、启动各模块（网络接入 + HTTP 服务）
├── user_config.h                # 板级引脚定义（V2 官方）+ 屏幕方向 / 逻辑分辨率 / 版面常量
├── lvgl_port.c / .h             # QSPI LCD(AXS15231B) + 触摸 + LVGL 移植
│                                #   ★ 横屏软件转置、触摸坐标映射、运行时朝向切换(/api/rot)
├── i2c_bsp.c / .h               # I2C0(47/48) 与 I2C1(17/18) 总线（官方）
└── src/
    ├── board/
    │   ├── board_io.c / .h      # ★ TCA9554 扩展 IO、背光 PWM、电源闩锁、长按 PWR 关机
    ├── music/
    │   ├── wav_player.c / .h    # ★ WAV 解析 + 播放引擎（独立任务）
    │   ├── music_ui.c / .h      # ★ 横屏播放器页（左列表 / 右播放控制）
    │   ├── wifi_link.cpp / .h   # ★ 网络接入：LAN 优先 + 热点兜底（NVS 存凭据）
    │   ├── ext_link.c / .h      # ★ 电脑推送通道：歌名/歌手/专辑/LRC/进度/封面 + UDP 自动发现
    │   ├── web_player.c / .h    # ★ 板端 HTTP 服务 + 统一「正在播放」接口 np_*
    │   ├── player_page.cpp / .h # ★ 内嵌 HTML：接收控制台页（含屏幕方向）+ /wifi 配网页
    │   ├── lyrics.c / .h        # ★ LRC 解析 + 按时间轴定位（缓冲在 PSRAM）
    │   └── lyrics_ui.c / .h     # ★ 横屏 640×172 歌词页（左封面 + 右两行歌词，无按钮）
    ├── font/
    │   ├── lv_font_cjk_16.c     # ★ 字库 16px：GB2312 全集，单套（位图 872KB）
    │   ├── lv_font_cjk_20.c     # ★ 字库 20px 主：符号+一级汉字（位图 781KB）★非 const★
    │   ├── lv_font_cjk_20b.c    # ★ 字库 20px 补：GB2312 二级汉字（位图 549KB）
    │   ├── lv_font_cjk_setup.c  # ★ 把 20b 挂成 20 的 fallback（必须在画字前调用）
    │   └── lv_font_cjk.h        # ★ 字库声明
    ├── codec_board/             # 官方：ES8311/ES7210 板级配置与驱动（board_cfg.h）
    ├── esp_codec_dev/           # 官方：乐鑫统一 codec 设备框架
    ├── tca9554/                 # 官方：TCA9554 扩展 IO 驱动
    ├── axs15231b/               # 官方：LCD 驱动（QSPI）
    └── touch/                   # 官方：触摸驱动头文件
```

**电脑端**（在 `D:\wb\wave\tools\`）：

```
tools/
├── kugou_bridge.py              # ★ 歌词桥：读酷狗播放状态 + 抓 LRC/封面 + 推送到板子
├── cover_probe.py               # ★ 排查用：探查酷狗专辑封面接口的返回结构
├── uia_probe.py / uia_probe2.py # ★ 排查用：导出酷狗 UI 树，定位歌名/歌手/进度控件
├── smtc_watch.py                # ★ 排查用：监视 Windows SMTC 会话
├── extract_pages.py             # ★ 校验内嵌网页的 JS 语法并生成预览
├── serial_boot.py               # ★ 复位板子并抓完整启动日志
├── gen_font.py / font_render.py   # ★ 中文字库生成与离线校验
├── font_audit.py                  # ★ 字库审计：解析 .c 求覆盖码点，与源码/文本对账
├── font_preview.py                # ★ 按 LVGL fallback 链渲染预览，烧录前肉眼确认
├── font_variant_probe.py          # ★ 排查用：对比 bpp/字符集取舍的画质与体积
├── lyrics_cache/                # 抓到的歌词缓存（同一首歌第二次秒开）
└── covers/                      # 抓到的封面缓存（156×156 RGB565）
```

★ = 本项目新增/修改的代码；其余为官方 BSP，未作改动以便后续升级。

### 关键设计

- **`wav_player.c`**：独立 FreeRTOS 任务（核心 1）负责取数与 `esp_codec_dev_write`；
  UI/主任务通过互斥锁保护的命令队列 `post_cmd()` 投递播放指令，互不阻塞。
- **采样率自适应**：每首歌打开文件时读 WAV 头，按实际采样率 `close → open` 重配 ES8311 与 I2S。
- **格式归一化**：`pcm_to_s16_stereo()` 把 8/24/32bit 与单声道统一转成 16bit 立体声，
  与官方示例的 `esp_codec_dev_open(2ch / 16bit)` 保持一致，稳定性最好。
- **功放使能**：扬声器功放（NS4150）由 TCA9554 的 `EXIO7` 控制，`board_exio_init()` 置高，
  **没有这一步扬声器完全不发声**，这是最容易踩的坑。
- **背光**：GPIO42 走 LEDC PWM 调光（0~255），同时 `EXIO1(BL_EN)` 使能；默认亮度 200/255。

---

## 8. 关键引脚速查（V2）

| 功能 | 引脚 |
|---|---|
| LCD QSPI | CS=9, PCLK=10, D0=11, D1=12, D2=13, D3=14, TE=21 |
| LCD 复位 / 背光使能 | TCA9554 `EXIO5` / `EXIO1` |
| 背光 PWM | GPIO42 |
| 触摸 I2C | SDA=17, SCL=18，地址 `0x3B` |
| 系统 I2C（codec/扩展/RTC/IMU） | SDA=47, SCL=48 |
| I2S（ES8311/ES7210） | MCLK=7, BCLK=15, WS=46, DIN=6(麦克风), DOUT=45(扬声器) |
| 音频功放使能 | TCA9554 `EXIO7`（NS_MODE） |
| **锂电池电源自锁** | TCA9554 **`EXIO6`（SYS_EN，必须置 1）** |
| TF 卡（SDMMC 1bit） | CLK=41, CMD=39, D0=40 |
| 电池 ADC / SYS_OUT | GPIO4（1:3 分压） / GPIO16（PWR 键，高=电池） |

---

## 9. 常见问题

| 现象 | 排查 |
|---|---|
| 屏幕不亮 | 确认是 V2 板（背光引脚变了）；检查 `EXAMPLE_PIN_NUM_BK_LIGHT` 与 EXIO `BL_EN` |
| **接上锂电池但仍只能用 USB 供电，一拔线就断电** | 电源闩锁没拉起来。看开机日志有没有 `[PWR] 电池自锁 SYS_EN(EXIO6)=1 -> ESP_OK`；`board_power_hold()` 必须在 `setup()` 最前面调用，且 `EXIO6` 要**输出 + 高电平**。详见 §1.1 |
| 电池供电时按住 PWR 屏亮、一松手就灭 | 同上：固件还没跑到 `board_power_hold()` 就没电了。按住 PWR 的时间再长一点（约 2 秒，等日志打出来再松） |
| 网页「电池电压」显示 `未接电池` | ①电池没插好或电量耗尽；②A/B 款差别：不带电池的 `-EN` 型号没有分压电路，读到的必然是 0 |
| 电池电压显示偏低/偏高 | 分压系数按官方 `01_ADC_Test` 取 ×3（`EXAMPLE_BAT_ADC_DIVIDER`）；ADC1 未做校准时会退化到线性换算，误差约 ±3% |
| 有进度条但不发声 | ①扬声器是否插好；②`EXIO7(NS_MODE)` 是否置高；③用 `08_Audio_Test` 官方例程验证硬件 |
| 声音卡顿/爆音 | 换高质量 TF 卡；降低曲目采样率（44.1kHz 最稳）；确认 `PSRAM = OPI PSRAM` |
| 找不到音乐 | 本地播放文件名后缀必须是 `.wav`；TF 卡必须 FAT32；点 Rescan |
| 手机打不开网页 | 看板子屏幕右下角那行字（**只有没在正常推歌词时才显示**）：`等待电脑推送 · <ip>` 里的 IP 就是控制台地址；`热点` 模式下连 `ESP32-Player` 访问 `http://192.168.4.1/`（**不要用 https**） |
| **电脑推送没反应** | ①先跑 `kugou_bridge.py --probe`，看 UIA 那栏能不能读到歌名/进度；②板子必须和电脑在**同一个路由器**下（板子右下角显示的 IP 才算连上局域网）；③酷狗主窗口**不能收进托盘**（UIA 要读它的界面） |
| **桥接脚本找不到板子** | 自动发现失败时用 `--host <板子IP>` 手动指定。UDP 广播会被某些防火墙拦；板子 IP 见其屏幕右下角（或路由器的 DHCP 列表） |
| **长按 PWR 关不了机** | ①插着 USB 时本来就关不掉（USB 还在供电），日志会打印「已重新闩上 SYS_EN（未断电）」；②拔掉 USB 后再长按 2 秒；③长按要**从松开状态开始**，若一直按着从头开机会等一次松手 |
| 长按 PWR 后屏幕黑了但插上 USB 又自己活了 | 正常：断电只断开电池那一路，USB 一路是独立的 |
| **板子一直显示"等待电脑连接"** | 板子不在局域网（还在热点模式），或电脑和板子不同网段。先在手机上通过 `/wifi` 把板子接进家里路由器 |
| **歌词比歌声快/慢** | 网页控制台 →「歌词对轴」：板子歌词比歌声快就点减号，比歌声慢就点加号。也可启动脚本时用 `--offset -300` 之类的参数 |
| **推过去的歌词是错的** | 自动匹配按「歌名+歌手+时长」打分，同名翻唱容易匹配错。在网页控制台「手动上传歌词」传正确 `.lrc` 覆盖 |
| 酷狗有声音但 `--probe` 读不到位置 | 酷狗的 SMTC 会话不给时间轴（`pos=0 dur=0` 是正常的），位置只能靠 UIA 读界面；若 UIA 也读不到，多半是酷狗版本改了界面结构，跑 `uia_probe.py` 看新结构 |
| 在 `/wifi` 填了密码却连不上 | ①路由器是 **2.4G** 吗（板子不支持 5G）？②密码有没有多余空格？③路由开了「AP 隔离/客户端隔离」也会导致手机访问不到板子 |
| 填错密码后板子失联了 | 不会失联：15 秒连不上会自动退回热点 `ESP32-Player`，再进 `http://192.168.4.1/wifi` 改 |
| 局域网模式下 IP 变了 | 建议在路由器里给板子的 MAC 绑定固定 IP；板子已实现掉线重连与 IP 变化自动刷新 |
| 网页能放歌但板子没歌词 | 音频同目录要有**同名 `.lrc`**；板子底部显示「歌词:无」即为缺文件 |
| **歌名/歌词里有几个字显示成方框** | 字库缺字。跑 `python tools/font_audit.py "有问题的歌名"` 看是哪些字；若是 GB2312 之外的字（生僻字/繁体/emoji/⚠ 等），用 `gen_font.py` 把对应码点加进字符集后重生成并重烧 |
| 歌词中文**全**变成方框 | 中文字库未参与编译，检查 `src/font/lv_font_cjk_*.c` 是否都在工程内 |
| 列表只显示前 48 首 | 见下节「显示更多曲目」 |
| 编译报 `lv_font_montserrat_xx` 未定义 | `lv_conf.h` 没放对位置，或没把字体宏打开 |
| 编译慢/内存不足 | 确认分区方案为 **Custom**，且 `MusicPlayer/partitions.csv` 存在（app 7MB×2）。若报 `text section exceeds available space`，说明回落到默认 1.2MB 分区了 |

### 显示更多曲目

界面默认最多显示 48 首（受 LVGL 内存池限制）。

1. 打开 `libraries/lvgl/lv_conf.h`，把 `LV_MEM_SIZE` 从 `(48U * 1024U)` 调到 `(96U * 1024U)`
2. 在 `src/music/music_ui.c` 顶部把 `UI_MAX_ITEMS` 改成你想要的数量

### 想要更大的字体

界面中文字体是自研字库（`src/font/`），不走 `LV_FONT_MONTSERRAT_*`。
要换字号就改 `tools/gen_font.py` 里的 `TIER_SPECS`（字号 / bpp / 字符集），
重新生成后改 `lyrics_ui.c`、`music_ui.c` 顶部的 `F_*` / `FONT_*` 宏即可。
注意单套位图必须 < 1 MB（`bitmap_index` 只有 20 bit），超了就要像 20px 那样拆套。

### 想要 MP3 支持

`wav_player.c` 的 `pcm_to_s16_stereo()` 之上再加一层解码即可：
把解码器（如 Helix / minimp3）输出的 PCM 交给同一段「转 16bit 立体声 → `esp_codec_dev_write`」流程，
播放任务与 UI 无需改动。

---

## 10. V1 版适配

若使用 V1 板（无法识别 V1/V2 时看 PCB 丝印，无 `Rev1.1` 即为 V1），需要修改：

1. **`user_config.h`** 换成 V1 版本（把下面几行替换）：

```c
#define EXAMPLE_PIN_NUM_LCD_RST    (GPIO_NUM_21)
#define EXAMPLE_PIN_NUM_BK_LIGHT   (GPIO_NUM_8)
/* 删除 LCD_TE / EXIO_INT / EXIO_* 相关宏 */
#define EXAMPLE_LVGL_TICK_PERIOD_MS    5
```

2. **`lvgl_port.c`** 换成 V1 示例中的版本（V1 用 GPIO 直接控制 LCD 复位与背光，
   不经过 TCA9554；`setup_ui()` 一行同样改为 `music_ui_init()`）。

3. **`src/board/board_io.c`** 中：

```c
/* V1：背光为 GPIO8，PA 使能为 TCA9554 PIN7 */
#define EXAMPLE_PIN_NUM_BK_LIGHT  GPIO_NUM_8
```

即把 `board_exio_init()` 里对 `BL_EN` 的部分去掉，只保留 `IO_EXPANDER_PIN_NUM_7` 置高。

4. **`src/codec_board/board_cfg.h`** 中 `S3_LCD_3_49` 段无需改动（V1/V2 的 I2S/I2C 引脚相同）。

---

## 11. 参考

- 产品文档：<https://docs.waveshare.net/ESP32-S3-Touch-LCD-3.49/>
- Arduino 开发：<https://docs.waveshare.net/ESP32-S3-Touch-LCD-3.49/Arduino>
- ESP-IDF 开发：<https://docs.waveshare.net/ESP32-S3-Touch-LCD-3.49/ESP-IDF>
- V2 示例仓库：<https://gitee.com/waveshare/esp32-s3-touch-lcd-3.49-v2>
