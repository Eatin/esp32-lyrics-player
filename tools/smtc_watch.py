#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
smtc_watch.py — 监视 Windows SMTC，看酷狗到底会不会汇报播放状态

用法：python smtc_watch.py [秒数]
用途：在酷狗点播放，观察这里是否出现会话 / 歌名 / 位置。
      全程没有输出 => 酷狗不支持 SMTC，需要换别的取状态方式。
"""
import json
import sys
import time

sys.path.insert(0, ".")
from kugou_bridge import Smtc  # noqa: E402


def main():
    dur = int(sys.argv[1]) if len(sys.argv) > 1 else 180
    s = Smtc()
    print(f"[watch] SMTC 绑定 = {s.backend}   (可用={s.available})", flush=True)
    if not s.available:
        return 1

    t0 = time.time()
    last = "___INIT___"
    seen_any = False

    while time.time() - t0 < dur:
        try:
            info = s.read(prefer_kugou=False)
        except Exception as e:
            info = None
            print("[watch] read error:", e, flush=True)

        if info:
            seen_any = True
            key = (info["app"], info["title"], info["artist"], info["state"])
            if key != last:
                last = key
                print(
                    time.strftime("[%H:%M:%S]") +
                    f" 会话出现 -> app={info['app']!r} title={info['title']!r} "
                    f"artist={info['artist']!r} dur={info['dur_ms']} pos={info['pos_ms']} "
                    f"state={info['state']}  all={info.get('sessions')}",
                    flush=True,
                )
            else:
                # 每秒报一次位置，确认位置是否在推进
                print(
                    time.strftime("[%H:%M:%S]") +
                    f"  pos={info['pos_ms']:>7}  state={info['state']}",
                    flush=True,
                )
        else:
            if last != "NONE":
                last = "NONE"
                print(time.strftime("[%H:%M:%S]") + " 无任何媒体会话", flush=True)
        time.sleep(1.0)

    print(f"[watch] 结束。整个过程中是否出现过会话: {seen_any}", flush=True)
    return 0 if seen_any else 2


if __name__ == "__main__":
    sys.exit(main())
