#include "wifi_feature.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <logger.h>

#include "icons.h"
#include "ota_feature.h"

namespace logger {

// The status page. Everything it shows comes from /status, so it stays in step with MQTT status.
static const char PAGE[] PROGMEM = R"html(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Logger status</title>
<link rel="icon" type="image/png" href="/icon-192.png"><link rel="apple-touch-icon" href="/icon-180.png">
<link rel="manifest" href="/manifest.json"><meta name="theme-color" content="#1f488c">
<meta name="apple-mobile-web-app-capable" content="yes"><meta name="apple-mobile-web-app-title" content="Logger">
<style>
:root{--bg:#f6f7f9;--card:#fff;--text:#1c2330;--muted:#677184;--border:#dde1e8;--ok:#1f9d55;--warn:#c27c0e;--err:#d64545}
@media (prefers-color-scheme:dark){:root{--bg:#11141a;--card:#1a1f28;--text:#e6e9ef;--muted:#8b93a3;--border:#2b323e;--ok:#3dd68c;--warn:#f5a524;--err:#ff6369}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.4 system-ui,-apple-system,"Segoe UI",sans-serif;padding:16px}
h1{font-size:18px;margin:0 0 4px}.sub{color:var(--muted);font-size:13px;margin-bottom:16px}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:14px;margin-bottom:12px}
.card h2{font-size:12px;text-transform:uppercase;letter-spacing:.05em;color:var(--muted);margin:0 0 10px}
.big{font-size:40px;font-weight:700;font-variant-numeric:tabular-nums}
.bar{height:10px;border-radius:5px;background:var(--border);overflow:hidden;margin:8px 0}.bar i{display:block;height:100%;background:var(--ok)}
dl{display:grid;grid-template-columns:auto 1fr;gap:4px 14px;margin:0;font-variant-numeric:tabular-nums}dt{color:var(--muted)}dd{margin:0;font-weight:600}
.ok{color:var(--ok)}.warn{color:var(--warn)}.err{color:var(--err)}#age{color:var(--muted);font-size:13px}
label{display:block;font-size:13px;color:var(--muted);margin:10px 0 4px}
input{width:100%;font:inherit;padding:8px 10px;border-radius:8px;border:1px solid var(--border);background:var(--bg);color:var(--text)}
button{font:inherit;padding:9px 14px;border-radius:8px;border:1px solid var(--border);background:var(--card);color:var(--text);margin-top:12px;margin-right:8px}
button.primary{background:#2f6fde;border-color:#2f6fde;color:#fff}.hint{color:var(--muted);font-size:13px;margin:8px 0 0}
</style></head><body>
<h1 id="title">Logger</h1><div class="sub" id="sub"></div>
<div class="card"><h2>Battery</h2><div class="big" id="pct">–</div><div class="bar"><i id="bar" style="width:0"></i></div><dl id="batt"></dl></div>
<div class="card"><h2>Logger</h2><dl id="logger"></dl></div>
<div class="card"><h2>Recording</h2><dl id="rec"></dl>
<div id="recControls">
<label for="session">Session name</label><input id="session" maxlength="40" placeholder="e.g. pump2-baseline" autocomplete="off">
<button class="primary" id="recStart" type="button">Start</button><button id="recStop" type="button">Pause</button>
<p class="hint">Start with a new name to begin a new session; letters, digits, - and _ (others become _).
Pause closes the file; Start carries on in the same session.</p>
</div>
<p class="hint" id="recMaster" hidden>The master is connected and controls recording. Use its dashboard.</p>
<p class="hint" id="recMsg"></p>
</div>
<div class="card" id="fwCard" hidden><h2>Firmware</h2><dl id="fw"></dl>
<div id="fwControls">
<label for="fwFile">Firmware file (.bin, from <code>pio run -e vibration</code>)</label><input id="fwFile" type="file" accept=".bin">
<button class="primary" id="fwUpload" type="button">Upload &amp; install</button>
<div class="bar" style="margin-top:10px"><i id="fwBar" style="width:0"></i></div>
<p class="hint">The logger checks the file is for this model, installs it, and restarts. If the new firmware
doesn't keep running for a minute it switches back by itself. Not while recording.</p>
</div>
<p class="hint" id="fwMaster" hidden>The master is connected: update from its dashboard.</p>
<p class="hint" id="fwMsg"></p>
</div>
<div class="card"><h2>Battery saver</h2>
<p class="hint" style="margin-top:0">Turns WiFi off completely: no hotspot, and no looking for the master. Recording
carries on, and the battery lasts much longer. To get WiFi back, switch the logger off and on.
<b>While recording this happens by itself a minute after you disconnect from this hotspot.</b></p>
<button id="radioOff" type="button">Turn WiFi off until restart</button>
<p class="hint" id="radioMsg"></p>
</div>
<div class="card"><h2>Network</h2>
<p class="hint" id="nets">Known networks: …</p>
<form id="netForm">
<label for="ssid">Network name (SSID)</label><input id="ssid" name="ssid" required maxlength="32" autocomplete="off">
<label for="password">Password</label><input id="password" name="password" type="password" maxlength="63" placeholder="blank keeps a known network's password" autocomplete="off">
<label for="mqtt">Master address (optional)</label><input id="mqtt" name="mqtt_host" placeholder="found automatically" maxlength="63" autocomplete="off">
<label for="mqttUser">Master username (if the master has a password)</label><input id="mqttUser" maxlength="32" placeholder="e.g. logger" autocomplete="off">
<label for="mqttPass">Master password</label><input id="mqttPass" type="password" maxlength="63" placeholder="blank keeps the stored one" autocomplete="off">
<button class="primary" type="submit">Save &amp; reboot</button><button type="button" id="forget">Forget all networks</button>
</form>
<p class="hint" id="netMsg">The logger tries known networks in order, newest last. It reboots to join; if it can't find any, this hotspot comes back.</p>
</div>
<div id="age"></div>
<script>
const $=id=>document.getElementById(id);
function rows(el,list){el.innerHTML="";for(const[k,v,c]of list){const dt=document.createElement("dt"),dd=document.createElement("dd");dt.textContent=k;dd.textContent=v;if(c)dd.className=c;el.append(dt,dd)}}
const mins=ms=>{const s=Math.floor(ms/1000);return Math.floor(s/3600)+"h "+String(Math.floor(s/60)%60).padStart(2,"0")+"m "+String(s%60).padStart(2,"0")+"s"};
let last=0;
async function tick(){
  try{
    const r=await fetch("/status",{cache:"no-store"});const s=await r.json();const f=s.features||{};last=Date.now();
    $("title").textContent="Logger "+(s.device_id||"");$("sub").textContent="State: "+s.state+" · up "+mins(s.uptime_ms);
    const b=f.battery||{};
    if(b.present){
      const p=Math.round(b.percent),c=p<10?"err":p<30?"warn":"ok";
      $("pct").textContent=p+"%";$("pct").className="big "+c;$("bar").style.width=Math.max(0,Math.min(100,p))+"%";
      $("bar").style.background="var(--"+c+")";
      rows($("batt"),[["Voltage",b.voltage.toFixed(3)+" V"],["Level",b.level],
        ["Power",b.usb_power?(b.charging?"USB, charging":b.full?"USB, full":"USB"):"Battery"],
        ["Rate",(b.charge_rate_pct_per_h>0?"+":"")+b.charge_rate_pct_per_h.toFixed(1)+" %/h"]]);
    }else{$("pct").textContent="no gauge";rows($("batt"),[])}
    const a=f.accel||{},sd=f.sd||{},rec=f.recording||{},w=f.wifi||{};
    rows($("logger"),[
      ["Accelerometer",a.ok?"OK, "+(a.frames||0).toLocaleString()+" samples":"not responding",a.ok?"ok":"err"],
      ["SD card",sd.present?(sd.free_mb/1024).toFixed(1)+" GB free":"no card",sd.present?"ok":"err"],
      ["Recording",rec.recording?(rec.file||"yes"):"no",rec.recording?"ok":""],
      ["Chip temp",f.core&&f.core.temperature_c!=null?f.core.temperature_c.toFixed(1)+" °C":"–"],
      ["WiFi",w.connected?(w.ssid+" · "+w.rssi_dbm+" dBm"):w.hotspot?("hotspot "+w.hotspot+(w.trying?" · looking for "+w.trying:"")):"off"],
      ["Master",w.master?"connected":"not connected",w.master?"ok":""]]);
    const auto={counting:"counting down to auto-start",waiting_for_master:"waiting for the master",skipped_master:"skipped: master in control",started:"started automatically",off:"off"}[rec.autostart]||rec.autostart||"–";
    rows($("rec"),[["State",rec.recording?"recording":"paused / stopped",rec.recording?"ok":""],["Session",rec.session||"–"],
      ["File",rec.file||"–"],["Auto-start",auto+(rec.countdown_s?" ("+rec.countdown_s+" s)":"")]]);
    $("recControls").hidden=!!w.master;$("recMaster").hidden=!w.master;
    const o=f.ota;$("fwCard").hidden=!o;
    if(o){const st={idle:"up to date",downloading:"installing "+(o.progress_pct||0)+"%",verifying:"checking",rebooting:"restarting",confirming:"new firmware, confirming ("+o.confirm_s+" s left)",failed:"last update failed"}[o.state]||o.state;
      rows($("fw"),[["Version",o.version||"–"],["Update",st,o.state==="failed"?"err":""]].concat(o.last_error?[["Last error",o.last_error,"err"]]:[]).concat(o.previous_version?[["Previous",o.previous_version]]:[]));
      $("fwControls").hidden=!!w.master;$("fwMaster").hidden=!w.master;}
    if(!$("session").value&&rec.session&&document.activeElement!==$("session"))$("session").placeholder=rec.session;
  }catch(e){}
  $("age").textContent=last?"Updated "+Math.round((Date.now()-last)/1000)+" s ago":"Connecting…";
}
async function nets(){
  try{const n=await (await fetch("/networks",{cache:"no-store"})).json();
    $("nets").textContent="Known networks: "+(n.networks.length?n.networks.join(", "):"none")+(n.mqtt_host?" · master "+n.mqtt_host:"")+(n.mqtt_user?" · master login "+n.mqtt_user:"");
    if(n.mqtt_user&&!$("mqttUser").value)$("mqttUser").placeholder=n.mqtt_user;}catch(e){}
}
async function post(body){
  $("netMsg").textContent="Saving…";
  try{const r=await fetch("/networks",{method:"POST",body:new URLSearchParams(body)});
    $("netMsg").textContent=await r.text();}catch(e){$("netMsg").textContent="Saved. The logger is rebooting; reconnect to its network."}
}
async function rec(action){
  const body={action};if(action==="start"&&$("session").value.trim())body.session=$("session").value.trim();
  $("recMsg").textContent="…";
  try{const r=await fetch("/recording",{method:"POST",body:new URLSearchParams(body)});$("recMsg").textContent=await r.text();
    if(r.ok)$("session").value="";tick();}catch(e){$("recMsg").textContent="No reply from the logger."}
}
$("fwUpload").onclick=()=>{
  const file=$("fwFile").files[0];if(!file){$("fwMsg").textContent="Choose a .bin file first.";return}
  if(!confirm("Install "+file.name+"? The logger restarts afterwards."))return;
  const x=new XMLHttpRequest(),fd=new FormData();fd.append("firmware",file,file.name);
  x.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded*100/e.total);$("fwBar").style.width=p+"%";$("fwMsg").textContent="Uploading "+p+"%"}};
  x.onload=()=>{$("fwMsg").textContent=x.responseText};x.onerror=()=>{$("fwMsg").textContent="Upload finished; the logger is restarting. Reconnect in about 30 s."};
  x.open("POST","/update");x.send(fd);
};
$("radioOff").onclick=async()=>{
  if(!confirm("Turn WiFi off? This page and the master lose the logger until it's switched off and on."))return;
  try{const r=await fetch("/wifi/off",{method:"POST"});$("radioMsg").textContent=await r.text();}
  catch(e){$("radioMsg").textContent="WiFi is off."}
};
$("recStart").onclick=()=>rec("start");$("recStop").onclick=()=>rec("stop");
$("netForm").onsubmit=e=>{e.preventDefault();const b={ssid:$("ssid").value,password:$("password").value,mqtt_host:$("mqtt").value};
  if($("mqttUser").value)b.mqtt_user=$("mqttUser").value;if($("mqttPass").value)b.mqtt_password=$("mqttPass").value;post(b)};
$("forget").onclick=()=>{if(confirm("Forget all networks? The logger will only run its own hotspot."))post({clear:"1"})};
tick();nets();setInterval(tick,2000);
</script></body></html>)html";

