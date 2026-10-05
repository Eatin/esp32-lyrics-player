#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uia_probe.py — 用 Windows UI Automation 扒开酷狗窗口，看歌词文字能不能被读出来

用途：如果 SMTC（系统媒体控制）读不到酷狗，就试试这条路——
      酷狗自带「桌面歌词」，如果那个窗口的文字能通过 UIA 读到，
      我们就能直接把当前歌词行中继给 ESP32（连时间轴都不用自己算）。
"""
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

import uiautomation as auto  # noqa: E402

MAX_DEPTH = int(sys.argv[1]) if len(sys.argv) > 1 else 6


def safe(fn, default=""):
    try:
        return fn()
    except Exception:
        return default


def dump(c, depth, out):
    if depth > MAX_DEPTH:
        return
    ctype = safe(lambda: c.ControlTypeName, "?")
    cls = safe(lambda: c.ClassName, "")
    name = safe(lambda: c.Name, "")
    aid = safe(lambda: c.AutomationId, "")
    if name or aid or cls:
        out.append("  " * depth + f"[{ctype}] cls={cls!r} id={aid!r} name={name!r}")
    if depth == MAX_DEPTH:
        return
    try:
        children = c.GetChildren()
    except Exception:
        return
    for ch in children:
        dump(ch, depth + 1, out)


def main():
    root = auto.GetRootControl()
    try:
        tops = root.GetChildren()
    except Exception as e:
        print("拿不到顶层窗口:", e)
        return 1

    print(f"顶层窗口数: {len(tops)}")
    targets = []
    for w in tops:
        cls = safe(lambda: w.ClassName, "")
        nm = safe(lambda: w.Name, "")
        pid = safe(lambda: w.ProcessId, 0)
        mark = "  <== 酷狗" if "kugou" in (cls + nm).lower() else ""
        print(f"  pid={pid:<7} cls={cls!r} name={nm!r}{mark}")
        if "kugou" in (cls + nm).lower():
            targets.append(w)

    if not targets:
        print("\n⚠ 没找到名字里带 kugou 的顶层窗口。")
        print("  酷狗可能把歌词窗口设成了不可见/工具窗口，试试扩大匹配。")
        return 2

    for w in targets:
        print("\n" + "=" * 70)
        print(f"展开窗口: cls={safe(lambda: w.ClassName, '')!r} name={safe(lambda: w.Name, '')!r}")
        print("=" * 70)
        out = []
        dump(w, 0, out)
        for line in out[:120]:
            print(line)
        if len(out) > 120:
            print(f"  ... 还有 {len(out) - 120} 行")
    return 0


if __name__ == "__main__":
    sys.exit(main())
