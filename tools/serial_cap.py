#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""抓 N 秒串口日志到文件（含时间戳），用于复现崩溃/观察运行状态。"""
import sys, time, serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
OUT = sys.argv[3] if len(sys.argv) > 3 else "serial_cap.log"

s = serial.Serial(PORT, 115200, timeout=0.2)
s.dtr = False
s.rts = False
t0 = time.time()
buf = b""
with open(OUT, "w", encoding="utf-8") as f:
    while time.time() - t0 < SECS:
        d = s.read(8192)
        if not d:
            continue
        buf += d
        while b"\n" in buf:
            ln, buf = buf.split(b"\n", 1)
            txt = ln.rstrip(b"\r").decode("utf-8", "replace")
            f.write(f"{time.time()-t0:7.2f}  {txt}\n")
            f.flush()
s.close()
print(f"# captured to {OUT}")
