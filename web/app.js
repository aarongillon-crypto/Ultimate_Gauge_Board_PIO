// Gauge SPA — talks to the JSON API (see src/web/api.cpp).
// GET = read, POST = write. Small writes use query args; themes use JSON bodies.
'use strict';
var MN = ['M1', 'M2', 'M3', 'M4'];          // generic; local buttons get real labels from config
var COLOR_KEYS = ['text','low','mid','high','bg','modeLabel','linkIcon','needle','peak'];
var themesCache = null;   // /api/themes payload
var chansCache = [];      // /api/channels payload (registry + live values)
var cfgCache = null;      // /api/config payload
var editing = -1;         // slot open in the editor, -1 = closed
var liveOpen = false;

function $(id){ return document.getElementById(id); }
function toast(msg, err){
  var t = $('toast'); t.textContent = msg;
  t.className = 'toast show' + (err ? ' err' : '');
  clearTimeout(t._h); t._h = setTimeout(function(){ t.className = 'toast'; }, 1800);
}
function jget(u){ return fetch(u).then(function(r){ if(!r.ok) throw new Error(u+' '+r.status); return r.json(); }); }
function post(u, body){
  var opt = { method: 'POST' };
  if (body !== undefined) { opt.headers = {'Content-Type':'application/json'}; opt.body = JSON.stringify(body); }
  return fetch(u, opt).then(function(r){ if(!r.ok) throw new Error(u+' '+r.status); return r.text(); });
}
function act(name, q){ return post('/api/action/' + name + (q || '')).then(function(v){ refreshState(); return v; }).catch(function(){ toast('Failed', 1); }); }

// ---------- state ----------
function renderState(s){
  $('devname').textContent = s.name;
  document.title = s.name;
  $('chippeers').textContent = s.peers;
  $('chipmode').textContent = s.modeLabel || ('M' + (s.mode + 1));
  $('chippage').textContent = s.page === 0 ? 'GAUGE' : 'GLOWCRAFT';
  $('chipslot').textContent = 'P' + s.slot + (s.tpsync ? ' ⟲' : '');
  $('chipcan').textContent = s.canOk ? 'OK' : 'DOWN';
  $('brval').textContent = s.bright; $('bright').value = s.bright;
  setTgl('test', 'Test', s.test); setTgl('stats', 'Stats', s.stats);
  setTgl('dbg', 'Debug', s.dbg); setTgl('peak', 'Peak Hold', s.peak);
  $('font').textContent = 'Font: ' + (s.font === 0 ? 'DSEG14' : 'Fira Mono');
  $('tpsync').textContent = 'Trim Sync: ' + (s.tpsync ? 'ON' : 'OFF');
  $('tpsync').classList.toggle('on', !!s.tpsync);
  for (var i = 0; i < 4; i++) $('m'+i).classList.toggle('active', s.mode === i);
  $('pg0').classList.toggle('active', s.page === 0);
  $('pg1').classList.toggle('active', s.page === 1);
  state_sec = s.sec;
  if (document.activeElement !== $('sec')) $('sec').value = s.sec;
  if (!$('newname').value) $('newname').value = s.name;
  $('footer').textContent = 'v' + s.fw + ' · built ' + s.build;
  if (themesCache && themesCache.active !== s.slot) loadThemes();  // rotary moved the slot
}
function setTgl(id, label, on){
  var b = $(id); b.textContent = label + ': ' + (on ? 'ON' : 'OFF'); b.classList.toggle('on', !!on);
}
function refreshState(){ return jget('/api/state').then(renderState).catch(function(){}); }

