"use strict";
// Master dashboard. State comes from the fleet service over a WebSocket (snapshot, then
// logger/event/data updates); actions go through the REST API.

const $ = id => document.getElementById(id);
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
const loggers = new Map();        // device_id -> summary
const series = new Map();         // `${id}/${pub}` -> [message, ...]
const SERIES_MAX = 1200;
let features = {};                // feature definitions
let current = null;               // device_id in the detail view
let tab = "overview";
let selected = new Set();
let masterOffsetMs = 0;           // master clock - browser clock

// ---------- helpers ----------
const SESSION_RE = /[^A-Za-z0-9_-]/g;
const slug = s => s.trim().replace(SESSION_RE, "_").slice(0, 40);
function ago(sec) {
  if (sec == null) return "never";
  if (sec < 60) return `${Math.round(sec)} s ago`;
  if (sec < 3600) return `${Math.round(sec / 60)} min ago`;
  if (sec < 86400) return `${(sec / 3600).toFixed(1)} h ago`;
  return `${(sec / 86400).toFixed(1)} days ago`;
}
const nowS = () => Date.now() / 1000;
const feats = lg => (lg.status && lg.status.features) || {};
const hasFeature = (lg, f) => (lg.info.features || []).some(x => x.feature === f);
function stateOf(lg) {
  if (lg.online === false) return "offline";
  return (lg.status && lg.status.state) || "unknown";
}
async function api(path, body, method) {
  const opt = body === undefined ? {} : { method: method || "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) };
  const r = await fetch(path, opt);
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(data.detail || `HTTP ${r.status}`);
  return data;
}
function showResult(el, res, okOverride) {
  const ok = okOverride ?? res.ok;
  el.innerHTML = `<div class="result ${ok ? "ok" : "err"}">${esc(res.summary || res.message || JSON.stringify(res))}</div>`;
}
function showError(el, e) { el.innerHTML = `<div class="result err">${esc(e.message || e)}</div>`; }

// ---------- live connection ----------
function connect() {
  const ws = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`);
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data);
    if (m.type === "snapshot") {
      loggers.clear();
      m.loggers.forEach(l => loggers.set(l.device_id, l));
      setBroker(m.broker_connected);
      renderFleet();
      if (current) openDetail(current, true);
      else if (location.hash === "#firmware") showPage("firmware");
      else if (location.hash.length > 1) {
        const [id, t] = location.hash.slice(1).split("/");
        if (loggers.has(id)) openDetail(id).then(() => t && setTab(t));
      }
    } else if (m.type === "logger") {
      loggers.set(m.logger.device_id, m.logger);
      renderFleetSoon();
      renderFirmwareSoon();
      if (m.logger.device_id === current) renderDetailSoon();
    } else if (m.type === "data") {
      addPoint(m.device_id, m.name, m.message);
      if (m.device_id === current) { if (tab === "live") drawPlotSoon(); if (tab === "orientation") updateOrientation(); }
    } else if (m.type === "event") {
      if (m.device_id === current && detailData) { detailData.events.push(m.event); if (tab === "events") renderEvents(); }
      if (m.event.source === "ota") renderFirmwareSoon();
    } else if (m.type === "broker") {
      setBroker(m.connected);
    }
  };
  ws.onclose = () => { setBroker(null); setTimeout(connect, 2000); };
}
function setBroker(up) {
  $("brokerDot").className = "dot " + (up ? "ok" : up === false ? "err" : "warn");
  $("brokerText").textContent = up ? "broker connected" : up === false ? "broker disconnected" : "dashboard reconnecting…";
}
function addPoint(id, name, msg) {
  const k = `${id}/${name}`;
  let s = series.get(k);
  if (!s) series.set(k, s = []);
  s.push(msg);
  if (s.length > SERIES_MAX) s.splice(0, s.length - SERIES_MAX);
}
let fleetTimer = null, detailTimer = null, plotTimer = null;
const renderFleetSoon = () => { if (!fleetTimer) fleetTimer = setTimeout(() => { fleetTimer = null; renderFleet(); }, 250); };
const renderDetailSoon = () => { if (!detailTimer) detailTimer = setTimeout(() => { detailTimer = null; renderDetail(); }, 250); };
const drawPlotSoon = () => { if (!plotTimer) plotTimer = requestAnimationFrame(() => { plotTimer = null; drawPlot(); }); };

// ---------- fleet view ----------
function allGroups() {
  const g = new Set();
  loggers.forEach(l => (l.groups || []).forEach(x => g.add(x)));
  return [...g].sort();
}
function fillGroupSelect(sel, first) {
  const v = sel.value, groups = allGroups();
  sel.innerHTML = (first ? `<option value="">${first}</option>` : "") + groups.map(g => `<option>${esc(g)}</option>`).join("");
  if ([...sel.options].some(o => o.value === v)) sel.value = v;
}
function battHtml(b) {
  if (!b || !b.present) return "–";
  const p = Math.round(b.percent), cls = p < 10 ? "critical" : p < 30 ? "low" : "";
  const power = b.usb_power ? (b.charging ? " · charging" : b.full ? " · full" : " · USB") : "";
  return `<span class="batt ${cls}"><i style="width:${Math.max(0, Math.min(100, p)) * 0.38}px"></i></span>${p}%` +
         `<span class="muted"> ${b.voltage != null ? b.voltage.toFixed(2) + " V" : ""}${power}</span>`;
}
function recHtml(r, lg) {
  if (!r) return "–";
  if (r.recording) return `<span class="ok-text">● recording</span> ${esc(r.session || "unnamed")}<br><span class="muted small">${esc(r.file || "")}</span>`;
  if (r.countdown_s != null) return `${r.autostart === "resuming" ? "resuming after a restart in" : "starts in"} ${r.countdown_s} s · ${esc(r.session || (lg.config.recording || {}).session || "auto-named")}`;
  if (r.autostart === "waiting_for_master") return `<span class="muted">waiting for the master before auto-starting</span>`;
  const why = r.autostart === "skipped_master" ? " · auto-start skipped: master in control" : "";
  return `<span class="muted">not recording${why}</span>${r.session ? " · next: " + esc(r.session) : ""}`;
}
function clockHtml(core) {
  const t = core && core.time;
  if (!t) return "–";
  if (!t.synced) return `<span class="warn-text">not set</span>`;
  return `${esc(t.source || "set")}${t.last_sync_age_s != null ? `, ${ago(t.last_sync_age_s)}` : ""}`;
}
function sensorsHtml(f) {
  const bits = [];
  for (const [k, v] of Object.entries(f)) {
    if (v && typeof v.ok === "boolean") bits.push(v.ok ? `<span class="ok-text">${esc(k)} OK</span>` : `<span class="err-text">${esc(k)} fault</span>`);
  }
  return bits.join(" · ") || "–";
}
function cardHtml(lg) {
  const st = stateOf(lg), f = feats(lg), dev = lg.info.device || {};
  const seen = lg.online ? "online" : lg.online === false ? `offline${lg.last_seen ? ", " + ago(nowS() - lg.last_seen) : ""}` : "waiting for a live update";
  const sd = f.sd ? (f.sd.present ? `${(f.sd.free_mb / 1024).toFixed(1)} GB free` : `<span class="err-text">no card</span>`) : "–";
  const wifi = f.wifi ? (f.wifi.rssi_dbm != null ? `${f.wifi.rssi_dbm} dBm` : esc(f.wifi.mode || "")) : "–";
  const where = lg.registry && lg.registry.assignment && lg.registry.assignment.location;
  const warn = (lg.mismatches || []).concat((lg.status.errors || []).map(e => typeof e === "string" ? e : JSON.stringify(e)));
  return `<div class="card ${lg.online === false ? "offline" : ""}" data-id="${esc(lg.device_id)}">
    <div class="top">
      <input type="checkbox" data-sel="${esc(lg.device_id)}" ${selected.has(lg.device_id) ? "checked" : ""}>
      <div style="flex:1;min-width:0">
        <div class="name">${esc(lg.label)}${dev.name && dev.name !== lg.label ? ` <span class="muted">· ${esc(dev.name)}</span>` : ""}</div>
        <div class="id">${lg.model ? `${esc(lg.model)}${lg.hardware_rev ? " rev " + esc(lg.hardware_rev) : ""} · ` : ""}${esc(lg.device_id)} · ${esc(dev.type || "?")} · fw ${esc(dev.firmware || "?")}</div>
      </div>
      <span class="pill ${st}">${st}</span>
    </div>
    <dl class="kv">
      <dt>Seen</dt><dd>${seen}</dd>
      ${where ? `<dt>Location</dt><dd>${esc(where)}</dd>` : ""}
      <dt>Recording</dt><dd>${recHtml(f.recording, lg)}</dd>
      <dt>Battery</dt><dd>${battHtml(f.battery)}</dd>
      <dt>SD card</dt><dd>${sd}</dd>
      <dt>Sensors</dt><dd>${sensorsHtml(f)}</dd>
      <dt>WiFi</dt><dd>${wifi}</dd>
      <dt>Clock</dt><dd>${clockHtml(f.core)}</dd>
      ${otaHtml(lg.ota) ? `<dt>Firmware</dt><dd>${otaHtml(lg.ota)}</dd>` : ""}
    </dl>
    ${warn.length ? `<ul class="warnings">${warn.map(w => `<li>${esc(w)}</li>`).join("")}</ul>` : ""}
  </div>`;
}
function renderFleet() {
  fillGroupSelect($("groupFilter"), "all groups");
  fillGroupSelect($("selGroup"));
  const g = $("groupFilter").value;
  const list = [...loggers.values()].filter(l => !g || (l.groups || []).includes(g))
    .sort((a, b) => a.label.localeCompare(b.label));
  $("cards").innerHTML = list.map(cardHtml).join("");
  $("empty").hidden = loggers.size > 0;
  const on = [...loggers.values()].filter(l => l.online).length;
  $("fleetCount").textContent = `${loggers.size} logger${loggers.size === 1 ? "" : "s"}, ${on} online`;
}
$("cards").addEventListener("click", ev => {
  const cb = ev.target.closest("[data-sel]");
  if (cb) { cb.checked ? selected.add(cb.dataset.sel) : selected.delete(cb.dataset.sel); ev.stopPropagation(); return; }
  const card = ev.target.closest(".card");
  if (card) openDetail(card.dataset.id);
});
$("groupFilter").onchange = renderFleet;
$("selMode").onchange = () => { $("selGroup").hidden = $("selMode").value !== "group"; };
$("fleetSession").oninput = () => {
  const v = $("fleetSession").value;
  $("sessionHint").textContent = v && slug(v) !== v ? `Will be saved as “${slug(v)}” (letters, digits, - and _ only).` : "";
};
function fleetSelection() {
  const mode = $("selMode").value;
  if (mode === "all") return { all: true };
  if (mode === "group") return { group: $("selGroup").value };
  if (!selected.size) throw new Error("Tick the loggers to act on, or choose a group or All.");
  return { ids: [...selected] };
}
document.querySelectorAll("[data-fleet]").forEach(btn => btn.onclick = async () => {
  const out = $("fleetResult");
  try {
    const selection = fleetSelection(), action = btn.dataset.fleet;
    let res;
    if (action === "set_clocks") res = await api("/api/time/loggers", { selection });
    else {
      const args = {};
      if (action === "start" && $("fleetSession").value.trim()) args.session = slug($("fleetSession").value);
      if (action === "mark") { const t = prompt("Marker text"); if (t == null) return; args.text = t; }
      res = await api("/api/command", { selection, cmd: action, args });
    }
    showResult(out, res);
  } catch (e) { showError(out, e); }
});

// ---------- detail view ----------
let detailData = null;
async function openDetail(id, keepTab) {
  current = id;
  document.body.classList.add("detail");
  document.body.classList.remove("firmware");
  if (!keepTab) setTab("overview");
  try {
    detailData = await api(`/api/loggers/${id}`);
    for (const [name, msgs] of Object.entries(detailData.publication_data || {})) series.set(`${id}/${name}`, msgs.slice(-SERIES_MAX));
  } catch (e) { detailData = { events: [] }; }
  configDirty = false;
  renderDetail(true);
  window.scrollTo(0, 0);
}
$("back").onclick = () => { current = null; document.body.classList.remove("detail"); history.replaceState(null, "", "#"); renderFleet(); };
function setTab(t) {
  tab = t;
  if (current) history.replaceState(null, "", `#${current}/${t}`);
  document.querySelectorAll("#tabs button").forEach(b => b.classList.toggle("on", b.dataset.tab === t));
  document.querySelectorAll("[data-pane]").forEach(p => p.hidden = p.dataset.pane !== t);
  if (t === "live") { fillPubSelect(); drawPlot(); }
  if (t === "orientation") { initScene(); updateOrientation(); }
  if (t === "events") renderEvents();
  if (t === "about") renderAbout();
}
$("tabs").onclick = ev => { const b = ev.target.closest("button"); if (b) setTab(b.dataset.tab); };

function renderDetail(full) {
  const lg = loggers.get(current);
  if (!lg) return;
  const dev = lg.info.device || {};
  $("dName").textContent = `${lg.label}${dev.name && dev.name !== lg.label ? " · " + dev.name : ""}`;
  const model = lg.model ? `${lg.model}${lg.hardware_rev ? " rev " + lg.hardware_rev : ""} · ` : "";
  $("dSub").textContent = `${model}${lg.device_id} · ${dev.type || "?"} · ${dev.hardware || ""} · fw ${dev.firmware || "?"} · ${stateOf(lg)}`;
  document.querySelectorAll("#tabs [data-needs]").forEach(b => b.hidden = !hasFeature(lg, b.dataset.needs));
  renderOverview(lg);
  renderControls(lg);
  if (full || !configDirty) renderConfig(lg);
  if (full) {
    renderRegistry(lg);
    if (lg.model && !modelInfo.has(lg.model))   // for the registry form's "model's list" hint
      fetchModel(lg.model).then(() => { if (current === lg.device_id) renderRegistry(lg); });
    if (tab === "about") renderAbout();
  }
  if (tab === "live") fillPubSelect();
}

function valueText(v) {
  if (v == null) return "–";
  if (typeof v === "number") return Number.isInteger(v) ? v.toLocaleString() : v.toFixed(3);
  if (typeof v === "object") return JSON.stringify(v);
  return String(v);
}
function renderOverview(lg) {
  const f = feats(lg), out = [];
  const head = [["State", stateOf(lg)], ["Uptime", lg.status.uptime_ms != null ? ago(lg.status.uptime_ms / 1000).replace(" ago", "") : "–"],
    ["Boot", lg.status.boot_id + (((lg.status.features || {}).core || {}).reset_reason ? ` (${lg.status.features.core.reset_reason.replace("_", " ")})` : "")], ["Groups", (lg.groups || []).join(", ") || "–"],
    ["Last seen", lg.last_seen ? ago(nowS() - lg.last_seen) : "–"],
    ["Firmware", (lg.firmware || "?") + (lg.ota && lg.ota.state && lg.ota.state !== "idle" ? ` (${lg.ota.state}${lg.ota.state === "downloading" ? ` ${lg.ota.progress_pct || 0}%` : ""})` : "")]];
  out.push(`<div class="panel"><h2>Logger</h2><dl class="kv">${head.map(([k, v]) => `<dt>${k}</dt><dd>${esc(valueText(v))}</dd>`).join("")}</dl>
    ${(lg.mismatches || []).length ? `<ul class="warnings">${lg.mismatches.map(w => `<li>${esc(w)}</li>`).join("")}</ul>` : ""}</div>`);
  for (const [inst, st] of Object.entries(f)) {
    const fe = (lg.info.features || []).find(x => x.instance === inst) || {};
    const def = features[fe.feature] || {};
    const rows = Object.entries(st || {}).map(([k, v]) => {
      const unit = def.status && def.status[k] && def.status[k].unit;
      return `<dt>${esc(k)}</dt><dd>${esc(valueText(v))}${unit && typeof v === "number" ? " " + esc(unit) : ""}</dd>`;
    }).join("");
    out.push(`<div class="panel"><h2>${esc(inst)}${fe.impl ? ` · ${esc(fe.impl)}` : ""}</h2><dl class="kv">${rows || "<dd class='muted'>no status</dd>"}</dl></div>`);
  }
  $("overview").innerHTML = out.join("");
}

// ---------- control ----------
function renderControls(lg) {
  const tests = (lg.info.features || []).filter(x => features[x.feature] && features[x.feature].commands && features[x.feature].commands.self_test);
  $("selfTests").innerHTML = tests.length ? tests.map(x => `<button data-test="${esc(x.instance)}">Test ${esc(x.instance)}</button>`).join("") : `<span class="muted">none</span>`;
  const s = $("cSession");
  if (document.activeElement !== s && !s.dataset.touched) s.value = (lg.config.recording || {}).session || "";
}
$("cSession").oninput = () => {
  const v = $("cSession").value;
  $("cSession").dataset.touched = "1";
  $("cSessionHint").textContent = v && slug(v) !== v ? `Will be saved as “${slug(v)}”.` : "Stored on the logger, so an automatic start after power-on records into the same session.";
};
async function runCmd(cmd, target, args) {
  const out = $("cResult");
  try {
    let res;
    if (cmd === "set_time") res = await api("/api/time/loggers", { selection: { ids: [current] } });
    else res = await api("/api/command", { selection: { ids: [current] }, cmd, target, args });
    const r = res.results && res.results[0];
    const extra = r && r.result && Object.keys(r.result).length ? "\n" + JSON.stringify(r.result, null, 1) : "";
    showResult(out, { ...res, summary: res.summary + extra });
  } catch (e) { showError(out, e); }
}
document.querySelectorAll("[data-cmd]").forEach(b => b.onclick = () => {
  const cmd = b.dataset.cmd, args = {};
  if (cmd === "start" && $("cSession").value.trim()) args.session = slug($("cSession").value);
  if (cmd === "mark") { const t = prompt("Marker text"); if (t == null) return; args.text = t; }
  if (cmd === "reboot" && !confirm("Reboot this logger? Recording restarts after the auto-start delay.")) return;
  if (cmd === "start") delete $("cSession").dataset.touched;
  runCmd(cmd, null, args);
});
$("selfTests").onclick = ev => { const b = ev.target.closest("[data-test]"); if (b) runCmd("self_test", b.dataset.test, {}); };

// ---------- config editor (generated from the feature definitions) ----------
let configDirty = false;
function fieldInput(path, spec, value, limit) {
  const t = spec.type, choices = limit || spec.enum;
  const attrs = `data-path="${esc(path)}" data-type="${esc(t)}"`;
  if (spec.secret) return `<input type="password" ${attrs} data-secret="1" placeholder="unchanged" autocomplete="new-password">`;
  if (choices) return `<select ${attrs}>${choices.map(c => `<option value='${esc(JSON.stringify(c))}' ${JSON.stringify(c) === JSON.stringify(value) ? "selected" : ""}>${esc(c)}</option>`).join("")}</select>`;
  if (t === "boolean") return `<input type="checkbox" ${attrs} ${value ? "checked" : ""}>`;
  if (t === "integer" || t === "number")
    return `<input type="number" ${attrs} value="${esc(value ?? "")}" ${spec.min != null ? `min="${spec.min}"` : ""} ${spec.max != null ? `max="${spec.max}"` : ""} step="${t === "integer" ? 1 : "any"}">`;
  if (t === "string") return `<input type="text" ${attrs} value="${esc(value ?? "")}" ${spec.max_length ? `maxlength="${spec.max_length}"` : ""}>`;
  return `<textarea rows="${t === "object" ? 4 : 3}" ${attrs}>${esc(JSON.stringify(value ?? spec.default ?? null, null, 1))}</textarea>`;
}
function renderConfig(lg) {
  const html = [];
  for (const fe of lg.info.features || []) {
    const def = features[fe.feature];
    if (!def || !def.config || !Object.keys(def.config).length) continue;
    const cur = lg.config[fe.instance] || {}, limits = fe.limits || {};
    const rows = Object.entries(def.config).map(([k, spec]) => {
      const value = cur[k] !== undefined ? cur[k] : spec.default;
      const hint = [spec.unit, spec.applies && spec.applies !== "now" ? `applies: ${spec.applies.replace("_", " ")}` : "", spec.description].filter(Boolean).join(" · ");
      return `<label>${esc(k)}</label>${fieldInput(`${fe.instance}.${k}`, spec, value, limits[k])}${hint ? `<div class="hint">${esc(hint)}</div>` : ""}`;
    }).join("");
    html.push(`<div class="panel"><h2>${esc(fe.instance)}${fe.instance !== fe.feature ? ` (${esc(fe.feature)})` : ""}</h2><div class="form">${rows}</div></div>`);
  }
  $("configForms").innerHTML = html.join("") || `<div class="panel muted">This logger hasn't published its features yet.</div>`;
  configDirty = false;
}
$("configForms").addEventListener("input", ev => { if (ev.target.dataset.path) { ev.target.classList.add("changed"); configDirty = true; } });
$("cfgReset").onclick = () => { const lg = loggers.get(current); if (lg) renderConfig(lg); $("cfgResult").innerHTML = ""; };
$("cfgSave").onclick = async () => {
  const changes = {}, errors = [];
  document.querySelectorAll("#configForms .changed").forEach(el => {
    const [inst, field] = el.dataset.path.split(".");
    let v;
    try {
      if (el.dataset.secret) { if (!el.value) return; v = el.value; }
      else if (el.tagName === "SELECT") v = JSON.parse(el.value);
      else if (el.type === "checkbox") v = el.checked;
      else if (el.type === "number") v = el.value === "" ? null : Number(el.value);
      else if (el.tagName === "TEXTAREA") v = JSON.parse(el.value);
      else v = el.value;
    } catch { errors.push(`${el.dataset.path}: not valid JSON`); return; }
    (changes[inst] = changes[inst] || {})[field] = v;
  });
  const out = $("cfgResult");
  if (errors.length) return showError(out, errors.join("; "));
  if (!Object.keys(changes).length) return showError(out, "Nothing changed.");
  try {
    const res = await api(`/api/loggers/${current}/config`, { changes });
    out.innerHTML = `<div class="result ${res.ok ? "ok" : "err"}">${esc(res.summary)}${res.restart_required ? `<br><b>Restart needed</b> for some changes to take effect. <button id="cfgReboot">Reboot now</button>` : ""}</div>`;
    if (res.ok) configDirty = false;
    const rb = $("cfgReboot");
    if (rb) rb.onclick = () => runCmd("reboot");
  } catch (e) { showError(out, e); }
};

// ---------- registry ----------
function renderRegistry(lg) {
  const r = lg.registry || {};
  const a = r.assignment || {};
  const last = r.last_seen || {};
  $("regForm").innerHTML = `
    <label>Serial</label><input type="text" id="rSerial" value="${esc(r.serial)}">
    <label>Type</label><input type="text" id="rType" value="${esc(r.type)}">
    <label>Model</label><input type="text" id="rModel" value="${esc(r.model)}" placeholder="e.g. VL-P1">
    <label>Hardware rev</label><input type="text" id="rHw" value="${esc(r.hardware_rev)}">
    <label>Location</label><input type="text" id="rLoc" value="${esc(a.location)}" placeholder="e.g. Pump house 2, motor DE">
    <label>Since</label><input type="text" id="rSince" value="${esc(a.since)}" placeholder="YYYY-MM-DD">
    <label>Expected features</label><input type="text" id="rExp" value="${esc((r.expected_features || []).join(", "))}"
      placeholder="${esc(modelInfo.has(lg.model) && modelInfo.get(lg.model) ? "model's list: " + modelInfo.get(lg.model).expected_features.join(", ") : "")}">
    <div class="hint">Leave empty to use the model's list (About tab). Fill in only for a unit built differently; missing features are flagged on the fleet view.</div>
    <label>Sensors</label><textarea id="rSensors" rows="3">${esc(JSON.stringify(r.sensors || {}, null, 1))}</textarea>
    <div class="hint">e.g. {"accel": {"board_serial": "…", "calibration": "…"}}</div>
    <label>Notes</label><textarea id="rNotes" rows="3" style="font-family:inherit">${esc(r.notes)}</textarea>
    <label>Last seen</label><div class="small muted">${esc(Object.entries(last).map(([k, v]) => `${k}: ${v}`).join(" · ") || "–")}</div>
    <label>Added</label><div class="small muted">${esc(r.added || "–")}</div>`;
}
$("regSave").onclick = async () => {
  const out = $("regResult");
  let sensors;
  try { sensors = JSON.parse($("rSensors").value || "{}"); } catch { return showError(out, "Sensors must be JSON."); }
  const body = {
    serial: $("rSerial").value.trim(), type: $("rType").value.trim(), model: $("rModel").value.trim(),
    hardware_rev: $("rHw").value.trim(),
    assignment: { location: $("rLoc").value.trim(), since: $("rSince").value.trim() },
    expected_features: $("rExp").value.split(",").map(s => s.trim()).filter(Boolean),
    sensors, notes: $("rNotes").value,
  };
  try { await api(`/api/registry/${current}`, body, "PUT"); showResult(out, { summary: "Saved." }, true); }
  catch (e) { showError(out, e); }
};

// ---------- about: the model's catalogue page ----------
const modelInfo = new Map();   // model -> page (null: no page on the master)
async function fetchModel(model) {
  if (!model) return null;
  if (!modelInfo.has(model)) {
    try { modelInfo.set(model, await api(`/api/models/${encodeURIComponent(model)}`)); }
    catch { modelInfo.set(model, null); }
  }
  return modelInfo.get(model);
}
async function renderAbout() {
  const lg = loggers.get(current);
  if (!lg) return;
  const el = $("about");
  if (!lg.model) {
    el.innerHTML = `<p>This logger doesn't report a model number, and the registry has none for it.</p>
      <p class="muted">Set its model on the Registry tab (e.g. <code>VL-P1</code>), or update its firmware, to see its LED guide and instructions here.</p>`;
    return;
  }
  modelInfo.delete(lg.model);   // re-read: pages are edited on the master
  el.innerHTML = `<p class="muted">Loading ${esc(lg.model)}…</p>`;
  const m = await fetchModel(lg.model);
  if (current !== lg.device_id) return;
  if (!m) {
    el.innerHTML = `<p>No catalogue page for model <b>${esc(lg.model)}</b> on the master.</p>
      <p class="muted">Add <code>registry/models/${esc(lg.model)}.md</code> (copy an existing page; the format is in
      <code>registry/models/README.md</code>). It shows up here straight away, no restart needed.</p>`;
    return;
  }
  const revOk = !lg.hardware_rev || !m.hardware_revs.length || m.hardware_revs.includes(lg.hardware_rev);
  const head = `<p class="model-head">This unit: ${esc(lg.label)} · model ${esc(m.model)}${lg.hardware_rev ? " rev " + esc(lg.hardware_rev) : ""}
    ${revOk ? "" : ` · <span class="warn-text">rev ${esc(lg.hardware_rev)} isn't one of this page's revisions (${esc(m.hardware_revs.join(", "))})</span>`}
    · from <code>registry/models/${esc(m.file)}</code></p>`;
  // Trusted content: Markdown written by whoever runs the master, served from its own disk
  el.innerHTML = head + (m.error ? `<p class="err-text">${esc(m.error)}</p>` : "") +
    (window.marked ? marked.parse(m.markdown || "") : `<pre>${esc(m.markdown || "")}</pre>`);
}

// ---------- events ----------
function renderEvents() {
  const ev = (detailData && detailData.events) || [];
  $("events").innerHTML = ev.slice().reverse().map(e => {
    const t = e.t ? new Date(e.t).toLocaleString() : e.received ? new Date(e.received * 1000).toLocaleString() : `uptime ${Math.round((e.uptime_ms || 0) / 1000)} s`;
    const text = EVENT_TEXT[e.name] && e.detail ? EVENT_TEXT[e.name](e.detail) : "";
    return `<div><b>${esc(e.name)}</b> <span class="muted">${esc(e.source || "")} · ${esc(t)}</span>${text ? `<br>${esc(text)}` : e.detail ? `<br><span class="small muted">${esc(JSON.stringify(e.detail))}</span>` : ""}</div>`;
  }).join("") || `<span class="muted">No events since the master started.</span>`;
}

// ---------- live data plot ----------
const COLORS = ["--c1", "--c2", "--c3", "--warn", "--accent", "--muted"].map(v => getComputedStyle(document.documentElement).getPropertyValue(v).trim());
function fillPubSelect() {
  const lg = loggers.get(current);
  if (!lg) return;
  const names = [...new Set([...(lg.publications || []), ...[...series.keys()].filter(k => k.startsWith(current + "/")).map(k => k.split("/")[1])])].sort();
  const sel = $("pubSel"), v = sel.value;
  sel.innerHTML = names.map(n => `<option>${esc(n)}</option>`).join("");
  if (names.includes(v)) sel.value = v;
  fillStatSelect();
}
function fillStatSelect() {
  const s = series.get(`${current}/${$("pubSel").value}`) || [];
  const last = s[s.length - 1];
  const keys = last && last.values ? Object.keys(last.values) : [];
  const sel = $("statSel"), v = sel.value;
  sel.innerHTML = keys.map(k => `<option>${esc(k)}</option>`).join("");
  if (keys.includes(v)) sel.value = v;
  else if (keys.includes("ac_rms")) sel.value = "ac_rms";
}
$("pubSel").onchange = () => { fillStatSelect(); drawPlot(); };
$("statSel").onchange = drawPlot;
$("winSel").onchange = drawPlot;
function drawPlot() {
  const lg = loggers.get(current), cv = $("plot");
  const dpr = window.devicePixelRatio || 1, W = cv.clientWidth, H = cv.clientHeight;
  cv.width = W * dpr; cv.height = H * dpr;
  const ctx = cv.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);
  const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
  const s = series.get(`${current}/${$("pubSel").value}`) || [];
  const stat = $("statSel").value, win = +$("winSel").value, tEnd = nowS();
  const pts = s.filter(m => m.received >= tEnd - win && m.values && m.values[stat]);
  const rec = lg && (feats(lg).recording || {}).recording;
  $("liveInfo").textContent = !s.length
    ? (rec ? "Waiting for the first publication…" : "No live data yet: waiting for the logger's first publication.")
    : `${s[s.length - 1].method} · ${(s[s.length - 1].window || {}).frames || "?"} frames per message · last ${ago(tEnd - s[s.length - 1].received)}` +
      (rec ? "" : " · logger not recording, so no new data");
  const chans = (pts[0] && pts[0].channels) || ["x", "y", "z"];
  const unit = (pts[0] && pts[0].unit) || "";
  $("legend").innerHTML = chans.map((c, i) => `<span><i style="background:${COLORS[i % COLORS.length]}"></i>${esc(c)}</span>`).join("");
  const L = 56, R = 10, T = 10, B = 24, pw = W - L - R, ph = H - T - B;
  let lo = Infinity, hi = -Infinity;
  pts.forEach(m => m.values[stat].forEach(v => { if (v < lo) lo = v; if (v > hi) hi = v; }));
  if (!isFinite(lo)) { lo = 0; hi = 1; }
  if (hi - lo < 1e-6) { hi += 0.5 * (Math.abs(hi) || 1e-3); lo -= 0.5 * (Math.abs(lo) || 1e-3); }
  const pad = (hi - lo) * 0.08; lo -= pad; hi += pad;
  const x = t => L + (t - (tEnd - win)) / win * pw, y = v => T + (hi - v) / (hi - lo) * ph;
  ctx.strokeStyle = css("--grid"); ctx.fillStyle = css("--muted"); ctx.font = "11px system-ui"; ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const v = lo + (hi - lo) * i / 4, yy = y(v);
    ctx.beginPath(); ctx.moveTo(L, yy); ctx.lineTo(W - R, yy); ctx.stroke();
    ctx.fillText(Math.abs(v) < 0.01 && v !== 0 ? v.toExponential(1) : v.toFixed(3), 4, yy + 4);
  }
  for (let i = 0; i <= 4; i++) {
    const t = tEnd - win + win * i / 4, xx = x(t);
    ctx.fillText(new Date(t * 1000).toLocaleTimeString(), Math.min(xx - 20, W - 60), H - 6);
  }
  ctx.fillText(`${stat} (${unit})`, L + 4, T + 12);
  chans.forEach((c, i) => {
    ctx.strokeStyle = COLORS[i % COLORS.length]; ctx.lineWidth = 1.5; ctx.beginPath();
    let prev = null;
    pts.forEach(m => {
      const xx = x(m.received), yy = y(m.values[stat][i]);
      if (prev != null && m.received - prev > 5) ctx.moveTo(xx, yy); else if (prev == null) ctx.moveTo(xx, yy); else ctx.lineTo(xx, yy);
      prev = m.received;
    });
    ctx.stroke();
  });
}
window.addEventListener("resize", () => { if (tab === "live") drawPlot(); });

