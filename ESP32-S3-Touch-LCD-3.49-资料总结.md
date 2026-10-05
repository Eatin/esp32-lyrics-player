# 微雪 ESP32-S3-Touch-LCD-3.49 学习笔记

> 资料来源：<https://docs.waveshare.net/ESP32-S3-Touch-LCD-3.49/?variant=ESP32-S3-Touch-LCD-3.49>
> 整理时间：2026-10-05

---

## 1. 产品概述

ESP32-S3-Touch-LCD-3.49 是微雪（Waveshare）设计的一款**高性能、高集成的微控制器开发板**。板载 3.49 英寸电容高清 IPS 屏、电源管理芯片、六轴传感器（三轴加速度计 + 三轴陀螺仪）、RTC、低功耗音频编解码芯片和回声消除电路等外设，方便开发并嵌入到产品中。

典型应用方向：智能音箱 / 语音交互终端、便携音乐播放器、物联网 HMI 面板、可穿戴设备原型。

---

## 2. 型号与版本

### 2.1 在售型号（SKU）

| SKU | 型号 | 说明 |
|---|---|---|
| 32373 | ESP32-S3-Touch-LCD-3.49 | A 款，带锂电池 |
| 32374 | ESP32-S3-Touch-LCD-3.49-EN | A 款，不带锂电池 |
| 32375 | ESP32-S3-Touch-LCD-3.49B | B 款，带锂电池 |
| 32376 | ESP32-S3-Touch-LCD-3.49B-EN | B 款，不带锂电池 |

### 2.2 V1 / V2 版本差异 ⚠️

> **V1 版本已停产，自 2026 年 6 月 8 日起统一发货 V2 版本。**
> 两个版本**引脚定义不同，示例程序不可混用**，背光引脚更换后混用会导致屏幕不亮。

**版本识别方式**
- PCB 丝印：V2 版本带有 `Rev1.1` 丝印
- 外壳 QC 标签：V2 版本贴有 `V2` 标签

**V1 → V2 主要变化**

| 变化项 | 说明 |
|---|---|
| TP_INT 引脚 | 新增引出 |
| 锂电池充电电流 | 优化 |
| LCD_TE 与 LCD_RESET | 引脚互换 |
| LCD_BL 与 EXIO_INT | 引脚互换 |

---

## 3. 核心规格

| 类别 | 参数 |
|---|---|
| 主控 | **ESP32-S3R8**，Xtensa 32 位 LX7 双核，主频最高 **240MHz** |
| 无线 | 2.4GHz Wi-Fi (802.11 b/g/n) + Bluetooth 5 (LE)，板载 2.4G 天线，可改外接 |
| 存储 | 内置 512KB SRAM + 384KB ROM，**叠封 8MB PSRAM + 16MB Flash** |
| 显示屏 | 3.49 英寸电容触摸 IPS 屏，**172 × 640**，16.7M 彩色 |
| 显示驱动 | **AXS15231B**（显示/触摸一体，QSPI + I2C 接口） |
| 音频输出 | **ES8311** DAC（高性能低功耗音频数模转换） |
| 音频输入 | **ES7210** ADC（支持多路麦克风），双数字麦克风阵列，支持降噪与回声消除 |
| 运动传感 | **QMI8658** 六轴 IMU（3 轴加速度 + 3 轴陀螺仪），可做姿态检测、计步 |
| 实时时钟 | **PCF85063** RTC，支持时间保持 |
| GPIO 扩展 | **TCA9554PWR** 8 位 I2C GPIO 扩展芯片 |
| Flash | W25Q128JVSI（16MB） |
| 按键 | PWR、BOOT、RESET（均位于背面，功能可自定义） |
| 扩展 | 预留 **22PIN 2.54mm 间距通讯焊盘**；Micro SD 卡槽；MX1.25 2PIN 扬声器接口；IPEX 1 代座子 |
| 供电 | 3.7V MX1.25 锂电池充电接口（USB Type-C 用于烧录与日志） |