// ---------- theme slots ----------
function renderSlots(t){
  themesCache = t;
  var html = '';
  t.slots.forEach(function(s, i){
    var sw = COLOR_KEYS.slice(0, 5).map(function(k){ return '<i style="background:' + s.colors[k] + '"></i>'; }).join('');
    html += '<div class="slot' + (i === t.active ? ' activeslot' : '') + '">'
      + '<div class="swatches">' + sw + '</div>'
      + '<div class="name">' + esc(s.name) + '<small>P' + i + (i === t.active ? ' · ACTIVE' : '') + '</small></div>'
      + '<button class="sm" onclick="openEditor(' + i + ')">Edit</button>'
      + '<button class="sm" onclick="copySlot(' + i + ')">Copy</button>'
      + (i === t.active ? '' : '<button class="sm" onclick="activate(' + i + ')">Activate</button>')
      + '</div>';
  });
  $('slots').innerHTML = html;
}
function esc(s){ return String(s).replace(/[&<>"]/g, function(c){ return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]; }); }
function loadThemes(){ return jget('/api/themes').then(renderSlots).catch(function(){}); }
function activate(i){
  post('/api/themes/' + i + '/activate').then(function(){ toast('Slot P' + i + ' live'); loadThemes(); refreshState(); })
    .catch(function(){ toast('Failed', 1); });
}
function tglSync(){ act('tpsync').then(loadThemes); }
function copySlot(src){
  var dst = prompt('Copy P' + src + ' into which slot? (0-3)');
  if (dst === null) return;
  dst = parseInt(dst, 10);
  if (isNaN(dst) || dst < 0 || dst > 3 || dst === src) { toast('Bad slot', 1); return; }
  post('/api/themes/' + dst + '/copy?from=' + src)
    .then(function(){ toast('P' + src + ' → P' + dst); loadThemes(); refreshState(); })
    .catch(function(){ toast('Copy failed', 1); });
}

// ---------- editor ----------
function openEditor(i){
  var s = themesCache.slots[i];
  editing = i;
  $('edslot').textContent = i;
  $('edname').value = s.name;
  COLOR_KEYS.forEach(function(k){ $('c_' + k).value = s.colors[k]; });
  $('c_g2').value = s.gradient.c2; $('c_g3').value = s.gradient.c3;
  $('g_type').value = s.gradient.type; $('g_stops').value = s.gradient.stops;
  $('g_angle').value = s.gradient.angle; $('gaval').textContent = s.gradient.angle + '°';
  $('edhint').textContent = (i === themesCache.active)
    ? 'This is the ACTIVE slot — Save applies live and pushes to all gauges.'
    : 'Inactive slot — Save stores it; the display only changes when you Activate.';
  gradUI();
  $('editor').classList.remove('hidden');
  $('editor').scrollIntoView({ behavior: 'smooth' });
}
function closeEditor(){ editing = -1; $('editor').classList.add('hidden'); }
function gradUI(){
  var t = +$('g_type').value, s = +$('g_stops').value;
  $('gsrow').style.display = t == 0 ? 'none' : '';
  $('g2row').style.display = t == 0 ? 'none' : '';
  $('g3row').style.display = (t != 0 && s == 3) ? '' : 'none';
  var ang = (t == 3 || t == 5);
  $('garow').style.display = ang ? '' : 'none';
  $('g_angle').style.display = ang ? '' : 'none';
}
function collectTheme(){
  var colors = {};
  COLOR_KEYS.forEach(function(k){ colors[k] = $('c_' + k).value; });
  return {
    fmt: 'ugb-theme', v: 1,
    name: $('edname').value.trim() || ('P' + editing),
    colors: colors,
    gradient: { type: +$('g_type').value, stops: +$('g_stops').value,
                angle: +$('g_angle').value, c2: $('c_g2').value, c3: $('c_g3').value }
  };
}
function saveTheme(){
  if (editing < 0) return;
  post('/api/themes/' + editing, collectTheme())
    .then(function(){ toast('Slot P' + editing + ' saved'); loadThemes(); refreshState(); })
    .catch(function(){ toast('Save failed', 1); });
}
function exportTheme(){
  if (editing < 0) return;
  var data = JSON.stringify(collectTheme(), null, 2);
  var a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([data], { type: 'application/json' }));
  a.download = 'theme-p' + editing + '.json';
  a.click();
  URL.revokeObjectURL(a.href);
}
function toggleImport(){ $('importbox').classList.toggle('hidden'); }
function importTheme(){
  var slot = +$('importslot').value, obj;
  try { obj = JSON.parse($('importjson').value); } catch (e) { toast('Bad JSON', 1); return; }
  post('/api/themes/' + slot, obj)
    .then(function(){ toast('Imported into P' + slot); $('importjson').value = ''; toggleImport(); loadThemes(); })
    .catch(function(){ toast('Import rejected', 1); });
}

// ---------- controls ----------
function tglBtn(name){ act(name); }
function tglFont(){ act('font'); }
function setMode(m){ act('mode', '?v=' + m); }
function setPage(p){ act('page', '?v=' + p); }
function rename(){
  var n = $('newname').value.trim();
  if (!n) { toast('Name required', 1); return; }
  post('/api/action/name?v=' + encodeURIComponent(n))
    .then(function(){ toast('Renamed — device restarting'); })
    .catch(function(){ toast('Rename failed', 1); });
}

// ---------- channels ----------
function chanOptions(selected, allowNone){
  var html = allowNone ? '<option value="0"' + (selected == 0 ? ' selected' : '') + '>None</option>' : '';
  chansCache.forEach(function(c){
    if (!c.key) return;   // bit channels are not bindable
    html += '<option value="' + c.key + '"' + (c.key == selected ? ' selected' : '') + '>'
      + esc(c.name) + (c.unit ? ' (' + c.unit + ')' : '') + '</option>';
  });
  return html;
}
function loadChannels(){
  return jget('/api/channels').then(function(d){
    chansCache = d.channels;
    $('sec').innerHTML = chanOptions(state_sec, true);
    if (cfgCache) renderConfig(cfgCache);   // re-render selects with names
    if (liveOpen) renderLive();
  }).catch(function(){});
}
var state_sec = 0;

// ---------- behavior config ----------
var BEHAV_FIELDS = ['min', 'max', 'z1', 'z2'];
var UNIT_LABELS = { psi: ['Pressure: psi', 'Pressure: kPa'], degF: ['Temp: °F', 'Temp: °C'],
                    mph: ['Speed: mph', 'Speed: km/h'], afr: ['Lambda: AFR', 'Lambda: λ'] };
function renderConfig(c){
  cfgCache = c;
  var html = '';
  c.modes.forEach(function(m, i){
    html += '<div class="crow"><input type="text" id="b_' + i + '_label" value="' + esc(m.label) + '" maxlength="13" style="width:90px;margin:0">'
      + '<select id="b_' + i + '_chan" style="flex:1;margin-left:8px">' + chanOptions(m.chan, false) + '</select></div>';
    html += '<div class="crow" style="margin-top:2px">'
      + BEHAV_FIELDS.map(function(f){
          return '<input type="number" step="any" id="b_' + i + '_' + f + '" value="' + m[f] + '" placeholder="' + f + '" title="' + f + '" style="width:23%">';
        }).join('') + '</div>';
  });
  $('behav').innerHTML = html;
  $('smooth').value = Math.round(c.smoothing * 100);
  $('smval').textContent = c.smoothing.toFixed(2);
  $('maxrate').value = c.maxRate;
  $('pkhold').value = Math.round(c.peakHoldMs / 1000);
  // Units toggles + local mode button labels
  Object.keys(UNIT_LABELS).forEach(function(k){
    var on = c.units[k];
    var b = $('u_' + k);
    b.textContent = UNIT_LABELS[k][on ? 0 : 1];
    b.classList.toggle('on', !!on);
  });
  c.modes.forEach(function(m, i){ $('m' + i).textContent = m.label; });
}
function tglUnit(k){
  if (!cfgCache) return;
  cfgCache.units[k] = !cfgCache.units[k];
  renderConfig(cfgCache);
  toast('Unit changed — Save Behavior to apply');
}
function loadConfig(){ return jget('/api/config').then(renderConfig).catch(function(){}); }
function saveConfig(){
  if (!cfgCache) return;
  var modes = [];
  for (var i = 0; i < 4; i++) {
    var m = { chan: +$('b_' + i + '_chan').value, label: $('b_' + i + '_label').value.trim() || ('M' + (i+1)) };
    var bad = false;
    BEHAV_FIELDS.forEach(function(f){
      var v = parseFloat($('b_' + i + '_' + f).value);
      if (isNaN(v)) bad = true;
      m[f] = v;
    });
    if (bad || !(m.min < m.max)) { toast('Mode ' + (i+1) + ': min must be < max', 1); return; }
    modes.push(m);
  }
  var body = {
    modes: modes,
    smoothing: (+$('smooth').value) / 100,
    maxRate: parseFloat($('maxrate').value) || 40,
    peakHoldMs: (parseInt($('pkhold').value, 10) || 30) * 1000,
    units: cfgCache.units
  };
  post('/api/config', body)
    .then(function(){ toast('Behavior saved + synced'); loadConfig(); refreshState(); })
    .catch(function(){ toast('Save failed', 1); });
}

// ---------- live channels ----------
function renderLive(){
  var rows = chansCache.filter(function(c){ return c.age !== undefined && c.age < 3000; });
  if (!rows.length) { $('livechans').innerHTML = '<small>No live CAN data (check bus / enable Test mode).</small>'; return; }
  $('livechans').innerHTML = rows.map(function(c){
    return '<div class="crow" style="margin:2px 0"><span>' + esc(c.name) + '</span><b>'
      + c.val + ' ' + esc(c.unit || '') + '</b></div>';
  }).join('');
}
function toggleLive(){
  liveOpen = !liveOpen;
  $('livechans').classList.toggle('hidden', !liveOpen);
  $('livebtn').textContent = liveOpen ? 'Hide live channel data' : 'Show live channel data';
  if (liveOpen) loadChannels();
}

// ---------- fleet ----------
function renderFleet(f){
  if (!f.peers.length) { $('fleet').innerHTML = '<small>No peers seen yet.</small>'; return; }
  var html = '';
  f.peers.forEach(function(p){
    var isV2 = p.proto >= 2;
    var title = isV2 && p.name ? esc(p.name) : 'Gauge ' + p.mac.slice(-6);
    var sub = 'Mode ' + (MN[p.mode] || '?')
      + (isV2 ? ' · v' + p.fw + ' · P' + p.slot : ' · <span style="color:#ffb020">legacy</span>');
    var modeBtns = MN.map(function(n, m){
      return '<button class="sm" onclick="peerMode(\'' + p.mac + '\',' + m + ')">' + n + '</button>';
    }).join('');
    html += '<div class="slot" style="flex-wrap:wrap">'
      + '<div class="name" style="min-width:100%">' + title + '<small>' + sub + '</small></div>'
      + modeBtns
      + '<button class="sm" onclick="pushThemeTo(\'' + p.mac + '\')">Theme…</button>'
      + '<button class="sm" onclick="post(\'/api/fleet/' + p.mac + '/config\').then(function(){toast(\'Config sent\')})">Config</button>'
      + (isV2 ? '<button class="sm" onclick="post(\'/api/fleet/' + p.mac + '/identify\').then(function(){toast(\'Blinking\')})">Identify</button>' : '')
      + '</div>';
  });
  $('fleet').innerHTML = html;
}
function peerMode(mac, m){
  post('/api/fleet/' + mac + '/mode?v=' + m).then(function(){ toast('Sent'); setTimeout(loadFleet, 600); })
    .catch(function(){ toast('Failed', 1); });
}
function pushThemeTo(mac){
  var slot = prompt('Push which local slot? (0-3)' + (mac === 'ALL' ? ' — to ALL gauges' : ''));
  if (slot === null) return;
  slot = parseInt(slot, 10);
  if (isNaN(slot) || slot < 0 || slot > 3) { toast('Bad slot', 1); return; }
  var activate = confirm('Also ACTIVATE it on the target (OK = activate, Cancel = just store)?');
  post('/api/fleet/' + mac + '/theme?slot=' + slot + '&activate=' + (activate ? 1 : 0))
    .then(function(){ toast('Theme pushed'); })
    .catch(function(){ toast('Push failed', 1); });
}
function loadFleet(){ return jget('/api/fleet').then(renderFleet).catch(function(){}); }

// ---------- boot ----------
(function(){
  refreshState(); loadThemes(); loadFleet(); loadConfig(); loadChannels();
  setInterval(refreshState, 3000);
  setInterval(loadFleet, 5000);
  setInterval(function(){ if (liveOpen) loadChannels(); }, 2000);
})();