// ---------- orientation (three.js, like tools/accel-viewer) ----------
// Scene layers, outermost first:
//   poseGroup    fixed screen pose: shown axis Z points up the screen
//   boardGroup   the shown axes (sensor axes through the axis map) + arrows, tilted live since zero
//   modelHolder  the model, turned relative to the arrows (model_align)
// Zero, axis map, model turn and the model file are kept on the master per logger (/api/loggers/<id>/view).
const AX = ["X", "Y", "Z"];
const IDENTITY = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
const ROT90 = [
  [[1, 0, 0], [0, 0, -1], [0, 1, 0]],   // about X
  [[0, 0, 1], [0, 1, 0], [-1, 0, 0]],   // about Y
  [[0, -1, 0], [1, 0, 0], [0, 0, 1]],   // about Z
];
const matMul = (A, B) => A.map(r => [0, 1, 2].map(j => r[0] * B[0][j] + r[1] * B[1][j] + r[2] * B[2][j]));
const det3 = m => m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                  m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
const INVERT = i => IDENTITY.map((r, k) => r.map(v => (k === i ? -v : v)));

let scene, camera, renderer, boardGroup, modelHolder, customHolder, defaultBoard, axisArrows = [];
let view = { zero: null, axis_map: IDENTITY, model_align: [0, 0, 0, 1], model: null };
let viewFor = null;   // device_id whose view is loaded

