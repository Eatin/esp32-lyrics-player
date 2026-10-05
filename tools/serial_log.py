"""
录制 COM4 串口日志到文件，用于复现「一触摸就灭屏」问题。

用法:
    python serial_log.py <秒数> <输出文件>

行为:
  1. 打开 COM4 -> 触发复位 -> 记录启动日志
  2. 持续读取直到指定秒数，实时写入文件
  3. 不自动关闭串口直到结束
"""
import sys, time, serial

DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0
OUT = sys.argv[2] if len(sys.argv) > 2 else r"D:\wb\wave\tools\touch_test.log"

s = serial.Serial("COM4", 115200, timeout=0.2)
# 复位以便拿到完整启动日志
s.setDTR(False); s.setRTS(True); time.sleep(0.12)
s.setRTS(False); time.sleep(0.05)
s.reset_input_buffer()

t0 = time.time()
with open(OUT, "wb") as f:
    f.write(b"===== CAPTURE START =====\n")
    while time.time() - t0 < DUR:
        d = s.read(8192)
        if d:
            f.write(d); f.flush()
    f.write(b"\n===== CAPTURE END =====\n")
s.close()
print("done ->", OUT)
