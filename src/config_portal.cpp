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
#include <vector>
#include <algorithm>

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
static String tts_mode;         /* "sam", "groq", "openai", "elevenlabs", "off" */
static String tts_voice;        /* voice name or ElevenLabs voice_id */

/* per-service API keys */
static String key_groq;
static String key_gemini;
static String key_openai;
static String key_elevenlabs;
static String key_custom;
static String key_deepgram;
static String custom_url;       /* user-supplied base URL for "custom" */

/* provider selections */
static String llm_provider;     /* "groq", "gemini", "openai", "custom" */
static String stt_provider;     /* "groq", "openai", "custom" */
static String stt_model;        /* e.g. "whisper-large-v3-turbo" */
static String stt_custom_url;   /* base URL when stt_provider == "custom" */
static String stt_custom_key;   /* API key  when stt_provider == "custom" */
static String tts_custom_url;   /* base URL when tts_mode == "custom" */
static String tts_custom_key;   /* API key  when tts_mode == "custom" */
static String tts_model;        /* model    when tts_mode == "custom" */

static const char DEFAULT_SYS_PROMPT[] =
    "You are a mystical Magic 8 Ball. Give a brief, mysterious answer "
    "to whatever the user is wondering about. Keep your response to "
    "one short sentence (under 10 words). Be creative and mystical. "
    "Do not use quotes. Do not explain yourself. Just give the answer.";

/* ── provider URL table ────────────────────────────────────────── */
static String url_for_provider(const String &prov) {
    if (prov == "groq")      return "https://api.groq.com/openai/v1";
    if (prov == "gemini")    return "https://generativelanguage.googleapis.com/v1beta/openai";
    if (prov == "openai")    return "https://api.openai.com/v1";
    if (prov == "custom")    return custom_url;
    if (prov == "deepgram")  return "https://api.deepgram.com/v1";
    return "";
}