const mapVec = v => view.axis_map.map(r => r[0] * v[0] + r[1] * v[1] + r[2] * v[2]);
const unit = v => { const m = Math.hypot(...v) || 1; return v.map(x => x / m); };
function oMsg(text) { $("oMsg").textContent = text || ""; }

function latestMean() {
  for (const [k, s] of series) {
    if (!k.startsWith(current + "/") || !s.length) continue;
    const m = s[s.length - 1];
    const v = m.values && (m.values.mean || m.values.latest);
    if (v && v.length >= 3 && (m.channels || []).slice(0, 3).join("") === "xyz") return { v: v.slice(0, 3), m };
  }
  return null;
}

function cssColor(name) {
  return new THREE.Color(getComputedStyle(document.documentElement).getPropertyValue(name).trim() || "#888");
}
function makeLabel(text, color) {
  const c = document.createElement("canvas"); c.width = c.height = 64;
  const g = c.getContext("2d"); g.fillStyle = color; g.font = "bold 44px system-ui"; g.textAlign = "center";
  g.textBaseline = "middle"; g.fillText(text, 32, 34);
  const s = new THREE.Sprite(new THREE.SpriteMaterial({ map: new THREE.CanvasTexture(c), depthTest: false }));
  s.scale.set(0.3, 0.3, 0.3); return s;
}
function drawAxisArrows() {
  axisArrows.forEach(o => boardGroup.remove(o)); axisArrows = [];
  ["--c1", "--c2", "--c3"].forEach((v, i) => {
    const d = new THREE.Vector3(i === 0 ? 1 : 0, i === 1 ? 1 : 0, i === 2 ? 1 : 0), col = cssColor(v);
    const arrow = new THREE.ArrowHelper(d, new THREE.Vector3(0, 0, 0.08), 1.0, col, 0.16, 0.09);
    // On top, so they stay visible inside a loaded model
    [arrow.line, arrow.cone].forEach(o => { o.material.depthTest = false; o.renderOrder = 999; });
    const label = makeLabel(AX[i], "#" + col.getHexString());
    label.renderOrder = 999;
    label.position.copy(d.clone().multiplyScalar(1.18)).add(new THREE.Vector3(0, 0, 0.08));
    boardGroup.add(arrow, label); axisArrows.push(arrow, label);
  });
}