namespace {
constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;  // per network, before trying the next
}

bool WifiFeature::begin(Logger& host, JsonObjectConst config) {
    host_ = &host;
    const char* hotspot = config["hotspot"] | "no_master";
    hotspotMode_ = !strcmp(hotspot, "always")       ? "always"
                   : !strcmp(hotspot, "never")      ? "never"
                   : !strcmp(hotspot, "no_network") ? "no_network"
                                                    : "no_master";
    hotspotPassword_ = config["hotspot_password"] | "";
    autoOffAfterMs_ = uint32_t(max(0, int(config["auto_off_after_s"] | 60))) * 1000;
    for (JsonObjectConst n : config["networks"].as<JsonArrayConst>()) {
        const char* ssid = n["ssid"] | "";
        if (!*ssid || nNetworks_ >= MAX_NETWORKS) continue;
        networks_[nNetworks_++] = {ssid, n["password"] | ""};
    }
    enabled_ = (config["enabled"] | true) && (nNetworks_ || strcmp(hotspotMode_, "never") != 0);
    if (!enabled_) {
        WiFi.mode(WIFI_OFF);
        return true;
    }
    // Hotspot name: the serial if set (VL-0001), else VL- and the last 6 hex digits of the device id
    const String& id = host.deviceId();
    ssid_ = host.serial().length() ? host.serial() : "VL-" + id.substring(id.length() - 6);
    ssid_.toUpperCase();

    WiFi.persistent(false);  // settings live in our config, not the WiFi driver's flash
    WiFi.setHostname(("logger-" + id).c_str());
    WiFi.mode(nNetworks_ ? WIFI_STA : WIFI_AP);
    if (!nNetworks_ || !strcmp(hotspotMode_, "always")) setHotspot(true);
    if (nNetworks_) {
        WiFi.setAutoReconnect(true);
        tryNext();
    }

    server_.on("/", [this] { server_.send_P(200, "text/html", PAGE); });
    server_.on("/status", [this] { sendJson(false); });
    server_.on("/info", [this] { sendJson(true); });
    server_.on("/networks", HTTP_GET, [this] { sendNetworks(); });
    server_.on("/networks", HTTP_POST, [this] { saveNetwork(); });
    server_.on("/recording", HTTP_POST, [this] { recordingAction(); });
    server_.on("/update", HTTP_POST, [this] { firmwareUploadDone(); }, [this] { firmwareUpload(); });
    server_.on("/wifi/off", HTTP_POST, [this] {
        JsonDocument args, result;
        String message;
        command("off_until_restart", args.as<JsonObjectConst>(), result.to<JsonObject>(), message);
        server_.send(200, "text/plain", "WiFi turns off in a moment. Switch the logger off and on to get it back.");
    });
    serveIcons();
    masterSeen_ = millis();
    server_.onNotFound([this] { server_.send(404, "text/plain", "not found"); });
    server_.begin();
    return true;
}

