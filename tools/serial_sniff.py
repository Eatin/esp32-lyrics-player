import sys, time, serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
READ = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0

MARKERS = ("Music Player", "ESP32-S3-Touch", "WiFi AP", "mDNS", "web player")

def read_for(s, secs):
    t0 = time.time()
    buf = b""
    lines = []
    while time.time() - t0 < secs:
        d = s.read(4096)
        if d:
            buf += d
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                lines.append(ln.rstrip(b"\r").decode("utf-8", "replace"))
    return lines

def hit(lines):
    return any(any(m in l for m in MARKERS) for l in lines)

s = serial.Serial()
s.port = PORT
s.baudrate = 115200
s.timeout = 0.2
s.dtr = False
s.rts = False
s.open()
s.reset_input_buffer()

# ESP32-S3 USB-Serial-JTAG: RTS->EN(reset, active low), DTR->GPIO0
# Try several pulse patterns until the boot banner shows up.
combos = [
    ("rts pulse, dtr=0", lambda: (s.setDTR(False), s.setRTS(True),  time.sleep(.15), s.setRTS(False))),
    ("rts pulse, dtr=1", lambda: (s.setDTR(True),  s.setRTS(True),  time.sleep(.15), s.setRTS(False))),
    ("dtr pulse, rts=0", lambda: (s.setRTS(False), s.setDTR(True),  time.sleep(.15), s.setDTR(False))),
    ("dtr+latch",        lambda: (s.setDTR(True),  s.setRTS(True),  time.sleep(.05), s.setDTR(False), time.sleep(.15), s.setRTS(False))),
]

all_lines = []
for name, fn in combos:
    fn()
    time.sleep(0.1)
    lines = read_for(s, READ)
    all_lines += lines
    print(f"# --- combo [{name}] -> {len(lines)} lines, banner={hit(lines)} ---")
    if hit(lines):
        break

s.close()
print("==================== FULL LOG ====================")
print("\n".join(all_lines))