function initScene() {
  if (renderer) { resizeScene(); return; }
  renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
  renderer.setPixelRatio(window.devicePixelRatio);
  $("scene").appendChild(renderer.domElement);
  scene = new THREE.Scene();
  camera = new THREE.PerspectiveCamera(40, 1, 0.1, 100);
  camera.position.set(3.2, 2.6, 3.6); camera.lookAt(0, 0, 0);
  scene.add(new THREE.AmbientLight(0xffffff, 0.6));
  const sun = new THREE.DirectionalLight(0xffffff, 0.8); sun.position.set(3, 5, 2); scene.add(sun);
  const grid = new THREE.GridHelper(4, 8, 0x888888, 0x888888);
  grid.position.y = -0.9; grid.material.opacity = 0.35; grid.material.transparent = true; scene.add(grid);
  scene.add(new THREE.ArrowHelper(new THREE.Vector3(0, -1, 0), new THREE.Vector3(-1.6, 0.6, -1.2), 0.9, 0x999999, 0.15, 0.08));

  const poseGroup = new THREE.Group(); scene.add(poseGroup);
  poseGroup.quaternion.setFromAxisAngle(new THREE.Vector3(1, 0, 0), -Math.PI / 2);  // +Z -> screen up
  boardGroup = new THREE.Group(); poseGroup.add(boardGroup);
  modelHolder = new THREE.Group(); boardGroup.add(modelHolder);
  customHolder = new THREE.Group(); modelHolder.add(customHolder);
  defaultBoard = new THREE.Group(); modelHolder.add(defaultBoard);
  defaultBoard.add(new THREE.Mesh(new THREE.BoxGeometry(2.0, 1.2, 0.08),   // lies in XY, Z = board normal
    new THREE.MeshStandardMaterial({ color: 0x1f6f4a, roughness: 0.7 })));
  const chip = new THREE.Mesh(new THREE.BoxGeometry(0.4, 0.4, 0.06), new THREE.MeshStandardMaterial({ color: 0x222222 }));
  chip.position.z = 0.07; defaultBoard.add(chip);
  drawAxisArrows();

  new ResizeObserver(resizeScene).observe($("scene"));
  resizeScene();
  (function anim() { requestAnimationFrame(anim); if (tab === "orientation") renderer.render(scene, camera); })();
}
function resizeScene() {
  if (!renderer) return;
  const w = $("scene").clientWidth;
  if (!w) return;
  renderer.setSize(w, w); camera.aspect = 1; camera.updateProjectionMatrix();
}