void WifiFeature::tryNext() {
    const Network& n = networks_[current_];
    WiFi.begin(n.ssid.c_str(), n.password.isEmpty() ? nullptr : n.password.c_str());
    attemptStarted_ = millis();
}

void WifiFeature::setHotspot(bool on) {
    if (on == hotspotOn_) return;
    if (on) {
        WiFi.mode(nNetworks_ ? WIFI_AP_STA : WIFI_AP);
        hotspotOn_ = WiFi.softAP(ssid_.c_str(), hotspotPassword_.length() >= 8 ? hotspotPassword_.c_str() : nullptr);
    } else {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        hotspotOn_ = false;
    }
}

bool WifiFeature::stationConnected() const {
    return enabled_ && nNetworks_ && WiFi.status() == WL_CONNECTED;
}

void WifiFeature::sendJson(bool info) {
    JsonDocument d;
    info ? host_->buildInfo(d.to<JsonObject>()) : host_->buildStatus(d.to<JsonObject>());
    if (!info) d["device_id"] = host_->deviceId();
    String out;
    serializeJson(d, out);
    server_.sendHeader("Cache-Control", "no-store");
    server_.send(200, "application/json", out);
}

void WifiFeature::sendNetworks() {
    JsonDocument d;
    JsonArray a = d["networks"].to<JsonArray>();
    for (JsonVariantConst n : host_->config()["wifi"]["networks"].as<JsonArrayConst>()) a.add(n["ssid"] | "");
    d["mqtt_host"] = host_->config()["core"]["mqtt_host"] | "";
    d["mqtt_user"] = host_->config()["core"]["mqtt_user"] | "";  // never the password
    String out;
    serializeJson(d, out);
    server_.sendHeader("Cache-Control", "no-store");
    server_.send(200, "application/json", out);
}