---

## 4. LCD 参数

| 参数 | 值 | 参数 | 值 |
|---|---|---|---|
| 显示面板 | LCD | 显示尺寸 | 3.49 英寸 |
| 显示分辨率 | 172 × 640 | 显示颜色 | 16.7M |
| 显示亮度 | 350 cd/m² | 对比度 | 1200:1 |
| 显示接口 | QSPI | 驱动芯片 | AXS15231B |
| 触摸接口 | I2C | 触摸类型 | 电容式感应 |

> 竖屏（Portrait）为 172(宽) × 640(高)；若软件旋转 90°，则变为 640 × 172。

---

## 5. 资源简介（板载器件清单）

| # | 器件 | 说明 |
|---|---|---|
| 01 | ESP32-S3R8 | Wi-Fi/蓝牙 SoC，240MHz，叠封 8MB PSRAM |
| 02 | 贴片天线 | 2.4GHz Wi-Fi + BT5(LE) |
| 03 | TCA9554PWR | 8 位 I2C GPIO 扩展 |
| 04 | W25Q128JVSI | 16MB Flash |
| 05 | ES8311 | DAC 音频编码芯片 |
| 06 | ES7210 | ADC 音频解码芯片（多路麦克风输入） |
| 07 | QMI8658 | 六轴 IMU |
| 08 | 双麦克风阵列 | 支持降噪、回声消除、远场唤醒 |
| 09 | Micro SD 卡槽 | 支持 FAT32 格式 SD 卡 |
| 10 | RESET 按键（背面） | 配合 BOOT 进入下载模式 |
| 11 | PWR 电源键（背面） | 配合程序实现锂电池供电控制 |
| 12 | BOOT 按键（背面） | 按住 BOOT + 单击 RESET 进入下载模式 |
| 13 | MX1.25 2PIN 扬声器接口（背面） | 音频输出，支持外接扬声器 |
| 14 | IPEX 1 代座子 | 拆掉电阻可切换为外部天线 |
| 15 | 22PIN 2.54mm 通孔焊盘 | 连接外部模块 / 功能扩展 |
| 16 | Type-C 接口 | 烧录程序与日志打印 |
| 17 | PCF85063 | RTC 时钟芯片 |

---

## 6. 引脚定义（实测自官方示例源码）

官方文档页面的接口图是图片，以下引脚表整理自官方示例程序的 `user_config.h` 与
`src/codec_board/board_cfg.h`（Arduino 版），**是写代码时真正需要的部分**。

### 6.1 V2 版引脚

| 功能 | 引脚 / 定义 |
|---|---|
| LCD QSPI CS | GPIO9 |
| LCD QSPI PCLK | GPIO10 |
| LCD QSPI D0 ~ D3 | GPIO11 / 12 / 13 / 14 |
| LCD TE | GPIO21（V2 由 LCD_RESET 换到此脚） |
| LCD 复位 | **无独立 GPIO**，走 TCA9554 `EXIO5` |
| 背光 PWM | **GPIO42**（LEDC 调光，另需 `EXIO1 = BL_EN` 使能） |
| EXIO 中断 | GPIO8 |
| 触摸 I2C | SDA = GPIO17，SCL = GPIO18，器件地址 `0x3B` |
| 系统 I2C（codec/扩展/RTC/IMU） | SDA = GPIO47，SCL = GPIO48 |
| I2S（ES8311 / ES7210） | MCLK=7, BCLK=15, WS(LRCK)=46, DIN=6, DOUT=45 |
| TF 卡（SDMMC 1 线） | CLK = GPIO41，CMD = GPIO39，D0 = GPIO40 |
| 电池电压 ADC | GPIO4 |
| SYS_OUT | GPIO16 |
| RTC 地址 | `0x51` |
| IMU 地址 | `0x6B` |
| TCA9554 地址 | `0x20` |

**TCA9554 扩展 IO 分配（V2）**