static String key_for_provider(const String &prov) {
    if (prov == "groq")      return key_groq;
    if (prov == "gemini")    return key_gemini;
    if (prov == "openai")    return key_openai;
    if (prov == "custom")    return key_custom;
    if (prov == "deepgram")  return key_deepgram;
    return "";
}

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
h3{color:#99b;font-size:14px;margin:14px 0 4px;padding-left:4px;border-left:3px solid #336}
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
table.keys{width:100%;border-collapse:collapse;margin-top:8px}
table.keys th{text-align:left;font-size:13px;color:#77a;padding:6px 4px;border-bottom:1px solid #222}
table.keys td{padding:4px}
table.keys td:first-child{width:90px;font-size:14px;color:#bbc;padding-left:4px}
table.keys input{margin-top:0;font-size:14px;padding:8px}
.custom-row{display:none}
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

<!-- ═══════ API Keys ═══════ -->
<h2>&#x1F511; API Keys</h2>
<p class="note">Add keys for each service you want to use. Only services with keys will appear in the provider dropdowns.</p>
<table class="keys">
<tr><th>Service</th><th>API Key</th></tr>
<tr><td>Groq</td><td><input id="key_groq" type="password" value="{{KEY_GROQ}}" placeholder="gsk_..."></td></tr>
<tr><td>Gemini</td><td><input id="key_gemini" type="password" value="{{KEY_GEMINI}}" placeholder="AIza..."></td></tr>
<tr><td>OpenAI</td><td><input id="key_openai" type="password" value="{{KEY_OPENAI}}" placeholder="sk-..."></td></tr>
<tr><td>ElevenLabs</td><td><input id="key_elevenlabs" type="password" value="{{KEY_11L}}" placeholder="xi-..."></td></tr>
<tr><td>Deepgram</td><td><input id="key_deepgram" type="password" value="{{KEY_DEEPGRAM}}" placeholder="deepgram_..."></td></tr>
</table>
<button class="btn btn-blue" type="button" onclick="saveKeys()">Save API Keys</button>
<p id="keys_msg" class="msg" style="color:#4a8"></p>

<!-- ═══════ Provider Selection ═══════ -->
<h2>&#x1F527; Provider Settings</h2>

<h3>&#x1F916; Language Model (LLM)</h3>
<label>Provider</label>
<select id="llm_provider">
  <option value="groq" {{LP_GROQ}}>Groq</option>
  <option value="gemini" {{LP_GEMINI}}>Gemini</option>
  <option value="openai" {{LP_OPENAI}}>OpenAI</option>
  <option value="custom" {{LP_CUSTOM}}>Custom</option>
</select>
<div id="custom_url_row" class="custom-row">
<label>Custom API Base URL</label>
<input id="custom_url" value="{{CUSTOM_URL}}" placeholder="https://your-api.example.com/v1">
<label>Custom API Key</label>
<input id="key_custom" type="password" value="{{KEY_CUSTOM}}" placeholder="key for custom URL">
</div>
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
<p class="note" id="model_note">Click Scan to fetch available models from the selected LLM provider.</p>

<h3>&#x1F3A4; Speech-to-Text (STT)</h3>
<label>Provider</label>
<select id="stt_provider" onchange="updateSttCustom()">
  <option value="groq" {{SP_GROQ}}>Groq (Whisper)</option>
  <option value="openai" {{SP_OPENAI}}>OpenAI (Whisper)</option>
  <option value="deepgram" {{SP_DEEPGRAM}}>Deepgram (Nova)</option>
  <option value="custom" {{SP_CUSTOM}}>Custom (OpenAI-compatible)</option>
</select>
<div id="stt_custom_row" class="custom-row">
<label>Custom STT Base URL</label>
<input id="stt_custom_url" value="{{STT_CUSTOM_URL}}" placeholder="https://your-api.example.com/v1">
<label>Custom STT API Key</label>
<input id="stt_custom_key" type="password" value="{{STT_CUSTOM_KEY}}" placeholder="API key">
</div>
<div class="row">
  <div>
    <label>STT Model</label>
    <select id="stt_model_sel">
      <option value="{{STT_MODEL}}">{{STT_MODEL}}</option>
    </select>
  </div>
  <button class="btn btn-teal btn-sm" type="button" onclick="scanSttModels()"
    style="width:90px;margin-bottom:1px" id="sttModelBtn">Scan</button>
</div>
<p class="note" id="stt_model_note">Click Scan to fetch available Whisper models from the selected STT provider.</p>

<h3>&#x1F50A; Text-to-Speech (TTS)</h3>
<label>Engine</label>
<select id="tts_mode" onchange="updateTtsVoices()">
  <option value="sam" {{SEL_SAM}}>SAM (offline, robotic)</option>
  <option value="groq" {{SEL_GROQ}}>Groq Orpheus (online, natural)</option>
  <option value="openai" {{SEL_OPENAI}}>OpenAI TTS (online, natural)</option>
  <option value="elevenlabs" {{SEL_11L}}>ElevenLabs (online, many voices)</option>
  <option value="custom" {{SEL_CUSTOM_TTS}}>Custom (OpenAI-compatible)</option>
  <option value="off" {{SEL_OFF}}>Off</option>
</select>
<p class="note">SAM runs locally. Others use their respective API key.</p>
<div id="groq_voice_row">
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
</div>
<div id="openai_voice_row" style="display:none">
<label>OpenAI TTS Voice</label>
<select id="tts_voice_openai">
  <option value="alloy" {{OV_ALLOY}}>Alloy (Neutral)</option>
  <option value="ash" {{OV_ASH}}>Ash (Male)</option>
  <option value="coral" {{OV_CORAL}}>Coral (Female)</option>
  <option value="echo" {{OV_ECHO}}>Echo (Male)</option>
  <option value="fable" {{OV_FABLE}}>Fable (Male, British)</option>
  <option value="nova" {{OV_NOVA}}>Nova (Female)</option>
  <option value="onyx" {{OV_ONYX}}>Onyx (Male, deep)</option>
  <option value="sage" {{OV_SAGE}}>Sage (Female)</option>
  <option value="shimmer" {{OV_SHIMMER}}>Shimmer (Female)</option>
</select>
</div>
<div id="elevenlabs_voice_row" style="display:none">
<label>ElevenLabs Voice</label>
<select id="tts_voice_11l" onchange="update11lCustom()">
  <option value="21m00Tcm4TlvDq8ikWAM" {{EV_RACHEL}}>Rachel (Female, warm)</option>
  <option value="pNInz6obpgDQGcFmaJgB" {{EV_ADAM}}>Adam (Male, deep)</option>
  <option value="EXAVITQu4vr4xnSDxMaL" {{EV_BELLA}}>Bella (Female, young)</option>
  <option value="ErXwobaYiN019PkySvjV" {{EV_ANTONI}}>Antoni (Male, crisp)</option>
  <option value="VR6AewLTigWG4xSOukaG" {{EV_ARNOLD}}>Arnold (Male, gruff)</option>
  <option value="MF3mGyEYCl7XYWbV9V6O" {{EV_ELLI}}>Elli (Female, child-like)</option>
  <option value="TxGEqnHWrfWFTfGW9XjX" {{EV_JOSH}}>Josh (Male, warm)</option>
  <option value="yoZ06aMxZJJ28mfd3POQ" {{EV_SAM}}>Sam (Male, raspy)</option>
  <option value="__custom__" {{EV_CUSTOM_SEL}}>Custom Voice ID...</option>
</select>
<div id="11l_custom_row" style="display:none">
<input id="tts_voice_11l_custom" value="{{EV_CUSTOM}}" placeholder="e.g. abc123def456">
<p class="note">Browse voices at elevenlabs.io/voice-library</p>
</div>
</div>

<div id="tts_custom_row" style="display:none">
<label>Custom TTS Base URL</label>
<input id="tts_custom_url" value="{{TTS_CUSTOM_URL}}" placeholder="https://your-api.example.com/v1">
<label>Custom TTS API Key</label>
<input id="tts_custom_key" type="password" value="{{TTS_CUSTOM_KEY}}" placeholder="API key">
<div class="row">
  <div>
    <label>TTS Model</label>
    <select id="tts_model_sel">
      <option value="{{TTS_MODEL}}">{{TTS_MODEL}}</option>
    </select>
  </div>
  <button class="btn btn-teal btn-sm" type="button" onclick="scanTtsModels()"
    style="width:90px;margin-bottom:1px" id="ttsModelBtn">Scan</button>
</div>
<p class="note" id="tts_model_note">Click Scan to fetch available TTS models from the custom endpoint.</p>
</div>

<button class="btn btn-blue" type="button" onclick="saveProviders()">Save Provider Settings</button>
<p id="api_msg" class="msg" style="color:#4a8"></p>

<!-- ═══════ Prompt Section ═══════ -->
<h2>&#x1F52E; System Prompt</h2>
<textarea id="sys_prompt" rows="4">{{SYS_PROMPT}}</textarea>
<p class="note">Customise the 8 Ball's personality and response style.</p>
<button class="btn btn-green" type="button" onclick="savePrompt()">Save Prompt</button>
<p id="prompt_msg" class="msg" style="color:#4a8"></p>

<script>
var providerUrls={
  groq:'https://api.groq.com/openai/v1',
  gemini:'https://generativelanguage.googleapis.com/v1beta/openai',
  openai:'https://api.openai.com/v1',
  custom:''
};
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
function saveKeys(){
  var d='key_groq='+encodeURIComponent(document.getElementById('key_groq').value)
       +'&key_gemini='+encodeURIComponent(document.getElementById('key_gemini').value)
       +'&key_openai='+encodeURIComponent(document.getElementById('key_openai').value)
       +'&key_elevenlabs='+encodeURIComponent(document.getElementById('key_elevenlabs').value)
       +'&key_deepgram='+encodeURIComponent(document.getElementById('key_deepgram').value)
       +'&key_custom='+encodeURIComponent(document.getElementById('key_custom').value)
       +'&custom_url='+encodeURIComponent(document.getElementById('custom_url').value);
  postForm('/save_keys',d,document.getElementById('keys_msg'),'API keys saved!');
}
function saveProviders(){
  var ttsMode=document.getElementById('tts_mode').value;
  var voice='';
  if(ttsMode==='groq') voice=document.getElementById('tts_voice').value;
  else if(ttsMode==='openai') voice=document.getElementById('tts_voice_openai').value;
  else if(ttsMode==='elevenlabs'){
    var sel=document.getElementById('tts_voice_11l').value;
    if(sel==='__custom__'){
      voice=document.getElementById('tts_voice_11l_custom').value.trim();
    } else {
      voice=sel;
    }
  }
  var d='llm_provider='+encodeURIComponent(document.getElementById('llm_provider').value)
       +'&model='+encodeURIComponent(document.getElementById('model_sel').value)
       +'&stt_provider='+encodeURIComponent(document.getElementById('stt_provider').value)
       +'&stt_model='+encodeURIComponent(document.getElementById('stt_model_sel').value)
       +'&stt_custom_url='+encodeURIComponent(document.getElementById('stt_custom_url').value)
       +'&stt_custom_key='+encodeURIComponent(document.getElementById('stt_custom_key').value)
       +'&tts_mode='+encodeURIComponent(ttsMode)
       +'&tts_voice='+encodeURIComponent(voice)
       +'&tts_custom_url='+encodeURIComponent(document.getElementById('tts_custom_url').value)
       +'&tts_custom_key='+encodeURIComponent(document.getElementById('tts_custom_key').value)
       +'&tts_model='+encodeURIComponent(document.getElementById('tts_model_sel').value);
  postForm('/save_api',d,document.getElementById('api_msg'),'Provider settings saved!');
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
function getActiveUrl(){
  var prov=document.getElementById('llm_provider').value;
  if(prov==='custom') return document.getElementById('custom_url').value;
  return providerUrls[prov]||'';
}
function getActiveKey(){
  var prov=document.getElementById('llm_provider').value;
  return document.getElementById('key_'+prov).value;
}
function scanModels(){
  var btn=document.getElementById('modelBtn');
  var note=document.getElementById('model_note');
  btn.disabled=true; btn.innerHTML='<span class="spinner"></span>';
  note.textContent='Fetching models...';
  var url=getActiveUrl();
  var key=getActiveKey();
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
function getSttKey(){
  var prov=document.getElementById('stt_provider').value;
  if(prov==='custom')   return document.getElementById('stt_custom_key').value;
  if(prov==='deepgram') return document.getElementById('key_deepgram').value;
  var el=document.getElementById('key_'+prov);
  return el?el.value:'';
}
function scanSttModels(){
  var prov=document.getElementById('stt_provider').value;
  if(prov==='deepgram'){populateSttModels(DG_MODELS);return;}
  var btn=document.getElementById('sttModelBtn');
  var note=document.getElementById('stt_model_note');
  btn.disabled=true; btn.innerHTML='<span class="spinner"></span>';
  note.textContent='Fetching models...';
  var key=getSttKey();
  var customUrl=(prov==='custom')?document.getElementById('stt_custom_url').value:'';
  fetch('/scan_stt_models?stt_provider='+encodeURIComponent(prov)+'&api_key='+encodeURIComponent(key)+'&custom_url='+encodeURIComponent(customUrl))
    .then(function(r){return r.json()})
    .then(function(list){
      populateSttModels(list.map(function(m){return m.id;}));
      btn.disabled=false;btn.textContent='Scan';
      note.textContent='Found '+list.length+' model(s).';
    }).catch(function(e){btn.disabled=false;btn.textContent='Scan';note.textContent='Error: '+e;});
}
function scanTtsModels(){
  var btn=document.getElementById('ttsModelBtn');
  var note=document.getElementById('tts_model_note');
  btn.disabled=true; btn.innerHTML='<span class="spinner"></span>';
  note.textContent='Fetching models...';
  var url=document.getElementById('tts_custom_url').value;
  var key=document.getElementById('tts_custom_key').value;
  fetch('/scan_tts_models?custom_url='+encodeURIComponent(url)+'&api_key='+encodeURIComponent(key))
    .then(function(r){return r.json()})
    .then(function(list){
      var sel=document.getElementById('tts_model_sel');
      var cur=sel.value;
      sel.innerHTML='';
      if(list.length===0){sel.innerHTML='<option value="">No models found</option>';btn.disabled=false;btn.textContent='Scan';note.textContent='No TTS models found.';return;}
      list.forEach(function(m){
        var o=document.createElement('option');o.value=m.id;
        o.textContent=m.id;
        if(m.id===cur)o.selected=true;
        sel.appendChild(o);
      });
      btn.disabled=false;btn.textContent='Scan';
      note.textContent='Found '+list.length+' model(s).';
    }).catch(function(e){btn.disabled=false;btn.textContent='Scan';note.textContent='Error: '+e;});
}
var DG_MODELS=['nova-3','nova-2','nova-2-general','nova-2-meeting','nova-2-phonecall','nova','enhanced','base'];
function populateSttModels(list){
  var sel=document.getElementById('stt_model_sel');
  var cur=sel.value;
  sel.innerHTML='';
  list.forEach(function(id){
    var o=document.createElement('option');o.value=id;o.textContent=id;
    if(id===cur)o.selected=true;
    sel.appendChild(o);
  });
  if(!sel.value&&list.length>0)sel.value=list[0];
}
/* show/hide custom URL row */
function updateCustom(){
  var v=document.getElementById('llm_provider').value;
  document.getElementById('custom_url_row').style.display=(v==='custom')?'block':'none';
}
function updateSttCustom(){
  var v=document.getElementById('stt_provider').value;
  document.getElementById('stt_custom_row').style.display=(v==='custom')?'block':'none';
  var btn=document.getElementById('sttModelBtn');
  var note=document.getElementById('stt_model_note');
  if(v==='deepgram'){
    btn.style.display='none';
    populateSttModels(DG_MODELS);
    note.textContent='Deepgram model list (hardcoded).';
  } else {
    btn.style.display='';
    note.textContent='Click Scan to fetch available Whisper models from the selected STT provider.';
  }
}
document.getElementById('llm_provider').addEventListener('change',updateCustom);
document.getElementById('stt_provider').addEventListener('change',updateSttCustom);
updateCustom();
updateSttCustom();
function updateTtsVoices(){
  var m=document.getElementById('tts_mode').value;
  document.getElementById('groq_voice_row').style.display=(m==='groq')?'block':'none';
  document.getElementById('openai_voice_row').style.display=(m==='openai')?'block':'none';
  document.getElementById('elevenlabs_voice_row').style.display=(m==='elevenlabs')?'block':'none';
  document.getElementById('tts_custom_row').style.display=(m==='custom')?'block':'none';
  if(m==='elevenlabs') update11lCustom();
}
function update11lCustom(){
  var sel=document.getElementById('tts_voice_11l').value;
  document.getElementById('11l_custom_row').style.display=(sel==='__custom__')?'block':'none';
}
updateTtsVoices();
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
    /* API keys */
    page.replace("{{KEY_GROQ}}",      htmlEscape(key_groq));
    page.replace("{{KEY_GEMINI}}",    htmlEscape(key_gemini));
    page.replace("{{KEY_OPENAI}}",    htmlEscape(key_openai));
    page.replace("{{KEY_11L}}",       htmlEscape(key_elevenlabs));
    page.replace("{{KEY_DEEPGRAM}}",  htmlEscape(key_deepgram));
    page.replace("{{KEY_CUSTOM}}",    htmlEscape(key_custom));
    page.replace("{{CUSTOM_URL}}",    htmlEscape(custom_url));
    /* LLM provider */
    page.replace("{{LP_GROQ}}",   llm_provider == "groq"   ? "selected" : "");
    page.replace("{{LP_GEMINI}}", llm_provider == "gemini" ? "selected" : "");
    page.replace("{{LP_OPENAI}}", llm_provider == "openai" ? "selected" : "");
    page.replace("{{LP_CUSTOM}}", llm_provider == "custom" ? "selected" : "");
    page.replace("{{MODEL}}", htmlEscape(model_name));
    /* STT provider */
    page.replace("{{SP_GROQ}}",    stt_provider == "groq"     ? "selected" : "");
    page.replace("{{SP_OPENAI}}",  stt_provider == "openai"   ? "selected" : "");
    page.replace("{{SP_DEEPGRAM}}",stt_provider == "deepgram" ? "selected" : "");
    page.replace("{{SP_CUSTOM}}",  stt_provider == "custom"   ? "selected" : "");
    page.replace("{{STT_MODEL}}", htmlEscape(stt_model));
    page.replace("{{STT_CUSTOM_URL}}", htmlEscape(stt_custom_url));
    page.replace("{{STT_CUSTOM_KEY}}", htmlEscape(stt_custom_key));
    /* TTS custom */
    page.replace("{{SEL_CUSTOM_TTS}}", tts_mode == "custom" ? "selected" : "");
    page.replace("{{TTS_CUSTOM_URL}}", htmlEscape(tts_custom_url));
    page.replace("{{TTS_CUSTOM_KEY}}", htmlEscape(tts_custom_key));
    page.replace("{{TTS_MODEL}}",      htmlEscape(tts_model));
    /* System prompt */
    page.replace("{{SYS_PROMPT}}", htmlEscape(system_prompt));
    /* TTS mode selection */
    page.replace("{{SEL_SAM}}",    tts_mode == "sam"    ? "selected" : "");
    page.replace("{{SEL_GROQ}}",   tts_mode == "groq"   ? "selected" : "");
    page.replace("{{SEL_OPENAI}}", tts_mode == "openai" ? "selected" : "");
    page.replace("{{SEL_11L}}",    tts_mode == "elevenlabs" ? "selected" : "");
    page.replace("{{SEL_OFF}}",    tts_mode == "off"    ? "selected" : "");
    /* Groq TTS voice selection */
    page.replace("{{V_TARA}}",   tts_voice == "tara"   ? "selected" : "");
    page.replace("{{V_AUTUMN}}", tts_voice == "autumn" ? "selected" : "");
    page.replace("{{V_DIANA}}",  tts_voice == "diana"  ? "selected" : "");
    page.replace("{{V_HANNAH}}", tts_voice == "hannah" ? "selected" : "");
    page.replace("{{V_TROY}}",   tts_voice == "troy"   ? "selected" : "");
    page.replace("{{V_AUSTIN}}", tts_voice == "austin" ? "selected" : "");
    page.replace("{{V_DANIEL}}", tts_voice == "daniel" ? "selected" : "");
    /* OpenAI TTS voice selection */
    page.replace("{{OV_ALLOY}}",   tts_voice == "alloy"   ? "selected" : "");
    page.replace("{{OV_ASH}}",     tts_voice == "ash"     ? "selected" : "");
    page.replace("{{OV_CORAL}}",   tts_voice == "coral"   ? "selected" : "");
    page.replace("{{OV_ECHO}}",    tts_voice == "echo"    ? "selected" : "");
    page.replace("{{OV_FABLE}}",   tts_voice == "fable"   ? "selected" : "");
    page.replace("{{OV_NOVA}}",    tts_voice == "nova"    ? "selected" : "");
    page.replace("{{OV_ONYX}}",    tts_voice == "onyx"    ? "selected" : "");
    page.replace("{{OV_SAGE}}",    tts_voice == "sage"    ? "selected" : "");
    page.replace("{{OV_SHIMMER}}", tts_voice == "shimmer" ? "selected" : "");
    /* ElevenLabs voice selection */
    page.replace("{{EV_RACHEL}}",  tts_voice == "21m00Tcm4TlvDq8ikWAM" ? "selected" : "");
    page.replace("{{EV_ADAM}}",    tts_voice == "pNInz6obpgDQGcFmaJgB" ? "selected" : "");
    page.replace("{{EV_BELLA}}",   tts_voice == "EXAVITQu4vr4xnSDxMaL" ? "selected" : "");
    page.replace("{{EV_ANTONI}}",  tts_voice == "ErXwobaYiN019PkySvjV" ? "selected" : "");
    page.replace("{{EV_ARNOLD}}",  tts_voice == "VR6AewLTigWG4xSOukaG" ? "selected" : "");
    page.replace("{{EV_ELLI}}",    tts_voice == "MF3mGyEYCl7XYWbV9V6O" ? "selected" : "");
    page.replace("{{EV_JOSH}}",    tts_voice == "TxGEqnHWrfWFTfGW9XjX" ? "selected" : "");
    page.replace("{{EV_SAM}}",     tts_voice == "yoZ06aMxZJJ28mfd3POQ" ? "selected" : "");
    /* If using a custom ElevenLabs voice ID, populate the custom input */
    String ev_custom_id = "";
    if (tts_mode == "elevenlabs") {
        const char *builtins[] = {"21m00Tcm4TlvDq8ikWAM","pNInz6obpgDQGcFmaJgB","EXAVITQu4vr4xnSDxMaL","ErXwobaYiN019PkySvjV","VR6AewLTigWG4xSOukaG","MF3mGyEYCl7XYWbV9V6O","TxGEqnHWrfWFTfGW9XjX","yoZ06aMxZJJ28mfd3POQ"};
        bool is_builtin = false;
        for (int i = 0; i < 8; i++) if (tts_voice == builtins[i]) { is_builtin = true; break; }
        if (!is_builtin) ev_custom_id = tts_voice;
    }
    page.replace("{{EV_CUSTOM_SEL}}", ev_custom_id.length() > 0 ? "selected" : "");
    page.replace("{{EV_CUSTOM}}", htmlEscape(ev_custom_id));
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

                    /* ── Collect candidate models ── */
                    struct ModelEntry { String id; String owner; long created; };
                    std::vector<ModelEntry> models;
                    models.reserve(data.size());

                    bool is_openai = (url.indexOf("openai.com") >= 0);

                    for (JsonObject m : data) {
                        const char *id = m["id"] | "";
                        if (!id[0]) continue;

                        /* Strip "models/" prefix (Gemini returns this) */
                        if (strncmp(id, "models/", 7) == 0) id += 7;

                        /* Skip non-language models (audio, image, embedding, moderation, guard, TTS) */
                        if (strstr(id, "whisper") || strstr(id, "tts") ||
                            strstr(id, "dall-e")  || strstr(id, "imagen") ||
                            strstr(id, "embed")   || strstr(id, "moderat") ||
                            strstr(id, "guard")   || strstr(id, "safeguard") ||
                            strstr(id, "compound") || strstr(id, "orpheus") ||
                            strstr(id, "transcri") || strstr(id, "speech") ||
                            strstr(id, "realtime") || strstr(id, "audio")  ||
                            strstr(id, "distil-")  || strstr(id, "playai") ||
                            strstr(id, "davinci")  || strstr(id, "babbage") ||
                            strstr(id, "curie")    || strstr(id, "ada")    ||
                            strstr(id, "-live"))
                            continue;

                        /* OpenAI: whitelist known chat-capable model patterns */
                        if (is_openai) {
                            bool chat_ok = strstr(id, "turbo") ||
                                           strstr(id, "4o")    ||
                                           strstr(id, "chatgpt") ||
                                           strncmp(id, "o1", 2) == 0 ||
                                           strncmp(id, "o3", 2) == 0 ||
                                           strncmp(id, "o4", 2) == 0 ||
                                           strstr(id, "gpt-4.1") ||
                                           strstr(id, "gpt-4.5") ||
                                           strstr(id, "gpt-5");
                            if (!chat_ok) continue;
                        }

                        const char *owner = m["owned_by"] | "";
                        long created = m["created"] | 0L;
                        models.push_back({String(id), String(owner), created});
                    }

                    /* ── Sort: preferred models first, then by created desc ── */
                    /* Priority keywords (lower = better) */
                    auto priority = [](const String &id) -> int {
                        /* Top-tier large/versatile models */
                        if (id.indexOf("70b") >= 0 || id.indexOf("versatile") >= 0 ||
                            id.indexOf("gpt-4o") >= 0 || id.indexOf("gpt-4.1") >= 0 ||
                            id.indexOf("pro") >= 0 || id.indexOf("flash") >= 0)
                            return 0;
                        /* Mid-tier chat models */
                        if (id.indexOf("llama") >= 0 || id.indexOf("gemma") >= 0 ||
                            id.indexOf("qwen") >= 0 || id.indexOf("mistral") >= 0 ||
                            id.indexOf("gpt-4") >= 0 || id.indexOf("claude") >= 0 ||
                            id.indexOf("deepseek") >= 0 || id.indexOf("gemini") >= 0)
                            return 1;
                        /* Known smaller/mini models */
                        if (id.indexOf("mini") >= 0 || id.indexOf("8b") >= 0 ||
                            id.indexOf("3b") >= 0 || id.indexOf("1b") >= 0 ||
                            id.indexOf("nano") >= 0 || id.indexOf("gpt-3.5") >= 0)
                            return 3;
                        /* Everything else */
                        return 2;
                    };

                    std::sort(models.begin(), models.end(),
                        [&priority](const ModelEntry &a, const ModelEntry &b) {
                            int pa = priority(a.id), pb = priority(b.id);
                            if (pa != pb) return pa < pb;
                            return a.created > b.created;  /* newer first */
                        });

                    for (const auto &e : models) {
                        if (count > 0) json += ",";
                        json += "{\"id\":\"";
                        json += e.id;
                        json += "\",\"owned_by\":\"";
                        json += e.owner;
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

static void handleScanSttModels()
{
    String prov           = web.arg("stt_provider");
    String key            = web.arg("api_key");
    String custom_url_arg = web.arg("custom_url");
    if (prov.length() == 0) prov = stt_provider;

    String url;
    if (prov == "openai")       url = "https://api.openai.com/v1";
    else if (prov == "deepgram") url = "https://api.deepgram.com/v1";
    else if (prov == "custom")  url = custom_url_arg.length() > 0 ? custom_url_arg : stt_custom_url;
    else                        url = "https://api.groq.com/openai/v1";

    Serial.printf("[PORTAL] STT model scan: provider=%s\n", prov.c_str());

    String json = "[";
    int count = 0;

    /* Deepgram: return hardcoded model list — their /v1/models response is
       hundreds of KB and would OOM the ESP32 heap when parsed. The available
       transcription models are stable and well-documented. */
    if (prov == "deepgram") {
        static const char * const DG_MODELS[] = {
            "nova-3", "nova-2", "nova-2-general", "nova-2-meeting",
            "nova-2-phonecall", "nova", "enhanced", "base"
        };
        for (size_t i = 0; i < sizeof(DG_MODELS)/sizeof(DG_MODELS[0]); i++) {
            if (count > 0) json += ",";
            json += "{\"id\":\"";
            json += DG_MODELS[i];
            json += "\"}";
            count++;
        }
    } else {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        String endpoint = url + "/models";
        if (http.begin(client, endpoint)) {
            if (key.length() > 0)
                http.addHeader("Authorization", "Bearer " + key);
            http.setTimeout(10000);
            int code = http.GET();
            Serial.printf("[PORTAL] STT models HTTP %d\n", code);
            if (code == 200) {
                String body = http.getString();
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, body);
                body = "";
                if (!err && doc["data"].is<JsonArray>()) {
                    for (JsonObject m : doc["data"].as<JsonArray>()) {
                        const char *id = m["id"] | "";
                        if (!id[0]) continue;
                        if (strstr(id, "whisper") || strstr(id, "distil-whisper")) {
                            if (count > 0) json += ",";
                            json += "{\"id\":\"";
                            json += id;
                            json += "\"}";
                            count++;
                        }
                    }
                }
            }
            http.end();
        }
    }

    json += "]";
    Serial.printf("[PORTAL] Returning %d STT models\n", count);
    web.send(200, "application/json", json);
}

static void handleScanTtsModels()
{
    String url = web.arg("custom_url");
    String key = web.arg("api_key");
    if (url.length() == 0) url = tts_custom_url;

    Serial.printf("[PORTAL] TTS model scan: %s\n", url.c_str());

    String json = "[";
    int count = 0;

    if (url.length() > 0) {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        String endpoint = url;
        if (!endpoint.endsWith("/")) endpoint += "/";
        endpoint += "models";

        if (http.begin(client, endpoint)) {
            if (key.length() > 0)
                http.addHeader("Authorization", "Bearer " + key);
            http.setTimeout(10000);

            int code = http.GET();
            Serial.printf("[PORTAL] TTS models HTTP %d\n", code);

            if (code == 200) {
                String body = http.getString();
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, body);
                body = "";
                if (!err && doc["data"].is<JsonArray>()) {
                    for (JsonObject m : doc["data"].as<JsonArray>()) {
                        const char *id = m["id"] | "";
                        if (!id[0]) continue;
                        /* Keep TTS / speech / audio-generation models */
                        if (strstr(id, "tts")     || strstr(id, "speech") ||
                            strstr(id, "orpheus") || strstr(id, "audio-gen")) {
                            if (count > 0) json += ",";
                            json += "{\"id\":\"";
                            json += id;
                            json += "\"}";
                            count++;
                        }
                    }
                }
            }
            http.end();
        }
    }

    json += "]";
    Serial.printf("[PORTAL] Returning %d TTS models\n", count);
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

static void (*settings_changed_cb)(void) = NULL;

void config_portal_set_settings_changed_cb(void (*cb)(void))
{
    settings_changed_cb = cb;
}

static void handleSaveApi()
{
    llm_provider  = web.arg("llm_provider");
    model_name    = web.arg("model");
    stt_provider  = web.arg("stt_provider");
    stt_model     = web.arg("stt_model");
    stt_custom_url = web.arg("stt_custom_url");
    stt_custom_key = web.arg("stt_custom_key");
    tts_mode      = web.arg("tts_mode");
    tts_voice     = web.arg("tts_voice");
    tts_custom_url = web.arg("tts_custom_url");
    tts_custom_key = web.arg("tts_custom_key");
    tts_model     = web.arg("tts_model");
    if (llm_provider.length() == 0) llm_provider = "groq";
    if (model_name.length() == 0)   model_name   = "llama-3.3-70b-versatile";
    if (stt_provider.length() == 0) stt_provider = "groq";
    if (stt_model.length() == 0)    stt_model    = (stt_provider == "openai") ? "whisper-1" :
                                                   (stt_provider == "deepgram") ? "nova-3" :
                                                   "whisper-large-v3-turbo";
    if (tts_mode.length() == 0)     tts_mode     = "sam";
    if (tts_voice.length() == 0)    tts_voice    = "tara";

    /* Derive api_url and api_key from the provider selections */
    api_url = url_for_provider(llm_provider);
    api_key = key_for_provider(llm_provider);

    Serial.println("[PORTAL] Saving provider settings:");
    Serial.printf("[PORTAL]   LLM:   %s → %s\n", llm_provider.c_str(), api_url.c_str());
    Serial.printf("[PORTAL]   Model: %s\n", model_name.c_str());
    Serial.printf("[PORTAL]   STT:   %s / %s\n", stt_provider.c_str(), stt_model.c_str());
    Serial.printf("[PORTAL]   TTS:   %s / %s\n", tts_mode.c_str(), tts_voice.c_str());

    prefs.begin("m8b", false);
    prefs.putString("llm_prov",  llm_provider);
    prefs.putString("stt_prov",       stt_provider);
    prefs.putString("stt_model",      stt_model);
    prefs.putString("stt_custom_url", stt_custom_url);
    prefs.putString("stt_custom_key", stt_custom_key);
    prefs.putString("api_url",        api_url);
    prefs.putString("api_key",        api_key);
    prefs.putString("model",          model_name);
    prefs.putString("tts_mode",       tts_mode);
    prefs.putString("tts_voice",      tts_voice);
    prefs.putString("tts_custom_url", tts_custom_url);
    prefs.putString("tts_custom_key", tts_custom_key);
    prefs.putString("tts_model",      tts_model);
    prefs.end();

    if (settings_changed_cb) settings_changed_cb();

    web.send(200, "text/plain", "Provider settings saved!");
}

static void handleSaveKeys()
{
    key_groq       = web.arg("key_groq");
    key_gemini     = web.arg("key_gemini");
    key_openai     = web.arg("key_openai");
    key_elevenlabs = web.arg("key_elevenlabs");
    key_deepgram   = web.arg("key_deepgram");
    key_custom     = web.arg("key_custom");
    custom_url     = web.arg("custom_url");

    Serial.println("[PORTAL] Saving API keys:");
    Serial.printf("[PORTAL]   Groq:     %d chars\n", key_groq.length());
    Serial.printf("[PORTAL]   Gemini:   %d chars\n", key_gemini.length());
    Serial.printf("[PORTAL]   OpenAI:   %d chars\n", key_openai.length());
    Serial.printf("[PORTAL]   11Labs:   %d chars\n", key_elevenlabs.length());
    Serial.printf("[PORTAL]   Deepgram: %d chars\n", key_deepgram.length());
    Serial.printf("[PORTAL]   Custom:   %d chars (URL: %s)\n", key_custom.length(), custom_url.c_str());

    prefs.begin("m8b", false);
    prefs.putString("key_groq",      key_groq);
    prefs.putString("key_gemini",    key_gemini);
    prefs.putString("key_openai",    key_openai);
    prefs.putString("key_11l",       key_elevenlabs);
    prefs.putString("key_deepgram",  key_deepgram);
    prefs.putString("key_custom",    key_custom);
    prefs.putString("custom_url",    custom_url);
    prefs.end();

    /* Update derived api_url/api_key in case the active provider's key changed */
    api_url = url_for_provider(llm_provider);
    api_key = key_for_provider(llm_provider);
    if (settings_changed_cb) settings_changed_cb();

    web.send(200, "text/plain", "API keys saved!");
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
    /* per-service API keys */
    key_groq      = prefs.getString("key_groq",      prefs.getString("api_key", ""));
    key_gemini    = prefs.getString("key_gemini",     "");
    key_openai    = prefs.getString("key_openai",     "");
    key_elevenlabs = prefs.getString("key_11l",       "");
    key_deepgram  = prefs.getString("key_deepgram",   "");
    key_custom    = prefs.getString("key_custom",     "");
    custom_url    = prefs.getString("custom_url",     "");
    /* provider selections */
    llm_provider  = prefs.getString("llm_prov",  "groq");
    stt_provider  = prefs.getString("stt_prov",  "groq");
    stt_model     = prefs.getString("stt_model", "");
    stt_custom_url = prefs.getString("stt_custom_url", "");
    stt_custom_key = prefs.getString("stt_custom_key", "");
    tts_custom_url = prefs.getString("tts_custom_url", "");
    tts_custom_key = prefs.getString("tts_custom_key", "");
    tts_model     = prefs.getString("tts_model",  "");
    /* derived URL/key (or legacy fallback) */
    api_url       = url_for_provider(llm_provider);
    api_key       = key_for_provider(llm_provider);
    if (api_url.length() == 0) api_url = prefs.getString("api_url", "https://api.groq.com/openai/v1");
    if (api_key.length() == 0) api_key = prefs.getString("api_key", "");
    model_name    = prefs.getString("model",   "llama-3.3-70b-versatile");
    system_prompt = prefs.getString("sys_prmpt", DEFAULT_SYS_PROMPT);
    tts_mode      = prefs.getString("tts_mode",  "sam");
    tts_voice     = prefs.getString("tts_voice", "tara");
    prefs.end();

    /* Default STT model based on provider if not yet saved */
    if (stt_model.length() == 0)
        stt_model = (stt_provider == "openai") ? "whisper-1" :
                    (stt_provider == "deepgram") ? "nova-3" :
                    "whisper-large-v3-turbo";

    Serial.println("[PORTAL] Loaded NVS settings:");
    Serial.printf("[PORTAL]   SSID:  '%s'\n", wifi_ssid.c_str());
    Serial.printf("[PORTAL]   LLM:   %s → %s\n", llm_provider.c_str(), api_url.c_str());
    Serial.printf("[PORTAL]   STT:   %s / %s\n", stt_provider.c_str(), stt_model.c_str());
    Serial.printf("[PORTAL]   Model: %s\n", model_name.c_str());
    Serial.printf("[PORTAL]   Keys:  groq=%d gemini=%d openai=%d 11l=%d custom=%d\n",
                  key_groq.length(), key_gemini.length(),
                  key_openai.length(), key_elevenlabs.length(),
                  key_custom.length());

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

    web.on("/",                HTTP_GET,  handleRoot);
    web.on("/scan_wifi",       HTTP_GET,  handleScanWifi);
    web.on("/scan_models",     HTTP_GET,  handleScanModels);
    web.on("/scan_stt_models", HTTP_GET,  handleScanSttModels);
    web.on("/scan_tts_models", HTTP_GET,  handleScanTtsModels);
    web.on("/save_wifi",       HTTP_POST, handleSaveWifi);
    web.on("/save_keys",       HTTP_POST, handleSaveKeys);
    web.on("/save_api",        HTTP_POST, handleSaveApi);
    web.on("/save_prompt",     HTTP_POST, handleSavePrompt);
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
String config_portal_get_stt_provider(void)  { return stt_provider; }
String config_portal_get_stt_model(void)     { return stt_model;    }
String config_portal_get_stt_url(void) {
    if (stt_provider == "custom")   return stt_custom_url;
    if (stt_provider == "deepgram") return "https://api.deepgram.com/v1";
    return url_for_provider(stt_provider);
}
String config_portal_get_stt_key(void) {
    if (stt_provider == "custom")   return stt_custom_key;
    if (stt_provider == "deepgram") return key_deepgram;
    return key_for_provider(stt_provider);
}
String config_portal_get_tts_key(void) {
    if (tts_mode == "custom")     return tts_custom_key;
    if (tts_mode == "openai")     return key_openai;
    if (tts_mode == "elevenlabs") return key_elevenlabs;
    return key_groq;  /* groq or sam (sam doesn't use it) */
}
String config_portal_get_tts_url(void) {
    if (tts_mode == "custom") return tts_custom_url;
    if (tts_mode == "openai") return url_for_provider("openai");
    return url_for_provider("groq");
}
String config_portal_get_tts_model(void) { return tts_model; }