// Form from the page: add (or update) a network, optionally set the master's address, then
// reboot to join it. Passwords never come back out of /networks.
void WifiFeature::saveNetwork() {
    JsonDocument changes;
    JsonArray nets = changes["wifi"]["networks"].to<JsonArray>();
    if (!server_.hasArg("clear")) {
        const String ssid = server_.arg("ssid");
        if (ssid.isEmpty() || ssid.length() > 32) {
            server_.send(400, "text/plain", "Network name must be 1-32 characters.");
            return;
        }
        String password = server_.arg("password");
        for (JsonVariantConst n : host_->config()["wifi"]["networks"].as<JsonArrayConst>()) {
            if (n["ssid"] != ssid) nets.add(n);
            else if (password.isEmpty()) password = n["password"] | "";  // blank: keep the stored one
        }
        JsonObject n = nets.add<JsonObject>();
        n["ssid"] = ssid;
        n["password"] = password;
        if (server_.hasArg("mqtt_host")) changes["core"]["mqtt_host"] = server_.arg("mqtt_host");
        if (server_.hasArg("mqtt_user")) changes["core"]["mqtt_user"] = server_.arg("mqtt_user");
        if (server_.hasArg("mqtt_password")) changes["core"]["mqtt_password"] = server_.arg("mqtt_password");
    }
    JsonDocument result;
    String message;
    if (host_->setConfig(changes.as<JsonObjectConst>(), result.to<JsonObject>(), message) != CmdStatus::Ok) {
        server_.send(500, "text/plain", "Not saved: " + message);
        return;
    }
    server_.send(200, "text/plain", "Saved. Rebooting to join the network; this hotspot will disappear if it works.");
    rebootAt_ = millis() + 1500;  // let the reply go out first
}

