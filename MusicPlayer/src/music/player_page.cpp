/**
 * @file  player_page.cpp
 * @brief 板子内置网页（用 C++11 原始字符串字面量承载，避免 C 字符串转义地狱）
 *
 * 本文件里有两个页面：
 *   - player_page_html  →  GET /        接收控制台（电脑推送歌词时的状态与控制）
 *   - wifi_page_html    →  GET /wifi    WiFi 配网页
 *
 * 架构说明（重要）：
 *   音频**不在板子上播**，也不在网页里播。电脑上的酷狗负责放歌（声音从电脑出），
 *   PC 端脚本 kugou_bridge.py 读 Windows SMTC 拿到歌名/歌手/进度 + 抓 LRC，
 *   再 POST 给板子；板子只当一块歌词屏。
 *   所以这个网页的角色是「遥控器 / 状态台」：
 *     看连接状态、微调歌词对轴、必要时手动上传 .lrc、进 WiFi 设置。
 */
#include "player_page.h"

extern "C" {
const char player_page_html[] = R"HTMLPAGE(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark">
<title>ESP32 歌词机 · 控制台</title>
<style>
:root{
  --bg:#0d0f16; --card:#161b27; --line:#243049;
  --fg:#e8edf6; --dim:#8b98ae; --far:#6b7789;
  --acc:#4c8dff; --ok:#2ad4a8; --warn:#ffb454; --bad:#ff6b6b;
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{
  margin:0;background:var(--bg);color:var(--fg);
  font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;
  padding:14px 14px calc(28px + env(safe-area-inset-bottom));
  max-width:620px;margin:0 auto;
}
header{display:flex;align-items:baseline;justify-content:space-between;gap:10px;margin:4px 0 14px}
h1{font-size:17px;margin:0;font-weight:600;letter-spacing:.3px}
#net{font-size:12px;color:var(--dim);text-align:right;line-height:1.35}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin-bottom:12px}
h2{font-size:13px;margin:0 0 10px;color:var(--dim);font-weight:600;letter-spacing:.5px}
.row{display:flex;justify-content:space-between;align-items:center;gap:8px}
.k{font-size:13px;color:var(--dim)}
.v{font-size:13px;font-weight:600}
.v.ok{color:var(--ok)} .v.warn{color:var(--warn)} .v.idle{color:var(--far)}
#song{font-size:20px;font-weight:600;margin:10px 0 2px;word-break:break-all}
#artist{font-size:13px;color:var(--dim);min-height:18px;margin-bottom:10px}
.bar{height:5px;background:#0b0e15;border-radius:3px;overflow:hidden;margin-bottom:6px}
.bar i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--acc),var(--ok));transition:width .35s linear}
.small{font-size:12px;color:var(--far)}
.hint{font-size:12px;color:var(--far);margin:0 0 10px}
.offrow{display:grid;grid-template-columns:repeat(3,1fr) 1.25fr repeat(3,1fr);gap:6px;align-items:center}
.offrow button{padding:9px 0;font-size:12px}
.rotrow{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.rotrow button{padding:11px 0;font-size:13px}
.offval{text-align:center;font-size:13px;font-weight:600;color:var(--acc);font-variant-numeric:tabular-nums}
button{
  background:#1f2940;color:var(--fg);border:1px solid var(--line);border-radius:10px;
  padding:11px 12px;font-size:14px;font-family:inherit;cursor:pointer;
}
button:active{transform:scale(.97)}
button:disabled{opacity:.45}
.ghost{width:100%;margin-top:8px;background:transparent;color:var(--dim)}
.danger{width:100%;margin-top:8px;background:transparent;border-color:#5a2b2b;color:var(--bad)}
.btnlink{display:block;text-align:center;text-decoration:none;border-radius:10px;
  border:1px solid var(--line);padding:11px 12px;color:var(--fg);font-size:14px}
input,textarea{
  width:100%;background:#0b0e15;border:1px solid var(--line);border-radius:10px;
  color:var(--fg);padding:10px;font:14px/1.5 inherit;margin-bottom:8px;
}
textarea{resize:vertical;font-family:ui-monospace,Consolas,monospace;font-size:12px}
label.file{display:block;margin-bottom:8px}
label.file input{display:none}
label.file span{
  display:block;text-align:center;border:1px dashed var(--line);border-radius:10px;
  padding:10px;font-size:13px;color:var(--dim);margin:0;
}
#fupload{width:100%;background:var(--acc);border-color:var(--acc);font-weight:600}
#toast{
  position:fixed;left:50%;bottom:calc(20px + env(safe-area-inset-bottom));transform:translateX(-50%);
  background:#22303f;color:var(--fg);border:1px solid var(--line);border-radius:10px;
  padding:9px 16px;font-size:13px;opacity:0;transition:opacity .25s;pointer-events:none;
}
#toast.on{opacity:1}
.help ol{margin:0;padding-left:18px;font-size:13px;color:var(--dim)}
.help li{margin-bottom:5px}
.tiny{font-size:11px;color:#4f5b6e;margin:10px 0 0}
code{background:#0b0e15;padding:1px 5px;border-radius:4px;font-size:12px;color:var(--ok)}
</style>
</head>
<body>

<header>
  <h1>ESP32 歌词机</h1>
  <div id="net">读取中…</div>
</header>

<main>
  <section class="card">
    <div class="row"><span class="k">状态</span><span class="v idle" id="st">读取中…</span></div>
    <div id="song">—</div>
    <div id="artist"></div>
    <div class="bar"><i id="bar"></i></div>
    <div class="row small"><span id="time">0:00 / 0:00</span><span id="ago"></span></div>
  </section>

  <section class="card">
    <h2>歌词对轴</h2>
    <p class="hint">板子上的歌词比歌声<b>快</b>就点减号，比歌声<b>慢</b>就点加号。改完立刻生效。</p>
    <div class="offrow">
      <button data-d="-1000">−1s</button>
      <button data-d="-500">−0.5s</button>
      <button data-d="-100">−0.1s</button>
      <div class="offval" id="off">0.0s</div>
      <button data-d="100">+0.1s</button>
      <button data-d="500">+0.5s</button>
      <button data-d="1000">+1s</button>
    </div>
    <button class="ghost" id="offreset">重置为 0</button>
  </section>

  <section class="card">
    <h2>手动上传歌词</h2>
    <p class="hint">自动抓的歌词不对（对不上、错句）时，用这个覆盖板子当前显示的歌词。</p>
    <input id="ftitle" placeholder="歌名（可留空）">
    <input id="fartist" placeholder="歌手（可留空）">
    <label class="file"><span id="fname">选择 .lrc / .txt 文件（自动识别 UTF-8 / GBK）</span><input type="file" id="ffile" accept=".lrc,.txt,text/plain"></label>
    <textarea id="ftext" rows="6" placeholder="或直接把 LRC 文本粘贴到这里…"></textarea>
    <button id="fupload">推送到板子</button>
  </section>

  <section class="card">
    <h2>屏幕方向</h2>
    <p class="hint">板子横放时如果画面上下颠倒，点一下切换（立刻生效，不用重新烧录）。</p>
    <div class="rotrow">
      <button data-rot="0">方向 A</button>
      <button data-rot="1">方向 B</button>
    </div>
  </section>

  <section class="card">
    <h2>电源 / 锂电池</h2>
    <div class="row"><span class="k">当前供电</span><span class="v" id="pwrsrc">读取中…</span></div>
    <div class="row"><span class="k">电池电压</span><span class="v" id="pwrvolt">—</span></div>
    <p class="hint">拔出 USB 后这里仍能刷出数字，就说明锂电池自锁生效了。若拔线即断电，说明 <code>SYS_EN</code> 闩锁没拉起来。</p>
    <button class="danger" id="pwrbtn">关机（也可长按板背 PWR 键 2 秒）</button>
    <p class="hint">关机后会松开 <code>SYS_EN</code> 电源闩锁。若板子还插着 USB，它会继续运行（这里会提示未断电）；电池供电时才会真正熄火，之后再按 PWR 键才能开机。</p>
  </section>

  <section class="card">
    <h2>其他</h2>
    <a class="btnlink" href="/wifi">网络设置（连家里 WiFi）</a>
    <button class="danger" id="clearbtn">清空板子当前曲目</button>
  </section>

  <section class="card help">
    <h2>怎么用</h2>
    <ol>
      <li>电脑上运行 <code>kugou_bridge.py</code></li>
      <li>用酷狗放歌 —— 板子会自动亮起歌词</li>
      <li>对不齐就来这里微调，或手动上传歌词</li>
    </ol>
    <p class="tiny">本页只是控制台：歌始终在电脑上放，声音从电脑出，板子只负责显示歌词。</p>
  </section>
</main>

<div id="toast"></div>

<script>
"use strict";
var $ = function(s){ return document.querySelector(s); };
var toastTimer = null;

function toast(msg){
  var t = $('#toast');
  t.textContent = msg;
  t.classList.add('on');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(function(){ t.classList.remove('on'); }, 1800);
}

function fmt(ms){
  if (!ms || ms < 0) ms = 0;
  var s = Math.floor(ms / 1000);
  return Math.floor(s / 60) + ':' + ('0' + (s % 60)).slice(-2);
}

async function post(path, body){
  var r = await fetch(path, { method:'POST', body: body || '', cache:'no-store' });
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return r.json();
}

/* ---------------- 状态轮询 ---------------- */
var lastSrc = '';

function render(d){
  var st = $('#st'), cls = 'idle', txt = '等待电脑连接';
  if (d.ever && d.live)      { txt = '电脑推送中'; cls = 'ok'; }
  else if (d.ever)           { txt = '电脑已断开'; cls = 'warn'; }
  st.textContent = txt;
  st.className = 'v ' + cls;

  $('#song').textContent   = d.title || '—';
  $('#artist').textContent = d.ar || '';
  $('#time').textContent   = fmt(d.p) + ' / ' + fmt(d.d);
  $('#ago').textContent    = d.ever ? ((d.ago / 1000).toFixed(1) + 's 前推送') : '';

  var pc = d.d ? Math.min(100, d.p * 100 / d.d) : 0;
  $('#bar').style.width = pc.toFixed(1) + '%';

  var off = (d.off || 0) / 1000;
  $('#off').textContent = (off > 0 ? '+' : '') + off.toFixed(1) + 's';

  var src = d.ever && d.live ? 'pc' : 'none';
  if (src !== lastSrc) {
    lastSrc = src;
    /* 曲目换行时清空手动输入的歌名，避免覆盖错 */
    if (d.title) $('#ftitle').placeholder = '歌名（可留空）：' + d.title;
  }
}

async function tick(){
  try {
    var r = await fetch('/api/ext/state', { cache:'no-store' });
    render(await r.json());
  } catch (e) {
    $('#st').textContent = '板子无响应';
    $('#st').className = 'v warn';
  }
}

async function loadNet(){
  try {
    var r = await fetch('/api/net', { cache:'no-store' });
    var d = await r.json();
    $('#net').innerHTML = (d.mode === 'sta' ? '局域网' : '热点') + ' · ' + d.ip +
                          '<br>' + (d.saved ? ('已存 WiFi: ' + d.saved) : '未配置家里 WiFi');
  } catch (e) {
    $('#net').textContent = '';
  }
}

/* ---------------- 电源 / 锂电池 ---------------- */
async function loadPwr(){
  try {
    var r = await fetch('/api/pwr', { cache:'no-store' });
    var d = await r.json();
    var src = $('#pwrsrc');
    src.textContent = d.batt ? '锂电池（可拔 USB）' : 'USB 供电';
    src.className   = 'v ' + (d.batt ? 'ok' : 'idle');
    if (d.mv > 500) {
      $('#pwrvolt').textContent = (d.mv / 1000).toFixed(2) + ' V' +
                                  (d.pct >= 0 ? '（约 ' + d.pct + '%）' : '');
    } else {
      $('#pwrvolt').textContent = '未接电池 / 读不到';
    }
  } catch (e) {
    $('#pwrsrc').textContent = '板子无响应';
    $('#pwrsrc').className = 'v warn';
  }
}

/* ---------------- 对轴 ---------------- */
document.querySelectorAll('.offrow button').forEach(function(b){
  b.addEventListener('click', async function(){
    try {
      var d = await post('/api/ext/offset?d=' + b.dataset.d);
      render(d);
      toast('已调整到 ' + $('#off').textContent);
    } catch (e) { toast('调整失败：' + e.message); }
  });
});

$('#offreset').addEventListener('click', async function(){
  try { render(await post('/api/ext/offset?v=0')); toast('偏移已归零'); }
  catch (e) { toast('失败：' + e.message); }
});

/* ---------------- 屏幕方向 ---------------- */
document.querySelectorAll('[data-rot]').forEach(function(b){
  b.addEventListener('click', async function(){
    try { await post('/api/rot?dir=' + b.dataset.rot); toast('已切到方向 ' + (b.dataset.rot === '0' ? 'A' : 'B')); }
    catch (e) { toast('切换失败：' + e.message); }
  });
});

/* ---------------- 手动上传歌词 ---------------- */
function decodeText(buf){
  var u8 = new Uint8Array(buf);
  try {
    return new TextDecoder('utf-8', { fatal:true }).decode(u8);
  } catch (e) {
    try { return new TextDecoder('gbk').decode(u8); }
    catch (e2) { return new TextDecoder('utf-8').decode(u8); }
  }
}

$('#ffile').addEventListener('change', async function(ev){
  var f = ev.target.files && ev.target.files[0];
  if (!f) return;
  var buf = await f.arrayBuffer();
  var txt = decodeText(buf);
  $('#ftext').value = txt;
  $('#fname').textContent = f.name + '  (' + txt.split('\n').length + ' 行)';
  if (!$('#ftitle').value) $('#ftitle').value = f.name.replace(/\.(lrc|txt)$/i, '');
});

$('#fupload').addEventListener('click', async function(){
  var txt = $('#ftext').value;
  if (!txt.trim()) { toast('歌词是空的'); return; }
  var q = new URLSearchParams();
  q.set('title',  $('#ftitle').value  || '');
  q.set('artist', $('#fartist').value || '');
  try {
    await post('/api/ext/track?' + q.toString(), txt);
    toast('已推送到板子');
  } catch (e) { toast('推送失败：' + e.message); }
});

$('#clearbtn').addEventListener('click', async function(){
  if (!confirm('清空板子上当前曲目（歌词会一起消失）？')) return;
  try { await post('/api/ext/clear'); toast('已清空'); tick(); }
  catch (e) { toast('失败：' + e.message); }
});

/* ---------------- 关机 ---------------- */
$('#pwrbtn').addEventListener('click', async function(){
  if (!confirm('确定要关机吗？\n电池供电会立刻断电，之后需按板背 PWR 键才能开机。')) return;
  toast('正在关机…');
  try {
    var r = await fetch('/api/pwr?off=1', { method:'POST', cache:'no-store' });
    var d = await r.json().catch(function(){ return {}; });
    if (d.off) toast('已松开电源闩锁（若还亮着说明还在用 USB 供电）');
  } catch (e) {
    toast('板子已断电（收不到回复是正常的）');
  }
});

/* ---------------- 启动 ---------------- */
loadNet();
loadPwr();
tick();
setInterval(tick, 1000);
setInterval(loadNet, 15000);
setInterval(loadPwr, 30000);
</script>
</body>
</html>
)HTMLPAGE";

const unsigned int player_page_len = sizeof(player_page_html) - 1;

/* ================================================================== */
/* 网络配置页 /wifi                                                    */
/* ================================================================== */
const char wifi_page_html[] = R"WIFIPAGE(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>网络设置 - ESP32 歌词机</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:#0d0f16;color:#e8edf6;
 font:15px/1.6 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif;
 padding:0 0 30px}
header{padding:16px;border-bottom:1px solid #1d2433;display:flex;align-items:center;gap:10px}
header h1{margin:0;font-size:17px;font-weight:600;flex:1}
header a{color:#7c879c;text-decoration:none;font-size:14px}
main{padding:18px 16px}
.cur{padding:12px 14px;border-radius:10px;background:#141a26;font-size:13px;
 color:#a8b4c8;line-height:1.7;margin-bottom:20px}
.cur b{color:#2ad4a8;font-weight:600}
.cur.warn b{color:#ffb84d}
label{display:block;font-size:13px;color:#7c879c;margin:16px 0 6px}
input{width:100%;padding:12px 13px;border-radius:10px;border:1px solid #232b3b;
 background:#141a26;color:#e8edf6;font-size:16px}
input:focus{outline:none;border-color:#4c8dff}
button{width:100%;margin-top:22px;padding:14px;border:0;border-radius:10px;
 font-size:16px;font-weight:600;background:#4c8dff;color:#fff}
button:active{background:#3b78e0}
button.ghost{background:#1a2030;color:#a8b4c8;margin-top:10px;font-weight:400}
.hint{font-size:12px;color:#5f6b82;margin-top:10px;line-height:1.7}
.status{margin-top:18px;padding:12px;border-radius:10px;background:#141a26;
 font-size:13px;color:#a8b4c8;display:none;line-height:1.6}
</style>
</head>
<body>
<header>
  <h1>网络设置</h1>
  <a href="/">&lsaquo; 返回播放器</a>
</header>
<main>
  <div class="cur" id="cur">读取中…</div>

  <form id="f">
    <label for="s">WiFi 名称（SSID）</label>
    <input id="s" name="ssid" autocapitalize="off" autocorrect="off"
           spellcheck="false" placeholder="家里路由器的 2.4G WiFi 名">
    <label for="p">WiFi 密码</label>
    <input id="p" name="pass" type="password" placeholder="留空表示无密码">
    <div class="hint">
      注意：板子只支持 <b>2.4GHz</b>。很多路由器把 2.4G 与 5G 合成一个名字，
      如果连不上，请把路由器的 2.4G 单独设一个名字再填这里。
    </div>
    <button type="submit">保存并重启</button>
  </form>

  <button class="ghost" id="clr">清除配置，回到热点模式</button>
  <div class="status" id="st"></div>

  <div class="hint" style="margin-top:22px">
    保存后板子会重启：若成功连上你的路由器，就进入<b>局域网模式</b>，
    手机只要连着同一个 WiFi，浏览器打开板子的 IP 即可（地址会显示在板子屏幕上）。
    如果密码填错或路由器不可达，板子会自动退回热点 <b>ESP32-Player</b>，不会失联。
  </div>
</main>

<script>
(function(){
var $ = function(id){ return document.getElementById(id); };

function show(msg, ok){
  var el = $('st');
  el.style.display = 'block';
  el.textContent = msg;
}

function netInfo(){
  fetch('/api/net').then(function(r){ return r.json(); }).then(function(d){
    var el = $('cur');
    if(d.mode === 'ap'){
      el.className = 'cur warn';
      el.innerHTML = '当前：<b>热点模式</b>（ESP32 自建热点）<br>'+
        '本机地址 http://'+(d.ip||'')+'/<br>'+
        (d.saved ? '已保存的家庭 WiFi：<b>'+(d.saved)+'</b>（尚未连上）'
                 : '尚未配置家庭 WiFi');
    } else {
      el.className = 'cur';
      el.innerHTML = '当前：<b>局域网模式</b><br>'+
        '已连上 <b>'+(d.ssid||'')+'</b><br>'+
        '本机地址 http://'+(d.ip||'')+'/';
      $('s').value = d.saved || d.ssid || '';
    }
  }).catch(function(){ $('cur').textContent = '读取失败'; });
}

$('f').onsubmit = function(e){
  e.preventDefault();
  var ssid = $('s').value.trim();
  if(!ssid){ alert('请填写 WiFi 名称'); return; }
  show('正在保存…', true);
  fetch('/api/wifi', {
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent($('p').value)
  }).then(function(){ 
    show('已保存，板子正在重启…请稍候约 10 秒，然后按提示访问新地址。');
  }).catch(function(){ show('保存失败，请重试。'); });
};

$('clr').onclick = function(){
  if(!confirm('确定清除家庭 WiFi 配置并回到热点模式？')) return;
  show('正在清除…');
  fetch('/api/wifi/forget', { method:'POST' }).then(function(){
    show('已清除，板子正在重启为热点模式（ESP32-Player）。');
  }).catch(function(){ show('操作失败。'); });
};

netInfo();
})();
</script>
</body>
</html>
)WIFIPAGE";

const unsigned int wifi_page_len = sizeof(wifi_page_html) - 1;
}
