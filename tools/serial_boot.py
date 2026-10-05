import sys, time, serial
import serial.tools.list_ports as lp

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
READ = float(sys.argv[2]) if len(sys.argv) > 2 else 14.0

def drain(s, secs, sink):
    t0 = time.time(); buf = b""
    while time.time() - t0 < secs:
        d = s.read(8192)
        if d:
            buf += d
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                sink.append(ln.rstrip(b"\r").decode("utf-8", "replace"))

# 1) reset with GPIO0 high -> normal boot
try:
    s = serial.Serial(); s.port = PORT; s.baudrate = 115200; s.timeout = 0.2
    s.dtr = False; s.rts = False; s.open()
    s.setDTR(False); time.sleep(0.15)
    s.setRTS(True);  time.sleep(0.15)
    s.setRTS(False)
    s.close()
except Exception as e:
    print("# reset err:", repr(e))

# 2) wait for USB re-enumeration, then reopen and capture
lines = []
deadline = time.time() + 8.0
while time.time() < deadline:
    if any(p.device == PORT for p in lp.comports()):
        try:
            s = serial.Serial(PORT, 115200, timeout=0.2)
            s.dtr = False; s.rts = False
            drain(s, READ, lines)
            s.close()
            break
        except Exception:
            time.sleep(0.2)
    else:
        time.sleep(0.2)

print(f"# captured {len(lines)} lines")
print("\n".join(lines))