// ---------- per-logger view, kept on the master ----------
async function loadView() {
  if (!current || viewFor === current) return;
  const id = current;
  viewFor = id;
  let v = {};
  try { v = await api(`/api/loggers/${encodeURIComponent(id)}/view`); } catch (e) { oMsg(e.message); }
  if (current !== id) return;
  view = { zero: v.zero || null, axis_map: v.axis_map || IDENTITY, model_align: v.model_align || [0, 0, 0, 1], model: v.model || null };
  clearModel();
  if (view.model) {
    try {
      const r = await fetch(`/api/loggers/${encodeURIComponent(id)}/model`, { cache: "no-cache" });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const buffer = await r.arrayBuffer();
      if (current === id) await showModel(view.model.name, buffer);
    } catch (e) { oMsg("Couldn't load the stored model: " + e.message); }
  }
  updatePose(); updateAxisInfo(); updateOrientation();
}
async function saveView(changes) {
  try { await api(`/api/loggers/${encodeURIComponent(current)}/view`, changes, "PUT"); oMsg(""); }
  catch (e) { oMsg("Not saved: " + e.message); }
}

function parseModel(name, buffer) {
  const ext = name.split(".").pop().toLowerCase();
  if (ext === "stl") {
    const geo = new THREE.STLLoader().parse(buffer);
    if (!geo.attributes.normal) geo.computeVertexNormals();
    return Promise.resolve(new THREE.Mesh(geo, new THREE.MeshStandardMaterial({
      color: geo.hasColors ? 0xffffff : 0x9aa4b2, vertexColors: !!geo.hasColors, roughness: 0.6, metalness: 0.1 })));
  }
  if (ext === "obj") {
    const obj = new THREE.OBJLoader().parse(new TextDecoder().decode(buffer));
    obj.traverse(o => { if (o.isMesh && !o.material.map) o.material = new THREE.MeshStandardMaterial({ color: 0x9aa4b2, roughness: 0.6 }); });
    return Promise.resolve(obj);
  }
  if (ext === "glb") return new Promise((res, rej) => new THREE.GLTFLoader().parse(buffer, "", g => res(g.scene), rej));
  return Promise.reject(new Error(`unsupported file type .${ext} (use STL, OBJ or GLB)`));
}
async function showModel(name, buffer) {
  const obj = await parseModel(name, buffer);
  // Centre it and scale the largest dimension to 2 units, whatever the file's units
  const box = new THREE.Box3().setFromObject(obj), size = box.getSize(new THREE.Vector3());
  obj.position.sub(box.getCenter(new THREE.Vector3()));
  const fitted = new THREE.Group(); fitted.add(obj);
  fitted.scale.setScalar(2 / Math.max(size.x, size.y, size.z, 1e-9));
  customHolder.clear(); customHolder.add(fitted);
  defaultBoard.visible = false;
  $("oRemove").disabled = false;
  $("oModelInfo").textContent = `Model: ${name} (stored on the master for this logger).`;
}
function clearModel() {
  if (!customHolder) return;
  customHolder.clear();
  defaultBoard.visible = true;
  $("oRemove").disabled = true;
  $("oModelInfo").textContent = "Upload an STL, OBJ or GLB to replace the default board. It's stored on the master for this logger, so every browser sees it.";
}

