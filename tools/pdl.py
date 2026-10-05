#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
多代理并行分片下载器。

背景：本机代理(127.0.0.1:6628)阻断了 github.com/*/releases/download/*，
      但 GitHub 代理镜像站点可用。这里把大文件切片并发分给多个镜像，
      失败自动重试，最后按序拼接。
"""
import os, sys, subprocess, threading, time, json

PROXIES = [
    "https://ghproxy.net/",
    "https://ghfast.top/",
    "https://gh-proxy.com/",
]
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"

# (本地文件名, 完整URL)  —— 只下载 ESP32-S3 编译必需的包
FILES = [
    ("esp32-3.3.0.zip",
     "https://github.com/espressif/arduino-esp32/releases/download/3.3.0/esp32-3.3.0.zip"),
    ("esp32-arduino-libs-3.0.1.zip",
     "https://github.com/espressif/arduino-esp32/releases/download/3.0.1/esp32-arduino-libs-3.0.1.zip"),
    ("xtensa-esp32-elf.zip",
     "https://github.com/espressif/crosstool-NG/releases/download/esp-12.2.0_20230208/"
     "xtensa-esp32-elf-12.2.0_20230208-x86_64-w64-mingw32.zip"),
    ("esptool-3.3-windows.zip",
     "https://github.com/espressif/arduino-esp32/releases/download/2.0.2/esptool-3.3-windows.zip"),
]

OUT = sys.argv[1] if len(sys.argv) > 1 else r"D:\wb\wave\tools\dl"
CHUNK = 4 * 1024 * 1024
NTHREAD = 12

os.makedirs(OUT, exist_ok=True)
lock = threading.Lock()
stats = {"bytes": 0}


def human(n):
    return "%.1f MB" % (n / 1048576.0)


def size_of(url):
    for p in PROXIES:
        try:
            out = subprocess.check_output(
                ["curl", "-sIL", "--max-time", "40", "-A", UA, p + url],
                stderr=subprocess.DEVNULL).decode("utf-8", "ignore")
            for line in out.splitlines():
                if line.lower().startswith("content-length:"):
                    v = int(line.split(":")[1].strip())
                    if v > 1000:
                        return v
        except Exception:
            pass
    return None


def fetch_range(url, path, start, end, proxy, tag):
    """下载 [start,end] 到 path（原子：先写 .part 再改名）"""
    if os.path.exists(path) and os.path.getsize(path) == end - start + 1:
        return True
    part = path + ".part"
    for attempt in range(1, 7):
        rc = subprocess.call([
            "curl", "-sS", "-L", "--fail", "--connect-timeout", "20",
            "--max-time", "900", "-A", UA,
            "-r", "%d-%d" % (start, end), "-o", part, proxy + url,
        ], stderr=subprocess.DEVNULL)
        if rc == 0 and os.path.exists(part) and os.path.getsize(part) == end - start + 1:
            # 残留的 curl 进程可能短暂占用文件，重试几次再放弃
            for _ in range(10):
                try:
                    os.replace(part, path)
                    break
                except PermissionError:
                    time.sleep(1.0)
            else:
                continue
            with lock:
                stats["bytes"] += end - start + 1
            return True
        time.sleep(1.5 * attempt)
    return False


def do_file(name, url):
    final = os.path.join(OUT, name)
    if os.path.exists(final):
        print("  = %-30s 已存在 %s" % (name, human(os.path.getsize(final))), flush=True)
        return True

    total = size_of(url)
    if not total:
        print("  !! %-30s 无法获取文件大小" % name, flush=True)
        return False
    n = (total + CHUNK - 1) // CHUNK
    print("  + %-30s %s  %d 段 x %d 线程" % (name, human(total), n, NTHREAD), flush=True)

    partdir = os.path.join(OUT, "_parts_" + name)
    os.makedirs(partdir, exist_ok=True)
    jobs = [(i, i * CHUNK, min((i + 1) * CHUNK, total) - 1) for i in range(n)]

    idx = [0]
    fails = []

    def worker():
        while True:
            with lock:
                if idx[0] >= len(jobs):
                    return
                k = idx[0]
                idx[0] += 1
            i, s, e = jobs[k]
            proxy = PROXIES[k % len(PROXIES)]
            p = os.path.join(partdir, "%05d" % i)
            if not fetch_range(url, p, s, e, proxy, name):
                fails.append(i)

    ths = [threading.Thread(target=worker, daemon=True) for _ in range(NTHREAD)]
    t0 = time.time()
    for t in ths:
        t.start()
    for t in ths:
        t.join()
    dt = time.time() - t0

    if fails:
        print("  !! %-30s 失败段: %s" % (name, fails[:10]), flush=True)
        return False

    with open(final + ".tmp", "wb") as out:
        for i in range(n):
            p = os.path.join(partdir, "%05d" % i)
            with open(p, "rb") as f:
                while True:
                    b = f.read(1 << 20)
                    if not b:
                        break
                    out.write(b)
    if os.path.getsize(final + ".tmp") != total:
        print("  !! %-30s 拼接后大小不符" % name, flush=True)
        return False
    os.replace(final + ".tmp", final)
    print("  ✓ %-30s %s  用时 %.0fs (%.0f KB/s)"
          % (name, human(total), dt, total / 1024.0 / max(dt, 0.001)), flush=True)
    return True


def main():
    print("目标目录:", OUT, flush=True)
    ok = True
    for name, url in FILES:
        if not do_file(name, url):
            ok = False
    print("\nALL OK" if ok else "\nSOME FAILED", flush=True)


if __name__ == "__main__":
    main()
