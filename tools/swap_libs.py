"""
把正确的 IDF 5.5 预编译库安装到 arduino-cli 期望的位置。

背景：之前误装了 idf-release_v5.1-442a798083（Arduino core 3.0.1 的库），
缺少 driver/i2c_master.h。core 3.3.0 需要 idf-release_v5.5-b66b5448-v1。

策略（不做批量删除）：
  1. 把现有同名目录重命名为 ..._WRONG_v5.1 备份
  2. 解压正确的 zip 到全新目录 esp32-arduino-libs/idf-release_v5.5-b66b5448-v1/
"""
import os, sys, zipfile, shutil, time

ZIP = r"D:\wb\wave\esp32-arduino-libs-idf-release_v5.5-b66b5448-v1.zip"
BASE = r"C:\Users\Eatin\AppData\Local\Arduino15\packages\esp32\tools\esp32-arduino-libs"
WANT = "idf-release_v5.5-b66b5448-v1"
DST = os.path.join(BASE, WANT)


def log(m):
    print(m, flush=True)


def main():
    if not os.path.exists(ZIP):
        log("!! 找不到 zip: %s" % ZIP)
        return 1

    # 1) 备份旧的错误目录
    if os.path.isdir(DST):
        bak = os.path.join(BASE, WANT + "_WRONG_v5.1")
        if os.path.isdir(bak):
            log("备份目录已存在，跳过重命名: %s" % bak)
        else:
            os.rename(DST, bak)
            log("已重命名旧目录 -> %s" % bak)

    # 2) 探测 zip 内公共前缀
    z = zipfile.ZipFile(ZIP)
    names = [n for n in z.namelist() if not n.endswith("/")]
    root = None
    first = names[0].split("/")[0] if names else None
    if first and all(n.startswith(first + "/") for n in names):
        root = first
    log("zip 条目 %d，内部根目录 = %r" % (len(names), root))

    # 3) 解压
    os.makedirs(DST, exist_ok=True)
    t0 = time.time()
    n = 0
    for m in z.infolist():
        if m.is_dir():
            continue
        rel = m.filename
        if root:
            rel = rel[len(root) + 1:]
        if not rel:
            continue
        parts = rel.split("/")
        if ".." in parts:
            continue
        out = os.path.join(DST, *parts)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with z.open(m) as src, open(out, "wb") as f:
            shutil.copyfileobj(src, f)
        n += 1
    log("解压 %d 个文件 -> %s (%.0fs)" % (n, DST, time.time() - t0))

    # 4) 校验关键头文件
    key = os.path.join(DST, "esp32s3", "include", "esp_driver_i2c", "include", "driver", "i2c_master.h")
    log("关键头文件 i2c_master.h 存在: %s" % os.path.exists(key))
    s3 = os.path.join(DST, "esp32s3", "bin")
    log("esp32s3/bin 存在: %s" % os.path.isdir(s3))
    vf = os.path.join(DST, "versions.txt")
    if os.path.exists(vf):
        log("--- versions.txt ---")
        log(open(vf, encoding="utf-8", errors="ignore").read()[:400])
    return 0


if __name__ == "__main__":
    sys.exit(main())