$("oLoad").onclick = () => $("oFile").click();
$("oFile").onchange = async e => {
  const file = e.target.files[0]; e.target.value = "";
  if (!file || !current) return;
  try {
    const buffer = await file.arrayBuffer();
    await showModel(file.name, buffer);   // check it parses before storing it
    const r = await fetch(`/api/loggers/${encodeURIComponent(current)}/model?name=${encodeURIComponent(file.name)}`,
                          { method: "PUT", body: buffer });
    const d = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(d.detail || `HTTP ${r.status}`);
    view.model = d.model;
    oMsg("");
  } catch (err) { oMsg("Couldn't use that model: " + err.message); }
};
$("oRemove").onclick = async () => {
  clearModel();
  try { await api(`/api/loggers/${encodeURIComponent(current)}/model`, {}, "DELETE"); view.model = null; }
  catch (e) { oMsg(e.message); }
};

function updatePose() { if (modelHolder) modelHolder.quaternion.fromArray(view.model_align); }
function turnModel(axis) {
  const q = new THREE.Quaternion().fromArray(view.model_align);
  q.premultiply(new THREE.Quaternion().setFromAxisAngle(axis, Math.PI / 2));   // about the arrows' axes
  view.model_align = q.toArray().map(x => Math.round(x * 1e6) / 1e6);
  updatePose(); saveView({ model_align: view.model_align });
}
$("oRotX").onclick = () => turnModel(new THREE.Vector3(1, 0, 0));
$("oRotY").onclick = () => turnModel(new THREE.Vector3(0, 1, 0));
$("oRotZ").onclick = () => turnModel(new THREE.Vector3(0, 0, 1));

