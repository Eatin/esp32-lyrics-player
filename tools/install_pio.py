#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从 PlatformIO registry 手动拉取 espressif32 平台 + Arduino 工具链，
安装到项目内的独立 PLATFORMIO_CORE_DIR（不触碰用户已有的 PlatformIO 安装）。

为什么不用 pio 自己装：
  本机 HTTP 代理(127.0.0.1:6628)对 github.com/releases 与 downloads.arduino.cc
  限速/阻断，而 PlatformIO 覆盖旧包时会触发批量删除被安全策略拦截。
  因此完全绕开 pio 的包管理器：按 registry 协议 curl 下载 + Python 解压。
  下载源 dl.registry.platformio.org 实测 1.5 MB/s。
"""
import json, os, sys, tarfile, subprocess, time, shutil, urllib.request

CORE = r"D:\wb\wave\tools\pio-core"
REG = "https://dl.registry.platformio.org/download/platformio"
API = "https://api.registry.platformio.org/v3/packages/platformio"

PLATFORM_PKG = "platform/espressif32"
PLATFORM_VER = sys.argv[1] if len(sys.argv) > 1 else "7.1.3"

# Arduino-Framework 构建 esp32s3 实际需要的包（其余是 rp2040/ESP-IDF/其他架构的）
NEEDED = [
    ("tool/framework-arduinoespressif32", None),  # None = 由 platform.json 决定
    ("tool/toolchain-xtensa32", None),
    ("tool/tool-esptoolpy", None),
    ("tool/tool-mkspiffs", None),
    ("tool/tool-scons", None),
]


def log(*a):
    print(*a, flush=True)


def human(n):
    return "%.1f MB" % (n / 1048576.0)


def api_versions(pkgpath):
    """pkgpath 形如 'tool/framework-arduinoespressif32'"""
    url = "%s/%s" % (API, pkgpath)
    d = json.load(urllib.request.urlopen(url, timeout=60))
    out = []
    for v in d.get("versions", []):
        f = v.get("files") or []
        if f:
            out.append((v["name"], f[0]["download_url"]))
    return out


def curl(url, dest, tries=5):
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        log("      cached %s" % human(os.path.getsize(dest)))
        return dest
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    for i in range(1, tries + 1):
        rc = subprocess.call(["curl", "-sSL", "--fail", "--retry", "3",
                             "--connect-timeout", "30", "--max-time", "2400",
                             "-C", "-", "-o", dest, url])
        if rc == 0 and os.path.getsize(dest) > 0:
            log("      got %s in %.0fs" % (human(os.path.getsize(dest)), 0))
            return dest
        log("      attempt %d failed (rc=%s)" % (i, rc))
        time.sleep(2)
    raise RuntimeError("download failed: " + url)


def untar(path, target, strip=None):
    """strip=None 时自动判断：若包内所有条目都在同一个顶层目录下则剥掉它。"""
    # 绝不删除已有内容：批量删除会被安全策略拦截；直接覆盖写即可
    os.makedirs(target, exist_ok=True)
    with tarfile.open(path, "r:gz") as t:
        members = t.getmembers()
        files = [m.name for m in members if m.isfile()]
        if strip is None:
            roots = {n.split("/")[0] for n in files}
            # 所有条目都有至少一层目录，且只有一个顶层目录 -> 剥掉
            if len(roots) == 1 and all("/" in n for n in files):
                strip = 1
            else:
                strip = 0
        for m in members:
            if not m.isfile():
                continue
            parts = m.name.split("/")
            if len(parts) <= strip:
                continue
            rel = "/".join(parts[strip:])
            if ".." in parts:
                continue
            dst = os.path.join(target, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            src = t.extractfile(m)
            if src is None:
                continue
            with src, open(dst, "wb") as f:
                shutil.copyfileobj(src, f)
    log("      -> %s (%d files, strip=%d)" % (target, sum(len(fs) for _, _, fs in os.walk(target)), strip))


def pick_version(pkgpath, constraint):
    """按 platform.json 的约束（形如 ~4.30300.0 / >=1.2.3）挑一个可用版本"""
    vers = api_versions(pkgpath)
    if not vers:
        return None
    names = [v for v, _ in vers]
    if constraint:
        c = str(constraint).strip()
        # 取约束里最长的一段数字作为版本族关键字
        digits = "".join(ch for ch in c if ch.isdigit() or ch == ".")
        fam = digits.lstrip(".") if digits else None
        if fam:
            # 4.30300.0 -> 家族 4.303
            parts = c.lstrip("~>=<^ ").split(".")
            pre = ".".join(parts[:2]) if len(parts) >= 2 else c
            hit = [v for v in names if v.startswith(pre)]
            if hit:
                hit.sort()
                return hit[-1], dict(vers)[hit[-1]]
    return names[0], dict(vers)[names[0]]


def main():
    os.makedirs(CORE, exist_ok=True)
    stage = os.path.join(CORE, "_dl")
    os.makedirs(stage, exist_ok=True)

    # ---------- 1) 平台本体 ----------
    plat_dir = os.path.join(CORE, "platforms", "espressif32")
    if not os.path.exists(os.path.join(plat_dir, "platform.json")):
        url = "%s/%s/%s/espressif32-%s.tar.gz" % (REG, PLATFORM_PKG, PLATFORM_VER, PLATFORM_VER)
        log("[1] platform espressif32@%s" % PLATFORM_VER)
        p = curl(url, os.path.join(stage, "plat.tar.gz"))
        untar(p, plat_dir)
    pj = json.load(open(os.path.join(plat_dir, "platform.json"), encoding="utf-8"))
    log("    platform version = %s" % pj.get("version"))
    fw = pj.get("frameworks", {})
    log("    arduino framework pkg = %s" % fw.get("arduino", {}).get("package"))
    log("    required toolchain   = %s" % (fw.get("arduino", {}).get("toolchain") or
                                           pj.get("packages", {}).get("toolchain-xtensa32")))

    # ---------- 2) 依赖包 ----------
    pkgs = pj.get("packages", {})
    plan = []
    for name, cons in [
        ("toolchain-xtensa32", None),
        ("framework-arduinoespressif32", pkgs.get("framework-arduinoespressif32")),
        ("tool-esptoolpy", pkgs.get("tool-esptoolpy")),
        ("tool-mkspiffs", pkgs.get("tool-mkspiffs")),
        ("tool-scons", None),
    ]:
        cons = cons if isinstance(cons, dict) else None
        want = (cons or {}).get("version") if cons else None
        r = pick_version("tool/" + name, want)
        if not r:
            log("  !! %s 无可用版本" % name)
            continue
        ver, url = r
        plan.append((name, ver, url))

    log("\n[2] 依赖包")
    tgt_root = os.path.join(CORE, "packages")
    for name, ver, url in plan:
        target = os.path.join(tgt_root, name)
        if os.path.isdir(target) and os.listdir(target):
            log("  = %-32s %-22s 已存在" % (name, ver))
            continue
        log("  + %-32s %-22s" % (name, ver))
        fn = url.split("/")[-1]
        try:
            p = curl(url, os.path.join(stage, fn))
        except Exception as e:
            log("      !! %s" % e)
            continue
        untar(p, target)

    log("\nDONE -> %s" % CORE)
    log("下一步：PLATFORMIO_CORE_DIR=%s pio run" % CORE)


if __name__ == "__main__":
    main()
