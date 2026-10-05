#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""抓 ESP32-S3 启动日志：先做一次硬复位，再带重试地打开串口读取。

用途：验证 `board_power_hold()` 是否真的把 TCA9554 的 EXIO6(SYS_EN) 拉高
（该日志只在开机时打印一次，普通抓包抓不到）。

用法： python boot_cap.py COM4 25 boot_batt.log
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0
OUT = sys.argv[3] if len(sys.argv) > 3 else "boot_batt.log"


def hard_reset():
    """用 DTR/RTS 时序复位 ESP32-S3（USB-Serial/JTAG 同款做法）。"""
    s = serial.Serial()
    s.port = PORT
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    s.setRTS(True)
    s.setDTR(False)
    time.sleep(0.1)
    s.setDTR(True)
    s.setRTS(False)
    time.sleep(0.1)
    s.setRTS(True)
    time.sleep(0.1)
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.1)
    s.setDTR(False)
    s.setRTS(False)
    s.close()


def open_retry(deadline):
    """复位后 CDC 会重新枚举，端口短暂消失，这里重试打开。"""
    while time.time() < deadline:
        try:
            s = serial.Serial()
            s.port = PORT
            s.baudrate = 115200
            s.timeout = 0.2
            s.dtr = False
            s.rts = False
            s.open()
            return s
        except Exception:
            time.sleep(0.15)
    return None


try:
    hard_reset()
    print("# hard reset sent")
except Exception as e:  # 端口被占用也不致命，继续尝试读取
    print(f"# hard reset failed: {e}")

s = open_retry(time.time() + 10.0)
if s is None:
    print(f"# could not open {PORT}")
    sys.exit(1)

t0 = time.time()
buf = b""
with open(OUT, "w", encoding="utf-8") as f:
    while time.time() - t0 < SECS:
        try:
            d = s.read(8192)
        except Exception as e:
            print(f"# read error: {e}")
            break
        if not d:
            continue
        buf += d
        while b"\n" in buf:
            ln, buf = buf.split(b"\n", 1)
            txt = ln.rstrip(b"\r").decode("utf-8", "replace")
            f.write(f"{time.time() - t0:7.2f}  {txt}\n")
            f.flush()
try:
    s.close()
except Exception:
    pass
print(f"# captured to {OUT}")