// Started from this page, so the logger is working without the master: stop looking for it
// (saves the power of scanning) until the next power-on. The hotspot stays for now.
void WifiFeature::stopStationSearch() {
    if (stationStopped_ || !nNetworks_) return;
    stationStopped_ = true;
    WiFi.disconnect(false);
    if (hotspotOn_) WiFi.mode(WIFI_AP);
}

bool WifiFeature::masterConnected() const {
    return host_->net() && host_->net()->connected();
}

// The page's Start / Pause, through the same commands the master uses
void WifiFeature::recordingAction() {
    if (masterConnected()) {
        server_.send(409, "text/plain", "The master is connected and controls recording.");
        return;
    }
    const String action = server_.arg("action");
    if (action != "start" && action != "stop") {
        server_.send(400, "text/plain", "action must be start or stop");
        return;
    }
    JsonDocument args, result;
    if (action == "start" && server_.hasArg("session")) args["session"] = server_.arg("session");
    String message;
    CmdStatus st = host_->dispatch(action.c_str(), "recording", args.as<JsonObjectConst>(), result.to<JsonObject>(), message);
    if (st != CmdStatus::Ok) {
        server_.send(500, "text/plain", "Couldn't " + action + ": " + message);
        return;
    }
    if (action == "start") stopStationSearch();
    server_.send(200, "text/plain", action == "stop" ? "Paused."
        : "Recording in session " + host_->session() + ". Working without the master now; WiFi switches off a minute after you disconnect.");
}

