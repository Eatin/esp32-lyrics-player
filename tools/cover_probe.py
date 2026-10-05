#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""探查酷狗封面（专辑图）接口的返回结构，为 kugou_bridge 的封面推送做依据。"""
import json, ssl, sys, urllib.parse, urllib.request

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}
CTX = ssl.create_default_context()
CTX.check_hostname = False
CTX.verify_mode = ssl.CERT_NONE


def get(url, timeout=15, raw=False):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=timeout, context=CTX) as r:
        b = r.read()
        return b if raw else json.loads(b.decode("utf-8", "replace"))


def main():
    kw = sys.argv[1] if len(sys.argv) > 1 else "我怀念的 孙燕姿"
    print("== 1) search/song:", kw)
    u = "https://mobiles.kugou.com/api/v3/search/song?" + urllib.parse.urlencode(
        {"keyword": kw, "page": 1, "pagesize": 5, "showtype": 1})
    d = get(u)
    info = ((d.get("data") or {}).get("info")) or []
    print("   hits:", len(info))
    if not info:
        print(json.dumps(d, ensure_ascii=False)[:600])
        return
    it = info[0]
    keys = ["hash", "album_id", "songname", "singername", "duration", "audio_id",
            "album_audio_id", "album_name", "imgurl", "img", "union_cover",
            "trans_param", "grp", "privilege"]
    for k in keys:
        if k in it:
            print(f"   {k} = {json.dumps(it[k], ensure_ascii=False)[:200]}")

    aid = it.get("album_id")
    print("\n== 2) album/info albumid=", aid)
    for base in ("https://mobilecdn.kugou.com/api/v3/album/info?",
                 "http://mobilecdn.kugou.com/api/v3/album/info?"):
        try:
            u2 = base + urllib.parse.urlencode({"albumid": aid, "version": 9108})
            d2 = get(u2)
            data = d2.get("data") or {}
            print("   base:", base)
            print("   data keys:", sorted(data.keys())[:20])
            print("   imgurl:", data.get("imgurl"))
            print("   img:", data.get("img"))
            print("   albumnames:", data.get("albumname"), "| singername:", data.get("singername"))
            break
        except Exception as e:
            print("   FAIL", base, type(e).__name__, e)

    print("\n== 3) 试下载封面")
    img = (data.get("imgurl") if 'data' in dir() else None) or ""
    if img:
        for size in ("480", "240"):
            for host in ("https://imge.kugou.com/stdmusic/%s/%s" % (size, img),
                         "http://imge.kugou.com/stdmusic/%s/%s" % (size, img)):
                try:
                    b = get(host, raw=True)
                    print("   OK", host, len(b), "bytes", b[:4])
                    break
                except Exception as e:
                    print("   FAIL", host, type(e).__name__, e)
            else:
                continue
            break
    else:
        print("   无 imgurl，尝试其它来源")
        # 兜底：play/getdata
        try:
            u3 = "https://wwwapi.kugou.com/yy/index.php?" + urllib.parse.urlencode(
                {"r": "play/getdata", "hash": it.get("hash")})
            d3 = get(u3)
            print("   getdata keys:", sorted((d3.get("data") or {}).keys())[:25])
            print("   img:", (d3.get("data") or {}).get("img"))
        except Exception as e:
            print("   FAIL getdata", type(e).__name__, e)


if __name__ == "__main__":
    main()