function updateAxisInfo() {
  const parts = view.axis_map.map((r, i) => {
    const j = r.findIndex(v => v !== 0);
    return `${AX[i]} ← ${r[j] < 0 ? "−" : "+"}sensor ${AX[j]}`;
  });
  $("oAxisInfo").innerHTML = esc(parts.join(" · ")) + (det3(view.axis_map) < 0
    ? ` · <b class="warn-text">mirrored: one tilt direction will be reversed; invert one more axis to fix</b>` : "");
}
function setAxisMap(m) {
  view.axis_map = m;
  updateAxisInfo(); updateOrientation();
  saveView({ axis_map: m });
}
[0, 1, 2].forEach(i => {
  $("oAxRot" + AX[i]).onclick = () => setAxisMap(matMul(ROT90[i], view.axis_map));
  $("oAxInv" + AX[i]).onclick = () => setAxisMap(matMul(INVERT(i), view.axis_map));
});
$("oAxReset").onclick = () => setAxisMap(IDENTITY.map(r => r.slice()));

function updateOrientation() {
  if (!boardGroup) return;
  loadView();
  const lm = latestMean();
  if (!lm) {
    $("oG").textContent = "no data yet (waiting for the logger's live publication)";
    ["oTilt", "oDown", "oAge"].forEach(id => $(id).textContent = "–");
    return;
  }
  const a = unit(lm.v), z = unit(view.zero || [0, 0, 1]);
  // Tilt since zero, in the shown axes. At rest the mapped reading equals the mapped zero, so
  // changing the axis map never moves the picture, only how the model follows the real logger.
  const b = mapVec(a), b0 = mapVec(z);
  boardGroup.quaternion.copy(new THREE.Quaternion().setFromUnitVectors(new THREE.Vector3(...b), new THREE.Vector3(...b0)));
  const tilt = Math.acos(Math.max(-1, Math.min(1, a[0] * z[0] + a[1] * z[1] + a[2] * z[2]))) * 180 / Math.PI;
  let best = 0; for (let i = 1; i < 3; i++) if (Math.abs(b0[i]) > Math.abs(b0[best])) best = i;
  const g = mapVec(lm.v);
  $("oG").textContent = `${g.map(v => v.toFixed(3)).join(", ")} ${lm.m.unit || "g"} (|g| ${Math.hypot(...lm.v).toFixed(3)})`;
  $("oTilt").textContent = view.zero ? `${tilt.toFixed(1)}°` : `${tilt.toFixed(1)}° from +Z up (not zeroed)`;
  // The accelerometer reads +1 g on the axis pointing up, so down is the opposite sign
  $("oDown").textContent = `${b0[best] > 0 ? "−" : "+"}${AX[best]} (${(Math.abs(b0[best]) * 100).toFixed(0)}% aligned)`;
  $("oAge").textContent = ago(nowS() - lm.m.received);
}
$("oZero").onclick = () => {
  const lm = latestMean();
  if (!lm) return oMsg("No gravity reading yet: wait for the logger's live publication.");
  view.zero = lm.v.map(x => Math.round(x * 1e5) / 1e5);
  updateOrientation(); saveView({ zero: view.zero });
};
$("oClear").onclick = () => { view.zero = null; updateOrientation(); saveView({ zero: null }); };

// ---------- clocks ----------
async function checkClock() {
  try {
    const t0 = Date.now(), r = await api("/api/time"), t1 = Date.now();
    masterOffsetMs = r.utc_ms - (t0 + t1) / 2;
  } catch { return; }
  // The master is every logger's time source, so a wrong master clock is worth pointing out
  $("clockWarn").textContent = Math.abs(masterOffsetMs) > 5000
    ? `Master clock is ${Math.round(masterOffsetMs / 1000)} s off this device's: check the master's time.` : "";
}
setInterval(() => {
  $("clock").textContent = "master " + new Date(Date.now() + masterOffsetMs).toLocaleTimeString();
  if (current && tab === "orientation") $("oAge").textContent = (() => { const lm = latestMean(); return lm ? ago(nowS() - lm.m.received) : "–"; })();
}, 1000);
setInterval(renderFleetSoon, 10000);  // keeps "seen … ago" fresh
setInterval(checkClock, 60000);

// ---------- firmware updates (OTA, protocol/features/ota.json) ----------
// One line for a logger's update state, for cards, the overview and the firmware page
function otaHtml(o) {
  if (!o || !o.state) return "";
  const v = o.target_version ? ` ${esc(o.target_version)}` : "";
  switch (o.state) {
    case "downloading": return `<span class="prog"><i style="width:${o.progress_pct || 0}%"></i></span>updating to${v}: ${o.progress_pct || 0}%`;
    case "verifying": return `updating to${v}: checking`;
    case "rebooting": return `updating to${v}: restarting`;
    case "confirming": return `checking${v} is healthy${o.confirm_s != null ? ` (${o.confirm_s} s left)` : ""}`;
    case "failed": return `<span class="err-text">update failed: ${esc(o.last_error || "unknown error")}</span>`;
    default: return o.last_error ? `<span class="warn-text">${esc(o.last_error)}</span>` : "";
  }
}
const otaBusy = o => o && ["downloading", "verifying", "rebooting", "confirming"].includes(o.state);
const EVENT_TEXT = {
  update_started: d => `Firmware update to ${d.version} started`,
  update_failed: d => `Firmware update to ${d.version} failed: ${d.error}. The old firmware is still running.`,
  update_installed: d => `Firmware ${d.version} installed (was ${d.previous_version})`,
  update_rolled_back: d => `Firmware ${d.version} rolled back to ${d.previous_version}: ${d.reason}`,
};

function showPage(page) {
  document.body.classList.toggle("firmware", page === "firmware");
  if (page !== "detail") { current = null; document.body.classList.remove("detail"); }
  $("navFirmware").classList.toggle("on", page === "firmware");
  $("navFleet").classList.toggle("on", page === "fleet");
  history.replaceState(null, "", page === "firmware" ? "#firmware" : "#");
  if (page === "firmware") renderFirmware(true);
  if (page === "fleet") renderFleet();
}
$("navFleet").onclick = () => showPage("fleet");
$("navFirmware").onclick = () => showPage("firmware");

