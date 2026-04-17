/*
 *  config_portal.cpp – WiFi setup + settings web portal
 *
 *  On first boot (no saved SSID) starts a captive-portal AP.
 *  Settings: WiFi SSID/password, LLM API URL / key / model, TTS.
 */

#include "config_portal.h"
#include "llm_client.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

static WebServer  web(80);
static DNSServer  dns;
static Preferences prefs;

static bool ap_mode    = false;
static bool configured = false;

/* persisted settings */
static String wifi_ssid;
static String wifi_pass;
static String api_url;
static String api_key;
static String model_name;
static String system_prompt;
static String tts_mode;         /* "sam", "groq", "off" */
static String tts_voice;        /* Groq voice: tara, troy, autumn, etc. */

static const char DEFAULT_SYS_PROMPT[] =
    "You are a mystical Magic 8 Ball. Give a brief, mysterious answer "
    "to whatever the user is wondering about. Keep your response to "
    "one short sentence (under 10 words). Be creative and mystical. "
    "Do not use quotes. Do not explain yourself. Just give the answer.";

/* helper: HTML-escape a string */
static String htmlEscape(const String &s) {
    String o; o.reserve(s.length() + 16);
    for (unsigned i = 0; i < s.length(); i++) {
        char c = s[i];
        if      (c == '&')  o += "&amp;";
        else if (c == '<')  o += "&lt;";
        else if (c == '>')  o += "&gt;";
        else if (c == '"')  o += "&quot;";
        else                o += c;
    }
    return o;
}

