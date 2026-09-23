// The page the phone opens: draws the frames from /ws on a canvas, optional touch pad.
#pragma once

static const char WEB_PAGE[] = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black">
<title>Cardputer GB</title>
<style>
html,body{margin:0;height:100%;background:#000;color:#aaa;font:15px -apple-system,system-ui,sans-serif;overflow:hidden;
  -webkit-user-select:none;user-select:none;-webkit-touch-callout:none;touch-action:none}
/* Portrait: picture on top, a Game Boy style pad below. Landscape: pad on both sides of the picture. */
#app{position:fixed;inset:0;display:grid;box-sizing:border-box;
  padding:max(6px,env(safe-area-inset-top)) env(safe-area-inset-right) max(10px,env(safe-area-inset-bottom)) env(safe-area-inset-left);
  grid-template:"screen screen" auto "dpad ab" auto "ss ss" auto ". ." 1fr / minmax(0,1fr) minmax(0,1fr)}
#screen{grid-area:screen;display:flex;justify-content:center;min-width:0}
canvas{display:block;image-rendering:pixelated;image-rendering:crisp-edges;width:100%;max-height:58dvh;aspect-ratio:160/144;object-fit:contain}
#status{position:fixed;left:0;right:0;top:25%;text-align:center;pointer-events:none}
.pad{display:none}
body.pad-on .pad{display:grid}
.pad div{background:#3a3a3a;color:#ddd;display:flex;align-items:center;justify-content:center;font-weight:600}
.pad div.on{background:#aaa;color:#000}
.pad div.hit{background:none}
#dpad{grid-area:dpad;align-self:center;justify-self:center;margin-top:22px;grid-template:repeat(3,58px)/repeat(3,58px)}
#dpad div:not(.hit){border-radius:10px;font-size:20px}
#ab{grid-area:ab;align-self:center;justify-self:center;position:relative;width:160px;height:150px;display:none;margin:22px 10px 0 0}
body.pad-on #ab{display:block}
#ab div{position:absolute;width:72px;height:72px;border-radius:50%;font-size:20px;background:#8a2045;color:#fff}
#ab div.on{background:#e0507a}
#ss{grid-area:ss;justify-self:center;grid-auto-flow:column;gap:22px;padding:18px 0 4px}
#ss div{width:78px;height:28px;border-radius:14px;font-size:11px;letter-spacing:.5px;transform:rotate(-20deg)}
#padBtn{position:fixed;right:max(8px,env(safe-area-inset-right));bottom:max(8px,env(safe-area-inset-bottom));
  background:#222;color:#999;border:0;border-radius:8px;padding:6px 10px;font-size:13px;opacity:.7;z-index:2}
@media (orientation:landscape){
  /* Side columns always keep room for the pad; the picture takes what is left. */
  #app{grid-template:"dpad screen ab" 1fr "ss screen ab" auto / minmax(170px,1fr) auto minmax(170px,1fr);
    padding-left:max(8px,env(safe-area-inset-left));padding-right:max(8px,env(safe-area-inset-right))}
  #screen{align-items:center}
  canvas{width:auto;height:calc(100dvh - max(6px,env(safe-area-inset-top)) - max(10px,env(safe-area-inset-bottom)));
    max-height:none;max-width:calc(100vw - 356px - env(safe-area-inset-left) - env(safe-area-inset-right))}
  #dpad{margin:0;grid-template:repeat(3,52px)/repeat(3,52px)}
  #ab{margin:0;width:150px;height:140px}
  #ab div{width:64px;height:64px}
  #ab div[data-b="2"]{left:4px !important;top:62px !important}
  #ab div[data-b="1"]{left:82px !important;top:14px !important}
  #ss{grid-auto-flow:row;gap:14px;padding:0 0 36px}
}
</style>
</head>
<body class="pad-on">
<div id="app">
  <div id="screen"><canvas id="c" width="160" height="144"></canvas></div>
  <div class="pad" id="dpad">
    <div class="hit" data-b="96"></div><div data-b="64">▲</div><div class="hit" data-b="80"></div>
    <div data-b="32">◀</div><div class="hit" data-b="0"></div><div data-b="16">▶</div>
    <div class="hit" data-b="160"></div><div data-b="128">▼</div><div class="hit" data-b="144"></div>
  </div>
  <div class="pad" id="ab"><div data-b="2" style="left:0;top:62px">B</div><div data-b="1" style="left:86px;top:12px">A</div></div>
  <div class="pad" id="ss"><div data-b="4">SELECT</div><div data-b="8">START</div></div>
</div>
<div id="status">Connecting to Cardputer…</div>
<button id="padBtn">Buttons</button>
<script>
const W = 160, H = 144;
const cv = document.getElementById('c'), ctx = cv.getContext('2d');
const img = ctx.createImageData(W, H), px = new Uint32Array(img.data.buffer);
const idx = new Uint8Array(W * H), pal = new Uint32Array(64);
const statusEl = document.getElementById('status');
let ws = null, buttons = 0;

function rgba(v) {  // RGB565 -> canvas pixel (little endian ABGR)
  const r = v >> 11, g = (v >> 5) & 63, b = v & 31;
  return 0xFF000000 | (((b << 3) | (b >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2));
}

function paintLine(y) {
  for (let o = y * W, e = o + W; o < e; o++) px[o] = pal[idx[o]];
}

function onFrame(buf) {
  const m = new Uint8Array(buf);
  if (m[0] !== 1) return;
  let i = 2, all = false;
  if (m[1] & 1) {
    const n = m[i++];
    for (let c = 0; c < n; c++, i += 2) pal[c] = rgba(m[i] | (m[i + 1] << 8));
    all = true;
  }
  const changed = [];
  while (i + 2 <= m.length) {
    const y = m[i], runs = m[i + 1];
    i += 2;
    let o = y * W;
    if (runs === 0) { idx.set(m.subarray(i, i + W), o); i += W; }
    else for (let k = 0; k < runs; k++, i += 2) { idx.fill(m[i + 1], o, o + m[i]); o += m[i]; }
    changed.push(y);
  }
  if (all) for (let y = 0; y < H; y++) paintLine(y);
  else for (const y of changed) paintLine(y);
  ctx.putImageData(img, 0, 0);
}

function connect() {
  ws = new WebSocket('ws://' + location.host + '/ws');
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { statusEl.textContent = ''; sendButtons(); };
  ws.onmessage = e => onFrame(e.data);
  ws.onclose = () => { statusEl.textContent = 'Lost Cardputer, reconnecting…'; setTimeout(connect, 1000); };
  ws.onerror = () => ws.close();
}

function sendButtons() {
  if (ws && ws.readyState === 1) ws.send(new Uint8Array([2, buttons]));
}

// Touch pad: a finger can slide between buttons, several fingers press several.
const held = new Map();  // pointerId -> button bit
function recompute() {
  let b = 0;
  for (const v of held.values()) b |= v;
  document.querySelectorAll('.pad div:not(.hit)').forEach(d => d.classList.toggle('on', (b & d.dataset.b) != 0));
  if (b !== buttons) { buttons = b; sendButtons(); }
}
function bitAt(x, y) {
  const el = document.elementFromPoint(x, y);
  return el && el.dataset && el.dataset.b !== undefined ? +el.dataset.b : 0;
}
function onPointer(e) {
  if (!document.body.classList.contains('pad-on')) return;
  if (e.type === 'pointerup' || e.type === 'pointercancel') held.delete(e.pointerId);
  else if (e.type === 'pointerdown' || held.has(e.pointerId)) held.set(e.pointerId, bitAt(e.clientX, e.clientY));
  recompute();
}
['pointerdown', 'pointermove', 'pointerup', 'pointercancel'].forEach(t => document.addEventListener(t, onPointer));
// The pad is on unless it was turned off last time (the choice is kept on the phone).
try { if (localStorage.getItem('pad') === 'off') document.body.classList.remove('pad-on'); } catch (e) {}
document.getElementById('padBtn').addEventListener('click', e => {
  e.stopPropagation();
  const on = document.body.classList.toggle('pad-on');
  try { localStorage.setItem('pad', on ? 'on' : 'off'); } catch (e) {}
  held.clear();
  recompute();
});

connect();
</script>
</body>
</html>
)html";
