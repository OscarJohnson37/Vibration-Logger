"use strict";
// Logger data viewer (logger_reader/viewer.py). Plain canvas charts: works offline.

const $ = id => document.getElementById(id);
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
const css = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
const AXIS_COLORS = ["--c1", "--c2", "--c3", "--accent", "--warn"];

async function api(path, opts) {
  const r = await fetch(path, opts);
  const d = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(d.detail || `HTTP ${r.status}`);
  return d;
}
function msg(t) { $("msg").textContent = t || ""; }
function fmtBytes(b) { return b > 1e9 ? (b / 1e9).toFixed(1) + " GB" : (b / 1e6).toFixed(0) + " MB"; }
function fmtDur(s) {
  if (s >= 86400) return `${Math.floor(s / 86400)} d ${Math.round((s % 86400) / 3600)} h`;
  if (s >= 3600) return `${Math.floor(s / 3600)} h ${Math.round((s % 3600) / 60)} min`;
  if (s >= 60) return `${Math.floor(s / 60)} min ${Math.round(s % 60)} s`;
  return `${s.toFixed(s < 10 ? 2 : 1)} s`;
}
const fmtTime = t => new Date(t * 1000).toLocaleString([], { year: "numeric", month: "short", day: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit" });
const fmtNum = (v, d = 4) => v == null || !isFinite(v) ? "–" : Math.abs(v) >= 1000 ? v.toFixed(0) : Number(v.toPrecision(d)).toString();

// ------------------------------------------------------------------ chart

// Time or linear x, auto y. Series: {name, color, x, y} lines (nulls break the line), or
// {name, color, x, lo, hi} min/max bands (waveforms). Decorations: spans (shaded), vlines, ticks.
class Chart {
  constructor(host, opts = {}) {
    this.host = host; this.opts = opts;
    this.canvas = document.createElement("canvas"); host.appendChild(this.canvas);
    this.tip = document.createElement("div"); this.tip.className = "tip"; host.appendChild(this.tip);
    this.series = []; this.deco = {}; this.hidden = new Set();
    this.pad = { l: 58, r: 12, t: 8, b: 26 };
    new ResizeObserver(() => this.draw()).observe(host);
    let drag = null;
    this.canvas.addEventListener("mousedown", e => { if (this.opts.onZoom) drag = { x0: this.px(e), x1: this.px(e) }; });
    window.addEventListener("mousemove", e => { if (drag) { drag.x1 = this.px(e); this.drag = drag; this.draw(); } });
    window.addEventListener("mouseup", () => {
      if (!drag) return;
      const a = Math.min(drag.x0, drag.x1), b = Math.max(drag.x0, drag.x1);
      drag = null; this.drag = null;
      if (b - a > 4) this.opts.onZoom(this.xVal(a), this.xVal(b)); else this.draw();
    });
    this.canvas.addEventListener("dblclick", () => this.opts.onZoomOut && this.opts.onZoomOut());
    this.canvas.addEventListener("mousemove", e => this.hover(e));
    this.canvas.addEventListener("mouseleave", () => { this.tip.style.display = "none"; this.hoverX = null; this.draw(); });
  }
  px(e) { const r = this.canvas.getBoundingClientRect(); return e.clientX - r.left; }
  set(series, xRange, deco = {}, yRange = null) { this.series = series; this.xr = xRange; this.deco = deco; this.yFixed = yRange; this.draw(); }
  xPix(x) { return this.pad.l + (x - this.xr[0]) / (this.xr[1] - this.xr[0]) * this.w; }
  xVal(p) { return this.xr[0] + (p - this.pad.l) / this.w * (this.xr[1] - this.xr[0]); }
  yPix(y) { const v = this.opts.logY ? Math.log10(Math.max(y, 1e-9)) : y; return this.pad.t + (1 - (v - this.yr[0]) / (this.yr[1] - this.yr[0])) * this.h; }
  yRange() {
    if (this.yFixed) return this.yFixed;
    let lo = Infinity, hi = -Infinity;
    for (const s of this.series) {
      if (this.hidden.has(s.name)) continue;
      for (const arr of [s.y, s.lo, s.hi]) if (arr) for (const v of arr) if (v != null && isFinite(v)) {
        const u = this.opts.logY ? Math.log10(Math.max(v, 1e-9)) : v; if (u < lo) lo = u; if (u > hi) hi = u;
      }
    }
    if (!isFinite(lo)) return [0, 1];
    if (this.opts.zeroY && !this.opts.logY) lo = Math.min(lo, 0);
    if (hi === lo) { hi += 1; lo -= this.opts.zeroY ? 0 : 1; }
    const m = (hi - lo) * 0.06; return [this.opts.zeroY && lo === 0 ? 0 : lo - m, hi + m];
  }
  draw() {
    const W = this.host.clientWidth, H = this.host.clientHeight, dpr = window.devicePixelRatio || 1;
    if (!W || !H) return;
    this.canvas.width = W * dpr; this.canvas.height = H * dpr;
    const g = this.canvas.getContext("2d"); g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, W, H);
    this.w = W - this.pad.l - this.pad.r; this.h = H - this.pad.t - this.pad.b;
    if (!this.xr) return;
    this.yr = this.yRange();
    const grid = css("--grid"), muted = css("--muted");
    g.font = "11px system-ui, sans-serif"; g.fillStyle = muted; g.strokeStyle = grid; g.lineWidth = 1;
    // y grid
    for (const v of niceTicks(this.yr[0], this.yr[1], Math.max(3, Math.floor(this.h / 45)))) {
      const y = this.pad.t + (1 - (v - this.yr[0]) / (this.yr[1] - this.yr[0])) * this.h;
      g.beginPath(); g.moveTo(this.pad.l, y); g.lineTo(W - this.pad.r, y); g.stroke();
      g.textAlign = "right"; g.textBaseline = "middle";
      g.fillText(this.opts.logY ? fmtNum(Math.pow(10, v), 2) : fmtNum(v, 3), this.pad.l - 6, y);
    }
    // x grid
    const xt = this.opts.time ? timeTicks(this.xr[0], this.xr[1], Math.max(2, Math.floor(this.w / 110)))
      : niceTicks(this.xr[0], this.xr[1], Math.max(2, Math.floor(this.w / 80))).map(v => ({ v, label: fmtNum(v, 4) + (this.opts.xUnit || "") }));
    g.textAlign = "center"; g.textBaseline = "top";
    for (const t of xt) {
      const x = this.xPix(t.v); if (x < this.pad.l - 1 || x > W - this.pad.r + 1) continue;
      g.beginPath(); g.moveTo(x, this.pad.t); g.lineTo(x, this.pad.t + this.h); g.stroke();
      g.fillText(t.label, x, this.pad.t + this.h + 6);
    }
    g.save(); g.beginPath(); g.rect(this.pad.l, this.pad.t, this.w, this.h); g.clip();
    // gaps
    g.fillStyle = css("--grid");
    for (const s of this.deco.spans || []) { const a = this.xPix(s[0]), b = this.xPix(s[1]); g.fillRect(a, this.pad.t, Math.max(1, b - a), this.h); }
    // series
    for (const s of this.series) {
      if (this.hidden.has(s.name)) continue;
      g.strokeStyle = s.color; g.fillStyle = s.color; g.lineWidth = s.width || 1.4; g.globalAlpha = s.alpha || 1;
      if (s.lo) {
        g.beginPath();
        for (let i = 0; i < s.x.length; i++) {
          const x = this.xPix(s.x[i]); const a = this.yPix(s.lo[i]), b = this.yPix(s.hi[i]);
          g.moveTo(x, a); g.lineTo(x, Math.abs(b - a) < 1 ? a + 1 : b);
        }
        g.stroke();
      }
      if (s.y) {
        g.beginPath(); let pen = false;
        for (let i = 0; i < s.x.length; i++) {
          const v = s.y[i]; if (v == null || !isFinite(v)) { pen = false; continue; }
          const x = this.xPix(s.x[i]), y = this.yPix(v);
          if (pen) g.lineTo(x, y); else { g.moveTo(x, y); pen = true; }
        }
        g.stroke();
      }
      g.globalAlpha = 1;
    }
    // restarts, notes, clipping
    for (const v of this.deco.vlines || []) {
      const x = this.xPix(v.t); g.strokeStyle = v.color || muted; g.setLineDash(v.dash ? [4, 4] : []); g.lineWidth = 1;
      g.beginPath(); g.moveTo(x, this.pad.t); g.lineTo(x, this.pad.t + this.h); g.stroke(); g.setLineDash([]);
      if (v.flag) { g.fillStyle = v.color || muted; g.beginPath(); g.moveTo(x, this.pad.t); g.lineTo(x + 8, this.pad.t + 4); g.lineTo(x, this.pad.t + 8); g.fill(); }
    }
    g.fillStyle = css("--err");
    for (const t of this.deco.ticks || []) g.fillRect(this.xPix(t) - 0.5, this.pad.t + this.h - 6, 1.5, 6);
    if (this.drag) { g.fillStyle = css("--sel"); g.fillRect(Math.min(this.drag.x0, this.drag.x1), this.pad.t, Math.abs(this.drag.x1 - this.drag.x0), this.h); }
    if (this.hoverX != null) { g.strokeStyle = muted; g.beginPath(); g.moveTo(this.hoverX, this.pad.t); g.lineTo(this.hoverX, this.pad.t + this.h); g.stroke(); }
    g.restore();
    if (this.opts.yLabel) { g.save(); g.fillStyle = muted; g.translate(11, this.pad.t + this.h / 2); g.rotate(-Math.PI / 2); g.textAlign = "center"; g.textBaseline = "middle"; g.fillText(this.opts.yLabel, 0, 0); g.restore(); }
  }
  hover(e) {
    if (!this.xr || !this.series.length) return;
    const p = this.px(e); if (p < this.pad.l || p > this.pad.l + this.w) { this.tip.style.display = "none"; return; }
    const xv = this.xVal(p); this.hoverX = p; this.draw();
    const rows = [];
    for (const s of this.series) {
      if (this.hidden.has(s.name) || s.noTip) continue;
      const i = nearest(s.x, xv); if (i < 0) continue;
      const val = s.y ? s.y[i] : null;
      const txt = s.lo ? `${fmtNum(s.lo[i])} … ${fmtNum(s.hi[i])}` : fmtNum(val);
      rows.push(`<span class="sw" style="background:${s.color}"></span> ${esc(s.name)}: <b>${txt}</b>${s.unit ? " " + esc(s.unit) : ""}`);
    }
    const head = this.opts.time ? fmtTime(xv) : `${fmtNum(xv, 5)}${this.opts.xUnit || ""}`;
    this.tip.innerHTML = `<div class="muted">${esc(head)}</div>${rows.join("<br>")}`;
    this.tip.style.display = "block";
    const left = p + 14 + this.tip.offsetWidth > this.host.clientWidth ? p - 14 - this.tip.offsetWidth : p + 14;
    this.tip.style.left = left + "px"; this.tip.style.top = "6px";
  }
}
function nearest(xs, v) {
  if (!xs || !xs.length) return -1;
  let lo = 0, hi = xs.length - 1;
  while (hi - lo > 1) { const m = (lo + hi) >> 1; if (xs[m] < v) lo = m; else hi = m; }
  return Math.abs(xs[lo] - v) <= Math.abs(xs[hi] - v) ? lo : hi;
}
function niceTicks(lo, hi, n) {
  const span = hi - lo; if (!(span > 0)) return [lo];
  const step0 = span / n, mag = Math.pow(10, Math.floor(Math.log10(step0)));
  const step = [1, 2, 2.5, 5, 10].map(m => m * mag).find(s => s >= step0) || 10 * mag;
  const out = []; for (let v = Math.ceil(lo / step) * step; v <= hi + step * 1e-9; v += step) out.push(Math.abs(v) < step * 1e-9 ? 0 : v);
  return out;
}
// Time ticks on local-time boundaries (seconds … days)
function timeTicks(t0, t1, n) {
  const steps = [0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200, 86400, 172800, 604800];
  const step = steps.find(s => (t1 - t0) / s <= n) || 604800;
  const tz = -new Date(t0 * 1000).getTimezoneOffset() * 60;   // local midnight alignment
  const out = [];
  for (let v = Math.ceil((t0 + tz) / step) * step - tz; v <= t1; v += step) {
    const d = new Date(v * 1000);
    let label;
    if (step >= 86400) label = d.toLocaleDateString([], { month: "short", day: "numeric" });
    else if (step >= 60) label = (d.getHours() === 0 && d.getMinutes() === 0) ? d.toLocaleDateString([], { month: "short", day: "numeric" })
      : d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
    else if (step >= 1) label = d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
    else label = d.toLocaleTimeString([], { minute: "2-digit", second: "2-digit" }) + "." + String(Math.round((v % 1 + 1) % 1 * 1000)).padStart(3, "0");
    out.push({ v, label });
  }
  return out;
}

// ------------------------------------------------------------------ sessions list

let S = null;            // the open session's info
let range = null;        // [t0, t1] being shown
const stack = [];        // zoom history
let metric = "rms";
let overviewData = null, specData = null;

async function showList() {
  $("session").hidden = true; $("list").hidden = false; history.replaceState(null, "", "#");
  try {
    const d = await api("/api/sessions?refresh=1");
    $("folder").textContent = d.folder;
    $("sessions").innerHTML = d.sessions.map(s => `<tr class="click" data-key="${esc(s.key)}">
      <td><b>${esc(s.name)}</b></td><td>${esc(s.logger)}</td><td>${s.boots.length}</td><td>${s.files}</td>
      <td>${fmtBytes(s.bytes)}${s.unreadable ? ` <span class="err-text">(${s.unreadable} unreadable)</span>` : ""}</td>
      <td>${s.cached === s.files ? '<span class="ok-text">yes</span>' : `${s.cached} of ${s.files}`}</td></tr>`).join("")
      || `<tr><td colspan="6" class="muted">No .dlog files found in this folder.</td></tr>`;
    document.querySelectorAll("#sessions tr.click").forEach(tr => tr.onclick = () => openSession(tr.dataset.key));
  } catch (e) { msg(e.message); }
}
$("home").onclick = e => { e.preventDefault(); showList(); };

// ------------------------------------------------------------------ one session

async function openSession(key) {
  msg(""); $("list").hidden = true; $("session").hidden = false; $("body").hidden = true; $("detail").hidden = true;
  history.replaceState(null, "", "#" + encodeURIComponent(key));
  stack.length = 0;
  let info = await api(`/api/sessions/${encodeURIComponent(key)}`);
  $("title").textContent = `${info.name} · ${info.logger}`;
  if (!info.ready) {
    await api(`/api/sessions/${encodeURIComponent(key)}/prepare`, { method: "POST" });
    $("prep").hidden = false;
    while (true) {
      await new Promise(r => setTimeout(r, 1000));
      info = await api(`/api/sessions/${encodeURIComponent(key)}`);
      const j = info.job || {};
      $("prepText").textContent = j.state === "failed" ? `Scanning failed: ${j.error}` :
        `Scanning the files into summaries (once; cached on this computer): ${j.done || 0} of ${j.total || "?"}…`;
      $("prepBar").style.width = j.total ? (100 * j.done / j.total) + "%" : "3%";
      if (info.ready || j.state === "failed") break;
    }
    if (!info.ready) return;
    $("prep").hidden = true;
  }
  S = info; S.key = key;
  const dur = S.t_end - S.t_start;
  $("span").textContent = `${fmtTime(S.t_start)} → ${fmtTime(S.t_end)}`;
  const facts = [
    ["Recorded", `${fmtDur(S.seconds)} of data over ${fmtDur(dur)}`],
    ["Rate", `${S.rate_hz} Hz, ${S.channels.length} axes (${S.channels.join(", ")}) in ${S.unit}${S.range_g ? `, range ±${S.range_g} g` : ""}`],
    ["Power-ons", S.boots.map(b => `${b.boot}${b.synced ? "" : " (clock not set)"}`).join(", ")],
    ["Clipping", S.clipped_frames ? `<span class="warn-text">${S.clipped_frames.toLocaleString()} samples at full scale</span>` : "none"],
    ["Gaps", S.gap_seconds ? `${S.gap_seconds} s with lost samples` : "none"],
  ];
  if (S.markers.length) facts.push(["Notes", S.markers.filter(m => m.type === "note").map(m => esc(m.text)).join("; ") || `${S.markers.length} markers`]);
  if (S.notes.length) facts.push(["File issues", esc(S.notes.join("; "))]);
  if (S.unreadable && S.unreadable.length) facts.push(["Unreadable files", `<span class="err-text">${S.unreadable.length} skipped: ` +
    S.unreadable.map(u => esc(u.file)).join(", ") + `</span> (damaged storage; copy the card's files off and check it)`]);
  $("facts").innerHTML = facts.map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join("");
  $("sgAxis").innerHTML = S.channels.map(c => `<option>${esc(c)}</option>`).join("");
  $("legend").innerHTML = S.channels.map((c, i) => `<label><input type="checkbox" checked data-ch="${esc(c)}"><span class="sw" style="background:${css(AXIS_COLORS[i])}"></span>${esc(c)}</label>`).join("");
  $("legend").querySelectorAll("input").forEach(cb => cb.onchange = () => {
    for (const ch of [ovChart, waveChart, specChart]) cb.checked ? ch.hidden.delete(cb.dataset.ch) : ch.hidden.add(cb.dataset.ch);
    [ovChart, waveChart, specChart].forEach(c => c.draw());
  });
  $("body").hidden = false;
  setRange(S.t_start, S.t_end, false);
}

const ovChart = new Chart($("ovChart"), { time: true, zeroY: true, onZoom: (a, b) => setRange(a, b), onZoomOut: zoomOut });
const batChart = new Chart($("batChart"), { time: true, yLabel: "%", onZoom: (a, b) => setRange(a, b), onZoomOut: zoomOut });
const waveChart = new Chart($("waveChart"), { time: true, onZoom: (a, b) => setRange(a, b), onZoomOut: zoomOut });
const specChart = new Chart($("specChart"), { xUnit: " Hz", zeroY: true });
batChart.pad.t = 4;

function setRange(t0, t1, push = true) {
  t0 = Math.max(S.t_start, t0); t1 = Math.min(S.t_end, t1);
  if (t1 - t0 < 0.05) t1 = t0 + 0.05;
  if (push && range) stack.push(range);
  range = [t0, t1];
  loadOverview(); loadDetail();
}
function zoomOut() {
  if (stack.length) { range = stack.pop(); loadOverview(); loadDetail(); return; }
  const span = range[1] - range[0], mid = (range[0] + range[1]) / 2;
  setRange(mid - span * 1.5, mid + span * 1.5, false);
}
$("zoomOut").onclick = zoomOut;
$("zoomAll").onclick = () => setRange(S.t_start, S.t_end);
document.querySelectorAll("[data-metric]").forEach(b => b.onclick = () => {
  metric = b.dataset.metric;
  document.querySelectorAll("[data-metric]").forEach(x => x.classList.toggle("on", x === b));
  drawOverview();
});

let ovToken = 0;
async function loadOverview() {
  const tok = ++ovToken, [t0, t1] = range;
  const bins = Math.max(200, Math.min(3000, Math.round($("ovChart").clientWidth)));
  try {
    const d = await api(`/api/sessions/${encodeURIComponent(S.key)}/overview?t0=${t0}&t1=${t1}&bins=${bins}`);
    if (tok !== ovToken) return;
    overviewData = d; drawOverview();
  } catch (e) { msg(e.message); }
}
function drawOverview() {
  const d = overviewData; if (!d) return;
  const unit = S.unit;
  const series = [];
  S.channels.forEach((c, i) => {
    const ch = d.channels[c], color = css(AXIS_COLORS[i]);
    if (metric === "rms") {
      series.push({ name: c, color, x: d.t, y: ch.rms, unit });
      series.push({ name: c, color, x: d.t, y: ch.rms_max, unit, alpha: 0.25, noTip: true });
    } else if (metric === "peak") series.push({ name: c, color, x: d.t, y: ch.peak, unit });
    else series.push({ name: c, color, x: d.t, y: ch.mean, unit });
  });
  // gaps: runs of empty bins
  const spans = []; const first = d.channels[S.channels[0]].rms;
  for (let i = 0; i < first.length; i++) if (first[i] == null) {
    let j = i; while (j + 1 < first.length && first[j + 1] == null) j++;
    spans.push([d.t[i] - d.width_s / 2, d.t[j] + d.width_s / 2]); i = j;
  }
  const vlines = d.boots.slice(1).map(b => ({ t: b.t0, dash: true }))
    .concat(d.markers.filter(m => m.type === "note" || m.type === "event").map(m => ({ t: m.t, flag: true, color: css("--accent") })));
  const ticks = (d.clipped || []).map((c, i) => c ? d.t[i] : null).filter(v => v != null);
  ovChart.opts.yLabel = { rms: "RMS", peak: "peak", mean: "mean" }[metric] + ` (${unit})`;
  ovChart.opts.zeroY = metric !== "mean";
  ovChart.set(series, [d.t0, d.t1], { spans, vlines, ticks });
  if (d.battery) {
    const pi = d.battery.channels.indexOf("percent");
    batChart.set([{ name: "battery", color: css("--ok"), x: d.battery.t, y: d.battery.values.map(v => v[pi >= 0 ? pi : 0]), unit: pi >= 0 ? "%" : "" }],
      [d.t0, d.t1], { vlines: d.boots.slice(1).map(b => ({ t: b.t0, dash: true })) });
  }
}

// ------------------------------------------------------------------ zoomed in: waveform, spectrum, spectrogram

let detToken = 0;
async function loadDetail() {
  const [t0, t1] = range;
  if (t1 - t0 > S.max_window_s) { $("detail").hidden = true; return; }
  $("detail").hidden = false;
  $("winSpan").textContent = `${fmtTime(t0)} · ${fmtDur(t1 - t0)}`;
  const tok = ++detToken, k = encodeURIComponent(S.key);
  try {
    const w = await api(`/api/sessions/${k}/window?t0=${t0}&t1=${t1}&points=${Math.round($("waveChart").clientWidth)}`);
    if (tok !== detToken) return;
    const series = S.channels.map((c, i) => ({ name: c, color: css(AXIS_COLORS[i]), x: w.channels[c].t, lo: w.channels[c].min, hi: w.channels[c].max, unit: w.unit, width: 1 }));
    waveChart.opts.yLabel = w.unit;
    waveChart.set(series, [t0, t1], { ticks: w.clipped_t || [] });
    const st = w.stats;
    $("stats").innerHTML = S.channels.map((c, i) => { const s = st[c]; return s ? `<tr><td><span class="sw" style="background:${css(AXIS_COLORS[i])}"></span> ${esc(c)}</td>
      <td>${fmtNum(s.mean)}</td><td>${fmtNum(s.rms)}</td><td>${fmtNum(s.peak)}</td><td>${fmtNum(s.crest, 3)}</td><td>${fmtNum(s.min)}</td><td>${fmtNum(s.max)}</td></tr>` : ""; }).join("");
    $("statsNote").textContent = `${st._frames.toLocaleString()} samples, values in ${w.unit}. RMS and peak are about the mean (vibration only; gravity removed).` +
      (st._clipped_frames ? ` ${st._clipped_frames} samples hit full scale (red ticks): the true peaks there were higher.` : "");
  } catch (e) { msg(e.message); }
  loadSpectrum(tok); loadSpectrogram(tok);
}
async function loadSpectrum(tok) {
  const [t0, t1] = range;
  try {
    const d = await api(`/api/sessions/${encodeURIComponent(S.key)}/spectrum?t0=${t0}&t1=${t1}&nfft=${$("nfft").value}`);
    if (tok !== detToken) return;
    specData = d; drawSpectrum();
  } catch (e) { $("specHint").textContent = e.message; }
}
function drawSpectrum() {
  const d = specData; if (!d) return;
  const log = $("scale").value === "log";
  specChart.opts.logY = log; specChart.opts.zeroY = !log;
  specChart.opts.yLabel = `amplitude (${d.unit})`;
  const series = S.channels.map((c, i) => ({ name: c, color: css(AXIS_COLORS[i]), x: d.freq, y: d.channels[c], unit: d.unit, width: 1.1 }));
  specChart.set(series, [0, d.freq[d.freq.length - 1]]);
  // Strongest peaks, for the hint
  const peaks = S.channels.map(c => { const a = d.channels[c]; let bi = 1; for (let i = 2; i < a.length; i++) if (a[i] > a[bi]) bi = i; return `${c}: ${fmtNum(d.freq[bi], 4)} Hz (${fmtNum(a[bi], 3)} ${d.unit})`; });
  $("specHint").textContent = `Strongest: ${peaks.join(" · ")}. Resolution ${fmtNum(d.freq[1], 3)} Hz.`;
}
$("nfft").onchange = () => loadSpectrum(detToken);
$("scale").onchange = drawSpectrum;
$("sgAxis").onchange = () => loadSpectrogram(detToken);

async function loadSpectrogram(tok) {
  const [t0, t1] = range, host = $("sgChart");
  try {
    const d = await api(`/api/sessions/${encodeURIComponent(S.key)}/spectrogram?t0=${t0}&t1=${t1}&channel=${encodeURIComponent($("sgAxis").value)}`);
    if (tok !== detToken) return;
    drawSpectrogram(host, d);
  } catch (e) { host.innerHTML = `<p class="hint">${esc(e.message)}</p>`; }
}
function drawSpectrogram(host, d) {
  host.innerHTML = "";
  const c = document.createElement("canvas"); host.appendChild(c);
  const W = host.clientWidth, H = host.clientHeight, dpr = window.devicePixelRatio || 1, pad = { l: 58, r: 12, t: 8, b: 26 };
  c.width = W * dpr; c.height = H * dpr; c.style.width = W + "px"; c.style.height = H + "px";
  const g = c.getContext("2d"); g.setTransform(dpr, 0, 0, dpr, 0, 0);
  const bytes = Uint8Array.from(atob(d.data), ch => ch.charCodeAt(0));
  const img = new ImageData(d.cols, d.rows);
  for (let col = 0; col < d.cols; col++) for (let row = 0; row < d.rows; row++) {
    const v = bytes[col * d.rows + row], o = ((d.rows - 1 - row) * d.cols + col) * 4, [r, gg, b] = heat(v / 255);
    img.data[o] = r; img.data[o + 1] = gg; img.data[o + 2] = b; img.data[o + 3] = 255;
  }
  const off = document.createElement("canvas"); off.width = d.cols; off.height = d.rows; off.getContext("2d").putImageData(img, 0, 0);
  g.imageSmoothingEnabled = true;
  g.drawImage(off, pad.l, pad.t, W - pad.l - pad.r, H - pad.t - pad.b);
  g.fillStyle = css("--muted"); g.font = "11px system-ui, sans-serif";
  g.textAlign = "right"; g.textBaseline = "middle";
  for (const f of niceTicks(0, d.freq_max, 5)) g.fillText(fmtNum(f, 4) + " Hz", pad.l - 6, pad.t + (1 - f / d.freq_max) * (H - pad.t - pad.b));
  g.textAlign = "center"; g.textBaseline = "top";
  for (const t of timeTicks(d.t0, d.t1, Math.max(2, Math.floor((W - pad.l - pad.r) / 110))))
    g.fillText(t.label, pad.l + (t.v - d.t0) / (d.t1 - d.t0) * (W - pad.l - pad.r), H - pad.b + 6);
}
// Dark blue → purple → orange → yellow
function heat(x) {
  const stops = [[13, 8, 135], [126, 3, 168], [204, 71, 120], [248, 149, 64], [240, 249, 33]];
  const p = Math.min(0.9999, Math.max(0, x)) * (stops.length - 1), i = Math.floor(p), f = p - i;
  return stops[i].map((v, k) => Math.round(v + (stops[i + 1][k] - v) * f));
}

$("exportCsv").onclick = () => {
  const [t0, t1] = range;
  location.href = `/api/sessions/${encodeURIComponent(S.key)}/export?t0=${t0}&t1=${t1}`;
};

// ------------------------------------------------------------------ start
const startKey = decodeURIComponent(location.hash.slice(1));
if (startKey) openSession(startKey).catch(e => { msg(e.message); showList(); }); else showList();
