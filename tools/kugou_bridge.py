#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
kugou_bridge.py — 电脑端「歌词桥」

作用：把 **电脑上酷狗正在播放的歌 + 歌词 + 播放进度** 实时推送到 ESP32 歌词机。
      ESP32 完全不碰音频，只当一块歌词屏；声音还是从电脑音响出。

原理：
  1. 用 Windows SMTC（系统媒体控制）读到酷狗的 歌名 / 歌手 / 时长 / 播放位置 / 播放状态
     —— 这是微软官方接口，不需要去猜酷狗的窗口或注入进程
  2. 换歌时，用酷狗歌词接口按「文件 hash」精确匹配官方 LRC（拿不到就用网易云兜底）
  3. HTTP 推给板子：
        POST /api/ext/track?dur=&title=&artist=     body = LRC 原文
        POST /api/ext/pos?p=&s=                     每 0.5 秒一次
  4. 启动时先广播 UDP 48899 自动发现板子 IP，不用手填

依赖：
    pip install winsdk
    （若 winsdk 装不上，可改用模块化包：pip install winrt-runtime winrt-Windows.Media.Control
      winrt-Windows.Foundation winrt-Windows.Foundation.Collections winrt-Windows.Storage.Streams）

用法：
    python kugou_bridge.py                # 自动发现板子，开始桥接
    python kugou_bridge.py --host 192.168.1.23
    python kugou_bridge.py --probe        # 只打印 SMTC 能看到什么（排查用，不连板子）
    python kugou_bridge.py --dry-run      # 不推送，只打印将要推送的内容
