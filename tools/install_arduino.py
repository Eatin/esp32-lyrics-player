#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把已下载（或经 GitHub 代理镜像下载）的包安装到 arduino-cli 的目录结构。

只安装 ESP32-S3 编译/烧录**必需**的包，跳过 RISC-V 工具链、gdb 等用不到的大包。
"""
import json, os, sys, zipfile, shutil, time, subprocess

IDX = r"D:\wb\wave\tools\pkg_esp32_index.json"
DATA = r"C:\Users\Eatin\AppData\Local\Arduino15"
PKG = os.path.join(DATA, "packages")
STAGE = os.path.join(DATA, "staging", "packages")
DL_CANDIDATES = [r"D:\wb\wave", r"D:\wb\wave\tools\dl"]
HW = os.path.join(PKG, "esp32", "hardware", "esp32", "3.3.0")
TOOLS = os.path.join(PKG, "esp32", "tools")
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"
PROXIES = ["https://ghproxy.net/", "https://ghfast.top/", "https://gh-proxy.com/"]

# 必需包 -> (已下载文件名, index 中的工具名)
REQUIRED = {
    "esp-x32": "xtensa-esp32-elf-12.2.0_20230208-x86_64-w64-mingw32.zip",
    "esp32-arduino-libs": "esp32-arduino-libs-3.0.1.zip",
    "esptool_py": "esptool-3.3-windows.zip",
}
# 可选但体积小，装上省得 arduino-cli 报缺工具
OPTIONAL_SMALL = ["openocd-esp32", "mkspiffs", "mklittlefs"]
# 明确跳过（用不到，且很大）
SKIP = ["esp-rv32", "riscv32-esp-elf-gdb", "xtensa-esp-elf-gdb", "dfu-util",
        "esp32-arduino-libs-dfu", "riscv32-esp-elf-gcc"]


def log(*a):
    print(*a, flush=True)


def human(n):
    return "%.1f MB" % (n / 1048576.0)


def find_local(fn):
    """在候选目录里找已下载的包（用户可能放在项目根目录）"""
    if not fn:
        return None
    for d in DL_CANDIDATES:
        q = os.path.join(d, fn)
        if os.path.exists(q):
            return q
    return None


def fetch_via_proxy(url, dest, tries=5):
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        return dest
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    for i in range(1, tries + 1):
        proxy = PROXIES[(i - 1) % len(PROXIES)]
        rc = subprocess.call(["curl", "-sSL", "--fail", "--connect-timeout", "20",
                              "--max-time", "900", "-A", UA, "-o", dest, proxy + url],
                             stderr=subprocess.DEVNULL)
        if rc == 0 and os.path.getsize(dest) > 0:
            log("      代理下载成功 %s" % human(os.path.getsize(dest)))
            return dest
        time.sleep(2)
    return None


def extract_flat(zpath, target):
    if os.path.exists(os.path.join(target, "platform.txt")) and zpath.endswith("3.3.0.zip"):
        return
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
            with z.open(n) as s, open(dst, "wb") as o:
                shutil.copyfileobj(s, o)
    log("      -> %s (%d files)" % (target, sum(len(f) for _, _, f in os.walk(target))))


def ensure_builtin_placeholders():
    """arduino-cli 初始化会强制安装 builtin:dfu/serial/mdns-discovery 与 serial-monitor。
    它们走极慢的 Arduino CDN，本项目用不到（直接 -p COM4 烧录），建占位目录跳过下载。"""
    try:
        bidx = json.load(open(os.path.join(DATA, "package_index.json"), encoding="utf-8"))
    except Exception:
        return
    for t in bidx.get("tools", []):
        if not t["name"].startswith("builtin:"):
            continue
        n, v = t["name"], t["version"]
        p = os.path.join(PKG, "builtin", "tools", n.split(":", 1)[1], v)
        if os.path.isdir(p) and os.listdir(p):
            continue
        os.makedirs(p, exist_ok=True)
        with open(os.path.join(p, "PLACEHOLDER.txt"), "w", encoding="utf-8") as f:
            f.write("占位：本项目用 -p COMX 直接烧录，不需要该 discovery/monitor 工具。\n"
                    "如需 arduino-cli monitor，请删除本目录后重装该工具。\n")
        log("      placeholder %s@%s" % (n, v))


def main():
    d = json.load(open(IDX, encoding="utf-8"))
    esp = d["packages"][0]
    plat = [p for p in esp["platforms"] if p["version"] == "3.3.0"][0]
    toolidx = {}
    for t in esp["tools"]:
        for s in t["systems"]:
            toolidx[(t["name"], s["host"])] = s
    HOSTS = ("x86_64-pc-mingw32", "x86_64-mingw32", "i686-mingw32", "all")

    # 1) 平台本体
    log("[1] 平台 esp32:esp32@3.3.0")
    zp = os.path.join(STAGE, "esp32-3.3.0.zip")
    src = find_local("esp32-3.3.0.zip")
    if not os.path.exists(zp):
        if src:
            os.makedirs(STAGE, exist_ok=True)
            shutil.copy2(src, zp)
        else:
            fetch_via_proxy(plat["url"], zp)
    extract_flat(zp, HW)
    log("      platform.txt %s" % ("OK" if os.path.exists(os.path.join(HW, "platform.txt")) else "MISSING"))

    # 2) 必需工具
    log("[2] 必需工具")
    for dep in plat["toolsDependencies"]:
        name, ver = dep["name"], dep["version"]
        if name in SKIP:
            log("  - %-24s %-26s 跳过（ESP32-S3 用不到）" % (name, ver))
            continue
        target = os.path.join(TOOLS, name, ver)
        if os.path.isdir(target) and os.listdir(target):
            log("  = %-24s %-26s 已安装" % (name, ver))
            continue
        host = next((h for h in HOSTS if (name, h) in toolidx), None)
        if not host:
            log("  - %-24s %-26s 本机无 Windows 构建" % (name, ver))
            continue
        s = toolidx[(name, host)]
        local = REQUIRED.get(name)
        log("  + %-24s %-26s %s" % (name, ver, human(int(s.get("size", 0)))))
        local_path = find_local(local) if local else None
        if local_path:
            src = local_path
            log("      使用已下载文件")
        else:
            fn = s["url"].split("/")[-1]
            src = os.path.join(STAGE, fn)
            if not fetch_via_proxy(s["url"], src):
                log("      !! 下载失败，跳过")
                continue
        extract_flat(src, target)

    # 3) 内置工具占位
    log("[3] 内置工具占位")
    ensure_builtin_placeholders()
    log("\nDONE")


if __name__ == "__main__":
    main()