void WifiFeature::serveIcons() {
    auto png = [this](const char* path, const uint8_t* data, size_t len) {
        server_.on(path, HTTP_GET, [this, data, len] {
            server_.sendHeader("Cache-Control", "max-age=604800");
            server_.send_P(200, "image/png", reinterpret_cast<const char*>(data), len);
        });
    };
    png("/icon-180.png", ICON_180, sizeof(ICON_180));
    png("/icon-192.png", ICON_192, sizeof(ICON_192));
    png("/icon-512.png", ICON_512, sizeof(ICON_512));
    png("/favicon.ico", ICON_192, sizeof(ICON_192));
    server_.on("/manifest.json", HTTP_GET, [this] {
        JsonDocument d;
        d["name"] = "Logger " + ssid_;
        d["short_name"] = "Logger";
        d["start_url"] = "/";
        d["display"] = "standalone";
        d["background_color"] = "#11141a";
        d["theme_color"] = "#1f488c";
        JsonArray icons = d["icons"].to<JsonArray>();
        for (int s : {192, 512}) {
            JsonObject i = icons.add<JsonObject>();
            i["src"] = "/icon-" + String(s) + ".png";
            i["sizes"] = String(s) + "x" + String(s);
            i["type"] = "image/png";
        }
        String out;
        serializeJson(d, out);
        server_.send(200, "application/manifest+json", out);
    });
}

CmdStatus WifiFeature::command(const char* cmd, JsonObjectConst, JsonObject result, String& message) {
    if (strcmp(cmd, "off_until_restart") != 0) return CmdStatus::Unsupported;
    if (!enabled_) {
        message = "WiFi is already off";
        return CmdStatus::Failed;
    }
    host_->event("wifi_off", instance());
    radioOffAt_ = millis() + 1500;  // let the reply / ack go out first
    result["off_in_s"] = 1.5;
    return CmdStatus::Ok;
}

// Off until the next power-on: nothing is stored, so a restart brings WiFi back as configured
void WifiFeature::radioOff() {
    radioOffAt_ = 0;
    radioOff_ = true;
    enabled_ = false;
    MDNS.end();
    server_.stop();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    hotspotOn_ = false;
}

// Streams the uploaded file straight into the spare firmware slot (OtaFeature checks and installs it)
void WifiFeature::firmwareUpload() {
    HTTPUpload& up = server_.upload();
    if (up.status == UPLOAD_FILE_START) {
        uploadError_ = "";
        uploadOk_ = false;
        if (!ota_) uploadError_ = "firmware updates aren't enabled on this logger";
        else if (masterConnected()) uploadError_ = "the master is connected: update from its dashboard";
        else if (!ota_->uploadBegin(uploadError_)) {}
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (uploadError_.isEmpty() && !ota_->uploadWrite(up.buf, up.currentSize)) uploadError_ = "writing failed";
    } else if (up.status == UPLOAD_FILE_END) {
        if (uploadError_.isEmpty()) uploadOk_ = ota_->uploadEnd(uploadError_);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        if (ota_) ota_->uploadAbort();
        uploadError_ = "upload interrupted";
    }
}

