#include "web_server.h"
#include "../config_store.h"
#include "../themes.h"
#include "../fleet.h"
#include "Display_ST7701.h"
#include "esp_lcd_panel_rgb.h"
#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <ArduinoOTA.h>
#include <Update.h>

#define WIFI_CHANNEL 1

static WebServer server(80);

static String colorToHex(uint32_t color) {
  char buf[8]; snprintf(buf, sizeof(buf), "#%06X", color); return String(buf);
}
static uint32_t hexToColor(String hex) {
  hex.replace("#", ""); return strtoul(hex.c_str(), NULL, 16);
}

static void handleRoot() {
  String html;
  html.reserve(7400); // pre-allocate to avoid repeated heap reallocs under PSRAM pressure
  html = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><style>"
         "*{box-sizing:border-box}"
         "body{font-family:system-ui,'Segoe UI',Roboto,sans-serif;background:#0a0b0d;color:#e9eaec;margin:0 auto;padding:16px;max-width:540px}"
         "h1{font-size:21px;letter-spacing:3px;text-transform:uppercase;margin:0;font-weight:700;display:flex;align-items:center}"
         "h1::before{content:'';width:5px;height:22px;background:#ff8a00;margin-right:11px;border-radius:2px;box-shadow:0 0 8px rgba(255,138,0,.6)}"
         ".status{display:flex;gap:8px;flex-wrap:wrap;margin:10px 0}"
         ".chip{background:#1c1f25;border:1px solid #2a2e36;border-radius:20px;padding:4px 12px;font-size:11px;letter-spacing:1px;color:#868d97;text-transform:uppercase}"
         ".chip b{color:#ff8a00;font-weight:700}"
         ".card{background:#14161a;border:1px solid #2a2e36;border-left:3px solid #ff8a00;border-radius:10px;padding:15px;margin:13px 0}"
         ".lbl,.card h3{font-size:11px;letter-spacing:2.5px;text-transform:uppercase;color:#868d97;font-weight:600}"
         ".card h3{margin:0 0 13px}.lbl{margin:16px 4px 6px;display:block}.card h4{font-size:13px;margin:0 0 6px}"
         ".row{display:grid;grid-template-columns:1fr 1fr;gap:9px}"
         "button{font-size:15px;padding:11px 10px;border:1px solid #2a2e36;border-radius:8px;background:#1c1f25;color:#e9eaec;cursor:pointer;transition:.13s;width:100%;font-family:inherit;letter-spacing:.5px}"
         "button:hover{border-color:#ff8a00;background:#23262d}button:active{transform:translateY(1px)}"
         ".on,.on:hover{background:rgba(30,215,96,.15);border-color:#1ed760;color:#1ed760;box-shadow:0 0 12px rgba(30,215,96,.22)}"
         ".active,.active:hover{background:rgba(255,138,0,.16);border-color:#ff8a00;color:#ff8a00;box-shadow:0 0 12px rgba(255,138,0,.25)}"
         ".primary,.primary:hover{background:#ff8a00;border-color:#ff8a00;color:#0a0b0d;font-weight:700}"
         ".danger,.danger:hover{border-color:#ff3b30;color:#ff3b30}"
         "label{color:#868d97;font-size:14px}.crow{display:flex;justify-content:space-between;align-items:center;margin:8px 0}"
         "input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:6px;border-radius:3px;background:#2a2e36;outline:none;margin:4px 0 10px}"
         "input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;border-radius:50%;background:#ff8a00;cursor:pointer;box-shadow:0 0 8px rgba(255,138,0,.7)}"
         "input[type=range]::-moz-range-thumb{width:20px;height:20px;border:none;border-radius:50%;background:#ff8a00}"
         "input[type=color]{width:46px;height:34px;border:1px solid #2a2e36;border-radius:6px;background:none;padding:2px;cursor:pointer}"
         "input[type=text]{background:#1c1f25;color:#e9eaec;border:1px solid #2a2e36;border-radius:8px;padding:10px;font-size:15px;width:100%;margin-bottom:9px}"
         "select{background:#1c1f25;color:#e9eaec;border:1px solid #2a2e36;border-radius:8px;padding:9px;font-size:14px}"
         "small{color:#5f6670;font-size:12px}#brval{color:#ff8a00;font-weight:700;font-size:18px}"
         "</style></head><body>";
  html += "<h1>" + device_name + "</h1>";
  html += "<div class='status'><span class='chip'><b id='chippeers'>" + String(fleet_count) + "</b> peers</span><span class='chip'>Mode <b id='chipmode'>" + String(MODE_NAMES[current_mode]) + "</b></span><span class='chip'>Page <b id='chippage'>" + String(current_page==PAGE_GAUGE?"GAUGE":"GLOWCRAFT") + "</b></span><span class='chip'>Theme <b id='chipslot'>P" + String(active_theme) + "</b>" + String(trimpot_theme_sync?" \xE2\x9F\xB2":"") + "</span></div>";
  html += "<div class='card'><h3>Device Name</h3><form action='/name' method='get'><input type='text' name='n' value='" + device_name + "' maxlength='20'><button class='danger'>Rename</button></form><small>WiFi AP name (Haltech-[name]). Restarts device.</small></div>";

  html += "<div class='card'><h3>Trimpot Theme Sync</h3>";
  html += "<button id='tpsync' class='" + String(trimpot_theme_sync?"on":"") + "' onclick=\"tgl('/themesync','tpsync','Trim Sync: ')\">Trim Sync: " + String(trimpot_theme_sync?"ON":"OFF") + "</button>";
  html += "<span class='lbl'>Editing slot (Rotary Trim 3 position)</span><div class='row'>";
  for (int i = 0; i < THEME_SLOTS; i++)
    html += "<button id='sl" + String(i) + "' class='sl" + String(active_theme==i?" active":"") + "' onclick=\"setSlot(" + String(i) + ",this)\">P" + String(i) + "</button>";
  html += "</div><small>The colours below edit the selected slot. With Trim Sync ON, Rotary Trim 3 selects the live slot automatically.</small></div>";

  html += "<div class='card'><h3>Dynamic Elements</h3><form action='/theme' method='get' onsubmit='return subm(event,this)'><div class='crow'><label>Text</label><input type='color' name='ct' value='" + colorToHex(text_color) + "'></div><div class='crow'><label>Low</label><input type='color' name='cl' value='" + colorToHex(color_low) + "'></div><div class='crow'><label>Mid</label><input type='color' name='cm' value='" + colorToHex(color_mid) + "'></div><div class='crow'><label>High</label><input type='color' name='ch' value='" + colorToHex(color_high) + "'></div><button class='primary'>Apply to ALL</button></form></div>";

  html += "<div class='card'><h3>Static Elements</h3><form action='/uicolors' method='get' onsubmit='return subm(event,this)'><div class='crow'><label>Background</label><input type='color' name='cbg' value='" + colorToHex(color_background) + "'></div><div class='crow'><label>Mode Label</label><input type='color' name='cml' value='" + colorToHex(color_mode_label) + "'></div><div class='crow'><label>Link Icon</label><input type='color' name='cli' value='" + colorToHex(color_link_icon) + "'></div><div class='crow'><label>Needle</label><input type='color' name='cn' value='" + colorToHex(needle_color) + "'></div><div class='crow'><label>Peak Stripe</label><input type='color' name='cp' value='" + colorToHex(color_peak) + "'></div><button class='primary'>Apply to ALL</button></form></div>";

  // BACKGROUND GRADIENT (per active slot). Stop 1 = the slot background colour.
  {
    const char* GT[6] = {"Solid","Linear \xE2\x86\x95","Linear \xE2\x86\x94","Linear \xE2\x88\xA0","Radial","Conical"};
    html += "<div class='card'><h3>Background Gradient</h3><form action='/grad' method='get' onsubmit='return subm(event,this)'>";
    html += "<div class='crow'><label>Type</label><select name='gt' id='gt' onchange='gradUI()'>";
    for (int i = 0; i < 6; i++) html += "<option value='" + String(i) + "'" + String(bg_grad_type==i?" selected":"") + ">" + String(GT[i]) + "</option>";
    html += "</select></div>";
    html += "<div class='crow'><label>Stops</label><select name='gs' id='gs' onchange='gradUI()'><option value='2'" + String(bg_grad_stops==2?" selected":"") + ">2</option><option value='3'" + String(bg_grad_stops==3?" selected":"") + ">3</option></select></div>";
    html += "<div class='crow'><label>Stop 1 (Background)</label><input type='color' name='cbg' value='" + colorToHex(color_background) + "'></div>";
    html += "<div class='crow'><label>Stop 2</label><input type='color' name='b2' value='" + colorToHex(color_background2) + "'></div>";
    html += "<div class='crow' id='b3row'><label>Stop 3</label><input type='color' name='b3' value='" + colorToHex(color_background3) + "'></div>";
    html += "<div class='crow' id='garow'><label>Angle</label><span id='gaval'>" + String(bg_grad_angle) + "\xC2\xB0</span></div>";
    html += "<input type='range' id='ga' name='ga' min='0' max='360' value='" + String(bg_grad_angle) + "' oninput=\"document.getElementById('gaval').textContent=this.value+'\xC2\xB0'\">";
    html += "<button class='primary'>Apply to ALL</button></form><small>Stop 1 is the slot background. Angle applies to Linear \xE2\x88\xA0 and Conical.</small></div>";
  }

  html += "<div class='card'><h3>Global Controls</h3>";
  html += "<div class='crow'><label>Brightness</label><span id='brval'>" + String(current_brightness) + "</span></div>";
  html += "<input type='range' min='10' max='100' value='" + String(current_brightness) + "' oninput=\"document.getElementById('brval').textContent=this.value\" onchange=\"setBright(this.value,this)\">";
  html += "<div class='row'>";
  html += "<button id='test' class='" + String(test_mode_enabled?"on":"") + "' onclick=\"tgl('/test','test','Test: ')\">Test: " + String(test_mode_enabled?"ON":"OFF") + "</button>";
  html += "<button id='stats' class='" + String(show_perf_stats?"on":"") + "' onclick=\"tgl('/stats','stats','Stats: ')\">Stats: " + String(show_perf_stats?"ON":"OFF") + "</button>";
  html += "<button id='dbg' class='" + String(debug_mode_enabled?"on":"") + "' onclick=\"tgl('/debug','dbg','Debug: ')\">Debug: " + String(debug_mode_enabled?"ON":"OFF") + "</button>";
  html += "<button id='font' onclick=\"tglFont(this)\">Font: " + String(current_font == 0 ? "DSEG14" : "Fira Mono") + "</button>";
  html += "</div><div class='row'>";
  html += "<button onclick=\"location='/preview'\">Live Preview</button>";
  html += "<button onclick=\"location='/ota'\">OTA Update</button>";
  html += "</div></div>";

  html += "<div class='card'><h3>Display Page</h3><div class='row'>";
  html += "<button id='pg0' class='pg" + String(current_page==PAGE_GAUGE?" active":"") + "' onclick=\"setPage(0,this)\">Gauge</button>";
  html += "<button id='pg1' class='pg" + String(current_page==PAGE_GLOWCRAFT?" active":"") + "' onclick=\"setPage(1,this)\">GlowCraft</button>";
  html += "</div></div>";

  html += "<div class='card'><h3>Local Gauge</h3>";
  html += "<button id='peak' class='" + String(peak_hold_enabled?"on":"") + "' onclick=\"tgl('/peak','peak','Peak Hold: ')\">Peak Hold: " + String(peak_hold_enabled?"ON":"OFF") + "</button>";

  // SECONDARY METRIC (shown when peak hold is off)
  html += "<span class='lbl'>Secondary metric (Peak Hold OFF)</span>";
  html += "<form action='/secondary' method='get' onsubmit='return subm(event,this)' style='display:flex;gap:8px;align-items:center'>";
  html += "<select name='sm' style='flex:1'>";
  for (int i = 0; i < SECONDARY_COUNT; i++) {
    html += "<option value='" + String(i) + "'";
    if (secondary_metric == i) html += " selected";
    html += ">" + String(SECONDARY_NAMES[i]) + "</option>";
  }
  html += "</select>";
  html += "<button type='submit' class='primary' style='width:auto;padding:9px 16px'>Set</button>";
  html += "</form>";

  html += "<span class='lbl'>Display Metric</span><div class='row'>";
  html += "<button class='m" + String(current_mode==0?" active":"") + "' onclick=\"setMode(0,this)\">Boost</button>";
  html += "<button class='m" + String(current_mode==1?" active":"") + "' onclick=\"setMode(1,this)\">AFR</button>";
  html += "<button class='m" + String(current_mode==2?" active":"") + "' onclick=\"setMode(2,this)\">Water</button>";
  html += "<button class='m" + String(current_mode==3?" active":"") + "' onclick=\"setMode(3,this)\">Oil</button>";
  html += "</div></div>";

  {
    PeerGauge peers[10];                       // consistent copy — fleet[] is written from the ESP-NOW callback
    int n_peers = fleet_snapshot(peers, 10);
    if (n_peers > 0) {
      html += "<span class='lbl'>Remote Gauges</span>";
      for(int i=0; i<n_peers; i++) {
          if (millis() - peers[i].last_seen < 10000) {
              String macStr = "";
              for(int j=0; j<6; j++) { if(j>0) macStr += ":"; char buf[3]; sprintf(buf, "%02X", peers[i].mac[j]); macStr += buf; }
              String macClean = macStr; macClean.replace(":", "");
              html += "<div class='card'><h4>Gauge " + macClean.substring(9) + "</h4><div class='status'><span class='chip'>Mode <b>" + String(MODE_NAMES[constrain(peers[i].mode,0,3)]) + "</b></span></div><div class='row'><button onclick=\"rem('/rem?mac=" + macClean + "&mode=0',this)\">Boost</button><button onclick=\"rem('/rem?mac=" + macClean + "&mode=1',this)\">AFR</button><button onclick=\"rem('/rem?mac=" + macClean + "&mode=2',this)\">Water</button><button onclick=\"rem('/rem?mac=" + macClean + "&mode=3',this)\">Oil</button></div></div>";
          }
      }
    }
  }
  // Background-fetch helpers. Each control updates from the server's reported new
  // state and flashes green on confirmed success / red if the request failed.
  html += "<script>"
          "function flash(b,ok){if(!b)return;b.style.boxShadow='0 0 16px '+(ok?'#1ed760':'#ff3b30');setTimeout(function(){b.style.boxShadow='';},450);}"
          "function gt(u){return fetch(u).then(function(r){if(!r.ok)throw 0;return r.text();});}"
          "function tgl(u,id,pre){var b=document.getElementById(id);gt(u).then(function(s){var on=s.trim()=='1';b.textContent=pre+(on?'ON':'OFF');b.classList.toggle('on',on);flash(b,1);}).catch(function(){flash(b,0);});}"
          "var MN=['BOOST','AFR','WATER','OIL P'];"
          "function setMode(m,b){gt('/set?mode='+m).then(function(s){var i=parseInt(s),c=document.getElementById('chipmode');if(c)c.textContent=MN[i];document.querySelectorAll('.m').forEach(function(x){x.classList.remove('active')});b.classList.add('active');flash(b,1);}).catch(function(){flash(b,0);});}"
          "function tglFont(b){gt('/font').then(function(s){b.textContent='Font: '+(parseInt(s)==0?'DSEG14':'Fira Mono');flash(b,1);}).catch(function(){flash(b,0);});}"
          "function setPage(p,b){gt('/page?pg='+p).then(function(s){var pg=parseInt(s),c=document.getElementById('chippage');if(c)c.textContent=pg==0?'GAUGE':'GLOWCRAFT';document.querySelectorAll('.pg').forEach(function(x){x.classList.remove('active')});b.classList.add('active');flash(b,1);}).catch(function(){flash(b,0);});}"
          "function post(u,b){return gt(u).then(function(s){flash(b,true);return s;}).catch(function(){flash(b,false);});}"
          "function subm(ev,f){ev.preventDefault();post(f.getAttribute('action')+'?'+new URLSearchParams(new FormData(f)).toString(),f.querySelector('button'));return false;}"
          "function setBright(v,b){post('/bright?b='+v,b);}"
          "function rem(u,b){post(u,b);}"
          "function setSlot(s,b){gt('/themeslot?s='+s).then(function(){location.reload();}).catch(function(){flash(b,0);});}"
          // Show/hide gradient sub-controls: stop-3 only when 3 stops, angle only for Linear-angle(3)/Conical(5).
          "function gradUI(){var t=+document.getElementById('gt').value,s=+document.getElementById('gs').value;"
          "document.getElementById('b3row').style.display=(t!=0&&s==3)?'':'none';"
          "var ang=(t==3||t==5);document.getElementById('garow').style.display=ang?'':'none';document.getElementById('ga').style.display=ang?'':'none';}"
          "gradUI();"
          "</script>";
  html += "<footer style='text-align:center;opacity:0.5;font-size:12px;margin:24px 0 10px'>v" FIRMWARE_VERSION " &middot; built " FIRMWARE_BUILD "</footer>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

static void handleTheme() {
    if (server.hasArg("ct")) {
        text_color = hexToColor(server.arg("ct"));
        color_low = hexToColor(server.arg("cl"));
        color_mid = hexToColor(server.arg("cm"));
        color_high = hexToColor(server.arg("ch"));
        cfg_put_uint("ct", text_color); cfg_put_uint("cl", color_low);
        cfg_put_uint("cm", color_mid);  cfg_put_uint("ch", color_high);
        EspNowPacket pkt = {}; pkt.type = 3; pkt.c1=text_color; pkt.c2=color_low; pkt.c3=color_mid; pkt.c4=color_high;
        broadcast_packet(&pkt);
        flag_theme_update = true;
        globals_to_theme(active_theme); persist_theme(active_theme);  // edit lands in the active slot
    }
    server.send(200, "text/plain", "OK");
}
static void handleSet() {
    if (server.hasArg("mode")) {
        int m = constrain(server.arg("mode").toInt(), 0, 3);
        Serial.printf("[HTTP] handleSet: local mode -> %d (live)\n", m);
        current_mode = (GaugeMode)m;
        cfg_put_int("mode", m);
        flag_mode_update = true;   // apply live — no restart
    }
    // Reply with the applied mode index — the button confirms from this response.
    server.send(200, "text/plain", String((int)current_mode));
}
// Toggle handlers flip server-side and return the new state ("1"/"0") so the web
// UI can confirm the change actually took, rather than guessing optimistically.
static void handleTest() {
    test_mode_enabled = !test_mode_enabled;
    EspNowPacket pkt = {}; pkt.type = 4; pkt.value = test_mode_enabled?1:0; broadcast_packet(&pkt);
    server.send(200, "text/plain", test_mode_enabled ? "1" : "0");
}
static void handleStats() {
    show_perf_stats = !show_perf_stats;
    EspNowPacket pkt = {}; pkt.type = 6; pkt.value = show_perf_stats?1:0; broadcast_packet(&pkt);
    flag_stats_update = true;
    server.send(200, "text/plain", show_perf_stats ? "1" : "0");
}
static void handleDebug() {
    debug_mode_enabled = !debug_mode_enabled;
    cfg_put_bool("dbg", debug_mode_enabled);
    server.send(200, "text/plain", debug_mode_enabled ? "1" : "0");
}
static void handleBright() {
    if (server.hasArg("b")) {
        int b = constrain(server.arg("b").toInt(), 10, 100);
        current_brightness = b; set_backlight(b);
        cfg_put_int("bright", b);
        EspNowPacket pkt = {}; pkt.type = 5; pkt.value = b; broadcast_packet(&pkt);
    }
    server.send(200, "text/plain", String(current_brightness));
}
static void handlePeak() {
    peak_hold_enabled = !peak_hold_enabled;
    if (peak_hold_enabled) { peak_val = -999.0f; peak_low_val = 999.0f; }
    cfg_put_bool("peak", peak_hold_enabled);
    server.send(200, "text/plain", peak_hold_enabled ? "1" : "0");
}
static void handleSecondary() {
    if (server.hasArg("sm"))
        secondary_metric = (uint8_t)constrain(server.arg("sm").toInt(), 0, SECONDARY_COUNT - 1);
    cfg_put_uint("sm", secondary_metric);
    server.send(200, "text/plain", String((int)secondary_metric));
}
static void handlePage() {
    if (server.hasArg("pg")) {
        int p = constrain(server.arg("pg").toInt(), 0, 1);
        current_page = (DisplayPage)p;
        cfg_put_uint("page", (uint32_t)current_page);
        flag_page_update = true;
    }
    server.send(200, "text/plain", String((int)current_page));
}
static void handleFont() {
    current_font = (current_font == 0) ? 1 : 0;   // two fonts: DSEG14 (0) / Fira Mono (1)
    cfg_put_uint("font", current_font);
    flag_theme_update = true;
    server.send(200, "text/plain", String((int)current_font));
}
// Capture the live RGB framebuffer and serve it as a BMP image.
// The RGB panel keeps 2 PSRAM framebuffers; we grab whichever is current.
// Row-streamed: converts + sends one row at a time from a static buffer, so a
// snapshot no longer allocates ~691KB of PSRAM per request. Rate-limited since
// each request still blocks the loop (LVGL) for the duration of the send.
// The framebuffer is read unlocked while the panel scans it — occasional
// tearing is accepted for a diagnostic view; do NOT lock against the vsync path.
static void handleSnapshot() {
    static uint32_t last_snap_ms = 0;
    if (last_snap_ms && millis() - last_snap_ms < 500) {
        server.send(429, "text/plain", "Too fast");
        return;
    }
    last_snap_ms = millis();

    void *fb0 = nullptr, *fb1 = nullptr;
    if (esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &fb0, &fb1) != ESP_OK || fb0 == nullptr) {
        server.send(500, "text/plain", "Framebuffer unavailable");
        return;
    }

    const int W = 480, H = 480;
    const int row_stride = (W * 3 + 3) & ~3;  // BMP rows padded to 4 bytes
    const int file_size  = 54 + row_stride * H;
    static uint8_t rowbuf[(480 * 3 + 3) & ~3];  // one padded BMP row (1440B)

    // --- BMP file (14) + DIB (40) header ---
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t*)(hdr + 2)  = file_size;
    *(uint32_t*)(hdr + 10) = 54;          // pixel data offset
    *(uint32_t*)(hdr + 14) = 40;          // DIB header size
    *(int32_t*) (hdr + 18) = W;
    *(int32_t*) (hdr + 22) = -H;          // negative = top-down row order
    *(uint16_t*)(hdr + 26) = 1;           // colour planes
    *(uint16_t*)(hdr + 28) = 24;          // bits per pixel (RGB888)
    *(uint32_t*)(hdr + 34) = row_stride * H;

    server.setContentLength(file_size);
    server.send(200, "image/bmp", "");
    WiFiClient client = server.client();
    client.write(hdr, sizeof(hdr));

    // --- Convert RGB565 → RGB888 one row at a time and stream it out ---
    const uint16_t* src = (const uint16_t*)fb0;
    memset(rowbuf, 0, sizeof(rowbuf));    // zero the padding bytes once
    for (int y = 0; y < H; y++) {
        const uint16_t* srow = src + y * W;
        for (int x = 0; x < W; x++) {
            uint16_t px = srow[x];
            rowbuf[x*3 + 0] = (px & 0x1F)         << 3;  // B
            rowbuf[x*3 + 1] = ((px >> 5)  & 0x3F) << 2;  // G
            rowbuf[x*3 + 2] = ((px >> 11) & 0x1F) << 3;  // R
        }
        if (client.write(rowbuf, row_stride) != (size_t)row_stride) return;  // client gone
        if ((y & 31) == 31) delay(0);     // feed watchdog / WiFi stack every 32 rows
    }
}