| EXIO | 功能 | 方向 |
|---|---|---|
| 0 | TOUCH_INT | 输入 |
| 1 | BL_EN（背光使能） | 输出 |
| 2 | IMU_INT1 | — |
| 3 | IMU_INT2 | — |
| 4 | RTC_INT | — |
| 5 | LCD_RST | 输出 |
| 6 | SYS_EN | — |
| 7 | **NS_MODE（音频功放使能）** | 输出（置 1 才有声音） |

**音频通道板级配置（`codec_board/board_cfg.h` 中的 `S3_LCD_3_49`）**

```
i2c: {sda: 47, scl: 48}
i2s: {mclk: 7, bclk: 15, ws: 46, din: 6, dout: 45}
out: {codec: ES8311, pa: -1, pa_gain: 6, use_mclk: 1}
in:  {codec: ES7210}
```

> `pa: -1` 表示功放不由 ESP32 GPIO 直控，而是通过 TCA9554 的 `EXIO7` 手动使能。
> `use_mclk: 1` 表示 MCLK 由 ESP32 I2S 输出给 ES8311，因此**必须按实际采样率配置**，否则 ES8311 无法锁定时钟。

### 6.2 V1 版引脚（已停产，仅作对照）

| 功能 | 引脚 |
|---|---|
| LCD QSPI CS / PCLK | GPIO9 / GPIO10 |
| LCD QSPI D0 ~ D3 | GPIO11 / 12 / 13 / 14 |
| LCD 复位 | **GPIO21** |
| 背光 | **GPIO8** |
| 触摸 I2C | SDA = GPIO17，SCL = GPIO18（地址 `0x3B`） |
| 系统 I2C | SDA = GPIO47，SCL = GPIO48 |
| I2S | MCLK=7, BCLK=15, WS=46, DIN=6, DOUT=45（与 V2 相同） |
| TF 卡 | CLK = GPIO41，CMD = GPIO39，D0 = GPIO40（与 V2 相同） |
| 音频功放使能 | TCA9554 `PIN7` |

---

## 7. 开发方式

板子同时支持两套开发框架：

| 框架 | 特点 | 适合人群 |
|---|---|---|
| **Arduino IDE** | 上手快、社区大、库丰富、封装复杂功能 | 初学者、爱好者、快速原型 |
| **ESP-IDF** | 完整工具链、系统级细粒度控制、性能可控 | 复杂项目、性能敏感应用 |

### 7.1 Arduino 开发

- 入门教程：<https://docs.waveshare.net/ESP32-Arduino-Tutorials>（共 14 节，含 LVGL 图形界面与综合项目）
  > ⚠️ 该通用教程以 **ESP32-S3-Zero** 为教学板，硬件代码基于其引脚，动手前要对照手中板子的引脚图。
- 所需库：

| 库 | 版本 | 安装方式 |
|---|---|---|
| LVGL 图形库 | v8.3.11 / v9.3.0 | 离线安装 |
| SensorLib 传感器库 | v0.3.1 | 离线 / 在线 |
| esp32 by Espressif Systems | **= 3.3.0** | 离线 / 在线 |

  > **LVGL 与驱动库版本依赖很强**：为 LVGL v8 写的驱动可能不兼容 v9；混用会导致编译失败或运行时异常，务必使用表中指定版本。

- 示例程序：
  - V1：<https://files.waveshare.net/wiki/ESP32-S3-Touch-LCD-3.49/ESP32-S3-Touch-LCD-3.49-Demo.zip>
  - V2：<https://gitee.com/waveshare/esp32-s3-touch-lcd-3.49-v2>

### 7.2 ESP-IDF 开发