void WifiFeature::firmwareUploadDone() {
    if (uploadOk_) server_.send(200, "text/plain", "Installed. The logger is restarting into the new firmware; reconnect in about 30 s.");
    else server_.send(400, "text/plain", "Not installed: " + (uploadError_.isEmpty() ? String("no file") : uploadError_));
}

void WifiFeature::poll() {
    if (radioOffAt_ && int32_t(millis() - radioOffAt_) >= 0) radioOff();
    if (!enabled_) return;

    // Recording on its own (no master), and the last phone has left the logger's hotspot: switch
    // WiFi off to save the battery. Never while the master is connected.
    if (hotspotOn_ && autoOffAfterMs_ && !masterConnected() && millis() - lastClientCheck_ >= 1000) {
        lastClientCheck_ = millis();
        if (WiFi.softAPgetStationNum() > 0) {
            hadClient_ = true;
            lastClientMs_ = millis();
        } else if (hadClient_ && host_->recording() && !radioOffAt_ && millis() - lastClientMs_ >= autoOffAfterMs_) {
            JsonDocument d;
            d["reason"] = "recording; hotspot unused";
            host_->event("wifi_off", instance(), d.as<JsonObjectConst>());
            radioOff();
            return;
        }
    }
    server_.handleClient();
    if (rebootAt_ && int32_t(millis() - rebootAt_) >= 0) {
        host_->stopRecording("reboot");  // closes the file cleanly
        delay(200);
        ESP.restart();
    }
    if (!nNetworks_ || stationStopped_) return;

    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !wasConnected_) {
        wasConnected_ = true;
        MDNS.begin(("logger-" + host_->deviceId()).c_str());
        MDNS.addService("http", "tcp", 80);
        if (!strcmp(hotspotMode_, "no_network")) setHotspot(false);
        JsonDocument d;
        d["ssid"] = WiFi.SSID();
        host_->event("wifi_connected", "wifi", d.as<JsonObjectConst>());
    } else if (!connected && wasConnected_) {
        wasConnected_ = false;
        MDNS.end();
        attemptStarted_ = millis();  // the driver reconnects by itself; give it the usual time
        host_->event("wifi_lost", "wifi");
    } else if (!connected && millis() - attemptStarted_ > CONNECT_TIMEOUT_MS) {
        // Not found: hotspot on (if wanted) while we keep cycling through the known networks
        if (strcmp(hotspotMode_, "never") != 0) setHotspot(true);
        current_ = (current_ + 1) % nNetworks_;
        tryNext();
    }

    // no_master: the hotspot is for when the master isn't around; give it time to connect first
    if (!strcmp(hotspotMode_, "no_master")) {
        static constexpr uint32_t MASTER_GRACE_MS = 20000;
        if (masterConnected()) {
            masterSeen_ = millis();
            setHotspot(false);
        } else if (millis() - masterSeen_ > MASTER_GRACE_MS) {
            setHotspot(true);
        }
    }
}

void WifiFeature::status(JsonObject out) const {
    if (!enabled_) {
        out["mode"] = "off";
        out["connected"] = false;
        if (radioOff_) out["off_until_restart"] = true;
        return;
    }
    if (stationStopped_) out["master_search"] = "stopped_until_restart";
    const bool connected = stationConnected();
    out["mode"] = connected ? "station" : hotspotOn_ ? "hotspot" : "station";
    out["connected"] = connected;
    out["master"] = masterConnected();
    if (connected) {
        out["ssid"] = WiFi.SSID();
        out["ip"] = WiFi.localIP().toString();
        out["rssi_dbm"] = WiFi.RSSI();
    } else if (nNetworks_) {
        out["trying"] = networks_[current_].ssid;
    }
    if (hotspotOn_) {
        out["hotspot"] = ssid_;
        out["hotspot_ip"] = WiFi.softAPIP().toString();
        out["clients"] = WiFi.softAPgetStationNum();
        if (!connected) {
            out["ssid"] = ssid_;
            out["ip"] = WiFi.softAPIP().toString();
        }
    }
}

}  // namespace logger