/* ── HTML page ─────────────────────────────────────────────────── */
static const char HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<title>Magic 8 Ball Setup</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*{box-sizing:border-box}
body{font-family:Arial,sans-serif;background:#0a0a1a;color:#ddd;margin:0;padding:20px}
.c{max-width:440px;margin:auto}
h1{text-align:center;color:#4488cc;margin-bottom:4px}
p.sub{text-align:center;color:#667;margin-top:0}
h2{color:#77a;font-size:16px;margin:18px 0 6px;border-bottom:1px solid #222;padding-bottom:6px}
label{display:block;margin-top:12px;font-size:14px;color:#99a}
input,select,textarea{width:100%;padding:10px;margin-top:4px;background:#12122a;
      color:#eee;border:1px solid #335;border-radius:6px;font-size:15px}
input:focus,select:focus,textarea:focus{border-color:#4488cc;outline:none}
textarea{resize:vertical;font-size:14px}
.btn{width:100%;padding:12px;margin-top:14px;color:#fff;border:none;
     border-radius:8px;font-size:16px;cursor:pointer}
.btn:active{filter:brightness(0.8)}
.btn-blue{background:#2266aa}
.btn-green{background:#2a5533}
.btn-teal{background:#1a6666}
.btn-sm{padding:8px;font-size:14px;margin-top:8px}
.note{font-size:12px;color:#556;margin-top:6px}
.msg{font-size:13px;margin-top:6px}
.row{display:flex;gap:8px;align-items:flex-end}
.row>*:first-child{flex:1}
.spinner{display:inline-block;width:14px;height:14px;border:2px solid #555;
         border-top:2px solid #4488cc;border-radius:50%;animation:spin .8s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
</style></head><body>
<div class="c">
<h1>&#x1F3B1; Magic 8 Ball</h1>
<p class="sub">Setup</p>

<!-- ═══════ WiFi Section ═══════ -->
<h2>&#x1F4F6; WiFi</h2>
<div class="row">
  <div>
    <label>Network</label>
    <input id="wifi_ssid" list="wifi_list" value="{{SSID}}" placeholder="Select or type SSID" autocomplete="off">
    <datalist id="wifi_list"></datalist>
  </div>
  <button class="btn btn-teal btn-sm" type="button" onclick="scanWifi()"
    style="width:90px;margin-bottom:1px" id="scanBtn">Scan</button>
</div>
<label>Password</label>
<input id="wifi_pass" type="password" value="{{PASS}}" placeholder="wifi password">
<button class="btn btn-blue" type="button" onclick="saveWifi()">Save WiFi &amp; Reboot</button>
<p id="wifi_msg" class="msg" style="color:#4a8"></p>

<!-- ═══════ API Section ═══════ -->
<h2>&#x1F527; API Settings</h2>
<label>LLM API Base URL</label>
<input id="api_url" value="{{API_URL}}" placeholder="https://api.groq.com/openai/v1">
<p class="note">Any OpenAI-compatible API. Groq free tier is the default.</p>
<label>API Key</label>
<input id="api_key" type="password" value="{{API_KEY}}" placeholder="gsk_...">
<p class="note">Get a free key at <b>console.groq.com</b></p>
<div class="row">
  <div>
    <label>Model</label>
    <select id="model_sel">
      <option value="{{MODEL}}">{{MODEL}}</option>
    </select>
  </div>
  <button class="btn btn-teal btn-sm" type="button" onclick="scanModels()"
    style="width:90px;margin-bottom:1px" id="modelBtn">Scan</button>
</div>
<p class="note" id="model_note">Click Scan to fetch available models from the API.</p>

<h2>&#x1F50A; Text-to-Speech</h2>
<label>TTS Engine</label>
<select id="tts_mode">
  <option value="sam" {{SEL_SAM}}>SAM (offline, robotic)</option>
  <option value="groq" {{SEL_GROQ}}>Groq Orpheus (online, natural)</option>
  <option value="off" {{SEL_OFF}}>Off</option>
</select>
<p class="note">SAM runs locally. Groq uses your API key (free tier: 100 req/day).</p>
<label>Groq TTS Voice</label>
<select id="tts_voice">
  <option value="tara" {{V_TARA}}>Tara (Female)</option>
  <option value="autumn" {{V_AUTUMN}}>Autumn (Female)</option>
  <option value="diana" {{V_DIANA}}>Diana (Female)</option>
  <option value="hannah" {{V_HANNAH}}>Hannah (Female)</option>
  <option value="troy" {{V_TROY}}>Troy (Male)</option>
  <option value="austin" {{V_AUSTIN}}>Austin (Male)</option>
  <option value="daniel" {{V_DANIEL}}>Daniel (Male)</option>
</select>
<p class="note">Only used when TTS Engine is set to Groq.</p>

<button class="btn btn-blue" type="button" onclick="saveApi()">Save API Settings</button>
<p id="api_msg" class="msg" style="color:#4a8"></p>

<!-- ═══════ Prompt Section ═══════ -->
<h2>&#x1F52E; System Prompt</h2>
<textarea id="sys_prompt" rows="4">{{SYS_PROMPT}}</textarea>
<p class="note">Customise the 8 Ball's personality and response style.</p>
<button class="btn btn-green" type="button" onclick="savePrompt()">Save Prompt</button>
<p id="prompt_msg" class="msg" style="color:#4a8"></p>

<script>
function postForm(url,data,msgEl,okMsg){
  msgEl.style.color='#4a8'; msgEl.textContent='Saving...';
  fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:data})
    .then(function(r){return r.text()})
    .then(function(t){msgEl.style.color='#4a8'; msgEl.textContent=t||okMsg;})
    .catch(function(e){msgEl.style.color='#a44'; msgEl.textContent='Error: '+e;});
}
function saveWifi(){
  var s=document.getElementById('wifi_ssid').value;
  var p=document.getElementById('wifi_pass').value;
  if(!s){alert('Enter or select a WiFi network');return;}
  postForm('/save_wifi','ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p),
           document.getElementById('wifi_msg'),'Saved! Rebooting...');
}
function saveApi(){
  var d='api_url='+encodeURIComponent(document.getElementById('api_url').value)
       +'&api_key='+encodeURIComponent(document.getElementById('api_key').value)
       +'&model='+encodeURIComponent(document.getElementById('model_sel').value)
       +'&tts_mode='+encodeURIComponent(document.getElementById('tts_mode').value)
       +'&tts_voice='+encodeURIComponent(document.getElementById('tts_voice').value);
  postForm('/save_api',d,document.getElementById('api_msg'),'Saved!');
}
function savePrompt(){
  postForm('/save_prompt','sys_prompt='+encodeURIComponent(document.getElementById('sys_prompt').value),
           document.getElementById('prompt_msg'),'Prompt saved and applied!');
}
function scanWifi(){
  var btn=document.getElementById('scanBtn');
  btn.disabled=true; btn.innerHTML='<span class="spinner"></span>';
  fetch('/scan_wifi').then(function(r){return r.json()}).then(function(list){
    var dl=document.getElementById('wifi_list');
    var inp=document.getElementById('wifi_ssid');
    dl.innerHTML='';
    if(list.length===0){btn.disabled=false;btn.textContent='Scan';return;}
    var seen={};
    list.forEach(function(n){
      if(!n.ssid||seen[n.ssid])return; seen[n.ssid]=1;
      var o=document.createElement('option');
      o.value=n.ssid;
      o.textContent=n.ssid+' ('+n.rssi+'dBm'+(n.open?' open':' *')+')';
      dl.appendChild(o);
    });
    inp.value=''; inp.focus();
    btn.disabled=false;btn.textContent='Scan';
  }).catch(function(e){btn.disabled=false;btn.textContent='Scan';alert('Scan failed: '+e);});
}
function scanModels(){
  var btn=document.getElementById('modelBtn');
  var note=document.getElementById('model_note');
  btn.disabled=true; btn.innerHTML='<span class="spinner"></span>';
  note.textContent='Fetching models...';
  var url=document.getElementById('api_url').value;
  var key=document.getElementById('api_key').value;
  fetch('/scan_models?api_url='+encodeURIComponent(url)+'&api_key='+encodeURIComponent(key))
    .then(function(r){return r.json()})
    .then(function(list){
      var sel=document.getElementById('model_sel');
      var cur=sel.value;
      sel.innerHTML='';
      if(list.length===0){sel.innerHTML='<option value="">No models found</option>';btn.disabled=false;btn.textContent='Scan';note.textContent='No chat models found.';return;}
      list.forEach(function(m){
        var o=document.createElement('option');o.value=m.id;
        o.textContent=m.id+(m.owned_by?' ('+m.owned_by+')':'');
        if(m.id===cur)o.selected=true;
        sel.appendChild(o);
      });
      btn.disabled=false;btn.textContent='Scan';
      note.textContent='Found '+list.length+' models.';
    }).catch(function(e){btn.disabled=false;btn.textContent='Scan';note.textContent='Error: '+e;});
}
</script>
</div></body></html>
)rawliteral";

static const char HTML_SAVED[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<title>Saved</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>body{font-family:Arial;background:#0a0a1a;color:#ddd;text-align:center;padding:60px}</style>
</head><body>
<h1>&#x2714; Saved!</h1>
<p>Rebooting&hellip; reconnect to your WiFi network.</p>
</body></html>
)rawliteral";

/* ── handlers ──────────────────────────────────────────────────── */
static void handleRoot()
{
    Serial.println("[PORTAL] Serving config page");
    String page = FPSTR(HTML);
    /* WiFi */
    page.replace("{{SSID}}", htmlEscape(wifi_ssid));
    page.replace("{{PASS}}", htmlEscape(wifi_pass));
    /* API */
    page.replace("{{API_URL}}", htmlEscape(api_url));
    page.replace("{{API_KEY}}", htmlEscape(api_key));
    page.replace("{{MODEL}}", htmlEscape(model_name));
    /* System prompt */
    page.replace("{{SYS_PROMPT}}", htmlEscape(system_prompt));
    /* TTS mode selection */
    page.replace("{{SEL_SAM}}",  tts_mode == "sam"  ? "selected" : "");
    page.replace("{{SEL_GROQ}}", tts_mode == "groq" ? "selected" : "");
    page.replace("{{SEL_OFF}}",  tts_mode == "off"  ? "selected" : "");
    /* TTS voice selection */
    page.replace("{{V_TARA}}",   tts_voice == "tara"   ? "selected" : "");
    page.replace("{{V_AUTUMN}}", tts_voice == "autumn" ? "selected" : "");
    page.replace("{{V_DIANA}}",  tts_voice == "diana"  ? "selected" : "");
    page.replace("{{V_HANNAH}}", tts_voice == "hannah" ? "selected" : "");
    page.replace("{{V_TROY}}",   tts_voice == "troy"   ? "selected" : "");
    page.replace("{{V_AUSTIN}}", tts_voice == "austin" ? "selected" : "");
    page.replace("{{V_DANIEL}}", tts_voice == "daniel" ? "selected" : "");
    web.send(200, "text/html", page);
}

static void handleScanWifi()
{
    Serial.println("[PORTAL] WiFi scan requested");
    int n = WiFi.scanNetworks(false, false, false, 300);
    Serial.printf("[PORTAL] Found %d networks\n", n);

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n && i < 30; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["ssid"] = WiFi.SSID(i);
        obj["rssi"] = WiFi.RSSI(i);
        obj["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    }
    WiFi.scanDelete();

    String json;
    serializeJson(doc, json);
    web.send(200, "application/json", json);
}

static void handleScanModels()
{
    String url = web.arg("api_url");
    String key = web.arg("api_key");
    Serial.printf("[PORTAL] Model scan: %s\n", url.c_str());

    if (url.length() == 0) {
        web.send(400, "application/json", "[]");
        return;
    }

    /* Build result JSON manually to avoid large JsonDocument allocations
       that can crash the ESP32 WebServer handler (limited stack). */
    String json = "[";
    int count = 0;

    {   /* scope block – ensures all large objects are freed before web.send() */
        WiFiClientSecure client;
        client.setInsecure();

        HTTPClient http;
        String endpoint = url;
        if (!endpoint.endsWith("/")) endpoint += "/";
        endpoint += "models";

        if (http.begin(client, endpoint)) {
            http.addHeader("Content-Type", "application/json");
            if (key.length() > 0)
                http.addHeader("Authorization", "Bearer " + key);
            http.setTimeout(10000);

            int code = http.GET();
            Serial.printf("[PORTAL] Models API HTTP %d\n", code);

            if (code == 200) {
                String body = http.getString();
                Serial.printf("[PORTAL] Body length: %d\n", body.length());
                JsonDocument apiDoc;
                DeserializationError err = deserializeJson(apiDoc, body);
                body = "";   /* free early */
                if (!err && apiDoc["data"].is<JsonArray>()) {
                    JsonArray data = apiDoc["data"].as<JsonArray>();
                    for (JsonObject m : data) {
                        const char *id = m["id"] | "";
                        if (strstr(id, "whisper") || strstr(id, "orpheus") ||
                            strstr(id, "guard") || strstr(id, "safeguard") ||
                            strstr(id, "compound")) continue;
                        const char *owner = m["owned_by"] | "";
                        if (count > 0) json += ",";
                        json += "{\"id\":\"";
                        json += id;
                        json += "\",\"owned_by\":\"";
                        json += owner;
                        json += "\"}";
                        count++;
                    }
                } else {
                    Serial.printf("[PORTAL] JSON parse error: %s\n", err.c_str());
                }
                apiDoc.clear();
            }
            http.end();
        }
    }   /* WiFiClientSecure + HTTPClient destroyed here */

    json += "]";
    Serial.printf("[PORTAL] Returning %d models\n", count);
    web.send(200, "application/json", json);
}

static void handleSaveWifi()
{
    wifi_ssid = web.arg("ssid");
    wifi_pass = web.arg("pass");

    Serial.printf("[PORTAL] Saving WiFi: SSID='%s'\n", wifi_ssid.c_str());

    prefs.begin("m8b", false);
    prefs.putString("ssid", wifi_ssid);
    prefs.putString("pass", wifi_pass);
    prefs.end();

    web.send(200, "text/plain", "WiFi saved! Rebooting...");
    Serial.println("[PORTAL] Rebooting in 1.5s...");
    delay(1500);
    ESP.restart();
}

static void handleSaveApi()
{
    api_url    = web.arg("api_url");
    api_key    = web.arg("api_key");
    model_name = web.arg("model");
    tts_mode   = web.arg("tts_mode");
    tts_voice  = web.arg("tts_voice");
    if (api_url.length() == 0)   api_url   = "https://api.groq.com/openai/v1";
    if (model_name.length() == 0) model_name = "llama-3.3-70b-versatile";
    if (tts_mode.length() == 0)  tts_mode  = "sam";
    if (tts_voice.length() == 0) tts_voice = "tara";

    Serial.println("[PORTAL] Saving API settings:");
    Serial.printf("[PORTAL]   URL:   %s\n", api_url.c_str());
    Serial.printf("[PORTAL]   Model: %s\n", model_name.c_str());
    Serial.printf("[PORTAL]   TTS:   %s / %s\n", tts_mode.c_str(), tts_voice.c_str());

    prefs.begin("m8b", false);
    prefs.putString("api_url",   api_url);
    prefs.putString("api_key",   api_key);
    prefs.putString("model",     model_name);
    prefs.putString("tts_mode",  tts_mode);
    prefs.putString("tts_voice", tts_voice);
    prefs.end();

    /* Apply model change live */
    if (api_key.length() > 0) {
        llm_init(api_url, api_key, model_name);
    }

    web.send(200, "text/plain", "API settings saved!");
}

static void handleSavePrompt()
{
    system_prompt = web.arg("sys_prompt");
    if (system_prompt.length() == 0) system_prompt = DEFAULT_SYS_PROMPT;

    prefs.begin("m8b", false);
    prefs.putString("sys_prmpt", system_prompt);
    prefs.end();

    /* Apply immediately without reboot */
    llm_set_system_prompt(system_prompt);

    Serial.printf("[PORTAL] System prompt updated (%d chars) – no reboot\n",
                  system_prompt.length());
    web.send(200, "text/plain", "Prompt saved and applied!");
}

static void handleNotFound()
{
    Serial.printf("[PORTAL] Unknown URL: %s → redirect to /\n", web.uri().c_str());
    web.sendHeader("Location", "http://192.168.4.1/");
    web.send(302, "text/plain", "");
}

/* ── public API ────────────────────────────────────────────────── */

void config_portal_start_ap(void)
{
    WiFi.mode(WIFI_AP_STA);   /* AP + STA so we can still scan */
    WiFi.softAP("Magic8Ball-Setup");
    ap_mode = true;
    dns.start(53, "*", WiFi.softAPIP());
    Serial.printf("AP started – connect to Magic8Ball-Setup, open 192.168.4.1\n");
}

void config_portal_begin(void)
{
    /* load saved settings */
    prefs.begin("m8b", true);
    wifi_ssid     = prefs.getString("ssid",    "");
    wifi_pass     = prefs.getString("pass",    "");
    api_url       = prefs.getString("api_url", "https://api.groq.com/openai/v1");
    api_key       = prefs.getString("api_key", "");
    model_name    = prefs.getString("model",   "llama-3.3-70b-versatile");
    system_prompt = prefs.getString("sys_prmpt", DEFAULT_SYS_PROMPT);
    tts_mode      = prefs.getString("tts_mode",  "sam");
    tts_voice     = prefs.getString("tts_voice", "tara");
    prefs.end();

    Serial.println("[PORTAL] Loaded NVS settings:");
    Serial.printf("[PORTAL]   SSID:  '%s'\n", wifi_ssid.c_str());
    Serial.printf("[PORTAL]   URL:   %s\n", api_url.c_str());
    Serial.printf("[PORTAL]   Model: %s\n", model_name.c_str());
    Serial.printf("[PORTAL]   Key:   %d chars\n", api_key.length());

    if (wifi_ssid.length() > 0) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
        Serial.printf("[PORTAL] Connecting to '%s' ", wifi_ssid.c_str());

        int tries = 0;
        while (WiFi.status() != WL_CONNECTED && tries < 20) {
            delay(500);
            Serial.print('.');
            tries++;
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            configured = true;
            Serial.printf("[PORTAL] WiFi CONNECTED – IP: %s  RSSI: %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
        } else {
            Serial.printf("[PORTAL] WiFi FAILED after %d tries (status=%d) – opening AP\n",
                          tries, WiFi.status());
            config_portal_start_ap();
        }
    } else {
        Serial.println("[PORTAL] No saved SSID – starting AP");
        config_portal_start_ap();
    }

    web.on("/",            HTTP_GET,  handleRoot);
    web.on("/scan_wifi",   HTTP_GET,  handleScanWifi);
    web.on("/scan_models", HTTP_GET,  handleScanModels);
    web.on("/save_wifi",   HTTP_POST, handleSaveWifi);
    web.on("/save_api",    HTTP_POST, handleSaveApi);
    web.on("/save_prompt", HTTP_POST, handleSavePrompt);
    web.onNotFound(handleNotFound);
    web.begin();
    Serial.println("[PORTAL] Web server started on port 80");
}

void config_portal_loop(void)
{
    if (ap_mode) dns.processNextRequest();
    web.handleClient();
}

bool   config_portal_is_configured(void) { return configured;  }
bool   config_portal_is_ap_mode(void)    { return ap_mode;     }
String config_portal_get_api_url(void)   { return api_url;     }
String config_portal_get_api_key(void)   { return api_key;     }
String config_portal_get_model(void)     { return model_name;  }
String config_portal_get_system_prompt(void) { return system_prompt; }
String config_portal_get_tts_mode(void)      { return tts_mode; }
String config_portal_get_tts_voice(void)     { return tts_voice; }