"""

import argparse
import base64
import json
import os
import re
import socket
import ssl
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

# --------------------------------------------------------------------------
# 控制台输出（Windows 控制台默认 GBK，中文会乱码）
# --------------------------------------------------------------------------
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def log(*a):
    print(time.strftime("[%H:%M:%S]"), *a, flush=True)


def unsupported_glyphs(*texts):
    """找出板子字库**没有**的字，在推送前就在电脑端报出来。

    板子字库是自研的 GB2312 全集 + 手工补充的标点/符号（共 7840 字形）。
    清单由 tools/gen_font.py 生成时导出到 tools/board_charset.txt ——
    不能简单用「能否 GB2312 编码」代替，因为 · (U+00B7)、— (U+2014)、
    ♪ 这些都不在 GB2312 里，但已经手工补进字库了。
    找不到清单时退回 GB2312 近似判断（会有少量误报）。

    板子上缺字会渲染成方框（LVGL 的 LV_USE_FONT_PLACEHOLDER）。
    返回 {字: 出现次数}。
    """
    global _BOARD_CHARSET
    if _BOARD_CHARSET is None:
        try:
            with open(CHARSET_TXT, encoding="utf-8") as f:
                _BOARD_CHARSET = set(f.read())
            log("板子字库字符清单已加载: %d 字（%s）" % (len(_BOARD_CHARSET), CHARSET_TXT))
        except OSError:
            _BOARD_CHARSET = False
            log("⚠ 没找到 board_charset.txt，改用 GB2312 近似判断缺字（可能有误报）")

    bad = {}
    for t in texts:
        if not t:
            continue
        for ch in t:
            if ord(ch) < 0x80:
                continue
            if _BOARD_CHARSET:
                ok = ch in _BOARD_CHARSET
            else:
                try:
                    ch.encode("gb2312")
                    ok = True
                except UnicodeEncodeError:
                    ok = False
            if not ok:
                bad[ch] = bad.get(ch, 0) + 1
    return bad


UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}

# 板子字库的字符清单（由 gen_font.py 生成时导出），用于推送前的缺字预警
CHARSET_TXT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "board_charset.txt")
_BOARD_CHARSET = None       # None=未加载；set=清单；False=没有清单，退回 GB2312 近似

BOARD_PORT = 80
UDP_PORT = 48899
CONF_NAME = "bridge_config.json"
CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lyrics_cache")
COVER_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "covers")

# 封面边长，必须与板端 XL_COVER_PX (ext_link.h) / COVER_PX (user_config.h) 一致
COVER_PX = 156

_SSL_CTX = ssl.create_default_context()
_SSL_CTX.check_hostname = False
_SSL_CTX.verify_mode = ssl.CERT_NONE          # 部分歌词接口证书链不全，跳过校验


# ==========================================================================
# 一、读 Windows SMTC（酷狗在放什么）
# ==========================================================================

class Smtc:
    """包装 winsdk / winrt-runtime 两种绑定，对外只暴露 read()"""

    def __init__(self):
        self.backend = None
        self.Mgr = None
        self.St = None
        self._load()

    def _load(self):
        try:
            from winsdk.windows.media.control import (
                GlobalSystemMediaTransportControlsSessionManager as Mgr,
                GlobalSystemMediaTransportControlsSessionPlaybackStatus as St,
            )
            self.Mgr, self.St, self.backend = Mgr, St, "winsdk"
            return
        except Exception:
            pass
        try:
            from winrt.windows.media.control import (
                GlobalSystemMediaTransportControlsSessionManager as Mgr,
                GlobalSystemMediaTransportControlsSessionPlaybackStatus as St,
            )
            self.Mgr, self.St, self.backend = Mgr, St, "winrt"
            return
        except Exception:
            pass

    @property
    def available(self):
        return self.Mgr is not None

    async def _read_async(self, prefer_kugou=True):
        mgr = await self.Mgr.request_async()
        sessions = list(mgr.get_sessions())
        if not sessions:
            return None

        sess = None
        if prefer_kugou:
            for s in sessions:
                if "kugou" in (s.source_app_user_model_id or "").lower():
                    sess = s
                    break
        if sess is None:
            # 没有酷狗就退而求其次：挑第一个"正在播放"的
            for s in sessions:
                try:
                    if s.get_playback_info().playback_status == self.St.PLAYING:
                        sess = s
                        break
                except Exception:
                    pass
        if sess is None:
            sess = sessions[0]

        try:
            props = await sess.try_get_media_properties_async()
        except Exception:
            props = None
        try:
            tl = sess.get_timeline_properties()
        except Exception:
            tl = None
        try:
            pb = sess.get_playback_info()
        except Exception:
            pb = None

        status = getattr(pb, "playback_status", None)
        try:
            playing = (status == self.St.PLAYING)
            paused = (status == self.St.PAUSED)
        except Exception:
            playing = paused = False
        state = 1 if playing else (2 if paused else 0)

        def secs(td):
            try:
                return td.total_seconds()
            except Exception:
                return 0.0

        pos_ms = secs(getattr(tl, "position", 0)) * 1000.0 if tl else 0.0
        end_ms = secs(getattr(tl, "end_time", 0)) * 1000.0 if tl else 0.0
        start_ms = secs(getattr(tl, "start_time", 0)) * 1000.0 if tl else 0.0
        dur_ms = max(0.0, end_ms - start_ms)

        # 有些播放器只在状态变化时才更新时间轴，位置会"停"在一个旧值上。
        # 用 LastUpdatedTime 把位置推进到"此刻"，歌词才对得准。
        if playing and tl is not None:
            lut = getattr(tl, "last_updated_time", None)
            if lut is not None:
                try:
                    now = _utcnow()
                    delta = (now - lut).total_seconds()
                    if 0 <= delta <= 5.0:
                        pos_ms += delta * 1000.0
                except Exception:
                    pass

        if dur_ms > 0 and pos_ms > dur_ms:
            pos_ms = dur_ms

        return {
            "app": sess.source_app_user_model_id or "",
            "title": (getattr(props, "title", "") or "").strip() if props else "",
            "artist": (getattr(props, "artist", "") or "").strip() if props else "",
            "album": (getattr(props, "album_title", "") or "").strip() if props else "",
            "dur_ms": int(dur_ms),
            "pos_ms": int(pos_ms),
            "state": state,
            "sessions": [
                f"{s.source_app_user_model_id}"
                for s in sessions
            ],
        }

    def read(self, prefer_kugou=True):
        """同步读一次；失败返回 None"""
        if not self.available:
            return None
        import asyncio
        try:
            return asyncio.run(self._read_async(prefer_kugou))
        except Exception as e:
            log("SMTC 读取失败:", e)
            return None


def _utcnow():
    from datetime import datetime, timezone
    return datetime.now(timezone.utc)


# ==========================================================================
# 一·B、用 UI Automation 直接读酷狗界面（位置只有这里能拿到）
# ==========================================================================

class UiaKugou:
    """
    为什么不能用 SMTC 拿位置？—— 实测（2026 酷狗 PC 版）：
    酷狗确实会注册一个 SMTC 会话，能读到歌名/歌手/播放状态，
    但 **Position 和 Duration 永远是 0**，它不提供时间轴。
    而酷狗主界面上那个 '00:39/04:49' 文本控件是实时更新的 ——
    所以「唱到第几秒」只能从 UI Automation 树上读。

    性能：遍历整棵 UI 树要 1~3 秒，所以只做一次「定位」，
    之后每轮只读 3~4 个已缓存控件的 Name（毫秒级）。
    """

    TIME_RE = re.compile(r"^\s*(\d{1,3}):(\d{2})\s*/\s*(\d{1,3}):(\d{2})\s*$")

    def __init__(self):
        self.auto = None
        self.err = None
        try:
            import uiautomation as auto
            self.auto = auto
        except Exception as e:
            self.err = e
        self._time = None
        self._title = None
        self._artist = None
        self._btn = None
        self._resolved_at = 0.0
        self._last_text_ms = -1
        self._anchor_wall = 0.0

    @property
    def available(self):
        return self.auto is not None

    # ---------------- 内部 ----------------
    @staticmethod
    def _walk(c, maxd, out, d=0):
        if d > maxd:
            return
        out.append(c)
        try:
            for ch in c.GetChildren():
                UiaKugou._walk(ch, maxd, out, d + 1)
        except Exception:
            pass

    def _main_window(self):
        """酷狗主窗口：ClassName 含 kugou，且不是「桌面歌词」那个"""
        try:
            for w in self.auto.GetRootControl().GetChildren():
                try:
                    cls = w.ClassName or ""
                    nm = w.Name or ""
                except Exception:
                    continue
                if "kugou" in cls.lower() and "桌面歌词" not in nm:
                    return w
        except Exception:
            pass
        return None

    def resolve(self):
        """定位关键控件（慢，1~3 秒）；成功返回 True"""
        if not self.available:
            return False
        win = self._main_window()
        if win is None:
            return False

        ctrls = []
        self._walk(win, 14, ctrls)

        # 1) 进度文本 '00:39/04:49'
        tc = None
        for c in ctrls:
            try:
                if c.ControlTypeName == "TextControl" and self.TIME_RE.match(c.Name or ""):
                    tc = c
                    break
            except Exception:
                continue
        if tc is None:
            return False
        self._time = tc

        # 2) 歌名 / 歌手：进度控件往上两层就是「正在播放」面板，
        #    里面的超链接依次是 歌名、歌手
        links = []
        try:
            panel = tc.GetParentControl()
            if panel is not None:
                panel = panel.GetParentControl()
            sub = []
            if panel is not None:
                self._walk(panel, 6, sub)
            for c in sub:
                try:
                    if c.ControlTypeName != "HyperlinkControl":
                        continue
                    n = (c.Name or "").strip()
                except Exception:
                    continue
                if not n or any(k in n for k in ("来源", "我的频道", "更多", "开通", "http")):
                    continue
                links.append(c)
        except Exception:
            pass
        self._title = links[0] if len(links) >= 1 else None
        self._artist = links[1] if len(links) >= 2 else None

        # 3) 播放/暂停按钮：按钮写着「暂停」= 正在播；写着「播放」= 已暂停
        btn = None
        for c in ctrls:
            try:
                if c.ControlTypeName == "ButtonControl" and (c.Name or "") in ("暂停", "播放"):
                    btn = c
                    break
            except Exception:
                continue
        self._btn = btn

        self._resolved_at = time.time()
        return True

    def read(self):
        if not self.available:
            return None
        need = (self._time is None) or (self._title is None) or \
               (time.time() - self._resolved_at > 30)
        if need and not self.resolve():
            return None

        try:
            name = self._time.Name or ""
        except Exception:
            self._time = None
            return None
        m = self.TIME_RE.match(name)
        if not m:
            self._time = None            # 界面变了，下轮重新定位
            return None

        pos_ms = (int(m.group(1)) * 60 + int(m.group(2))) * 1000
        dur_ms = (int(m.group(3)) * 60 + int(m.group(4))) * 1000

        title = artist = ""
        try:
            if self._title is not None:
                title = (self._title.Name or "").strip()
        except Exception:
            self._title = None
        try:
            if self._artist is not None:
                artist = (self._artist.Name or "").strip()
        except Exception:
            self._artist = None

        state = 0
        try:
            bn = (self._btn.Name or "") if self._btn is not None else ""
            if bn == "暂停":
                state = 1          # 按钮提示"暂停" => 当前正在播放
            elif bn == "播放":
                state = 2          # 按钮提示"播放" => 当前已暂停
        except Exception:
            self._btn = None
        if state == 0 and self._btn is None:
            state = 1 if pos_ms > 0 else 0

        # 进度文本 1 秒才跳一格；用挂钟把它补成连续值，歌词才不会一顿一顿
        if pos_ms != self._last_text_ms:
            self._last_text_ms = pos_ms
            self._anchor_wall = time.time()
        if state == 1:
            pos_ms = int(pos_ms + (time.time() - self._anchor_wall) * 1000)
            if dur_ms and pos_ms > dur_ms:
                pos_ms = dur_ms

        return {
            "app": "KuGou.exe (UIA)",
            "title": title,
            "artist": artist,
            "album": "",
            "dur_ms": dur_ms,
            "pos_ms": pos_ms,
            "state": state,
            "sessions": [],
        }


class Reader:
    """统一读取入口：UIA 为主（唯一能拿到位置的），SMTC 兜底"""

    def __init__(self, use_smtc=True):
        self.uia = UiaKugou()
        self.smtc = Smtc() if use_smtc else None

    def describe(self):
        s = f"UIA={'OK' if self.uia.available else '不可用'}"
        if self.smtc:
            s += f", SMTC={self.smtc.backend or '不可用'}"
        return s

    def read(self):
        u = self.uia.read() if self.uia.available else None
        if u and (u["title"] or u["pos_ms"] > 0):
            return u

        s = self.smtc.read() if (self.smtc and self.smtc.available) else None
        if s:
            s["app"] = s.get("app", "") + " (SMTC)"
            if not s.get("state"):
                s["state"] = 1 if s.get("title") else 0
            return s
        return u


# ==========================================================================
# 二、抓歌词（酷狗优先，网易云兜底）
# ==========================================================================

def http_json(url, timeout=15):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=timeout, context=_SSL_CTX) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def norm(s):
    """规范化歌名用于比对：去掉空格/标点/大小写差异"""
    if not s:
        return ""
    s = s.lower()
    s = re.sub(r"[\s\-_.,，。、'\"()（）\[\]【】!！?？~·]", "", s)
    return s


def kugou_find_song(title, artist, dur_ms):
    """搜歌，返回评分最高的那一条（含 hash / album_id / duration / album_name）

    拿 hash 去匹配歌词比用歌名关键词准得多；album_id 用来取专辑封面。
    """
    kw = (title + " " + artist).strip()
    if not kw:
        return None
    url = "https://mobiles.kugou.com/api/v3/search/song?" + urllib.parse.urlencode(
        {"keyword": kw, "page": 1, "pagesize": 10, "showtype": 1})
    d = http_json(url)
    info = ((d.get("data") or {}).get("info")) or []
    if not info:
        return None

    nt, na = norm(title), norm(artist)
    best, best_score = None, -1
    for it in info:
        sn = it.get("songname") or ""
        singers = it.get("singername") or ""
        if isinstance(it.get("singer"), list):
            singers = "、".join(x.get("name", "") for x in it["singer"])
        score = 0
        if norm(sn) == nt:
            score += 10
        elif nt and nt in norm(sn):
            score += 5
        if na and (na in norm(singers) or norm(singers) in na):
            score += 6
        # 时长越接近加分（SMTC 的时长和歌曲库时长一般差 <2s）
        sd = it.get("duration") or 0
        if dur_ms and sd:
            try:
                delta = abs(int(sd) * 1000 - int(dur_ms)) / 1000.0
                if delta < 3:
                    score += 6
                elif delta < 10:
                    score += 2
            except Exception:
                pass
        if score > best_score:
            best, best_score = it, score
    return best


def kugou_find_hash(title, artist, dur_ms):
    """搜歌拿文件 hash（兼容旧调用）"""
    it = kugou_find_song(title, artist, dur_ms)
    if not it:
        return None, 0
    return it.get("hash"), int(it.get("duration") or 0)


def kugou_cover_url(album_id, size=480):
    """专辑封面原图 URL（酷狗返回的 imgurl 自带 {size} 占位符）"""
    if not album_id:
        return None
    url = "https://mobilecdn.kugou.com/api/v3/album/info?" + urllib.parse.urlencode(
        {"albumid": album_id, "version": 9108})
    d = http_json(url)
    img = ((d.get("data") or {}).get("imgurl")) or ""
    if not img:
        return None
    return img.replace("{size}", str(size)).replace("http://", "https://", 1)


def kugou_lrc_by_hash(h, dur_sec):
    url = "https://krcs.kugou.com/search?" + urllib.parse.urlencode(
        {"ver": 1, "man": "yes", "client": "mobi", "hash": h or "",
         "duration": int(dur_sec * 1000) if dur_sec else ""})
    d = http_json(url)
    cands = d.get("candidates") or []
    if not cands:
        return None
    # 优先"官方推荐歌词"
    cands.sort(key=lambda c: 0 if "官方" in (c.get("product_from") or "") else 1)
    c = cands[0]
    u2 = "https://lyrics.kugou.com/download?" + urllib.parse.urlencode(
        {"ver": 1, "client": "pc", "id": c["id"], "accesskey": c["accesskey"],
         "fmt": "lrc", "charset": "utf8"})
    d2 = http_json(u2)
    content = d2.get("content")
    if not content:
        return None
    text = base64.b64decode(content).decode("utf-8", "replace")
    return text if text.strip() else None


def netease_lrc(title, artist, dur_ms):
    url = "https://music.163.com/api/search/get/web?" + urllib.parse.urlencode(
        {"s": (title + " " + artist).strip(), "type": 1, "offset": 0,
         "total": "true", "limit": 10})
    req = urllib.request.Request(url, headers=dict(UA, Referer="https://music.163.com/"))
    with urllib.request.urlopen(req, timeout=15, context=_SSL_CTX) as r:
        d = json.loads(r.read().decode("utf-8", "replace"))
    songs = ((d.get("result") or {}).get("songs")) or []
    if not songs:
        return None

    nt, na = norm(title), norm(artist)
    def score(s):
        v = 0
        if norm(s.get("name", "")) == nt:
            v += 10
        if na and any(na in norm(a.get("name", "")) for a in (s.get("artists") or [])):
            v += 6
        dd = s.get("duration") or 0
        if dur_ms and dd:
            try:
                if abs(dd - int(dur_ms)) / 1000.0 < 3:
                    v += 6
            except Exception:
                pass
        return v
    songs.sort(key=score, reverse=True)
    sid = songs[0]["id"]

    u2 = "https://music.163.com/api/song/lyric?" + urllib.parse.urlencode(
        {"id": sid, "lv": 1, "kv": 1, "tv": -1})
    req2 = urllib.request.Request(u2, headers=dict(UA, Referer="https://music.163.com/"))
    with urllib.request.urlopen(req2, timeout=15, context=_SSL_CTX) as r:
        d2 = json.loads(r.read().decode("utf-8", "replace"))
    lrc = ((d2.get("lrc") or {}).get("lyric")) or ""
    return lrc if lrc.strip() else None


_TAG_RE = re.compile(r"^\[\d{1,3}:\d{1,2}([.:]\d{1,3})?\]")


def _tagval(s, tag):
    """从 '[ti:xxx]' 取出 xxx；不是该标签就返回 None"""
    if not s.lower().startswith("[" + tag + ":"):
        return None
    return s[len(tag) + 2:-1].strip() if s.endswith("]") else ""


def ensure_tags(lrc, title, artist, album=""):
    """规范化 LRC：
       - 去掉 UTF-8 BOM 与 [id:$xxxx] 之类的 KRC 头
       - [ti:]/[ar:] 存在但**内容为空**时，用真实歌名/歌手补上（酷狗很常见）
    """
    lrc = lrc.lstrip("\ufeff")
    out = []
    got = {"ti": False, "ar": False, "al": False}

    for ln in lrc.splitlines():
        s = ln.strip()
        low = s.lower()

        hit = False
        for tag, val in (("ti", title), ("ar", artist), ("al", album)):
            v = _tagval(s, tag)
            if v is None:
                continue
            hit = True
            if v:
                got[tag] = True
                out.append(s)
            elif val:
                out.append(f"[{tag}:{val}]")
                got[tag] = True
            # v 为空且没有替代值 -> 直接丢掉
            break
        if hit:
            continue

        if low.startswith("[by:") or low.startswith("[offset:"):
            out.append(s)
            continue

        # 非时间轴、非已知元数据的方括号行（如 [id:$00000000]）一律丢弃
        if s.startswith("[") and not _TAG_RE.match(s):
            continue

        out.append(ln)

    head = []
    if not got["ti"] and title:
        head.append(f"[ti:{title}]")
    if not got["ar"] and artist:
        head.append(f"[ar:{artist}]")
    if not got["al"] and album:
        head.append(f"[al:{album}]")

    return "\n".join(head + out).strip() + "\n"


def _cover_cache_path(title, artist):
    key = re.sub(r"[^\w\u4e00-\u9fff]+", "_", f"{title}_{artist}")[:80]
    return os.path.join(COVER_DIR, key + ".rgb565")


def _to_rgb565_be(im, px):
    """缩放成 px*px，转成 RGB565「大端」字节流。

    为什么是「大端」：板端 lv_conf.h 里 LV_COLOR_16_SWAP=1，
    lv_color_t 在内存里就是 [高字节, 低字节]，正好等于 pack('>H', 标准RGB565)。
    顺便做 4x4 有序抖动，不然 5bit 的 R/B 在渐变封面上会有明显色带。
    """
    from PIL import Image
    im = im.convert("RGB").resize((px, px), Image.LANCZOS)
    src = im.tobytes()                       # RGBRGBRGB...
    out = bytearray(px * px * 2)

    bayer = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))
    i = 0
    for y in range(px):
        row = bayer[y & 3]
        for x in range(px):
            r = src[i * 3]
            g = src[i * 3 + 1]
            b = src[i * 3 + 2]
            t = row[x & 3] * 16 + 8          # 0..256，作为四舍五入阈值
            r5 = (r * 31 + t) // 255
            g6 = (g * 63 + t) // 255
            b5 = (b * 31 + t) // 255
            v = (min(31, r5) << 11) | (min(63, g6) << 5) | min(31, b5)
            out[i * 2]     = (v >> 8) & 0xFF
            out[i * 2 + 1] = v & 0xFF
            i += 1
    return bytes(out)


def fetch_cover(title, artist, dur_ms):
    """按歌名/歌手取专辑封面，返回 RGB565 字节流；拿不到返回 None。带磁盘缓存。"""
    if not title:
        return None

    path = _cover_cache_path(title, artist)
    if os.path.exists(path):
        try:
            with open(path, "rb") as f:
                d = f.read()
            if len(d) == COVER_PX * COVER_PX * 2:
                return d
        except Exception:
            pass

    try:
        from PIL import Image
    except Exception as e:
        log("  (没装 Pillow，跳过封面；pip install pillow)", e)
        return None
    import io as _io

    try:
        song = kugou_find_song(title, artist, dur_ms)
        url = kugou_cover_url((song or {}).get("album_id"))
        if not url:
            return None
        req = urllib.request.Request(url, headers=UA)
        with urllib.request.urlopen(req, timeout=15, context=_SSL_CTX) as r:
            raw = r.read()
        if not raw:
            return None
        im = Image.open(_io.BytesIO(raw))
        data = _to_rgb565_be(im, COVER_PX)
    except Exception as e:
        log("  封面抓取失败:", e)
        return None

    try:
        os.makedirs(COVER_DIR, exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    except Exception:
        pass
    return data


def fetch_lrc(title, artist, album, dur_ms):
    """带磁盘缓存"""
    key = re.sub(r"[^\w\u4e00-\u9fff]+", "_", f"{title}_{artist}")[:80]
    os.makedirs(CACHE_DIR, exist_ok=True)
    path = os.path.join(CACHE_DIR, key + ".lrc")
    if os.path.exists(path):
        try:
            with open(path, encoding="utf-8") as f:
                t = f.read()
            if t.strip():
                return t, "cache"
        except Exception:
            pass

    lrc, src = None, None

    try:
        h, sec = kugou_find_hash(title, artist, dur_ms)
        if h:
            lrc = kugou_lrc_by_hash(h, sec or (dur_ms / 1000.0))
            if lrc:
                src = "kugou"
    except Exception as e:
        log("  酷狗歌词失败:", e)

    if not lrc:
        try:
            lrc = netease_lrc(title, artist, dur_ms)
            if lrc:
                src = "netease"
        except Exception as e:
            log("  网易云歌词失败:", e)

    if not lrc:
        return None, None

    lrc = ensure_tags(lrc, title, artist, album)
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(lrc)
    except Exception:
        pass
    return lrc, src


# ==========================================================================
# 三、和板子通信
# ==========================================================================

class Board:
    def __init__(self, host, port=BOARD_PORT):
        self.host = host
        self.port = port
        self.base = f"http://{host}:{port}"
        self.fails = 0

    def _req(self, path, data=None, method=None, timeout=4.0):
        req = urllib.request.Request(self.base + path, data=data, method=method)
        req.add_header("Content-Type", "text/plain; charset=utf-8")
        req.add_header("Connection", "close")
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body = r.read().decode("utf-8", "replace")
        return json.loads(body) if body.strip().startswith("{") else {}

    def push_track(self, title, artist, album, dur_ms, lrc):
        q = urllib.parse.urlencode({"title": title, "artist": artist,
                                    "album": album, "dur": int(dur_ms)})
        return self._req(f"/api/ext/track?{q}", data=lrc.encode("utf-8"), method="POST")

    def push_pos(self, pos_ms, state, dur_ms=0):
        q = {"p": int(pos_ms), "s": int(state)}
        # 换歌瞬间界面还没给出时长，这里每次顺带补上，板子进度条就能自愈
        if dur_ms and dur_ms > 0:
            q["d"] = int(dur_ms)
        return self._req("/api/ext/pos?" + urllib.parse.urlencode(q),
                         data=b"", method="POST", timeout=2.0)

    def state(self):
        return self._req("/api/ext/state", method="GET", timeout=2.5)

    def push_cover(self, data):
        """data = COVER_PX*COVER_PX*2 字节 RGB565(大端)"""
        return self._req("/api/ext/cover", data=data, method="POST", timeout=10.0)

    def clear_cover(self):
        return self._req("/api/ext/cover?clear=1", data=b"", method="POST")

    def clear(self):
        return self._req("/api/ext/clear", data=b"", method="POST")

    def poll_cmd(self):
        """单独取一条板子投递的命令（暂停时用；正常走 push_pos 的响应捎带）"""
        return self._req("/api/ext/cmd", method="GET", timeout=2.0).get("cmd", "")


# ==========================================================================
# 三·五、把板子的命令翻成系统媒体键
# ==========================================================================
# 板子不播音频（音频一直在电脑的酷狗里），所以在板子上左滑切歌，本质是
# 「请电脑上的播放器切歌」。板子把命令捎在 /api/ext/pos 的响应里送回来，
# 这里收到后按一个 Windows 媒体键 —— 媒体键是全局的，不需要酷狗在前后台，
# 也正好走酷狗已经注册好的那个 SMTC 媒体会话。

VK_MEDIA_NEXT_TRACK = 0xB0
VK_MEDIA_PREV_TRACK = 0xB1
VK_MEDIA_PLAY_PAUSE = 0xB3

_CMD_TO_VK = {
    "next":   VK_MEDIA_NEXT_TRACK,
    "prev":   VK_MEDIA_PREV_TRACK,
    "toggle": VK_MEDIA_PLAY_PAUSE,
}
_CMD_CN = {"next": "下一曲", "prev": "上一曲", "toggle": "播放/暂停"}


def _build_sendinput():
    """用 ctypes 包一个 SendInput（比已废弃的 keybd_event 可靠）。"""
    import ctypes
    from ctypes import wintypes

    # 指针宽度的无符号整数；三处结构体都要用它，写错会导致 SendInput 失败
    ULONG_PTR = (ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8
                 else ctypes.c_ulong)

    class KEYBDINPUT(ctypes.Structure):
        _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                    ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                    ("dwExtraInfo", ULONG_PTR)]

    class MOUSEINPUT(ctypes.Structure):
        _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                    ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                    ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]

    class HARDWAREINPUT(ctypes.Structure):
        _fields_ = [("uMsg", wintypes.DWORD), ("wParamL", wintypes.WORD),
                    ("wParamH", wintypes.WORD)]

    class _UNION(ctypes.Union):
        # 必须是完整 union：INPUT 的大小由最大的成员（MOUSEINPUT）决定，
        # 少写一个成员会让 sizeof(INPUT) 偏小，SendInput 直接返回 0。
        _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("hi", HARDWAREINPUT)]

    class INPUT(ctypes.Structure):
        _fields_ = [("type", wintypes.DWORD), ("u", _UNION)]

    INPUT_KEYBOARD  = 1
    KEYEVENTF_KEYUP = 0x0002
    user32 = ctypes.windll.user32

    def send(vk):
        arr = (INPUT * 2)()
        arr[0].type = INPUT_KEYBOARD
        arr[0].u.ki = KEYBDINPUT(vk, 0, 0, 0, 0)                   # 按下
        arr[1].type = INPUT_KEYBOARD
        arr[1].u.ki = KEYBDINPUT(vk, 0, KEYEVENTF_KEYUP, 0, 0)     # 抬起
        return user32.SendInput(2, ctypes.byref(arr), ctypes.sizeof(INPUT)) == 2

    return send


_sendinput = None


def send_media_key(vk):
    global _sendinput
    if _sendinput is None:
        try:
            _sendinput = _build_sendinput()
        except Exception as e:
            log("  !! 媒体键不可用（非 Windows？）:", e)
            _sendinput = False
    if not _sendinput:
        return False
    try:
        return bool(_sendinput(vk))
    except Exception as e:
        log("  !! 发媒体键失败:", e)
        return False


def handle_cmd(cmd, no_remote_key=False):
    """处理板子捎回来的命令。返回是否处理了一条。

    不做去抖 —— 板子那边是「取走即清空」，本来就不会重复；
    反而要保证用户**快速连滑两次**能被老老实实执行两次。
    """
    if not cmd:
        return False
    vk = _CMD_TO_VK.get(cmd)
    if vk is None:
        log(f"  ? 板子发来未知命令: {cmd!r}")
        return False
    cn = _CMD_CN.get(cmd, cmd)
    if no_remote_key:
        log(f"  ▶ 板子要求「{cn}」（--no-remote-key：只报告，不按键）")
        return True
    ok = send_media_key(vk)
    log(f"  ▶ 板子要求「{cn}」-> 已发媒体键 {'✓' if ok else '✗ 失败'}")
    return True


def discover(timeout=3.0):
    """先试 mDNS 名字，再 UDP 广播。返回 (ip, info) 或 (None, None)"""
    # 1) mDNS（Windows 10+ 通常能解析 .local）
    try:
        req = urllib.request.Request("http://musicplayer.local/api/ext/state",
                                     headers=UA, method="GET")
        with urllib.request.urlopen(req, timeout=1.5) as r:
            if r.status == 200:
                return "musicplayer.local", "mdns"
    except Exception:
        pass

    # 2) UDP 广播
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(0.5)
    try:
        s.bind(("", 0))
    except Exception:
        pass

    found = []
    deadline = time.time() + timeout
    try:
        for _ in range(6):
            try:
                s.sendto(b"ESP32LYRICS?", ("255.255.255.255", UDP_PORT))
            except Exception:
                pass
            time.sleep(0.15)
        while time.time() < deadline:
            try:
                data, addr = s.recvfrom(1024)
            except socket.timeout:
                continue
            if data.startswith(b"ESP32LYRICS!"):
                info = data.decode("utf-8", "replace")
                if addr[0] not in [x[0] for x in found]:
                    found.append((addr[0], info))
                    if len(found) >= 3:
                        break
    finally:
        s.close()

    if found:
        return found[0][0], found[0][1]
    return None, None


def conf_path():
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), CONF_NAME)


def load_conf():
    try:
        with open(conf_path(), encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_conf(d):
    try:
        with open(conf_path(), "w", encoding="utf-8") as f:
            json.dump(d, f, ensure_ascii=False, indent=2)
    except Exception:
        pass


# ==========================================================================
# 四、主流程
# ==========================================================================

def probe():
    log("=== 方案 A：UI Automation 直接读酷狗界面（位置只能从这里拿）===")
    u = UiaKugou()
    if not u.available:
        log(f"  ❌ 不可用（{u.err}）")
        log("     请执行: pip install uiautomation")
    else:
        t0 = time.time()
        r = u.read()
        log(f"  首次定位耗时 {time.time() - t0:.2f}s")
        if r:
            log(f"  歌名 = {r['title']!r}    歌手 = {r['artist']!r}")
            log(f"  进度 = {r['pos_ms']}ms / {r['dur_ms']}ms    状态 = {r['state']}")
            before = r["pos_ms"]
            time.sleep(1.5)
            r2 = u.read()
            if r2:
                log(f"  1.5 秒后再读: {r2['pos_ms']}ms  "
                    f"(变化 {r2['pos_ms'] - before:+d}ms —— 播放中应在 1500 左右)")
        else:
            log("  ⚠ 没读到。可能原因：酷狗没在播放 / 主窗口被收进托盘 / 界面结构变了")

    log("")
    log("=== 方案 B：SMTC 系统媒体控制（歌名歌手状态有，位置通常没有）===")
    s = Smtc()
    if not s.available:
        log("  ❌ 不可用")
        log("     请执行: pip install winrt-runtime winrt-Windows.Media.Control "
            "winrt-Windows.Foundation winrt-Windows.Foundation.Collections")
    else:
        info = s.read(prefer_kugou=False)
        if info:
            for k in ("app", "title", "artist", "dur_ms", "pos_ms", "state"):
                log(f"  {k:8} = {info[k]}")
        else:
            log("  ⚠ 没有媒体会话")
    log("")
    log("（state: 0=停止 1=播放 2=暂停）")
    return 0


def main():
    ap = argparse.ArgumentParser(description="把电脑上酷狗的歌词推送给 ESP32 歌词机")
    ap.add_argument("--host", help="板子 IP（不填则自动发现）")
    ap.add_argument("--port", type=int, default=BOARD_PORT)
    ap.add_argument("--interval", type=float, default=0.5, help="位置推送间隔(秒)")
    ap.add_argument("--offset", type=int, default=0,
                    help="整体时间偏移(毫秒)，歌词偏早填负数、偏晚填正数")
    ap.add_argument("--probe", action="store_true", help="只探测 SMTC，不连板子")
    ap.add_argument("--dry-run", action="store_true", help="不推送，只打印")
    ap.add_argument("--no-kugou-filter", action="store_true",
                    help="不强制只认酷狗，任何播放器都桥接")
    ap.add_argument("--no-cover", action="store_true",
                    help="不抓专辑封面（板子左侧显示占位图）")
    ap.add_argument("--no-remote-key", action="store_true",
                    help="不在板子上响应切歌（左滑/右滑只打印日志，不发系统媒体键）")
    args = ap.parse_args()

    if args.probe:
        return probe()

    reader = Reader()
    if not reader.uia.available and not (reader.smtc and reader.smtc.available):
        log("❌ 缺少读取酷狗播放状态的组件。请执行：")
        log("   pip install uiautomation")
        log("   （可选，兜底用）pip install winrt-runtime winrt-Windows.Media.Control "
            "winrt-Windows.Foundation winrt-Windows.Foundation.Collections")
        return 1
    log(f"读取方式 = {reader.describe()}")

    # ---- 找板子 ----
    board = None
    if not args.dry_run:
        host = args.host or load_conf().get("last_host")
        if host:
            b = Board(host, args.port)
            try:
                st = b.state()
                log(f"✅ 板子在线: {host}   当前: {st.get('title') or '(空闲)'}")
                board = b
                save_conf({"last_host": host})
            except Exception as e:
                log(f"⚠ {host} 连不上（{e}），改为自动发现…")

        if board is None:
            log("正在自动发现板子…")
            ip, info = discover()
            if ip:
                log(f"✅ 发现板子: {ip}   ({info})")
                board = Board(ip, args.port)
                save_conf({"last_host": ip})
            else:
                log("❌ 没发现板子。请确认：")
                log("   1) 板子已连上同一个路由器（板子屏幕底部会显示「局域网 <ip>」）")
                log("   2) 也可以用 --host <板子IP> 手动指定")
                return 3
    else:
        log("dry-run 模式：不连接板子，只打印将要推送的内容")

    log("开始桥接。用酷狗放歌即可，歌词会自动出现在板子上。Ctrl+C 退出。")

    last_key = None
    pending = None          # 待抓的曲目
    lock = threading.Lock()
    cur = {"key": None, "lrc_pushed": False}

    def worker():
        """后台抓歌词/封面并推送 track（抓取要 1~3 秒，不能卡住位置推送）"""
        nonlocal pending
        while True:
            with lock:
                job = pending
                pending = None
            if not job:
                time.sleep(0.1)
                continue
            title, artist, album, dur = job
            log(f"🔎 抓歌词: {title} - {artist}")
            lrc, src = fetch_lrc(title, artist, album, dur)
            if lrc:
                log(f"   ✅ 拿到歌词（{src}，{len(lrc.splitlines())} 行）")
            else:
                log("   ⚠ 没找到歌词（板子会显示「这首歌没有歌词」）")

            # 字库预警：GB2312 之外的字符板子一定显示成方框，先在电脑端报出来
            bad = unsupported_glyphs(title, artist, album, lrc or "")
            if bad:
                log("   !! 板子字库可能显示成方框的字（GB2312 之外）: %s"
                    % " ".join("%s(U+%04X)" % (c, ord(c)) for c in bad))

            with lock:
                if cur["key"] != f"{title}|{artist}":
                    continue          # 已经换歌了，丢弃
            if args.dry_run:
                continue
            try:
                board.push_track(title, artist, album, dur, lrc or "")
                cur["lrc_pushed"] = True
            except Exception as e:
                log("   推送 track 失败:", e)

            # ---- 专辑封面（先推 track 再推图，保证立刻先出歌词）----
            if args.no_cover:
                continue
            with lock:
                if cur["key"] != f"{title}|{artist}":
                    continue
            try:
                cv = fetch_cover(title, artist, dur)
                if cv:
                    board.push_cover(cv)
                    log(f"   🖼  封面已推送（{len(cv) // 1024} KB）")
                else:
                    board.clear_cover()      # 没封面就让板子画占位图，别留上一首的
            except Exception as e:
                log("   封面推送失败:", e)

    if not args.dry_run:
        threading.Thread(target=worker, daemon=True).start()

    # ---- 主循环 ----
    last_pos_push = 0.0
    try:
        while True:
            info = reader.read()
            if info and info["title"]:
                title, artist = info["title"], info["artist"]
                key = f"{title}|{artist}"

                if key != last_key:
                    last_key = key
                    with lock:
                        cur["key"] = key
                        cur["lrc_pushed"] = False
                        pending = (title, artist, info["album"], info["dur_ms"])
                    log(f"🎵 换歌: {title} - {artist}   ({info['dur_ms']//1000}s)")

                # 位置推送：播放中每次推；暂停时 5 秒推一次
                now = time.time()
                due = (info["state"] == 1) or (now - last_pos_push > 5.0)
                if due and now - last_pos_push >= args.interval:
                    last_pos_push = now
                    pos = info["pos_ms"] + args.offset
                    if args.dry_run:
                        log(f"   → pos={pos}ms state={info['state']}")
                    else:
                        try:
                            r = board.push_pos(pos, info["state"], info["dur_ms"])
                            board.fails = 0
                            # 板子把「左滑下一曲」这类命令捎在这个响应里带回来
                            handle_cmd(r.get("cmd", ""), args.no_remote_key)
                        except Exception as e:
                            board.fails += 1
                            if board.fails in (1, 10, 50):
                                log(f"   ⚠ 推送位置失败 x{board.fails}: {e}")
                elif not args.dry_run:
                    # 暂停时这一轮不推 pos，命令就没法捎带 —— 单独去取一次
                    try:
                        handle_cmd(board.poll_cmd(), args.no_remote_key)
                    except Exception:
                        pass
            else:
                if last_key is not None:
                    log("⏸ 读不到播放信息（酷狗可能停播了）")
                    last_key = None

            time.sleep(max(0.1, args.interval))
    except KeyboardInterrupt:
        log("\n退出。正在通知板子清空…")
        if not args.dry_run:
            try:
                board.clear()
                log("已清空。")
            except Exception:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
