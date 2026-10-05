"""从 player_page.cpp 抽出内嵌 HTML 页面：
   1) 校验 JS 语法（把 <script> 抽出来交给 node --check）
   2) 生成带 fetch mock 的预览页，方便本地直接看 UI
"""
import re, subprocess, sys, pathlib
from urllib.parse import urlencode

SRC = pathlib.Path(r"D:/wb/wave/MusicPlayer/src/music/player_page.cpp")
OUT = pathlib.Path(r"D:/wb/wave/tools/preview")
OUT.mkdir(parents=True, exist_ok=True)

text = SRC.read_text(encoding="utf-8")

def extract(tag):
    m = re.search(r'R"' + tag + r'\((.*?)\)' + tag + r'"', text, re.S)
    if not m:
        sys.exit(f"!! 找不到原始字符串 {tag}")
    return m.group(1)

pages = {"player": extract("HTMLPAGE"), "wifi": extract("WIFIPAGE")}

# ---------- 1) JS 语法校验 ----------
node = r"C:/Users/Eatin/.workbuddy/binaries/node/versions/22.22.2-3/node.exe"
fail = 0
for name, html in pages.items():
    scripts = re.findall(r"<script>(.*?)</script>", html, re.S)
    js = "\n;\n".join(scripts)
    # 页面脚本是 IIFE，直接整体校验
    jsfile = OUT / f"{name}_page.js"
    jsfile.write_text(js, encoding="utf-8")
    r = subprocess.run([node, "--check", str(jsfile)], capture_output=True, text=True)
    ok = (r.returncode == 0)
    print(f"[JS ] {name:7s} blocks={len(scripts)} syntax={'OK' if ok else 'FAIL'}")
    if not ok:
        fail = 1
        print(r.stderr[:800])

# ---------- 2) 静态结构检查 ----------
checks = {
    "player": ['id="netbar"', "netInfo()", "/api/net"],
    "wifi":   ['id="s"', 'id="p"', 'id="clr"', "/api/wifi", "/api/wifi/forget", "/api/net"],
}
for name, keys in checks.items():
    miss = [k for k in keys if k not in pages[name]]
    print(f"[DOM] {name:7s} missing={miss if miss else 'none'}")

# ---------- 3) 生成带 mock 的预览页 ----------
MOCK = """<script>
/* === 仅用于本地预览的 fetch 模拟：模拟「热点模式」 === */
window.fetch = function(url, opt){
  var body = {ok:1};
  if(String(url).indexOf('/api/net') === 0){
    body = {mode:'ap', ssid:'ESP32-Player', ip:'192.168.4.1',
            url:'http://192.168.4.1/', saved:''};
  } else if(String(url).indexOf('/api/list') === 0){
    body = {sd:true, n:3, items:[
      {i:0,n:'晴天',u:'晴天.mp3',l:true,sz:10485760},
      {i:1,n:'稻香',u:'稻香.mp3',l:true,sz:9830400},
      {i:2,n:'夜曲',u:'夜曲.flac',l:false,sz:31457280}]};
  } else if(String(url).indexOf('/api/sync') === 0){
    body = {cmd:'none'};
  }
  return Promise.resolve({json:function(){return Promise.resolve(body);},
                          ok:true, status:200});
};
</script>
"""

for name, html in pages.items():
    preview = html.replace("<head>", "<head>\n" + MOCK, 1)
    banner = ('<div style="position:fixed;bottom:0;left:0;right:0;z-index:99;'
              'background:#4c8dff;color:#fff;font-size:11px;text-align:center;'
              'padding:3px">本地预览（数据为模拟）</div>')
    preview = preview.replace("</body>", banner + "\n</body>", 1)
    p = OUT / f"{name}_preview.html"
    p.write_text(preview, encoding="utf-8")
    print(f"[OUT] {p}  ({len(preview)} bytes)")

sys.exit(fail)
