#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uia_probe2.py — 定位「歌名 / 歌手 / 进度」在酷狗 UI 树里的稳定位置

思路：先用正则找到进度控件 (如 00:39/04:49)，再往上找它所在的「正在播放」面板，
      看看这个面板里同时还有哪些文本控件 —— 这样定位最稳，不依赖控件顺序。
"""
import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

import uiautomation as auto  # noqa: E402

TIME_RE = re.compile(r"^\s*(\d{1,3}):(\d{2})\s*/\s*(\d{1,3}):(\d{2})\s*$")


def nm(c):
    try:
        return c.Name or ""
    except Exception:
        return ""


def ctn(c):
    try:
        return c.ControlTypeName
    except Exception:
        return "?"


def all_controls(root, maxdepth=12):
    out = []

    def rec(c, d):
        if d > maxdepth:
            return
        out.append(c)
        try:
            for ch in c.GetChildren():
                rec(ch, d + 1)
        except Exception:
            pass

    rec(root, 0)
    return out


def main():
    root = auto.GetRootControl()
    kugou_wins = []
    for w in root.GetChildren():
        try:
            cls = w.ClassName or ""
            name = w.Name or ""
        except Exception:
            continue
        if "kugou" in cls.lower() and "桌面歌词" not in name:
            kugou_wins.append(w)

    if not kugou_wins:
        print("没找到酷狗主窗口")
        return 2

    win = kugou_wins[0]
    ctrls = all_controls(win)

    # 1) 找进度控件
    time_ctrl = None
    for c in ctrls:
        if ctn(c) in ("TextControl", "ListItemControl") and TIME_RE.match(nm(c)):
            time_ctrl = c
            break
    if not time_ctrl:
        print("没找到进度控件（酷狗可能没在播放）")
        return 3

    print(f"进度控件: [{ctn(time_ctrl)}] name={nm(time_ctrl)!r}")

    # 2) 往上找 4 层祖先，看每层里有什么文本
    node = time_ctrl
    for lvl in range(1, 5):
        try:
            node = node.GetParentControl()
        except Exception:
            break
        if node is None:
            break
        sub = all_controls(node, 6)
        texts = [nm(x) for x in sub if ctn(x) in ("TextControl", "HyperlinkControl")]
        texts = [t for t in texts if t]
        print(f"\n--- 祖先 #{lvl}: [{ctn(node)}] name={nm(node)!r}  共{len(sub)}个控件 ---")
        seen = []
        for t in texts:
            if t not in seen:
                seen.append(t)
        for t in seen[:25]:
            print("   文本:", repr(t))

        # 3) 逐层判断：这一层能否同时覆盖 歌名/歌手/时间
        if time_ctrl in sub:
            others = [x for x in sub if x is not time_ctrl and ctn(x) == "HyperlinkControl"]
            print(f"   >> 本层超链接({len(others)}个): {[nm(x) for x in others][:6]}")

    # 4) 顺便看看「正在播放」面板里所有按钮，用于判断播放/暂停
    print("\n--- 全窗口里与播放控制有关的按钮 ---")
    for c in ctrls:
        if ctn(c) == "ButtonControl" and nm(c) in ("播放", "暂停", "上一首", "下一首"):
            try:
                r = c.BoundingRectangle
                print(f"   Button {nm(c)!r}  rect=({r.left},{r.top},{r.right},{r.bottom})")
            except Exception:
                print(f"   Button {nm(c)!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