- 示例默认使用 **ESP-IDF V5.5.3**；若其它版本编译失败可回退到此版本。
- 推荐 **VS Code + ESP-IDF 扩展**（扩展版本 ≥ 2.0 会自动识别已安装的 IDF 环境）。
- 环境安装：前往 [ESP-IDF Installation Manager](https://dl.espressif.com/dl/eim/) 下载 **离线整合包(.zst) + 安装器(.exe)**，把两个文件放同一目录，运行安装器 → 「从存档安装」。
  - 安装路径**不要包含中文或空格**。
  - 安装完成后建议顺手点击「安装驱动程序」。
- 入门教程：<https://docs.waveshare.net/ESP32-ESP-IDF-Tutorials>（含 FreeRTOS、外设驱动、Wi-Fi、BLE）

---

## 8. 示例程序清单

### 8.1 Arduino 示例（10 个）

| 序号 | 示例 | 功能 | 依赖库 |
|---|---|---|---|
| 01 | ADC_Test | 读取锂电池电压值 | — |
| 02 | I2C_PCF85063 | 打印 RTC 芯片实时时间 | SensorLib |
| 03 | I2C_QMI8658 | 打印 IMU 原始数据 | SensorLib |
| 04 | SD_Card | 挂载并显示 SD 卡信息 | — |
| 05 | WIFI_AP | 设为 AP 模式，获取接入设备 IP | — |
| 06 | WIFI_STA | 设为 STA 模式，连接路由器获取 IP | — |
| 07 | BATT_PWR_Test | 锂电池供电下用 PWR 键控制电源 | LVGL |
| 08 | Audio_Test | 扬声器播放麦克风录到的声音 | LVGL |
| 09 | LVGL_V8_Test | LVGL V8 界面示例 | LVGL V8.4.0 |
| 10 | LVGL_V9_Test | LVGL V9 界面示例 | LVGL V9.3.0 |

### 8.2 ESP-IDF 示例（11 个）

与 Arduino 版一致，额外多一个：

| 序号 | 示例 | 功能 | 依赖库 |
|---|---|---|---|
| 11 | FactoryProgram | 综合测试显示、触摸、音频、SD 卡、RTC、IMU、Wi-Fi（也可直接烧官方 BIN） | LVGL V8.3.11 |

### 8.3 两个示例的关键代码要点

**08_Audio_Test（音频，最值得参考）**

```c
i2c_master_Init();      // 初始化 I2C 总线（port0: 47/48, port1: 17/18）
tca9554_init();         // 初始化功放 CTRL（EXIO7 置高）
lvgl_port_init();       // 初始化 LVGL 接口
user_audio_bsp_init();  // 初始化 audio 接口
```

- 单击 `Recording` 进入录音（3 秒自动结束）→ 单击 `Play` 回放
- ESP-IDF 版还支持 `Play Music` / `Music Exit`

**07 / 10 / 11 中的常用配置项**（`user_config.h`）

```c
#define Backlight_Testing 0
#define USER_DISP_ROT_90    1
#define USER_DISP_ROT_NONO  0
#define Rotated USER_DISP_ROT_NONO   // 软件实现旋转；改成 USER_DISP_ROT_90 即旋转 90°
```

**11_FactoryProgram 初始化顺序（ESP-IDF 参考模板）**

```c
setup_ui(&src_ui);
lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);   // 背光 PWM，255 级亮度
tca9554_init();                          // GPIO 扩展
button_Init();                           // 按键
adc_bsp_init();                          // 电压检测
i2c_rtc_setup();                         // RTC
i2c_rtc_setTime(2025,7,7,18,43,30);
i2c_imu_setup();                         // IMU
_sdcard_init();                          // SD 卡
espwifi_init();                          // Wi-Fi
user_audio_bsp_init();                   // 音频
audio_play_init();
```

- 长按 BOOT 进入 audio 界面；单击 BOOT 进入 Touch 画板界面；左滑调背光

---

## 9. 相关资料

### 9.1 硬件资料

- [V1 原理图](https://www.waveshare.net/w/upload/2/20/ESP32-S3-Touch-LCD-3.49-Schematic.pdf)
- [V2 原理图](https://gitee.com/waveshare/esp32-s3-touch-lcd-3.49-v2/raw/master/schematic/ESP32-S3-Touch-LCD-3.49%20V2.pdf)
- [结构文件（3D/尺寸）](https://www.waveshare.net/w/upload/5/52/ESP32-S3-Touch-LCD-3.49.zip)

### 9.2 数据手册

| 器件 | 链接 |
|---|---|
| ESP32-S3 规格书 | [中文](https://documentation.espressif.com/esp32-s3_datasheet_cn.pdf) / [英文](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf) |
| ESP32-S3 技术参考手册 | [中文](https://documentation.espressif.com/esp32-s3_technical_reference_manual_cn.pdf) / [英文](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf) |
| AXS15231B | <https://www.waveshare.net/w/upload/9/9b/AXS15231B_Datasheet_V0.5.pdf> |
| ES8311 数据手册 | <https://www.waveshare.net/w/upload/6/65/ES8311.DS.pdf> |
| ES8311 用户手册 | <https://www.waveshare.net/w/upload/5/56/ES8311.user.Guide.pdf> |
| QMI8658 | <https://www.waveshare.net/w/upload/2/23/QMI8658C_datasheet_rev_0.9.pdf> |
| PCF85063 | <https://www.waveshare.net/w/upload/c/c0/Pcf85063atl1118-NdPQpTGE-loeW7GbZ7.pdf> |
| ST7701S | <https://www.waveshare.net/w/upload/1/14/ST7701S_SPEC_V1.1.pdf> |

### 9.3 其它工具

- SquareLine Studio 界面设计教程：<https://www.waveshare.net/wiki/Waveshare_SquareLine_Studio>
- Arduino IDE 工程参数配置示意（FAQ 中提供的 Tools 菜单截图）
- 用 AI 辅助编程：文档页有「发给 AI」/「复制为 Markdown」按钮，可把整页内容丢给 AI

---

## 10. 常见问题（FAQ）

| 问题 | 解决办法 |
|---|---|
| Arduino 程序编译报错 | 检查 Arduino IDE → Tools 是否按文档正确配置（开发板、PSRAM、Flash、分区） |
| 重新下载时连不上串口 / 烧录失败 | 长按 BOOT → 单击 RESET → 松开 RESET → 松开 BOOT，进入下载模式 |
| VSCode 环境搭建失败 | 优先排查网络，尝试切换网络 |
| 程序首次编译超级慢 | 正常现象（首次编译 LVGL 很慢），耐心等待 |
| 找不到 AppData 文件夹 | 资源管理器 → 查看 → 勾选「隐藏的项目」 |
| 如何查看 COM 口 | Windows：设备管理器 → 端口(COM 和 LPT)，或 CMD 输入 `mode`；Linux：`ls /dev/ttyUSB*`、`dmesg` |
| 如何用 SquareLine Studio 设计界面 | 参考微雪 Wiki 的 SquareLine Studio 使用教程 |

---

## 11. 上手要点小结（踩坑提示）

1. **先确认版本**：V2 引脚与 V1 不同，混用示例会导致黑屏（背光脚换了）。
2. **功放必须使能**：音频输出走 TCA9554 的 `EXIO7`（NS_MODE 置 1），只初始化 ES8311 不会有声音。
3. **背光两处控制**：GPIO42 的 PWM 亮度 + `EXIO1` 使能，缺一不可（V2）。
4. **MCLK 必须匹配采样率**：`use_mclk: 1`，播放时按音频实际采样率重新 `close/open` codec。
5. **LVGL 版本严格匹配**：v8 工程必须用 v8.4.0，v9 工程必须用 v9.3.0。
6. **务必开 PSRAM（OPI PSRAM）**：LVGL 双缓冲 172×640×2×2 ≈ 440KB，全靠 PSRAM。
7. **SD 卡要 FAT32**，且音量/时长的读写建议放在 PSRAM。
8. 首次烧录后用 `11_FactoryProgram` 官方固件做**整机自检**（显示/触摸/音频/SD/RTC/IMU/Wi-Fi），比逐个例程试快得多。
