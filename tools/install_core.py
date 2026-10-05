#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
手动安装 arduino-esp32 core 3.3.0 到 Arduino15 目录。

背景：本机 HTTP 代理(127.0.0.1:6628)对 downloads.arduino.cc 限速到 ~3KB/s，
      而所有 ESP32 包都托管在 GitHub Releases（速度正常）。
      因此绕开 arduino-cli 的包下载器，直接按 package_esp32_index.json
      从 GitHub 拉取并解压到 arduino-cli 期望的目录结构。
"""
import json, os, sys, zipfile, shutil, time, subprocess, hashlib, io

IDX = r"D:\wb\wave\tools\pkg_esp32_index.json"
DATA = r"C:\Users\Eatin\AppData\Local\Arduino15"
PKG = os.path.join(DATA, "packages")
STAGE = os.path.join(DATA, "staging", "packages")
HW = os.path.join(PKG, "esp32", "hardware", "esp32", "3.3.0")
TOOLS = os.path.join(PKG, "esp32", "tools")

HOSTS = ("x86_64-pc-mingw32", "x86_64-mingw32", "i686-mingw32", "all")


def log(*a):
    print(*a, flush=True)


def human(n):
    return "%.1f MB" % (n / 1048576.0)


def sha1(path, url):
    """校验和形如 'SHA-256:...' 或 'SHA-1:...'"""
    if not url:
        return True
    algo, _, want = url.partition(":")
    algo = algo.strip().lower().replace("-", "")
    want = want.strip().lower()
    if not want:
        return True
    h = hashlib.new(algo)
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    got = h.hexdigest()
    ok = got == want
    log("      checksum %s %s" % ("OK" if ok else "MISMATCH", algo))
    if not ok:
        log("      want", want, "got", got)
    return ok


def fetch(url, dest):
    """用 curl 下载（urllib 走本地代理会被断开，curl 稳定且快）。"""
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        log("      cached %s" % human(os.path.getsize(dest)))
        return dest
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    t0 = time.time()
    for attempt in range(1, 6):
        rc = subprocess.call([
            "curl", "-sSL", "--fail", "--retry", "3", "--retry-delay", "2",
            "--connect-timeout", "30", "--max-time", "1800",
            "-C", "-", "-o", dest, url,
        ])
        if rc == 0 and os.path.exists(dest) and os.path.getsize(dest) > 0:
            dt = time.time() - t0
            sz = os.path.getsize(dest)
            log("      downloaded %s in %.1fs (%.1f MB/s)"
                % (human(sz), dt, sz / 1048576.0 / max(dt, 0.001)))
            return dest
        log("      attempt %d failed (rc=%d), retry..." % (attempt, rc))
        time.sleep(2)
    raise RuntimeError("download failed: " + url)


def extract_flat(zpath, target):
    """解压；若 zip 内只有一个顶层目录则剥掉它（与 arduino-cli 行为一致）。"""
    if os.path.isdir(target):
        shutil.rmtree(target, ignore_errors=True)
    os.makedirs(target, exist_ok=True)
    with zipfile.ZipFile(zpath) as z:
        names = [n for n in z.namelist() if not n.endswith("/")]
        roots = {n.split("/")[0] for n in names}
        prefix = ""
        if len(roots) == 1:
            only = roots.pop()
            if only and any(n.startswith(only + "/") for n in names):
                prefix = only + "/"
        for n in names:
            if prefix and not n.startswith(prefix):
                continue
            rel = n[len(prefix):] if prefix else n
            if not rel:
                continue
            dst = os.path.join(target, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with z.open(n) as src, open(dst, "wb") as out:
                shutil.copyfileobj(src, out)
    n = sum(len(f) for _, _, f in os.walk(target))
    log("      extracted %d files -> %s" % (n, target))


def main():
    d = json.load(open(IDX, encoding="utf-8"))
    esp = d["packages"][0]
    plat = [p for p in esp["platforms"] if p["version"] == "3.3.0"][0]
    toolidx = {}
    for t in esp["tools"]:
        for s in t["systems"]:
            toolidx[(t["name"], s["host"])] = s

    # ---------- 1) 平台本体 ----------
    if os.path.isdir(HW) and os.path.exists(os.path.join(HW, "platform.txt")):
        log("[1/2] platform 3.3.0 already installed")
    else:
        log("[1/2] platform esp32:esp32@3.3.0  %s" % human(int(plat.get("size", 0))))
        z = fetch(plat["url"], os.path.join(STAGE, "esp32-3.3.0.zip"))
        sha1(z, plat.get("checksum"))
        extract_flat(z, HW)
    log("      platform.txt: %s" % ("OK" if os.path.exists(os.path.join(HW, "platform.txt")) else "MISSING"))

    # ---------- 2) 工具链 ----------
    log("[2/2] tools")
    for dep in plat["toolsDependencies"]:
        name, ver = dep["name"], dep["version"]
        target = os.path.join(TOOLS, name, ver)
        if os.path.isdir(target) and os.listdir(target):
            log("  = %-22s %-26s (已安装)" % (name, ver))
            continue
        host = next((h for h in HOSTS if (name, h) in toolidx), None)
        if not host:
            log("  - %-22s %-26s 跳过（本机无对应构建，且非编译必需）" % (name, ver))
            continue
        s = toolidx[(name, host)]
        url = s["url"]
        fn = url.split("/")[-1]
        log("  + %-22s %-26s %s" % (name, ver, human(int(s.get("size", 0)))))
        try:
            z = fetch(url, os.path.join(STAGE, fn))
        except Exception as e:
            log("      !! 下载失败: %s" % e)
            continue
        sha1(z, s.get("checksum"))
        extract_flat(z, target)

    # ---------- 3) 占位 builtin 工具 ----------
    # arduino-cli 初始化时会强制安装 dfu/serial/mdns-discovery 与 serial-monitor。
    # 前两个已装好；后两个走慢速 CDN 且**本项目完全用不到**
    # （我们直接指定 -p COM4 烧录，不用 discovery 找板，也不用 monitor）。
    # 这里建占位目录让 arduino-cli 判定为"已安装"，从而跳过下载。
    log("[3/3] placeholder builtin tools")
    try:
        bidx = json.load(open(os.path.join(DATA, "package_index.json"), encoding="utf-8"))
    except Exception as e:
        bidx = {"tools": []}
        log("      (读 Arduino index 失败: %s)" % e)
    for t in bidx.get("tools", []):
        if not t["name"].startswith("builtin:"):
            continue
        n, v = t["name"], t["version"]
        p = os.path.join(PKG, "builtin", "tools", n.split(":", 1)[1], v)
        if os.path.isdir(p) and os.listdir(p):
            continue
        os.makedirs(p, exist_ok=True)
        with open(os.path.join(p, "PLACEHOLDER.txt"), "w", encoding="utf-8") as f:
            f.write("占位：本项目通过 -p COM4 直接烧录，不使用该 discovery/monitor 工具。\n"
                    "如需使用 arduino-cli monitor，请删除本目录后重新安装该工具。\n")
        log("      placeholder %s@%s" % (n, v))

    log("\nDONE. 接下来运行 arduino-cli compile。")


if __name__ == "__main__":
    main()
