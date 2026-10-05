#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
准备 PlatformIO 构建环境（项目内独立 core dir，不动用户已有安装）。

要点：
  1) 必须挑 **windows** 构建（registry 的 files[0] 是 darwin，会下到 Mach-O 二进制）
  2) 用 platform_packages 把 Arduino 框架覆盖成 4.30312.261001 (= Arduino-ESP32 3.3.12)，
     因为 espressif32 各版本都锁死在 Arduino 2.0.17（IDF 4.4，无 driver/i2c_master.h）
  3) 全程不删除任何文件（批量删除会被安全策略拦截）
"""
import json, os, sys, tarfile, shutil, subprocess, time, urllib.request

CORE = r"D:\wb\wave\tools\piobuild"
REG = "https://dl.registry.platformio.org/download/platformio"
API = "https://api.registry.platformio.org/v3/packages/platformio"
SRC_PLATFORM = r"D:\wb\wave\tools\pio-core\platforms\espressif32"   # 已下好且完整的 7.1.3

WANT = "windows"   # 只要 windows 构建

# (包路径, 版本)  —— framework 强制用 Arduino 3.3.12
PLAN = [
    ("tool/toolchain-xtensa32", "2.50200.97"),
    ("tool/framework-arduinoespressif32", "4.30312.261001"),
    ("tool/tool-esptoolpy", "2.41100.260830"),
    ("tool/tool-mkspiffs", "2.230.0"),
    ("tool/tool-scons", "4.41101.0"),
]


def log(*a):
    print(*a, flush=True)


def human(n):
    return "%.1f MB" % (n / 1048576.0)


def pick(pkgpath, ver):
    d = json.load(urllib.request.urlopen("%s/%s" % (API, pkgpath), timeout=60))
    for v in d.get("versions", []):
        if v["name"] != ver:
            continue
        cands = []
        for f in v.get("files", []):
            sysl = f.get("system")
            sysl = [sysl] if isinstance(sysl, str) else (sysl or [])
            if WANT in sysl or "*" in sysl:
                cands.append(f["download_url"])
        if not cands:                      # 退化：任何带 windows 的
            for f in v.get("files", []):
                if "windows" in f["download_url"]:
                    cands.append(f["download_url"])
        return cands[0] if cands else None
    return None


def curl(url, dest, tries=4):
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        log("      cached %s" % human(os.path.getsize(dest)))
        return True
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    for i in range(1, tries + 1):
        t0 = time.time()
        rc = subprocess.call(["curl", "-sSL", "--fail", "--retry", "3",
                              "--connect-timeout", "30", "--max-time", "1800",
                              "-C", "-", "-o", dest, url])
        if rc == 0 and os.path.getsize(dest) > 0:
            log("      %s in %.0fs" % (human(os.path.getsize(dest)), time.time() - t0))
            return True
        log("      attempt %d failed rc=%s" % (i, rc))
        time.sleep(2)
    return False


def untar(path, target):
    os.makedirs(target, exist_ok=True)
    with tarfile.open(path, "r:gz") as t:
        files = [m for m in t.getmembers() if m.isfile()]
        roots = {m.name.split("/")[0] for m in files}
        strip = 1 if (len(roots) == 1 and all("/" in m.name for m in files)) else 0
        for m in files:
            parts = m.name.split("/")
            if len(parts) <= strip or ".." in parts:
                continue
            dst = os.path.join(target, "/".join(parts[strip:]).replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            s = t.extractfile(m)
            if s is None:
                continue
            with s, open(dst, "wb") as o:
                shutil.copyfileobj(s, o)
    log("      -> %s (%d files)" % (target, sum(len(f) for _, _, f in os.walk(target))))


def main():
    stage = os.path.join(CORE, "_dl")
    os.makedirs(stage, exist_ok=True)

    # 平台：直接复制已下好的 7.1.3
    pdir = os.path.join(CORE, "platforms", "espressif32")
    if not os.path.exists(os.path.join(pdir, "platform.json")):
        os.makedirs(os.path.dirname(pdir), exist_ok=True)
        shutil.copytree(SRC_PLATFORM, pdir)
    log("platform espressif32 7.1.3  %s"
        % ("OK" if os.path.exists(os.path.join(pdir, "platform.json")) else "MISSING"))

    log("\npackages:")
    for pkg, ver in PLAN:
        name = pkg.split("/")[-1]
        target = os.path.join(CORE, "packages", name)
        if os.path.isdir(target) and os.listdir(target):
            log("  = %-32s %-20s 已存在" % (name, ver))
            continue
        url = pick(pkg, ver)
        if not url:
            log("  !! %-32s %-20s 无 windows 构建" % (name, ver))
            continue
        log("  + %-32s %-20s %s" % (name, ver, url.split("/")[-1]))
        p = os.path.join(stage, url.split("/")[-1])
        if not curl(url, p):
            log("      !! 下载失败")
            continue
        untar(p, target)

    # 校验：gcc 版本 & i2c_master.h 是否存在
    log("\n校验:")
    tc = os.path.join(CORE, "packages", "toolchain-xtensa32", "bin")
    for cand in ("xtensa-esp32-elf-gcc.exe", "xtensa-esp32-elf-gcc"):
        f = os.path.join(tc, cand)
        if os.path.exists(f):
            out = subprocess.run([f, "--version"], capture_output=True, text=True,
                                 errors="ignore").stdout
            log("  toolchain gcc: %s" % (out.splitlines()[0] if out else "?"))
            with open(f, "rb") as fh:
                log("  binary magic: %s" % fh.read(2).hex())
            break
    fw = os.path.join(CORE, "packages", "framework-arduinoespressif32")
    pj = os.path.join(fw, "package.json")
    if os.path.exists(pj):
        log("  framework: %s" % json.load(open(pj, encoding="utf-8")).get("version"))
    hit = []
    for root, _, files in os.walk(os.path.join(fw, "tools", "sdk")):
        if "i2c_master.h" in files:
            hit.append(os.path.join(root, "i2c_master.h"))
            break
    log("  driver/i2c_master.h: %s" % (hit[0] if hit else "!! 未找到（说明不是 IDF5.x）"))
    log("\nCORE=%s" % CORE)


if __name__ == "__main__":
    main()