// Auto-refreshing preview page — connect to gauge WiFi, open in browser
static void handlePreview() {
    String html = "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>" + device_name + " Preview</title>"
        "<style>body{background:#111;text-align:center;margin:0;padding:10px;font-family:sans-serif;color:#fff}"
        "img{width:480px;height:480px;max-width:100%;border-radius:50%;box-shadow:0 0 30px #333}"
        ".controls{margin:10px} a{color:#aaa;margin:0 10px;text-decoration:none}"
        "</style></head><body>"
        "<h2>" + device_name + "</h2>"
        "<img id='scr' src='/snapshot'>"
        "<div class='controls'>"
        "<a href='/preview'>Refresh</a>"
        "<a href='/'>Config</a>"
        "</div>"
        "<script>"
        "function reload(){document.getElementById('scr').src='/snapshot?t='+Date.now()}"
        "var iv=setInterval(reload,3000);"  // auto-refresh every 3s (snapshot is rate-limited)
        "</script>"
        "</body></html>";
    server.send(200, "text/html", html);
}

static void handleOTAPage() {
    String html = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<style>body{font-family:sans-serif;text-align:center;background:#222;color:#fff;padding:20px;}";
    html += "input,button{font-size:16px;padding:10px;margin:10px;border-radius:5px;border:none;}";
    html += "button{background:#e65100;color:white;width:200px;cursor:pointer;}</style></head><body>";
    html += "<h2>" + device_name + " - Firmware Update</h2>";
    html += "<p style='color:#aaa'>Select a .bin file built for this device.</p>";
    html += "<form method='POST' action='/ota' enctype='multipart/form-data'>";
    html += "<input type='file' name='firmware' accept='.bin'><br>";
    html += "<button type='submit'>Flash Firmware</button>";
    html += "</form><p><a href='/' style='color:#aaa'>Back</a></p></body></html>";
    server.send(200, "text/html", html);
}

static void handleOTAUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("[OTA] Success: %u bytes written\n", upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

static void handleOTADone() {
    bool ok = !Update.hasError();
    String html = "<html><body style='background:#222;color:#fff;text-align:center;font-family:sans-serif;padding:20px'>";
    if (ok) {
        html += "<h2>Update Successful!</h2><p>Rebooting in 3 seconds...</p>";
        html += "<script>setTimeout(()=>location.href='/',5000)</script>";
    } else {
        html += "<h2>Update Failed</h2><p><a href='/ota' style='color:#aaa'>Try again</a></p>";
    }
    html += "</body></html>";
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", html);
    if (ok) reboot_at_ms = millis() + 800;  // reboot from loop() after the response flushes
}

static void handleName() {
    if (server.hasArg("n")) {
        String n = server.arg("n");
        n.trim();
        if (n.length() > 0 && n.length() <= 20) {
            device_name = n;
            cfg_put_string("devname", device_name);
            server.sendHeader("Location", "/");
            server.send(303);
            reboot_at_ms = millis() + 400;  // restart (from loop) to apply new AP SSID
        } else {
            server.send(400, "text/plain", "Name must be 1-20 characters");
        }
    } else {
        server.send(400, "text/plain", "Missing name");  // was: no response at all (client hung)
    }
}

static void handleRemote() {
    if (server.hasArg("mac") && server.hasArg("mode")) {
      String macStr = server.arg("mac");
        int m = server.arg("mode").toInt();
        Serial.printf("[HTTP] handleRemote: remote mode -> %d, mac=%s\n", m, macStr.c_str());
        uint8_t targetMac[6];
        for (int i = 0; i < 6; i++) { String byteStr = macStr.substring(i*2, i*2+2); targetMac[i] = (uint8_t) strtol(byteStr.c_str(), NULL, 16); }
        send_remote_command(targetMac, m);
        server.send(200, "text/plain", "OK");
    } else { server.send(400, "text/plain", "Bad Request"); }
}

static void handleUIColors() {
    if (server.hasArg("cbg")) {
        color_background = hexToColor(server.arg("cbg"));
        color_mode_label = hexToColor(server.arg("cml"));
        color_link_icon = hexToColor(server.arg("cli"));
        needle_color = hexToColor(server.arg("cn"));
        color_peak = hexToColor(server.arg("cp"));
        cfg_put_uint("cbg", color_background);
        cfg_put_uint("cml", color_mode_label);
        cfg_put_uint("cli", color_link_icon);
        cfg_put_uint("cn", needle_color);
        cfg_put_uint("cp", color_peak);
        // Broadcast UI colors to fleet
        EspNowPacket pkt = {};
        pkt.type = 7;
        pkt.c1 = color_background;
        pkt.c2 = color_mode_label;
        pkt.c3 = color_link_icon;
        pkt.c4 = needle_color;
        pkt.value = (int)color_peak;
        broadcast_packet(&pkt);
        flag_theme_update = true;
        globals_to_theme(active_theme); persist_theme(active_theme);  // edit lands in the active slot
    }
    server.send(200, "text/plain", "OK");
}

// Background gradient editor. Sets the live gradient globals, persists the legacy
// live keys (for slot-0 migration), broadcasts to the fleet (type 8), and folds the
// edit into the active theme slot — mirroring handleUIColors().
static void handleGrad() {
    if (server.hasArg("gt")) {
        color_background  = hexToColor(server.arg("cbg"));
        color_background2 = hexToColor(server.arg("b2"));
        color_background3 = hexToColor(server.arg("b3"));
        bg_grad_type  = (uint8_t)constrain(server.arg("gt").toInt(), 0, 5);
        bg_grad_stops = (server.arg("gs").toInt() == 3) ? 3 : 2;
        bg_grad_angle = (uint16_t)constrain(server.arg("ga").toInt(), 0, 360);
        cfg_put_uint("cbg", color_background);
        cfg_put_uint("cbg2", color_background2);
        cfg_put_uint("cbg3", color_background3);
        cfg_put_uchar("cgt", bg_grad_type);
        cfg_put_uchar("cgs", bg_grad_stops);
        cfg_put_ushort("cga", bg_grad_angle);
        // Fleet sync: c1/c2 = stops 2/3, c3 = background (stop 1), value packs type|stops|angle.
        EspNowPacket pkt = {}; pkt.type = 8;
        pkt.c1 = color_background2; pkt.c2 = color_background3; pkt.c3 = color_background;
        pkt.value = (int)((uint32_t)bg_grad_type | ((uint32_t)bg_grad_stops << 4) | ((uint32_t)bg_grad_angle << 8));
        broadcast_packet(&pkt);
        flag_theme_update = true;
        globals_to_theme(active_theme); persist_theme(active_theme);  // edit lands in the active slot
    }
    server.send(200, "text/plain", "OK");
}

// Toggle the rotary-driven theme link. Enabling it snaps to the rotary's last
// known position immediately so the user doesn't have to twist the knob first.
static void handleThemeSync() {
    trimpot_theme_sync = !trimpot_theme_sync;
    cfg_put_bool("tpsync", trimpot_theme_sync);
    if (trimpot_theme_sync && last_trimpot3 >= 0 && last_trimpot3 < THEME_SLOTS) {
        active_theme = (uint8_t)last_trimpot3;
        cfg_put_uint("atheme", active_theme);
        theme_to_globals(active_theme);
        flag_theme_update = true;
    }
    server.send(200, "text/plain", trimpot_theme_sync ? "1" : "0");
}

// Select which slot is live / being edited (web-side preview; rotary overrides
// this when sync is on and the knob moves).
static void handleThemeSlot() {
    if (server.hasArg("s")) {
        active_theme = (uint8_t)constrain(server.arg("s").toInt(), 0, THEME_SLOTS - 1);
        cfg_put_uint("atheme", active_theme);
        theme_to_globals(active_theme);
        flag_theme_update = true;
    }
    server.send(200, "text/plain", String((int)active_theme));
}

void setup_wifi() {
  WiFi.mode(WIFI_AP_STA);

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  char ssid[32];
  snprintf(ssid, sizeof(ssid), "Haltech-%s", device_name.c_str());
  if (!WiFi.softAP(ssid, NULL, WIFI_CHANNEL)) {
    Serial.println("[WIFI] softAP FAILED (out of internal heap?) — web/OTA unreachable");
  }

  // Reduce WiFi power to minimize RF interference with display PSRAM bus
  esp_wifi_set_max_tx_power(34); // Reduce to ~8.5dBm to minimise PSRAM bus contention during TX bursts

  // Web server + OTA are the recovery path — bring them up BEFORE ESP-NOW so a
  // fleet-sync init failure can never take down the ability to re-flash.
  server.on("/", handleRoot);
  server.on("/theme", handleTheme); server.on("/set", handleSet); server.on("/rem", handleRemote); server.on("/name", handleName);
  server.on("/bright", handleBright); server.on("/test", handleTest); server.on("/stats", handleStats); server.on("/debug", handleDebug);
  server.on("/peak", handlePeak); server.on("/uicolors", handleUIColors); server.on("/font", handleFont);
  server.on("/secondary", handleSecondary); server.on("/page", handlePage);
  server.on("/themesync", handleThemeSync); server.on("/themeslot", handleThemeSlot);
  server.on("/grad", handleGrad);
  server.on("/preview", handlePreview); server.on("/snapshot", handleSnapshot);
  server.on("/ota", HTTP_GET, handleOTAPage);
  server.on("/ota", HTTP_POST, handleOTADone, handleOTAUpload);
  server.begin();

  // ArduinoOTA — allows PlatformIO upload direct over WiFi
  // Connect PC to gauge AP, then: pio run -t upload --upload-port 192.168.4.1
  ArduinoOTA.setHostname(device_name.c_str());
  ArduinoOTA.onStart([]()  { Serial.println("[OTA] ArduinoOTA start"); });
  ArduinoOTA.onEnd([]()    { Serial.println("[OTA] ArduinoOTA done"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] Error %u\n", e); });
  ArduinoOTA.begin();

  // ESP-NOW last — fleet sync is optional; web/OTA above must survive its failure.
  fleet_init();
}

void web_pump() {
  server.handleClient();
  ArduinoOTA.handle();
}