let fwImages = [], fwTimer = null;
const fwSel = new Set();   // device ids ticked on the firmware page
const renderFirmwareSoon = () => {
  if (document.body.classList.contains("firmware") && !fwTimer) fwTimer = setTimeout(() => { fwTimer = null; renderFirmware(); }, 300);
};
async function renderFirmware(reload) {
  if (reload) { try { fwImages = await api("/api/firmware"); } catch (e) { fwImages = []; } }
  $("fwImages").innerHTML = fwImages.length ? `<div class="tblwrap"><table class="tbl">
    <tr><th>Model</th><th>Version</th><th>Size</th><th>Uploaded</th><th>Notes</th><th></th></tr>
    ${fwImages.map(i => `<tr><td>${esc(i.model)}</td><td>${esc(i.version)}</td><td>${(i.size / 1024).toFixed(0)} KB</td>
      <td>${new Date(i.uploaded * 1000).toLocaleString()}</td><td class="small">${esc(i.notes || "")}${i.marker ? "" : ` <span class="warn-text">(no model marker in the image)</span>`}</td>
      <td><button class="danger small" data-fwdel="${esc(i.model)}/${esc(i.version)}">Delete</button></td></tr>`).join("")}
    </table></div>` : `<span class="muted">No firmware uploaded yet.</span>`;

  // One panel per model: its loggers, their versions and update state
  const models = new Set([...fwImages.map(i => i.model), ...[...loggers.values()].map(l => l.model).filter(Boolean)]);
  const keepVersion = {};
  document.querySelectorAll("[data-fwver]").forEach(sel => keepVersion[sel.dataset.fwver] = sel.value);
  $("fwModels").innerHTML = [...models].sort().map(model => {
    const imgs = fwImages.filter(i => i.model === model).sort((a, b) => b.uploaded - a.uploaded);
    const list = [...loggers.values()].filter(l => l.model === model).sort((a, b) => a.label.localeCompare(b.label));
    const opts = imgs.map(i => `<option value="${esc(i.version)}"${keepVersion[model] === i.version ? " selected" : ""}>${esc(i.version)}</option>`).join("");
    const rows = list.map(l => {
      const o = l.ota || {}, ota = l.info.features && l.info.features.some(f => f.feature === "ota");
      const rec = feats(l).recording && feats(l).recording.recording;
      const note = !ota ? `<span class="muted">no WiFi updates in this firmware (update over USB once)</span>`
        : otaHtml(o) || (rec ? `<span class="muted">recording: stop it to update</span>` : o.previous_version ? `<span class="muted">updated from ${esc(o.previous_version)}</span>` : "");
      return `<tr><td><input type="checkbox" data-fwsel="${esc(l.device_id)}" ${fwSel.has(l.device_id) ? "checked" : ""}></td>
        <td>${esc(l.label)}<div class="small muted">${esc(l.device_id)}</div></td>
        <td><span class="pill ${stateOf(l)}">${stateOf(l)}</span></td>
        <td>${esc(l.firmware || "?")}</td><td>${note}</td></tr>`;
    }).join("");
    return `<div class="panel"><h2>${esc(model)}</h2>
      ${list.length ? `<div class="tblwrap"><table class="tbl"><tr><th></th><th>Logger</th><th>State</th><th>Firmware</th><th>Update</th></tr>${rows}</table></div>`
                    : `<span class="muted">No ${esc(model)} loggers have connected yet.</span>`}
      <div class="bar" style="margin-top:10px">
        ${imgs.length ? `<label class="small muted">Install <select data-fwver="${esc(model)}">${opts}</select></label>
          <button class="primary" data-fwupd="${esc(model)}" data-scope="selected">Update selected</button>
          <button data-fwupd="${esc(model)}" data-scope="all">Update all ${esc(model)}</button>`
          : `<span class="muted small">Upload a ${esc(model)} image above to update these loggers.</span>`}
      </div>
      <div data-fwresult="${esc(model)}"></div></div>`;
  }).join("") || `<p class="muted">No loggers or firmware yet.</p>`;
}

// Firmware images say what they are: VLFW-MODEL=<model>;VLFW-VERSION=<version>;
$("fwFile").onchange = async () => {
  const f = $("fwFile").files[0];
  if (!f) return;
  const text = new TextDecoder("latin1").decode(await f.arrayBuffer());
  const m = /VLFW-MODEL=([A-Za-z0-9._-]{1,40});VLFW-VERSION=([A-Za-z0-9._-]{1,40});/.exec(text);
  if (m) { $("fwModel").value = m[1]; $("fwVersion").value = m[2]; }
};
$("fwUpload").onclick = async () => {
  const out = $("fwUploadResult"), f = $("fwFile").files[0];
  if (!f) return showError(out, "Choose a firmware.bin file first.");
  const q = new URLSearchParams({ model: $("fwModel").value.trim(), version: $("fwVersion").value.trim(),
                                  filename: f.name, notes: $("fwNotes").value.trim() });
  try {
    const r = await fetch(`/api/firmware?${q}`, { method: "PUT", body: await f.arrayBuffer() });
    const d = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(d.detail || `HTTP ${r.status}`);
    showResult(out, { summary: `Stored ${d.model} ${d.version} (${(d.size / 1024).toFixed(0)} KB).` }, true);
    $("fwFile").value = ""; $("fwNotes").value = "";
    renderFirmware(true);
  } catch (e) { showError(out, e); }
};
$("fwImages").addEventListener("click", async ev => {
  const b = ev.target.closest("[data-fwdel]");
  if (!b || !confirm(`Delete firmware ${b.dataset.fwdel} from the master? Loggers already running it are unaffected.`)) return;
  try { await api(`/api/firmware/${b.dataset.fwdel}`, {}, "DELETE"); renderFirmware(true); }
  catch (e) { showError($("fwUploadResult"), e); }
});
$("fwModels").addEventListener("change", ev => {
  const cb = ev.target.closest("[data-fwsel]");
  if (cb) cb.checked ? fwSel.add(cb.dataset.fwsel) : fwSel.delete(cb.dataset.fwsel);
});
$("fwModels").addEventListener("click", async ev => {
  const b = ev.target.closest("[data-fwupd]");
  if (!b) return;
  const model = b.dataset.fwupd, out = document.querySelector(`[data-fwresult="${CSS.escape(model)}"]`);
  const version = document.querySelector(`[data-fwver="${CSS.escape(model)}"]`).value;
  const ids = [...loggers.values()].filter(l => l.model === model && (b.dataset.scope === "all" || fwSel.has(l.device_id))).map(l => l.device_id);
  if (!ids.length) return showError(out, "Tick the loggers to update, or use Update all.");
  if (!confirm(`Install ${model} firmware ${version} on ${ids.length} logger${ids.length === 1 ? "" : "s"}?\n\n` +
      `• Loggers that are recording refuse: stop them first.\n` +
      `• Each one downloads the image (about a minute), restarts, and is offline for a few seconds.\n` +
      `• If the new firmware isn't healthy (sensors OK, back on the master) within its confirm time ` +
      `(5 minutes by default), the logger switches back to its current firmware by itself.`)) return;
  try { showResult(out, await api("/api/update", { selection: { ids }, model, version, timeout: 10 })); }
  catch (e) { showError(out, e); }
});

// ---------- start ----------
$("navFleet").classList.add("on");   // the page opens on the fleet (or #firmware: showPage moves it)
(async () => {
  try { features = await api("/api/features"); } catch {}
  checkClock();
  connect();
})();
